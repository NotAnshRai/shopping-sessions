#pragma once
#include "input.hpp"
#include <numeric>

using Dictionary = std::unordered_map<Text, Id>;
struct Campaign { Text id, channel; };
struct Catalog {
    Dictionary product_category, campaign_id;
    std::vector<Text> categories{"unknown"};
    std::vector<Campaign> campaigns{{"organic", "organic"}};

    Catalog(Inputs& files, const fs::path& data) {
        Dictionary cat_ids{{"unknown", 0}};
        for (Text block : files.add(data / "products.csv")) lines(block, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f)) throw std::runtime_error("Malformed product dimension");
            auto [it, added] = cat_ids.try_emplace(f[1], categories.size());
            if (added) categories.push_back(f[1]);
            product_category.emplace(f[0], it->second);
        });
        for (Text block : files.add(data / "campaigns.csv")) lines(block, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f)) throw std::runtime_error("Malformed campaign dimension");
            campaign_id.emplace(f[0], campaigns.size());
            campaigns.push_back({f[0], f[1]});
        });
    }
};

struct People {
    Dictionary links;
    std::vector<Text> names;
    struct Partition { std::mutex mutex; Dictionary ids; };
    std::array<Partition, 128> singletons;
    std::atomic<Id> next{0};

    explicit People(const std::vector<Text>& chunks) {
        std::vector<Id> parent, size, smallest;
        auto add = [&](Text s) {
            auto [it, added] = links.try_emplace(s, parent.size());
            if (added) {
                Id i = it->second;
                names.push_back(s); parent.push_back(i); size.push_back(1); smallest.push_back(i);
            }
            return it->second;
        };
        auto root = [&](Id i) {
            while (i != parent[i]) { parent[i] = parent[parent[i]]; i = parent[i]; }
            return i;
        };
        for (Text chunk : chunks) lines(chunk, [&](Text line) {
            std::array<Text, 2> f;
            if (!fields(line, f) || f[0].empty() || f[1].empty()) return;
            Id a = root(add(f[0])), b = root(add(f[1]));
            if (a == b) return;
            if (size[a] < size[b]) std::swap(a, b);
            parent[b] = a;
            size[a] += size[b];
            if (names[smallest[b]] < names[smallest[a]]) smallest[a] = smallest[b];
        });
        for (auto& [s, id] : links) id = smallest[root(id)];
        next.store(names.size());
    }
    Id resolve(Text s, std::size_t hash) {
        const auto& linked = std::as_const(links);
        auto found = linked.find(s);
        if (found != linked.end()) return found->second;
        auto& part = singletons[hash % singletons.size()];
        std::lock_guard lock(part.mutex);
        auto [it, added] = part.ids.try_emplace(s, 0);
        if (added) it->second = next.fetch_add(1, std::memory_order_relaxed);
        return it->second;
    }
    void finish() {
        names.resize(next.load());
        for (auto& part : singletons) {
            for (auto [s, id] : part.ids) names[id] = s;
            Dictionary{}.swap(part.ids);
        }
        Dictionary{}.swap(links);
    }
};

// The first atomic OR linearizes concurrent copies. The second filter marks all
// potential duplicates. False positives only send additional rows to the exact
// reducer, where complete event IDs and ingest times decide the winner.
class DuplicateFilter {
    using Word = std::atomic<std::uint64_t>;
    std::size_t size_;
    std::unique_ptr<Word[]> seen_, suspects_;
    static std::uint64_t bits(std::size_t h) {
        return (1ULL << ((h >> 32) & 63)) | (1ULL << ((h >> 40) & 63)) | (1ULL << ((h >> 48) & 63));
    }
public:
    explicit DuplicateFilter(std::size_t bytes)
        : size_(std::bit_ceil(std::clamp<std::size_t>(bytes / 400, 64, 1u << 25))),
          seen_(std::make_unique<Word[]>(size_)), suspects_(std::make_unique<Word[]>(size_)) {}
    void observe(std::size_t h) {
        auto i = h & (size_ - 1), mask = bits(h);
        if ((seen_[i].fetch_or(mask, std::memory_order_relaxed) & mask) == mask)
            suspects_[i].fetch_or(mask, std::memory_order_relaxed);
    }
    void seal() { seen_.reset(); }
    bool possible(std::size_t h) const {
        auto mask = bits(h);
        return (suspects_[h & (size_ - 1)].load(std::memory_order_relaxed) & mask) == mask;
    }
};

constexpr unsigned shards = 64;
struct Event { I64 ts; Id person, price, category; };
struct Click { Text id; I64 ts; Id person, campaign; };
struct Candidate { Text id; I64 ingest; Event event; };

