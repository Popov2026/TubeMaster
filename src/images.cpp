#include "images.hpp"

#include <fstream>
#include <filesystem>
#include <vector>

#include "stb_image.h"

namespace sw {

ImageCache::ImageCache(HttpClient& net) : net_(net), decode_(std::make_unique<ThreadPool>(2)) {}

ImageCache::~ImageCache() {
    *alive_ = false;
    decode_.reset();
    for (auto& [u, im] : map_) if (im.tex) gl::DeleteTextures(1, &im.tex);
}

const Image& ImageCache::get(const std::string& url) {
    Image& im = map_[url];
    im.last_used = now_sec();
    if (im.tex || im.inflight || im.failed || url.empty() || inflight_ >= 6) return im;
    im.inflight = true;
    ++inflight_;
    HttpRequest req;
    req.url = url;
    req.headers = {"Accept: image/jpeg,image/*"};
    req.max_bytes = 4u << 20;
    std::weak_ptr<bool> alive = alive_;
    auto finish = [this, url, alive](std::shared_ptr<std::vector<uint8_t>> px, int w, int h) {
        post_main([this, url, alive, px, w, h] {
            if (alive.expired() || !*alive.lock()) return;
            --inflight_;
            Image& im = map_[url];
            im.inflight = false;
            if (!px) { im.failed = true; return; }
            gl::upload_rgba(im.tex, im.w, im.h, px->data(), w, h);
        });
    };
    if (url.rfind("file:", 0) == 0) {
        // Miniature enregistrée sur le disque : lecture et décodage sur le thread de travail.
        std::string path = url.substr(5);
        decode_->submit([path, finish] {
            std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            int w = 0, h = 0, n = 0;
            unsigned char* px = body.empty() ? nullptr
                                             : stbi_load_from_memory(reinterpret_cast<const unsigned char*>(body.data()),
                                                                     int(body.size()), &w, &h, &n, 4);
            if (!px) { finish(nullptr, 0, 0); return; }
            auto v = std::make_shared<std::vector<uint8_t>>(px, px + size_t(w) * size_t(h) * 4);
            stbi_image_free(px);
            finish(v, w, h);
        });
        return im;
    }
    net_.get(std::move(req), [this, url, alive](HttpResponse&& r) {
        auto body = std::make_shared<std::string>(r.ok() ? std::move(r.body) : std::string());
        auto done = [this, url, alive](std::shared_ptr<std::vector<uint8_t>> px, int w, int h) {
            post_main([this, url, alive, px, w, h] {
                if (alive.expired() || !*alive.lock()) return;
                --inflight_;
                Image& im = map_[url];
                im.inflight = false;
                if (!px) { im.failed = true; return; }
                gl::upload_rgba(im.tex, im.w, im.h, px->data(), w, h);
            });
        };
        if (body->empty()) { done(nullptr, 0, 0); return; }
        decode_->submit([body, done] {
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(body->data()),
                                                      int(body->size()), &w, &h, &n, 4);
            if (!px) { done(nullptr, 0, 0); return; }
            auto v = std::make_shared<std::vector<uint8_t>>(px, px + size_t(w) * size_t(h) * 4);
            stbi_image_free(px);
            done(v, w, h);
        });
    });
    return im;
}

void ImageCache::gc() {
    double now = now_sec();
    if (now - last_gc_ < 15) return;
    last_gc_ = now;
    for (auto it = map_.begin(); it != map_.end();) {
        if (!it->second.inflight && now - it->second.last_used > 120) {
            if (it->second.tex) gl::DeleteTextures(1, &it->second.tex);
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace sw
