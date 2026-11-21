#pragma once
#include "input.hpp"
#include <immintrin.h>

using Key = std::uint64_t;
constexpr Key external_key = Key{1} << 63;
constexpr Key ascii_mask = 0x7f7f7f7f7f7f7f7fULL;

inline Key mix(Key h) {
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ULL;
    return h ^ (h >> 32);
}

inline Key pack_ascii(Key bytes) {
#ifdef __BMI2__
    return _pext_u64(bytes, ascii_mask);
#else
    Key result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= ((bytes >> (8*i)) & 127) << (7*i);
    return result;
#endif
}

inline Key unpack_ascii(Key packed) {
#ifdef __BMI2__
    return _pdep_u64(packed, ascii_mask);
#else
    Key result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= ((packed >> (7*i)) & 127) << (8*i);
    return result;
#endif
}

// Pack up to nine ASCII bytes in lexical order. Reserve the high bit for
// exact strings that are longer, contain non-ASCII bytes, or end in NUL.
class Keys {
    std::mutex mutex_;
    std::unordered_map<Text,Key> index_;
    std::vector<Text> strings_;
    [[gnu::noinline]] Key intern(Text s) {
        std::lock_guard lock(mutex_);
        auto [it, fresh] = index_.try_emplace(s, external_key + strings_.size());
        if (fresh) strings_.push_back(s);
        return it->second;
    }
public:
    Key encode(Text s) {
        if (s.empty()) return 0;
        if (s.size() <= 9 && s.back() != 0) {
            Key first = 0;
            if (s.size() >= 8) std::memcpy(&first, s.data(), 8);
            else std::memcpy(&first, s.data(), s.size());
            unsigned last = s.size() == 9 ? static_cast<unsigned char>(s[8]) : 0;
            if (!(first & ~ascii_mask) && last < 128)
                return (pack_ascii(__builtin_bswap64(first)) << 7) | last;
        }
        return intern(s);
    }
    const std::vector<Text>& overflow() const { return strings_; }
    static Text decode(Key k, char (&buf)[9], const std::vector<Text>& overflow) {
        if (k & external_key) return overflow[k ^ external_key];
        Key bytes = __builtin_bswap64(unpack_ascii(k >> 7));
        std::memcpy(buf, &bytes, 8);
        buf[8] = k & 127;
        unsigned n = 9;
        while (n && !buf[n-1]) --n;
        return Text(buf,n);
    }
    Text decode(Key k, char (&buf)[9]) const { return decode(k,buf,strings_); }
    static bool less(Key a, Key b, const std::vector<Text>& overflow) {
        if (!((a | b) & external_key)) return a < b;
        char x[9], y[9];
        return decode(a,x,overflow) < decode(b,y,overflow);
    }
    bool less(Key a, Key b) const { return less(a,b,strings_); }
};

class KeyIndex {
    std::vector<Key> keys_;
    std::vector<Id> values_;
    std::size_t mask_ = 0, used_ = 0;
    unsigned skip_;
    std::size_t slot(Key k) const {
        auto pos = (mix(k) >> skip_) & mask_;
        while (keys_[pos] && keys_[pos] != k) pos = (pos+1) & mask_;
        return pos;
    }
    void allocate(std::size_t n) {
        auto keys = std::move(keys_);
        auto values = std::move(values_);
        keys_.assign(n,0);
        values_.resize(n);
        mask_ = n-1;
        for (std::size_t i=0; i<keys.size(); ++i) if (Key k=keys[i]) {
            auto pos = slot(k);
            keys_[pos] = k; values_[pos] = values[i];
        }
    }
public:
    explicit KeyIndex(std::size_t estimate=0, unsigned skip=0) : skip_(skip) {
        allocate(std::bit_ceil(std::max<std::size_t>(16, estimate+estimate/2+1)));
    }
    std::pair<Id*,bool> insert(Key k, Id value) {
        if ((used_+1)*4 > keys_.size()*3) allocate(keys_.size()*2);
        auto pos = slot(k);
        if (keys_[pos]) return {&values_[pos],false};
        ++used_; keys_[pos]=k; values_[pos]=value;
        return {&values_[pos],true};
    }
    Id get(Key k) const {
        auto pos=slot(k);
        return keys_[pos] ? values_[pos] : absent;
    }
    template<class F> void each(F f) const {
        for (std::size_t i=0; i<keys_.size(); ++i) if (keys_[i]) f(keys_[i],values_[i]);
    }
};
