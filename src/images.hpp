// TubeMaster — miniatures des vidéos : téléchargement asynchrone, décodage JPEG
// sur un thread de travail, texture GPU créée sur le thread principal.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "gl.hpp"
#include "net.hpp"
#include "util.hpp"

namespace sw {

struct Image {
    gl::GLuint tex = 0;
    int w = 0, h = 0;
    bool inflight = false, failed = false;
    double last_used = 0;
};

class ImageCache {
public:
    explicit ImageCache(HttpClient& net);
    ~ImageCache();                                  // thread principal (contexte GL actif)
    // Lance le téléchargement au premier appel ; « file:chemin » : image sur le disque.
    const Image& get(const std::string& url);
    void gc();                                      // libère les images plus affichées
private:
    HttpClient& net_;
    std::unique_ptr<ThreadPool> decode_;
    std::unordered_map<std::string, Image> map_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    double last_gc_ = 0;
    int inflight_ = 0;
};

} // namespace sw
