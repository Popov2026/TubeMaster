// TubeMaster — lecteur intégré : la page YouTube (lecteur officiel) affichée dans la fenêtre
// par Edge WebView2 (Windows 10/11).
#pragma once
#include <functional>
#include <memory>
#include <string>

struct SDL_Window;

namespace sw {

enum class WebKey { PrevChannel, NextChannel, PrevViewed, Fullscreen, Escape, ToggleStrip, Mute, Favorite,
                    Click, DoubleClick,     // clics dans la page (mur vidéo)
                    WheelUp, WheelDown,     // molette dans la page
                    Ended };                // fin de la vidéo (enchaînement)

class WebView {
public:
    // Création asynchrone : ready() devient vrai quelques centaines de ms plus tard.
    // Renvoie nullptr (et la raison) si WebView2 n'est pas disponible.
    static std::unique_ptr<WebView> create(SDL_Window* parent, const std::string& user_data_dir, std::string* why);
    virtual ~WebView() = default;

    virtual bool ready() const = 0;
    virtual bool failed() const = 0;
    virtual const std::string& error() const = 0;
    virtual bool loading() const = 0;
    virtual void navigate(const std::string& url) = 0;
    virtual void set_bounds(int x, int y, int w, int h) = 0;   // pixels, zone cliente de la fenêtre
    virtual void set_visible(bool v) = 0;
    virtual void set_muted(bool m) = 0;
    virtual void focus() = 0;
    // Plein écran : la vue passe dans une fenêtre à part, sans OpenGL, qui couvre
    // l'écran (sinon le pilote graphique affiche l'image OpenGL par-dessus la page).
    virtual void set_detached_fullscreen(bool on) = 0;
    virtual bool detached_fullscreen() const = 0;
    // Script lancé au début de chaque page chargée (à appeler avant ou après ready()).
    virtual void add_startup_script(const std::string& js) = 0;
    virtual void run_script(const std::string& js) = 0;
    // Exécute un script et renvoie son résultat (JSON) sur le thread principal.
    virtual void eval(const std::string& js, std::function<void(const std::string&)> done) = 0;

    // Rappels exécutés sur le thread principal.
    std::function<void(WebKey)> on_key;
    std::function<void(bool)> on_fullscreen;   // la page passe (ou sort) en plein écran
    std::function<void(const std::string&)> on_source;   // adresse de la page (aussi la navigation interne du site)
};

} // namespace sw
