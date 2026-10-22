// TubeMaster — gestionnaire de liens YouTube en C++ : fenêtre SDL2, OpenGL 3.3, Dear ImGui.
// La boucle dort quand rien ne bouge et tourne à la fréquence de l'écran
// (synchro verticale) dès qu'une vidéo joue ou que des images arrivent.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include "app.hpp"
#include "gl.hpp"
#include "platform.hpp"
#include "util.hpp"

namespace {

bool file_ok(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::u8path(p), ec);
}

std::string font_dir() {
#ifdef _WIN32
    const char* w = std::getenv("WINDIR");
    return std::string(w ? w : "C:\\Windows") + "\\Fonts\\";
#else
    return "/usr/share/fonts/truetype/dejavu/";
#endif
}

// Polices : Segoe UI + symboles, puis, pour les seuls caractères présents dans les titres
// (coréen, chinois, japonais, hindi, emoji…), les polices de Windows qui les contiennent.
sw::Fonts load_fonts(float scale, const std::vector<unsigned>& extra) {
    ImGuiIO& io = ImGui::GetIO();
    static ImVector<ImWchar> ranges;
    ranges.clear();
    ImFontGlyphRangesBuilder b;
    b.AddRanges(reinterpret_cast<const ImWchar*>(sw::kBaseGlyphRanges));
    for (unsigned c : extra) b.AddChar(ImWchar(c));
    b.BuildRanges(&ranges);
    std::string dir = font_dir();
#ifdef _WIN32
    std::string reg = dir + "segoeui.ttf", bold = dir + "segoeuib.ttf";
    std::vector<std::string> fallbacks = {dir + "seguisym.ttf", dir + "malgun.ttf", dir + "msyh.ttc", dir + "YuGothM.ttc",
                                          dir + "meiryo.ttc", dir + "msjh.ttc", dir + "Nirmala.ttf", dir + "arial.ttf",
                                          dir + "seguiemj.ttf"};
#else
    std::string reg = dir + "DejaVuSans.ttf", bold = dir + "DejaVuSans-Bold.ttf";
    std::vector<std::string> fallbacks;
#endif
    auto add = [&](const std::string& path, float px, bool merge_all) -> ImFont* {
        if (!file_ok(path)) return nullptr;
        ImFontConfig c;
        c.OversampleH = 2;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale, &c, ranges.Data);
        if (!f) return nullptr;
        for (size_t i = 0; i < fallbacks.size(); ++i) {
            if (!merge_all && i > 0) break;          // titres de l'interface : symboles seulement
            if (!file_ok(fallbacks[i])) continue;
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 1;
            io.Fonts->AddFontFromFileTTF(fallbacks[i].c_str(), px * scale, &m, ranges.Data);
        }
        return f;
    };
    sw::Fonts f;
    f.main = add(reg, 17, true);
    if (!f.main) {
        ImFontConfig c;
        c.SizePixels = 13 * scale;
        f.main = io.Fonts->AddFontDefault(&c);
    }
    f.small = add(reg, 14, true);
    f.title = add(bold, 23, false);
    if (!f.small) f.small = f.main;
    if (!f.title) f.title = f.main;
    f.big = f.title;
    return f;
}

} // namespace

int main(int, char**) {
    sw::platform_init();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init : %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    float scale = 1.f;
    float ddpi = 0;
    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0) {
#ifdef _WIN32
        scale = ddpi / 96.f;
#endif
    }
    if (const char* s = std::getenv("TUBEMASTER_SCALE")) scale = float(std::atof(s));
    if (scale < 0.75f || scale > 4.f) scale = 1.f;

    int ww = int(1360 * scale), wh = int(840 * scale);
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {          // jamais plus grande que l'écran
        ww = std::min(ww, usable.w - 40);
        wh = std::min(wh, usable.h - 60);
    }
    SDL_Window* win = SDL_CreateWindow("TubeMaster — liens YouTube", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       ww, wh,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                           SDL_WINDOW_HIDDEN);
    if (!win) {
        std::fprintf(stderr, "Fenêtre : %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 900, 560);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TubeMaster",
                                 "OpenGL 3.3 indisponible : mettez à jour le pilote de la carte graphique.", win);
        return 1;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    if (!gl::load(reinterpret_cast<gl::ProcLoader>(SDL_GL_GetProcAddress))) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TubeMaster", "Fonctions OpenGL manquantes.", win);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // pas de fichier imgui.ini à côté du programme
    sw::Fonts fonts = load_fonts(scale, {});
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    Uint32 wake_ev = SDL_RegisterEvents(1);
    sw::main_queue().wake = [wake_ev] {
        SDL_Event e{};
        e.type = wake_ev;
        SDL_PushEvent(&e);
    };
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_EventState(SDL_DROPTEXT, SDL_ENABLE);

    {
        sw::App app(win, fonts, scale);
        int extra_frames = 3;
        auto handle = [&](SDL_Event& e) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) app.request_quit();
            else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                     e.window.windowID == SDL_GetWindowID(win))
                app.request_quit();
            else if (e.type == SDL_DROPFILE && e.drop.file) {
                app.on_drop_file(e.drop.file);
                SDL_free(e.drop.file);
            } else if (e.type == SDL_DROPTEXT && e.drop.file) {
                app.on_drop_text(e.drop.file);
                SDL_free(e.drop.file);
            }
        };
        while (!app.want_quit()) {
            SDL_Event e;
            bool any = false;
            bool minimized = SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED;
            if (minimized || (!app.animating() && extra_frames <= 0)) {
                if (SDL_WaitEventTimeout(&e, minimized ? 250 : app.wait_timeout_ms())) { handle(e); any = true; }
            }
            while (SDL_PollEvent(&e)) { handle(e); any = true; }
            // Quelques images après chaque événement : ImGui finit ses transitions (survol…).
            if (any) extra_frames = 3;
            else if (extra_frames > 0) --extra_frames;

            sw::main_queue().drain();
            app.update();
            // Nouveaux caractères dans les titres : l'atlas des polices est reconstruit.
            std::vector<unsigned> extra;
            if (app.take_font_request(extra)) {
                io.Fonts->Clear();
                fonts = load_fonts(scale, extra);
                ImGui_ImplOpenGL3_DestroyFontsTexture();
                ImGui_ImplOpenGL3_CreateFontsTexture();
                app.set_fonts(fonts);
            }
            if (minimized) continue;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            app.draw();
            ImGui::Render();
            int w, h;
            SDL_GL_GetDrawableSize(win, &w, &h);
            gl::Viewport(0, 0, w, h);
            gl::ClearColor(0.05f, 0.05f, 0.08f, 1.f);
            gl::Clear(gl::COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(win);
        }
    }
    sw::main_queue().wake = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    sw::platform_shutdown();
    return 0;
}
