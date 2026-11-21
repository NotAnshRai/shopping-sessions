#include "csv.hpp"
#include "dates.hpp"
#include <iostream>
#include <random>

// Keep the scalar parser as an oracle for quote and block-boundary cases.
template<std::size_t N> bool check_rows(std::mt19937_64& rng) {
    std::string input;
    for (unsigned row=0;row<22000;++row) {
        unsigned columns=N;
        if (row%13==0) columns=N-1;
        if (row%17==0) columns=N+1;
        for (unsigned i=0;i<columns;++i) {
            if (i) input+=',';
            std::size_t n=rng()%17;
            if (i==0 && row%5==0) n=60+rng()%11;
            if (i==1 && row%9==0) n=117+rng()%18;
            auto mode=rng()%16;
            if (mode<6 || mode==12) input+='"';
            for (std::size_t k=0;k<n;++k) {
                if (mode==13 && k==n/2) input+='"';
                else input+=" aX2\r"[rng()%5];
            }
            if (mode<6 || mode==14) input+='"';
        }
        if (row+1<22000) input+='\n';
    }
    using Row=std::array<Text,N>;
    std::vector<Row> slow,fast;
    lines(input,[&](Text line){Row row; if(fields(line,row)) slow.push_back(row);});
    csv_rows<N>(input,[&](auto& row){fast.push_back(row);});
    return slow==fast;
}

int main(int argc,char** argv) {
    if (argc!=2) return 2;
    std::mt19937_64 rng(86472);
    if (!check_rows<2>(rng) || !check_rows<4>(rng) || !check_rows<7>(rng)) return 1;
    Window window(argv[1]); Dates dates(window);
    for (unsigned i=0;i<200000;++i) {
        std::string text;
        if (i%2==0) {
            auto value=rng()%10000000000000ULL;
            text=std::to_string(value);
            text.insert(0,13-text.size(),'0');
        } else {
            int day=utc_day(window.start)+int(rng()%35)-10;
            auto d=std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{day}}};
            char iso[48];
            std::snprintf(iso,sizeof(iso),"%04d-%02u-%02uT%02u:%02u:%02u",int(d.year()),unsigned(d.month()),unsigned(d.day()),
                          unsigned(rng()%25),unsigned(rng()%61),unsigned(rng()%61));
            text=iso;
            text+=i%4==1?"Z":"-05:45";
        }
        if (i%3==0) text[rng()%text.size()]=char(rng()%256);
        if (dates.parse(text)!=timestamp(text)) return 1;
    }
    std::cout<<"CSV and timestamp fast paths agree with scalar decoding\n";
}
