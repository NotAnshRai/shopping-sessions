#pragma once
#include "keys.hpp"
#include "csv.hpp"
#include "dates.hpp"
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
    KeyIndex index;
    std::vector<Key> names;
    std::vector<Id> parent,size;
    struct Alias { Key raw=0,canonical=0; };
    std::vector<Alias> aliases;
    std::size_t mask=0;
    explicit People(std::size_t n) : index(n) {}
    Id add(Key k) {
        auto [p,added]=index.insert(k,names.size());
        if (added) { names.push_back(k); parent.push_back(*p); size.push_back(1); }
        return *p;
    }
    Id root(Id i) {
        while (i!=parent[i]) { parent[i]=parent[parent[i]]; i=parent[i]; }
        return i;
    }
    void link(Key ka,Key kb,const std::vector<Text>& overflow) {
        Id a=root(add(ka)), b=root(add(kb));
        if (a==b) return;
        if (size[a]<size[b]) std::swap(a,b);
        parent[b]=a; size[a]+=size[b];
        if (Keys::less(names[b],names[a],overflow)) names[a]=names[b];
    }
    void finish() {
        for (Id i=0; i<names.size(); ++i) names[i]=names[root(i)];
        std::size_t count=0;
        index.each([&](Key k,Id i){ count+=k!=names[i]; });
        aliases.resize(std::bit_ceil(std::max<std::size_t>(8,count*2)));
        mask=aliases.size()-1;
        index.each([&](Key k,Id i){
            if (k==names[i]) return;
            auto slot=mix(k)&mask;
            while (aliases[slot].raw) slot=(slot+1)&mask;
            aliases[slot]={k,names[i]};
        });
        index=KeyIndex{};
        std::vector<Key>{}.swap(names);
        std::vector<Id>{}.swap(parent);
        std::vector<Id>{}.swap(size);
    }
    void prefetch(Key k) const { __builtin_prefetch(&aliases[mix(k)&mask]); }
    Key canonical(Key k) const {
        auto slot=mix(k)&mask;
        while (aliases[slot].raw) {
            if (aliases[slot].raw==k) return aliases[slot].canonical;
            slot=(slot+1)&mask;
        }
        return k;
    }
};

constexpr unsigned shards=128, id_shards=256;
struct Sale { Id price,category; };
struct Values {
    static constexpr Id external=Id{1}<<31;
    struct Overflow { std::vector<I64> times; std::vector<Sale> sales; };
    std::array<Overflow,8> overflow;
    I64 base;
    bool narrow;
    explicit Values(const Window& w) : base(w.start-week_ms), narrow(w.end-base<=external) {}
    Id stamp(I64 ts,unsigned worker) {
        auto offset=std::uint64_t(ts-base);
        if (offset<external) return Id(offset);
        auto& v=overflow[worker].times;
        Id id=external | Id(v.size()*8+worker);
        v.push_back(ts); return id;
    }
    I64 time(Id code) const {
        if (code<external) return base+code;
        return overflow[code%8].times[(code^external)/8];
    }
    Id sale(Id price,Id category,unsigned worker) {
        if (category<128) return (category<<24)|price;
        auto& v=overflow[worker].sales;
        Id id=external | Id(v.size()*8+worker);
        v.push_back({price,category}); return id;
    }
    Sale unpack(Id v) const {
        if (v<external) return {v & 0xffffffu,v>>24};
        return overflow[v%8].sales[(v^external)/8];
    }
};
struct Event { Key person; Id ts,sale; };
struct Click { Key person,id; Id ts,campaign; };
struct Candidate { Key id; I64 ingest; Event event; };
struct Worker {
    std::array<std::vector<Event>,shards> events;
    std::array<std::vector<Click>,shards> clicks;
    std::array<std::vector<Candidate>,id_shards> candidates;
    std::vector<std::pair<Key,Key>> links;
};

template<class T,std::size_t N> std::vector<T> collect(std::vector<Worker>& workers,std::size_t b,
                                                     std::array<std::vector<T>,N> Worker::*member) {
    std::size_t n=0;
    for (auto& w:workers) n+=(w.*member)[b].size();
    std::vector<T> result;
    result.reserve(n);
    for (auto& w:workers) {
        auto& part=(w.*member)[b];
        result.insert(result.end(),part.begin(),part.end());
        std::vector<T>{}.swap(part);
    }
    return result;
}

