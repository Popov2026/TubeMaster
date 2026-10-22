#include "net.hpp"

#include <curl/curl.h>

namespace sw {

const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/128.0.0.0 Safari/537.36";

struct HttpClient::Job {
    HttpRequest req;
    HttpCallback cb;
    HttpResponse resp;
    curl_slist* hdrs = nullptr;
    bool truncated = false;
    char errbuf[CURL_ERROR_SIZE] = {0};
};

static size_t on_write(char* data, size_t sz, size_t n, void* user) {
    auto* job = static_cast<HttpClient::Job*>(user);
    size_t len = sz * n;
    size_t room = job->req.max_bytes > job->resp.body.size() ? job->req.max_bytes - job->resp.body.size() : 0;
    if (len > room) {
        job->resp.body.append(data, room);
        job->truncated = true;
        return 0;                       // interrompt le transfert : on garde le début
    }
    job->resp.body.append(data, len);
    return len;
}

HttpClient::HttpClient(int max_connections) : max_conn_(max_connections) {
    static bool inited = (curl_global_init(CURL_GLOBAL_DEFAULT), true);
    (void)inited;
    multi_ = curl_multi_init();
    curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, long(max_connections));
    curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, long(max_connections));
    curl_multi_setopt(multi_, CURLMOPT_MAXCONNECTS, long(max_connections * 2));
    th_ = std::thread([this] { loop(); });
}

HttpClient::~HttpClient() {
    stop_ = true;
    curl_multi_wakeup(multi_);
    th_.join();
    std::lock_guard<std::mutex> lk(m_);
    for (auto* q : {&urgent_, &normal_})
        for (Job* j : *q) delete j;
    curl_multi_cleanup(multi_);
}

void HttpClient::get(HttpRequest req, HttpCallback cb) {
    auto* j = new Job;
    j->req = std::move(req);
    j->cb = std::move(cb);
    {
        std::lock_guard<std::mutex> lk(m_);
        (j->req.urgent ? urgent_ : normal_).push_back(j);
    }
    curl_multi_wakeup(multi_);
}

void HttpClient::set_max_connections(int n) {
    max_conn_ = n < 1 ? 1 : n;
    conn_changed_ = true;
    curl_multi_wakeup(multi_);
}

size_t HttpClient::queued() {
    std::lock_guard<std::mutex> lk(m_);
    return urgent_.size() + normal_.size();
}

void HttpClient::loop() {
    unsigned picks = 0;
    while (!stop_) {
        if (conn_changed_.exchange(false)) {
            long n = max_conn_;
            curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, n);
            curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, n);
            curl_multi_setopt(multi_, CURLMOPT_MAXCONNECTS, n * 2);
        }
        // Alimente le multi tant qu'il reste des places (les urgents d'abord).
        for (;;) {
            if (active_ >= max_conn_) break;
            Job* j = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_);
                // Les urgents d'abord, mais une place sur quatre pour la file normale :
                // le flot continu des photos ne doit pas bloquer les vérifications de fond.
                bool normal = !normal_.empty() && (urgent_.empty() || ++picks % 4 == 0);
                auto& q = normal ? normal_ : urgent_;
                if (q.empty()) break;
                j = q.front();
                q.pop_front();
            }
            CURL* e = curl_easy_init();
            curl_easy_setopt(e, CURLOPT_URL, j->req.url.c_str());
            curl_easy_setopt(e, CURLOPT_PRIVATE, j);
            curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
            curl_easy_setopt(e, CURLOPT_WRITEDATA, j);
            curl_easy_setopt(e, CURLOPT_ERRORBUFFER, j->errbuf);
            curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
            curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, j->req.timeout_ms);
            curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
            curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(e, CURLOPT_PIPEWAIT, 1L);
            curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");   // gzip/br si libcurl les gère
            curl_easy_setopt(e, CURLOPT_USERAGENT, kUserAgent);
            for (auto& h : j->req.headers) j->hdrs = curl_slist_append(j->hdrs, h.c_str());
            if (j->hdrs) curl_easy_setopt(e, CURLOPT_HTTPHEADER, j->hdrs);
            curl_multi_add_handle(multi_, e);
            ++active_;
        }

        int running = 0;
        curl_multi_perform(multi_, &running);

        int left = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi_, &left)) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL* e = msg->easy_handle;
            Job* j = nullptr;
            curl_easy_getinfo(e, CURLINFO_PRIVATE, &j);
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &j->resp.status);
            char* ct = nullptr;
            curl_easy_getinfo(e, CURLINFO_CONTENT_TYPE, &ct);
            if (ct) j->resp.content_type = ct;
            CURLcode rc = msg->data.result;
            if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && j->truncated))
                j->resp.error = j->errbuf[0] ? j->errbuf : curl_easy_strerror(rc);
            curl_multi_remove_handle(multi_, e);
            curl_easy_cleanup(e);
            curl_slist_free_all(j->hdrs);
            --active_;
            if (!stop_ && j->cb) j->cb(std::move(j->resp));
            delete j;
        }
        curl_multi_poll(multi_, nullptr, 0, 250, nullptr);
    }
    // Arrêt : abandonne les transferts en cours sans rappeler personne.
    CURL** handles = curl_multi_get_handles(multi_);
    if (handles) {
        for (CURL** h = handles; *h; ++h) {
            Job* j = nullptr;
            curl_easy_getinfo(*h, CURLINFO_PRIVATE, &j);
            curl_multi_remove_handle(multi_, *h);
            curl_easy_cleanup(*h);
            if (j) { curl_slist_free_all(j->hdrs); delete j; }
        }
        curl_free(handles);
    }
}

} // namespace sw