struct Worker {
    struct CacheEntry { Text name; Id person; };
    std::vector<CacheEntry> cache = std::vector<CacheEntry>(8192);
    std::array<std::vector<Event>, shards> events;
    std::array<std::vector<Click>, shards> clicks;
    std::array<std::vector<Candidate>, shards> repeats;
    Id person(Text user, People& people) {
        auto h = hash_text(user);
        auto& slot = cache[h % cache.size()];
        if (slot.name != user) slot = {user, people.resolve(user, h)};
        return slot.person;
    }
};

inline DuplicateFilter identify_repeats(const std::vector<Text>& chunks, int threads) {
    std::size_t bytes = 0;
    for (auto c : chunks) bytes += c.size();
    DuplicateFilter filter(bytes);
    parallel(chunks.size(), threads, [&](int, std::size_t i) {
        lines(chunks[i], [&](Text line) {
            auto comma = line.find(',');
            if (comma == Text::npos || comma == 0) return;
            Text id = line.substr(0, comma);
            if (id.size() >= 2 && id.front() == '"' && id.back() == '"')
                id = id.substr(1, id.size() - 2);
            if (!id.empty()) filter.observe(hash_text(id));
        });
    });
    filter.seal();
    return filter;
}

inline void parse_events(const std::vector<Text>& chunks, const DuplicateFilter& filter, const Window& w,
                         const Catalog& cat, People& people, std::vector<Worker>& workers) {
    parallel(chunks.size(), workers.size(), [&](int worker, std::size_t i) {
        auto& local = workers[worker];
        lines(chunks[i], [&](Text line) {
            std::array<Text, 7> f;
            if (!fields(line, f) || f[0].empty()) return;
            auto h = hash_text(f[0]);
            bool repeat = filter.possible(h);
            I64 ingest = timestamp(f[3]), ts = timestamp(f[2]);
            Event ev{ts, absent, 0, 0};
            bool valid = ts >= w.start && ts < w.end && ingest != bad_time && ingest <= ts + day_ms &&
                         !f[1].empty() && !f[5].empty();
            if (valid && f[4] == "purchase") {
                ev.price = price_cents(f[6]); valid = ev.price > 0;
                auto found = cat.product_category.find(f[5]);
                if (found != cat.product_category.end()) ev.category = found->second;
            } else if (f[4] != "view" && f[4] != "cart") valid = false;
            if (valid) ev.person = local.person(f[1], people);
            if (repeat) local.repeats[h % shards].push_back({f[0], ingest, ev});
            else if (valid) local.events[ev.person % shards].push_back(ev);
        });
    });
}

inline void reduce_repeats(std::vector<Worker>& workers) {
    parallel(shards, workers.size(), [&](int owner, std::size_t b) {
        std::unordered_map<Text, const Candidate*> winners;
        std::size_t count = 0;
        for (auto& w : workers) count += w.repeats[b].size();
        winners.reserve(count);
        for (auto& w : workers) for (auto& row : w.repeats[b]) {
            auto [it, inserted] = winners.try_emplace(row.id, &row);
            if (!inserted && row.ingest < it->second->ingest) it->second = &row;
        }
        for (auto& entry : winners) {
            auto& ev = entry.second->event;
            if (ev.person != absent) workers[owner].events[ev.person % shards].push_back(ev);
        }
        for (auto& w : workers) std::vector<Candidate>{}.swap(w.repeats[b]);
    });
}

inline void parse_clicks(const std::vector<Text>& chunks, const Window& w, const Catalog& cat,
                         People& people, std::vector<Worker>& workers) {
    parallel(chunks.size(), workers.size(), [&](int worker, std::size_t i) {
        auto& local = workers[worker];
        lines(chunks[i], [&](Text line) {
            std::array<Text, 4> f;
            if (!fields(line, f) || f[0].empty() || f[1].empty()) return;
            I64 ts = timestamp(f[3]);
            if (ts < w.start - week_ms || ts >= w.end) return;
            auto found = cat.campaign_id.find(f[2]);
            if (found == cat.campaign_id.end()) return;
            Id p = local.person(f[1], people);
            local.clicks[p % shards].push_back({f[0], ts, p, found->second});
        });
    });
}

template<class T> std::vector<T> collect(std::vector<Worker>& workers, std::size_t shard,
                                        std::array<std::vector<T>, shards> Worker::*member) {
    std::vector<T> all;
    std::size_t n = 0;
    for (auto& w : workers) n += (w.*member)[shard].size();
    all.reserve(n);
    for (auto& w : workers) {
        auto& part = (w.*member)[shard];
        all.insert(all.end(), part.begin(), part.end());
        std::vector<T>{}.swap(part);
    }
    return all;
}