struct Parsed {
    std::vector<Worker> workers;
    Keys keys;
    Values values;
    std::unique_ptr<People> people;
    Parsed(Inputs& files,const fs::path& path,const Window& w,const Catalog& cat,int n) : workers(n),values(w) {
        Dates dates(w);
        auto links=files.add(path/"links");
        parallel(links.size(),n,[&](int owner,std::size_t j) {
            csv_rows<2>(links[j],[&](auto& f){
                if (!f[0].empty() && !f[1].empty())
                    workers[owner].links.emplace_back(keys.encode(f[0]),keys.encode(f[1]));
            });
        });
        // Other workers may intern new strings while the link task runs.
        const auto link_strings=keys.overflow();
        auto events=files.add(path/"events"), clicks=files.add(path/"clicks");
        std::size_t event_bytes=0,click_bytes=0;
        for (auto s:events) event_bytes+=s.size();
        for (auto s:clicks) click_bytes+=s.size();
        for (auto& local:workers) {
            for (auto& bin:local.candidates) bin.reserve(event_bytes/(73ULL*n*id_shards));
            for (auto& bin:local.clicks) bin.reserve(click_bytes/(38ULL*n*shards));
        }
        parallel(1+events.size()+clicks.size(),n,[&](int owner,std::size_t j){
            if (!j) { resolve_links(link_strings); return; }
            --j;
            if (j<events.size()) {
                csv_rows<7>(events[j],[&](auto& f){
                    Key id=keys.encode(f[0]);
                    if (!id) return;
                    I64 ingest=dates.parse(f[3]), ts=dates.parse(f[2]);
                    Event e{};
                    bool valid=ts>=w.start && ts<w.end && ingest!=bad_time && ingest<=ts+day_ms
                               && !f[1].empty() && !f[5].empty();
                    if (valid && f[4]=="purchase") {
                        Id price=price_cents(f[6]);
                        valid=price>0;
                        if (valid) {
                            auto it=cat.product_category.find(f[5]);
                            e.sale=values.sale(price,it==cat.product_category.end()?0:it->second,owner);
                        }
                    } else if (f[4]!="view" && f[4]!="cart") valid=false;
                    if (valid) { e.person=keys.encode(f[1]); e.ts=values.stamp(ts,owner); }
                    workers[owner].candidates[mix(id)%id_shards].push_back({id,ingest,e});
                });
            } else {
                csv_rows<4>(clicks[j-events.size()],[&](auto& f){
                    if (f[0].empty() || f[1].empty()) return;
                    I64 ts=dates.parse(f[3]);
                    if (ts<w.start-week_ms || ts>=w.end) return;
                    auto it=cat.campaign_id.find(f[2]);
                    if (it==cat.campaign_id.end()) return;
                    Key k=keys.encode(f[1]);
                    workers[owner].clicks[mix(k)%shards].push_back({k,keys.encode(f[0]),values.stamp(ts,owner),it->second});
                });
            }
        });
    }
    void resolve_links(const std::vector<Text>& link_strings) {
        std::size_t count=0;
        for (auto& w:workers) count+=w.links.size();
        people=std::make_unique<People>(count+count/2);
        for (auto& w:workers) {
            for (auto [a,b]:w.links) people->link(a,b,link_strings);
            std::vector<std::pair<Key,Key>>{}.swap(w.links);
        }
        people->finish();
    }
    void distribute(std::vector<Worker>& sorted) {
        parallel(id_shards,workers.size(),[&](int owner,std::size_t b){
            std::size_t count=0;
            for (auto& w:workers) count+=w.candidates[b].size();
            auto capacity=std::bit_ceil(std::max<std::size_t>(8,count+count/2));
            std::vector<std::uint64_t> slots(capacity);
            std::array<Candidate*,8> first{};
            for (std::size_t t=0;t<workers.size();++t) first[t]=workers[t].candidates[b].data();
            for (unsigned t=0;t<workers.size();++t) {
                auto& part=workers[t].candidates[b];
                for (Id i=0;i<part.size();++i) {
                    auto& row=part[i];
                    auto h=mix(row.id);
                    auto slot=(h>>std::countr_zero(id_shards))&(capacity-1);
                    auto value=(h & 0xffffffff00000000ULL) | (1+8*i+t);
                    for (;;) {
                        auto old=slots[slot];
                        if (!old) { slots[slot]=value; break; }
                        if (((old^value)>>32)==0) {
                            Id ref=Id(old)-1;
                            auto& prior=first[ref%8][ref/8];
                            if (prior.id==row.id) {
                                if (row.ingest<prior.ingest) { prior.event.person=0; slots[slot]=value; }
                                else row.event.person=0;
                                break;
                            }
                        }
                        slot=(slot+1)&(capacity-1);
                    }
                }
            }
            for (auto& w:workers) {
                auto& part=w.candidates[b];
                for (std::size_t i=0;i<part.size();++i) {
                    if (i+16<part.size()) people->prefetch(part[i+16].event.person);
                    auto& e=part[i].event;
                    if (!e.person) continue;
                    e.person=people->canonical(e.person);
                    sorted[owner].events[mix(e.person)%shards].push_back(e);
                }
                std::vector<Candidate>{}.swap(part);
            }
        });
        parallel(shards,workers.size(),[&](int owner,std::size_t b){
            for (auto& w:workers) {
                auto& part=w.clicks[b];
                for (std::size_t i=0;i<part.size();++i) {
                    if (i+16<part.size()) people->prefetch(part[i+16].person);
                    auto& c=part[i];
                    c.person=people->canonical(c.person);
                    sorted[owner].clicks[mix(c.person)%shards].push_back(c);
                }
                std::vector<Click>{}.swap(part);
            }
        });
    }
};
