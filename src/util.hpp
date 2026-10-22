// TubeMaster — utilitaires : temps, chaînes, files de tâches, pool de threads.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sw {

double now_sec();          // horloge monotone (secondes)
int64_t wall_ms();         // horloge murale (ms depuis 1970)

std::string lower(std::string s);
std::string trim(const std::string& s);
bool icontains(const std::string& hay, const std::string& needle);
bool starts_with(const std::string& s, const char* p);
bool ends_with_i(const std::string& s, const char* p);

// Dates locales au format de l'ancienne version ("%Y-%m-%d %H:%M:%S").
std::string fmt_local(time_t t, const char* fmt = "%Y-%m-%d %H:%M:%S");
time_t parse_local(const std::string& s);
// Date ISO 8601 ("2024-05-01T12:34:56.123Z", "+02:00"...) ; 0 si illisible.
time_t parse_iso(const std::string& s);

// Journal de diagnostic (tubemaster_log.txt) : appelable depuis tous les threads.
void diag_open(const std::string& path);
void diag(const std::string& msg);

std::string fmt_ago(time_t t);    // "il y a 3h05", "jamais vu"
std::string fmt_since(time_t t);  // "1h12", "45min"

// File de tâches exécutées sur le thread principal (UI / OpenGL).
class MainQueue {
public:
    void post(std::function<void()> fn);
    void drain();
    std::function<void()> wake;   // réveille la boucle principale
private:
    std::mutex m_;
    std::vector<std::function<void()>> q_;
};
MainQueue& main_queue();
inline void post_main(std::function<void()> fn) { main_queue().post(std::move(fn)); }
// Réveille la boucle principale (appelable depuis n'importe quel thread).
inline void wake_main() { if (main_queue().wake) main_queue().wake(); }

class ThreadPool {
public:
    explicit ThreadPool(unsigned n);
    ~ThreadPool();
    void submit(std::function<void()> fn);
private:
    std::vector<std::thread> th_;
    std::deque<std::function<void()>> q_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace sw
