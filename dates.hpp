#pragma once
#include "input.hpp"
#include <cstdio>

class Dates {
    struct Date { std::uint64_t prefix=0; std::uint16_t suffix=0; I64 midnight=0; };
    std::array<Date,256> dates_{};
    static unsigned slot(const char* s) {
        return ((s[6]&3)<<6) | ((s[8]&3)<<4) | (s[9]&15);
    }
    template<class T> static T load(const char* p) {
        T v; std::memcpy(&v,p,sizeof v); return v;
    }
public:
    explicit Dates(const Window& w) {
        int first=utc_day(w.start)-10, last=std::min(first+95,utc_day(w.end)+8);
        for (int day=first; day<=last; ++day) {
            auto date=std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{day}}};
            char s[24];
            int n=std::snprintf(s,sizeof(s),"%04d-%02u-%02u",int(date.year()),unsigned(date.month()),unsigned(date.day()));
            if (n==10) dates_[slot(s)]={load<std::uint64_t>(s),load<std::uint16_t>(s+8),I64(day)*day_ms};
        }
    }
    I64 parse(Text s) const {
        if (s.size()!=20 && s.size()!=25) return timestamp(s);
        auto& d=dates_[slot(s.data())];
        if (!d.prefix || load<std::uint64_t>(s.data())!=d.prefix || load<std::uint16_t>(s.data()+8)!=d.suffix)
            return timestamp(s);
        auto raw=load<std::uint64_t>(s.data()+11);
        constexpr std::uint64_t pattern=0x30303a30303a3030ULL, hi=0xf0f0fff0f0fff0f0ULL;
        auto digits=raw & 0x0f0f000f0f000f0fULL;
        if (((raw^pattern)&hi) || ((digits+0x0606000606000606ULL)&0x1010001010001010ULL) || s[10]!='T')
            return bad_time;
        int h=int((digits&15)*10+((digits>>8)&15));
        int m=int(((digits>>24)&15)*10+((digits>>32)&15));
        int sec=int(((digits>>48)&15)*10+(digits>>56));
        if (h>23 || m>59 || sec>59) return bad_time;
        int offset=0;
        if (s.size()==20) { if (s[19]!='Z') return bad_time; }
        else {
            if ((s[19]!='+' && s[19]!='-') || s[22]!=':') return bad_time;
            int oh=pair_digits(s,20), om=pair_digits(s,23);
            if (oh<0 || oh>23 || om<0 || om>59) return bad_time;
            offset=(s[19]=='+'?1:-1)*(60*oh+om);
        }
        return d.midnight+I64((h*60+m-offset)*60+sec)*1000;
    }
};
