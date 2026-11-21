#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using Text = std::string_view;
namespace fs = std::filesystem;
using I64 = std::int64_t;
using Id = std::uint32_t;
constexpr Id absent = std::numeric_limits<Id>::max();
constexpr I64 bad_time = std::numeric_limits<I64>::min();
constexpr I64 day_ms = 86400000, week_ms = 7 * day_ms, gap_ms = 1800000;

inline std::size_t hash_text(Text s) { return std::hash<Text>{}(s); }

class MappedFile {
    char* bytes_ = nullptr;
    std::size_t size_ = 0;
public:
    explicit MappedFile(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("open: " + path.string());
        struct stat info{};
        if (fstat(fd, &info) != 0) { close(fd); throw std::runtime_error("stat: " + path.string()); }
        size_ = info.st_size;
        if (size_) bytes_ = static_cast<char*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0));
        close(fd);
        if (bytes_ == MAP_FAILED) throw std::runtime_error("mmap: " + path.string());
    }
    MappedFile(const MappedFile&) = delete;
    ~MappedFile() { if (size_) munmap(bytes_, size_); }
    Text view() const { return size_ ? Text(bytes_, size_) : Text{}; }
};

// Chunks always start at a physical line boundary. Only LF terminates a row.
struct Inputs {
    std::vector<std::unique_ptr<MappedFile>> files;
    std::vector<Text> add(const fs::path& path) {
        std::vector<fs::path> paths;
        if (fs::is_directory(path)) {
            for (auto& entry : fs::directory_iterator(path))
                if (entry.path().extension() == ".csv" && entry.is_regular_file()) paths.push_back(entry.path());
            std::sort(paths.begin(), paths.end());
        } else paths.push_back(path);
        std::vector<Text> chunks;
        for (const auto& p : paths) {
            auto& file = files.emplace_back(std::make_unique<MappedFile>(p));
            auto data = file->view();
            auto header = data.find('\n');
            if (header == Text::npos) continue;
            data.remove_prefix(header + 1);
            constexpr std::size_t chunk_bytes = 2u << 20;
            while (!data.empty()) {
                auto cut = data.size() > chunk_bytes ? data.find('\n', chunk_bytes) : Text::npos;
                auto len = cut == Text::npos ? data.size() : cut + 1;
                chunks.push_back(data.substr(0, len));
                data.remove_prefix(len);
            }
        }
        return chunks;
    }
};

template<class F> void lines(Text chunk, F&& process) {
    while (!chunk.empty()) {
        auto end = chunk.find('\n');
        if (end == Text::npos) { process(chunk); break; }
        process(chunk.substr(0, end));
        chunk.remove_prefix(end + 1);
    }
}

template<std::size_t N> bool fields(Text line, std::array<Text, N>& out) {
    for (std::size_t i = 0; i < N; ++i) {
        auto comma = line.find(',');
        if ((i + 1 == N) != (comma == Text::npos)) return false;
        Text value = line.substr(0, comma);
        if (!value.empty() && value.front() == '"') {
            if (value.size() < 2 || value.back() != '"') return false;
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (value.find('"') != Text::npos) return false;
        out[i] = value;
        if (comma != Text::npos) line.remove_prefix(comma + 1);
    }
    return true;
}

inline int pair_digits(Text s, unsigned i) {
    unsigned a = static_cast<unsigned char>(s[i]) - unsigned('0');
    unsigned b = static_cast<unsigned char>(s[i + 1]) - unsigned('0');
    return a < 10 && b < 10 ? int(a * 10 + b) : -1;
}

inline I64 timestamp(Text s) {
    if (s.empty()) return bad_time;
    if ((s.size() == 20 || s.size() == 25) && s[4] == '-') {
        if (s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return bad_time;
        int century = pair_digits(s, 0), yy = pair_digits(s, 2);
        int m = pair_digits(s, 5), d = pair_digits(s, 8);
        int h = pair_digits(s, 11), minute = pair_digits(s, 14), sec = pair_digits(s, 17);
        if (century < 0 || yy < 0 || m < 1 || m > 12 || d < 1 || d > 31 ||
            h < 0 || h > 23 || minute < 0 || minute > 59 || sec < 0 || sec > 59) return bad_time;
        std::chrono::year_month_day date{std::chrono::year{100 * century + yy},
            std::chrono::month{unsigned(m)}, std::chrono::day{unsigned(d)}};
        if (!date.ok()) return bad_time;
        int offset = 0;
        if (s.size() == 20) {
            if (s[19] != 'Z') return bad_time;
        } else {
            if ((s[19] != '+' && s[19] != '-') || s[22] != ':') return bad_time;
            int oh = pair_digits(s, 20), om = pair_digits(s, 23);
            if (oh < 0 || oh > 23 || om < 0 || om > 59) return bad_time;
            offset = (s[19] == '+' ? 1 : -1) * (60 * oh + om);
        }
        return I64(std::chrono::sys_days{date}.time_since_epoch().count()) * day_ms +
            ((h * 60 + minute - offset) * 60 + sec) * I64(1000);
    }
    std::uint64_t ms = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), ms);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size() || ms > std::uint64_t(INT64_MAX))
        return bad_time;
    return static_cast<I64>(ms);
}

inline unsigned price_cents(Text s) {
    unsigned n = 0;
    if (s.empty()) return 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), n);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size() && n <= 10000000 ? n : 0;
}

inline int utc_day(I64 ms) { return int(ms / day_ms - (ms % day_ms < 0)); }

struct Window {
    I64 start, end;
    explicit Window(const fs::path& path) {
        std::ifstream f(path);
        std::string json{std::istreambuf_iterator<char>(f), {}};
        auto get = [&](const std::string& key) {
            std::smatch m;
            std::regex field("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"");
            if (!std::regex_search(json, m, field)) throw std::runtime_error("Missing " + key);
            return timestamp(m[1].str());
        };
        start = get("window_start"); end = get("window_end");
        if (start == bad_time || end == bad_time || start >= end) throw std::runtime_error("Invalid window");
    }
};

template<class F> void parallel(std::size_t count, int workers, F process) {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> failures(workers);
    for (int i = 0; i < workers && std::size_t(i) < count; ++i) {
        threads.emplace_back([&, i] {
            try {
                for (;;) {
                    auto j = next.fetch_add(1, std::memory_order_relaxed);
                    if (j >= count) break;
                    process(i, j);
                }
            } catch (...) { failures[i] = std::current_exception(); next.store(count); }
        });
    }
    for (auto& t : threads) t.join();
    for (auto& e : failures) if (e) std::rethrow_exception(e);
}
