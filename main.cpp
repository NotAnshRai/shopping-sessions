#include <utility>
#include "pipeline.hpp"
#include "output.hpp"
#include <iostream>

using CampaignDays = std::unordered_map<std::uint64_t, Metrics>;
inline std::uint64_t campaign_day(Id campaign, int day) {
    return std::uint64_t(campaign) << 32 | std::uint32_t(day);
}

// Each person has a sliding seven-day window. A campaign enters the active list
// on its first eligible click and leaves when its last eligible click expires.
class ActiveCampaigns {
    std::vector<unsigned> counts_, positions_;
public:
    std::vector<Id> active;
    explicit ActiveCampaigns(std::size_t n) : counts_(n), positions_(n) {}
    void enter(Id c) {
        if (counts_[c]++ == 0) { positions_[c] = active.size(); active.push_back(c); }
    }
    void leave(Id c) {
        if (--counts_[c] == 0) {
            Id last = active.back();
            active[positions_[c]] = last;
            positions_[last] = positions_[c];
            active.pop_back();
        }
    }
    void clear() { for (Id c : active) counts_[c] = 0; active.clear(); }
};

void aggregate(std::vector<Event>& events, std::vector<Click>& clicks, const Catalog& cat,
               const People& people, UserOutput& users, CampaignDays& totals) {
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.person != b.person ? a.person < b.person : a.ts < b.ts;
    });
    std::sort(clicks.begin(), clicks.end(), [](const Click& a, const Click& b) {
        if (a.person != b.person) return a.person < b.person;
        if (a.ts != b.ts) return a.ts < b.ts;
        return a.id > b.id;  // The last click at a timestamp wins.
    });
    // Copies have identical decoded fields, so they are adjacent here.
    clicks.erase(std::unique(clicks.begin(), clicks.end(), [](const Click& a, const Click& b) {
        return a.person == b.person && a.ts == b.ts && a.id == b.id;
    }), clicks.end());

    ActiveCampaigns active(cat.campaigns.size());
    std::vector<I64> category_sum(cat.categories.size());
    std::vector<Id> touched;
    std::size_t ei = 0, ci = 0;
    while (ei < events.size()) {
        Id person = events[ei].person;
        while (ci < clicks.size() && clicks[ci].person < person) ++ci;
        auto left = ci, right = ci;
        while (ci < clicks.size() && clicks[ci].person == person) ++ci;

        int day = utc_day(events[ei].ts);
        I64 sessions = 0, event_count = 0, revenue = 0;
        auto finish_day = [&] {
            Id top = absent;
            for (Id c : touched)
                if (top == absent || category_sum[c] > category_sum[top] ||
                    (category_sum[c] == category_sum[top] && cat.categories[c] < cat.categories[top])) top = c;
            users.add(people.names[person], day, sessions, event_count, revenue,
                      top == absent ? nullptr : &cat.categories[top]);
            for (Id c : touched) category_sum[c] = 0;
            touched.clear(); sessions = event_count = revenue = 0;
        };
        do {
            I64 start = events[ei].ts, last;
            int session_day = utc_day(start);
            if (day != session_day) { finish_day(); day = session_day; }
            ++sessions;
            I64 sale = 0;
            do {
                const auto& ev = events[ei++];
                last = ev.ts;
                ++event_count;
                if (ev.price) {
                    if (category_sum[ev.category] == 0) touched.push_back(ev.category);
                    category_sum[ev.category] += ev.price;
                    sale += ev.price;
                }
            } while (ei < events.size() && events[ei].person == person && events[ei].ts - last <= gap_ms);
            revenue += sale;

            while (right < ci && clicks[right].ts <= start) active.enter(clicks[right++].campaign);
            while (left < right && clicks[left].ts < start - week_ms) active.leave(clicks[left++].campaign);
            Id winner = right == left ? 0 : clicks[right - 1].campaign;
            auto& credit = totals[campaign_day(winner, day)];
            ++credit.sessions; credit.purchasing += (sale > 0); credit.revenue += sale;
            for (Id c : active.active) ++totals[campaign_day(c, day)].assists;
        } while (ei < events.size() && events[ei].person == person);
        finish_day();
        active.clear();
    }
}

fs::path setting(const char* key, const char* fallback) {
    auto* s = std::getenv(key);
    return s && *s ? s : fallback;
}

void run(const fs::path& data, const fs::path& out, int n) {
    Inputs files;
    Catalog catalog(files, data);
    Window window(data / "manifest.json");
    People people(files.add(data / "links"));
    auto events = files.add(data / "events"), clicks = files.add(data / "clicks");
    std::vector<Worker> workers(n);
    {
        auto filter = identify_repeats(events, n);
        parse_events(events, filter, window, catalog, people, workers);
    }
    reduce_repeats(workers);
    parse_clicks(clicks, window, catalog, people, workers);
    people.finish();
    fs::create_directories(out / "user_daily");
    fs::create_directories(out / "campaign_daily");
    std::vector<std::unique_ptr<UserOutput>> writers;
    std::vector<CampaignDays> campaign_days(n);
    for (int i = 0; i < n; ++i)
        writers.emplace_back(std::make_unique<UserOutput>(out / "user_daily" / ("part-" + std::to_string(i) + ".parquet")));
    parallel(shards, n, [&](int owner, std::size_t shard) {
        auto ev = collect(workers, shard, &Worker::events);
        auto cl = collect(workers, shard, &Worker::clicks);
        aggregate(ev, cl, catalog, people, *writers[owner], campaign_days[owner]);
    });
    for (auto& writer : writers) writer->close();
    auto& totals = campaign_days.front();
    for (int i = 1; i < n; ++i)
        for (auto& [k, m] : campaign_days[i]) totals[k].merge(m);
    CampaignOutput result(out / "campaign_daily" / "part-0.parquet");
    for (auto& [key, m] : totals) {
        const auto& c = catalog.campaigns[key >> 32];
        result.add(c.id, c.channel, std::bit_cast<std::int32_t>(std::uint32_t(key)), m);
    }
    result.close();
    // A rerun in the same output directory can use fewer workers.
    for (int i = n; i < 8; ++i)
        fs::remove(out / "user_daily" / ("part-" + std::to_string(i) + ".parquet"));
}

int main(int argc, char** argv) {
    try {
        auto data = argc > 1 ? fs::path(argv[1]) : setting("DATA_DIR", "/data");
        auto output = argc > 2 ? fs::path(argv[2]) : setting("OUTPUT_DIR", "/output");
        int n = std::getenv("NCPU") ? std::stoi(std::getenv("NCPU")) : 8;
        run(data, output, std::clamp(n, 1, 8));
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Shopping Sessions: " << e.what() << '\n';
        return 1;
    }
}
