// TubeMaster — lancement d'outils externes (yt-dlp, ffmpeg…) sans fenêtre console :
// chaque ligne de sortie est remontée au fur et à mesure ; « Stop » arrête aussi les
// processus enfants (ffmpeg lancé par yt-dlp).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sw {

class Process : public std::enable_shared_from_this<Process> {
public:
    using LineFn = std::function<void(const std::string&)>;   // thread de lecture
    using ExitFn = std::function<void(int code)>;              // thread de lecture, après la dernière ligne
    // extra_path : dossier ajouté en tête du PATH (outils à côté du programme : deno, ffmpeg…).
    static std::shared_ptr<Process> start(const std::string& exe, const std::vector<std::string>& args,
                                          const std::string& extra_path, LineFn on_line, ExitFn on_exit,
                                          std::string* error);
    ~Process();
    void kill();
    bool running() const { return running_; }

private:
    Process() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

// Ligne de commande affichable (journal).
std::string command_line(const std::string& exe, const std::vector<std::string>& args);

} // namespace sw
