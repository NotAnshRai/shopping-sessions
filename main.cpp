#include <utility>
#include "pipeline.hpp"
#include "output.hpp"
#include <iostream>

using SparseTotals = std::unordered_map<std::uint64_t, Metrics>;
inline std::uint64_t campaign_day(Id c,int day) { return (std::uint64_t(c)<<32)|std::uint32_t(day); }

class CampaignDays {
    int first_;
    std::size_t campaigns_;
    std::vector<Metrics> dense_;
    SparseTotals sparse_;
public:
    CampaignDays(const Window& w,std::size_t n) : first_(utc_day(w.start)), campaigns_(n) {
        std::size_t days=utc_day(w.end-1)-first_+1;
        if (days<=131072/n) dense_.resize(days*n);
    }
    Metrics& at(Id campaign,int day) {
        if (dense_.empty()) return sparse_[campaign_day(campaign,day)];
        return dense_[(day-first_)*campaigns_+campaign];
    }
    void merge_into(SparseTotals& totals) const {
        for (const auto& [k,m]:sparse_) totals[k].merge(m);
        for (std::size_t i=0;i<dense_.size();++i) {
            auto& m=dense_[i];
            if (m.sessions || m.assists) totals[campaign_day(i%campaigns_,first_+int(i/campaigns_))].merge(m);
        }
    }
};

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

struct Group {
    Key person;
    Id events=0,clicks=0,event_end=0,click_end=0;
};

template<bool narrow> void aggregate(std::vector<Event>& events,std::vector<Click>& clicks,
                                    const Catalog& cat,const Keys& keys,const Values& values,
                                    UserOutput& users,CampaignDays& totals) {
    auto time=[&](Id code)->I64 {
        if constexpr(narrow) return values.base+code;
        return values.time(code);
    };
    KeyIndex index(events.size()/24,std::countr_zero(shards));
    std::vector<Group> groups;
    for (auto& e:events) {
        auto [id,added]=index.insert(e.person,groups.size());
        if (added) groups.push_back({e.person});
        ++groups[*id].events; e.person=*id;
    }
    for (auto& c:clicks) {
        Id id=index.get(c.person);
        if (id!=absent) ++groups[id].clicks;
        c.person=id;
    }
    Id event_count=0,click_count=0;
    for (auto& g:groups) {
        g.event_end=event_count; g.click_end=click_count;
        event_count+=g.events; click_count+=g.clicks;
    }
    auto ordered_events=std::make_unique_for_overwrite<Event[]>(event_count);
    auto ordered_clicks=std::make_unique_for_overwrite<Click[]>(click_count);
    for (auto& e:events) ordered_events[groups[e.person].event_end++]=e;
    for (auto& c:clicks) if (c.person!=absent) ordered_clicks[groups[c.person].click_end++]=c;
    ActiveCampaigns active(cat.campaigns.size());
    std::vector<I64> category_sum(cat.categories.size());
    std::vector<Id> touched;
    for (auto& g:groups) {
        auto* ev=ordered_events.get()+(g.event_end-g.events);
        auto* cl=ordered_clicks.get()+(g.click_end-g.clicks);
        auto earlier=[&](const auto& a,const auto& b) {
            if constexpr(narrow) return a.ts<b.ts;
            return time(a.ts)<time(b.ts);
        };
        std::sort(ev,ev+g.events,earlier);
        std::sort(cl,cl+g.clicks,earlier);
        std::size_t ei=0,left=0,right=0,latest=0;
        char buf[9];
        Text person=keys.decode(g.person,buf);
        int day=utc_day(time(ev[0].ts));
        I64 sessions=0,events_today=0,revenue=0;
        auto finish_day=[&] {
            Id top=absent;
            for (Id c:touched)
                if (top==absent || category_sum[c]>category_sum[top] ||
                   (category_sum[c]==category_sum[top] && cat.categories[c]<cat.categories[top])) top=c;
            users.add(person,day,sessions,events_today,revenue,top==absent?nullptr:&cat.categories[top]);
            for (Id c:touched) category_sum[c]=0;
            touched.clear();
            sessions=events_today=revenue=0;
        };
        do {
            I64 start=time(ev[ei].ts), last;
            int new_day=utc_day(start);
            if (day!=new_day) { finish_day(); day=new_day; }
            ++sessions;
            I64 sales=0;
            do {
                const auto& e=ev[ei++];
                last=time(e.ts);
                ++events_today;
                if (e.sale) {
                    auto [price,c]=values.unpack(e.sale);
                    if (!category_sum[c]) touched.push_back(c);
                    category_sum[c]+=price;
                    sales+=price;
                }
            } while (ei<g.events && time(ev[ei].ts)-last<=gap_ms);
            revenue+=sales;
            // Identical click copies enter and leave together. Both the set
            // of active campaigns and the minimum click ID at a tie are exact.
            while (right<g.clicks && time(cl[right].ts)<=start) {
                if (time(cl[right].ts)!=time(cl[latest].ts) || keys.less(cl[right].id,cl[latest].id)) latest=right;
                active.enter(cl[right++].campaign);
            }
            while (left<right && time(cl[left].ts)<start-week_ms) active.leave(cl[left++].campaign);
            Id winner=left==right?0:cl[latest].campaign;
            auto& credit=totals.at(winner,day);
            ++credit.sessions; credit.purchasing+=(sales>0); credit.revenue+=sales;
            for (Id c:active.active) ++totals.at(c,day).assists;
        } while (ei<g.events);
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
    Parsed parsed(files,data,window,catalog,n);
    std::vector<Worker> workers(n);
    parsed.distribute(workers);
    fs::create_directories(out / "user_daily");
    fs::create_directories(out / "campaign_daily");
    std::vector<std::unique_ptr<UserOutput>> writers;
    std::vector<CampaignDays> campaign_days;
    for (int i=0;i<n;++i) campaign_days.emplace_back(window,catalog.campaigns.size());
    for (int i = 0; i < n; ++i)
        writers.emplace_back(std::make_unique<UserOutput>(out / "user_daily" / ("part-" + std::to_string(i) + ".parquet")));
    parallel(shards, n, [&](int owner, std::size_t shard) {
        auto ev = collect(workers, shard, &Worker::events);
        auto cl = collect(workers, shard, &Worker::clicks);
        auto fn=parsed.values.narrow ? aggregate<true> : aggregate<false>;
        fn(ev,cl,catalog,parsed.keys,parsed.values,*writers[owner],campaign_days[owner]);
    });
    for (auto& writer : writers) writer->close();
    SparseTotals totals;
    for (auto& slice:campaign_days) slice.merge_into(totals);
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
#ifdef SELECT_NATIVE_SOLVER
        if (__builtin_cpu_supports("x86-64-v3")) {
            std::error_code ec;
            auto self=fs::read_symlink("/proc/self/exe",ec);
            if (!ec) { self += "-v3"; execv(self.c_str(),argv); }
        }
#endif
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
