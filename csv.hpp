#pragma once
#include "input.hpp"
#include <immintrin.h>

struct Delimiters { std::uint64_t comma, lf, quote; };

inline Delimiters scan64(const char* p) {
#if defined(__AVX512BW__)
    auto x=_mm512_loadu_si512(p);
    return {_mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8(',')),
            _mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8('\n')),
            _mm512_cmpeq_epi8_mask(x,_mm512_set1_epi8('"'))};
#elif defined(__AVX2__)
    auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    auto b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p+32));
    auto mask = [&](char c) {
        auto x = _mm256_set1_epi8(c);
        auto lo = std::uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a,x)));
        auto hi = std::uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b,x)));
        return (std::uint64_t(hi)<<32) | lo;
    };
    return {mask(','),mask('\n'),mask('"')};
#else
    Delimiters m{};
    for (unsigned i=0; i<64; i+=16) {
        auto x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p+i));
        auto mask = [&](char c) { return std::uint64_t(_mm_movemask_epi8(_mm_cmpeq_epi8(x,_mm_set1_epi8(c)))) << i; };
        m.comma |= mask(','); m.lf |= mask('\n'); m.quote |= mask('"');
    }
    return m;
#endif
}

template<std::size_t N,class F> void csv_rows(Text input, F&& process) {
    std::array<Text,N> fields_out;
    const char* p=input.data();
    const char* end=p+input.size();
    auto scalar=[&](Text s){ if (fields(s,fields_out)) process(fields_out); };
    while (end-p>=128) {
        auto lo=scan64(p);
        Delimiters hi{};
        unsigned len;
        if (lo.lf) len=std::countr_zero(lo.lf);
        else {
            hi=scan64(p+64);
            if (!hi.lf) {
                auto* newline=static_cast<const char*>(std::memchr(p+128,'\n',end-p-128));
                if (!newline) { scalar(Text(p,end-p)); return; }
                scalar(Text(p,newline-p)); p=newline+1; continue;
            }
            len=64+std::countr_zero(hi.lf);
        }
        auto limit=(std::uint64_t(1)<<(len&63))-1;
        if (len<64) { lo.comma &= limit; lo.quote &= limit; }
        else { hi.comma &= limit; hi.quote &= limit; }
        if (std::popcount(lo.comma)+std::popcount(hi.comma)==N-1) {
            auto d0=lo.comma, d1=hi.comma;
            if (len<64) d0 |= std::uint64_t(1)<<len;
            else d1 |= std::uint64_t(1)<<(len-64);
            auto starts0=(d0<<1)|1, starts1=(d1<<1)|(d0>>63);
            auto ends0=(d0>>1)|(d1<<63), ends1=d1>>1;
            bool valid=!(lo.quote & ~(starts0|ends0)) && !(hi.quote & ~(starts1|ends1));
#ifdef __BMI2__
            unsigned trim=0;
            if (lo.quote || hi.quote) {
                // Fold the quote bits at the start and end of each field into
                // two ordered bitsets. A quoted field needs two distinct quotes.
                auto after0=(lo.quote>>1)|(hi.quote<<63), after1=hi.quote>>1;
                auto before0=lo.quote<<1, before1=(hi.quote<<1)|(lo.quote>>63);
                unsigned open=unsigned(lo.quote&1) | (unsigned(_pext_u64(after0,lo.comma))<<1)
                    | (unsigned(_pext_u64(after1,hi.comma))<<(1+std::popcount(lo.comma)));
                unsigned close=unsigned(_pext_u64(before0,d0)) |
                    (unsigned(_pext_u64(before1,d1))<<std::popcount(d0));
                valid=valid && open==close && !(lo.quote&starts0&ends0) && !(hi.quote&starts1&ends1);
                trim=open;
            }
            if (!valid) { p+=len+1; continue; }
#endif
            unsigned at=0;
            for (unsigned j=0; j<N; ++j) {
                unsigned last;
                if (d0) { last=std::countr_zero(d0); d0 &= d0-1; }
                else { last=64+std::countr_zero(d1); d1 &= d1-1; }
#ifdef __BMI2__
                unsigned q=(trim>>j)&1;
                fields_out[j]=Text(p+at+q,last-at-2*q);
#else
                bool open=p[at]=='"', close=last>at && p[last-1]=='"';
                if (open!=close || (open && last-at<2)) { valid=false; break; }
                fields_out[j]=Text(p+at+open,last-at-open-close);
#endif
                at=last+1;
            }
            if (valid) process(fields_out);
        }
        p+=len+1;
    }
    lines(Text(p,end-p),scalar);
}
