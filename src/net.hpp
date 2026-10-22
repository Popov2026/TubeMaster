// TubeMaster — client HTTP asynchrone : un seul thread libcurl « multi »,
// des dizaines de requêtes en parallèle, connexions TLS réutilisées.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sw {

struct HttpRequest {
    std::string url;
    std::vector<std::string> headers;   // "Nom: valeur"
    size_t max_bytes = 2u << 20;        // au-delà, le corps est tronqué (pas une erreur)
    long timeout_ms = 8000;
    bool urgent = false;                // passe devant la file (survol, zapping…)
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string content_type;
    std::string error;                  // vide si le transfert a abouti
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

using HttpCallback = std::function<void(HttpResponse&&)>;

class HttpClient {
public:
    explicit HttpClient(int max_connections);
    ~HttpClient();
    // Le rappel est exécuté sur le thread réseau : il doit rester court
    // (analyse, puis post_main() pour toucher à l'état de l'appli).
    void get(HttpRequest req, HttpCallback cb);
    void set_max_connections(int n);
    int in_flight() const { return active_.load(); }
    size_t queued();
    struct Job;

private:
    void loop();
    void* multi_ = nullptr;
    std::thread th_;
    std::mutex m_;
    std::deque<Job*> urgent_, normal_;
    std::atomic<bool> stop_{false};
    std::atomic<int> active_{0};
    std::atomic<int> max_conn_;
    std::atomic<bool> conn_changed_{false};
};

extern const char* kUserAgent;

} // namespace sw
