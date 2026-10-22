#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace sw {

double now_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != hay.end();
}

bool starts_with(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool ends_with_i(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)p[i])) return false;
    return true;
}

static bool to_tm_local(time_t t, std::tm& out) {
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

static time_t utc_mktime(std::tm* tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

std::string fmt_local(time_t t, const char* fmt) {
    std::tm tm{};
    if (!t || !to_tm_local(t, tm)) return {};
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return buf;
}

time_t parse_local(const std::string& s) {
    int Y, M, D, h, m, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec; tm.tm_isdst = -1;
    time_t t = std::mktime(&tm);
    return t == (time_t)-1 ? 0 : t;
}

time_t parse_iso(const std::string& s) {
    int Y, M, D, h = 0, m = 0, sec = 0, n = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d%n", &Y, &M, &D, &n) != 3) return 0;
    const char* p = s.c_str() + n;
    if (*p == 'T' || *p == ' ') {
        int k = 0;
        if (std::sscanf(p + 1, "%d:%d:%d%n", &h, &m, &sec, &k) < 2) return 0;
        if (k == 0) {                       // "HH:MM" sans secondes
            std::sscanf(p + 1, "%d:%d%n", &h, &m, &k);
        }
        p += 1 + k;
        if (*p == '.') { ++p; while (*p >= '0' && *p <= '9') ++p; }
    }
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = sec;
    if (*p == 'Z') return utc_mktime(&tm);
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        std::sscanf(p + 1, "%d:%d", &oh, &om);
        if (om == 0 && oh > 99) { om = oh % 100; oh /= 100; }   // "+0200"
        time_t t = utc_mktime(&tm);
        int off = (oh * 3600 + om * 60) * (*p == '+' ? 1 : -1);
        return t - off;
    }
    tm.tm_isdst = -1;
    time_t t = std::mktime(&tm);
    return t == (time_t)-1 ? 0 : t;
}

static std::mutex g_diag_m;
static FILE* g_diag = nullptr;
static size_t g_diag_bytes = 0;

void diag_open(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_diag_m);
#ifdef _WIN32
    g_diag = _wfopen(std::filesystem::u8path(path).c_str(), L"w");
#else
    g_diag = std::fopen(path.c_str(), "w");
#endif
}

void diag(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_diag_m);
    if (!g_diag || g_diag_bytes > (4u << 20)) return;      // au plus 4 Mo par session
    std::string line = fmt_local(std::time(nullptr), "%H:%M:%S ") + msg + "\n";
    std::fwrite(line.data(), 1, line.size(), g_diag);
    std::fflush(g_diag);
    g_diag_bytes += line.size();
}

std::string fmt_ago(time_t t) {
    if (!t) return "jamais vu";
    long d = long(std::time(nullptr) - t);
    char buf[64];
    if (d < 0) return "à l'instant";
    if (d < 60) std::snprintf(buf, sizeof buf, "il y a %lds", d);
    else if (d < 3600) std::snprintf(buf, sizeof buf, "il y a %ldmin", d / 60);
    else if (d < 86400) std::snprintf(buf, sizeof buf, "il y a %ldh%02ld", d / 3600, (d % 3600) / 60);
    else std::snprintf(buf, sizeof buf, "il y a %ldj %ldh", d / 86400, (d % 86400) / 3600);
    return buf;
}

std::string fmt_since(time_t t) {
    if (!t) return {};
    long d = long(std::time(nullptr) - t);
    char buf[32];
    if (d < 0) return {};
    if (d < 60) std::snprintf(buf, sizeof buf, "%lds", d);
    else if (d < 3600) std::snprintf(buf, sizeof buf, "%ldmin", d / 60);
    else std::snprintf(buf, sizeof buf, "%ldh%02ld", d / 3600, (d % 3600) / 60);
    return buf;
}

// ── MainQueue ────────────────────────────────────────────────────────────────
void MainQueue::post(std::function<void()> fn) {
    bool was_empty;
    {
        std::lock_guard<std::mutex> lk(m_);
        was_empty = q_.empty();
        q_.push_back(std::move(fn));
    }
    if (was_empty && wake) wake();
}

void MainQueue::drain() {
    std::vector<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> lk(m_);
        q.swap(q_);
    }
    for (auto& f : q) f();
}

MainQueue& main_queue() {
    static MainQueue q;
    return q;
}

// ── ThreadPool ───────────────────────────────────────────────────────────────
ThreadPool::ThreadPool(unsigned n) {
    if (n == 0) n = 1;
    for (unsigned i = 0; i < n; ++i)
        th_.emplace_back([this] {
            for (;;) {
                std::function<void()> f;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                    if (stop_ && q_.empty()) return;
                    f = std::move(q_.front());
                    q_.pop_front();
                }
                f();
            }
        });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        q_.clear();
    }
    cv_.notify_all();
    for (auto& t : th_) t.join();
}

void ThreadPool::submit(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

} // namespace sw
