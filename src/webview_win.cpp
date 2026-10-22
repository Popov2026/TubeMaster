// TubeMaster — WebView2 (Windows). Pas de WebView2Loader.dll : le runtime Edge
// installé est trouvé dans le registre et chargé directement (même méthode que
// le chargeur intégré de webview/webview, licence MIT).
#include "webview.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <SDL.h>
#include <SDL_syswm.h>

#include "WebView2.h"
#include "util.hpp"

namespace sw {
namespace {

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Gestionnaire COM générique : IUnknown + Invoke délégué à une fonction.
template <class I, class... A>
class Callback final : public I {
public:
    explicit Callback(std::function<HRESULT(A...)> f) : fn_(std::move(f)) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(I))) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return fn_(a...); }

private:
    std::atomic<ULONG> ref_{1};
    std::function<HRESULT(A...)> fn_;
};

template <class I, class... A, class F> I* make_cb(F f) {
    return new Callback<I, A...>(std::function<HRESULT(A...)>(std::move(f)));
}

using CreateInternal_t = HRESULT(STDMETHODCALLTYPE*)(bool, int, PCWSTR, IUnknown*,
                                                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof buf - sizeof(wchar_t), type = 0;
    LONG r = RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    std::wstring s(buf, sz / sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

std::wstring with_slash(std::wstring d) {
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    return d;
}

bool exists(const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Version « 130.0.2849.56 » -> comparable.
std::vector<int> parse_ver(const std::wstring& s) {
    std::vector<int> v;
    int cur = -1;
    for (wchar_t ch : s) {
        if (ch >= L'0' && ch <= L'9') cur = (cur < 0 ? 0 : cur * 10) + (ch - L'0');
        else if (ch == L'.') { v.push_back(cur < 0 ? 0 : cur); cur = -1; }
        else return {};
    }
    if (cur >= 0) v.push_back(cur);
    return v.size() >= 3 ? v : std::vector<int>{};
}

// Toutes les copies possibles du moteur WebView2, de la plus probable à la moins probable :
// registre d'EdgeUpdate (machine, utilisateur), puis dossiers d'installation habituels
// du runtime WebView2 et de Microsoft Edge (qui contient le même moteur).
std::vector<std::wstring> runtime_candidates() {
    std::vector<std::wstring> out;
    auto add_dir = [&](const std::wstring& dir) {
        if (dir.empty()) return;
        std::wstring dll = with_slash(dir) + L"EBWebView\\x64\\EmbeddedBrowserWebView.dll";
        if (exists(dll) && std::find(out.begin(), out.end(), dll) == out.end()) out.push_back(dll);
    };
    const wchar_t* guid = L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (REGSAM view : {REGSAM(KEY_WOW64_32KEY), REGSAM(KEY_WOW64_64KEY)}) {
            add_dir(reg_string(root, (std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\") + guid).c_str(),
                               L"EBWebView", view));
            std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\") + guid;
            std::wstring loc = reg_string(root, key.c_str(), L"location", view);
            std::wstring pv = reg_string(root, key.c_str(), L"pv", view);
            if (!loc.empty() && !pv.empty()) add_dir(with_slash(loc) + pv);
        }
    auto env = [](const wchar_t* n) {
        wchar_t b[MAX_PATH];
        DWORD k = GetEnvironmentVariableW(n, b, MAX_PATH);
        return (k && k < MAX_PATH) ? std::wstring(b, k) : std::wstring();
    };
    std::vector<std::wstring> roots;
    for (const wchar_t* base : {L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA"}) {
        std::wstring b = env(base);
        if (b.empty()) continue;
        roots.push_back(with_slash(b) + L"Microsoft\\EdgeWebView\\Application");
        roots.push_back(with_slash(b) + L"Microsoft\\Edge\\Application");
    }
    for (auto& root : roots) {
        // Le dossier de version le plus récent d'abord.
        std::vector<std::pair<std::vector<int>, std::wstring>> vers;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((with_slash(root) + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            auto v = parse_ver(fd.cFileName);
            if (!v.empty()) vers.emplace_back(v, with_slash(root) + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(vers.begin(), vers.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (auto& v : vers) add_dir(v.second);
    }
    return out;
}

class WinWebView final : public WebView {
public:
    WinWebView(HWND parent, const std::string& data_dir) : parent_(parent) {
        // Le rendu OpenGL de la fenêtre ne doit pas recouvrir la vue web.
        SetWindowLongPtrW(parent_, GWL_STYLE, GetWindowLongPtrW(parent_, GWL_STYLE) | WS_CLIPCHILDREN);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(data_dir), ec);
        // 1) Chargeur officiel de Microsoft (WebView2Loader.dll livré à côté de l'exe).
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring loader = std::filesystem::path(std::wstring(exe, n)).parent_path().wstring() + L"\\WebView2Loader.dll";
        if (HMODULE l = LoadLibraryW(loader.c_str())) {
            auto off = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)>(
                reinterpret_cast<void*>(GetProcAddress(l, "CreateCoreWebView2EnvironmentWithOptions")));
            diag(std::string("webview : chargeur officiel ") + (off ? "OK" : "sans la fonction attendue"));
            if (off) {
                start(data_dir, [off](PCWSTR dir, ICoreWebView2EnvironmentOptions* o,
                                      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* h) {
                    return off(nullptr, dir, o, h);
                });
                return;
            }
        } else {
            diag("webview : WebView2Loader.dll absent, chargeur intégré");
        }
        // 2) Chargeur intégré : moteur trouvé dans le registre ou les dossiers d'Edge.
        CreateInternal_t create = nullptr;
        auto cands = runtime_candidates();
        if (cands.empty()) diag("webview : aucun EmbeddedBrowserWebView.dll trouvé (registre + dossiers Edge)");
        for (auto& dll : cands) {
            HMODULE lib = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            auto fn = lib ? reinterpret_cast<CreateInternal_t>(reinterpret_cast<void*>(
                                GetProcAddress(lib, "CreateWebViewEnvironmentWithOptionsInternal")))
                          : nullptr;
            diag("webview : essai " + U8(dll) + (lib ? (fn ? " -> OK" : " -> fonction absente")
                                                    : " -> chargement impossible (erreur " + std::to_string(GetLastError()) + ")"));
            if (fn) { lib_ = lib; create = fn; break; }
            if (lib) FreeLibrary(lib);
        }
        if (!create) {
            fail(cands.empty() ? "le runtime Microsoft Edge WebView2 n'est pas installé"
                               : "le runtime WebView2 trouvé ne se charge pas (voir tubemaster_log.txt)");
            return;
        }
        start(data_dir, [create](PCWSTR dir, ICoreWebView2EnvironmentOptions* o,
                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* h) {
            return create(true, 0, dir, o, h);
        });
    }

    using CreateFn = std::function<HRESULT(PCWSTR, ICoreWebView2EnvironmentOptions*,
                                           ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)>;
    // Pas d'objet d'options : sur le banc d'essai, WebView2 refuse de démarrer avec
    // (0x80070002). La lecture automatique passe par la variable d'environnement prévue.
    void start(const std::string& data_dir, const CreateFn& create) {
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", L"--autoplay-policy=no-user-gesture-required");
        ICoreWebView2EnvironmentOptions* opts = nullptr;
        auto* h = make_cb<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
            [this](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    char b[32];
                    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
                    fail(std::string("démarrage de WebView2 impossible (") + b + ")");
                    return S_OK;
                }
                env_ = env;
                env_->AddRef();
                auto* ch = make_cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
                    [this](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                        if (FAILED(hr2) || !c) {
                            char b[32];
                            std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr2));
                            fail(std::string("création de la vue web impossible (") + b + ")");
                            return S_OK;
                        }
                        on_controller(c);
                        return S_OK;
                    });
                env_->CreateCoreWebView2Controller(parent_, ch);
                ch->Release();
                return S_OK;
            });
        HRESULT hr = create(W(data_dir).c_str(), opts, h);
        h->Release();
        if (FAILED(hr)) {
            char b[32];
            std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
            fail(std::string("échec de l'initialisation de WebView2 (") + b + ")");
        }
    }

    ~WinWebView() override {
        if (popup_) set_detached_fullscreen(false);
        if (ctrl_) { ctrl_->Close(); ctrl_->Release(); }
        if (wv_) wv_->Release();
        if (env_) env_->Release();
        // lib_ reste chargée : le runtime peut encore avoir des threads actifs.
    }

    bool ready() const override { return wv_ != nullptr; }
    bool failed() const override { return failed_; }
    const std::string& error() const override { return error_; }
    bool loading() const override { return loading_; }

    void navigate(const std::string& url) override {
        if (!wv_) { pending_ = url; return; }
        loading_ = true;
        wv_->Navigate(W(url).c_str());
    }

    void set_bounds(int x, int y, int w, int h) override {
        if (popup_) { bounds_ = RECT{x, y, x + w, y + h}; return; }   // en plein écran : la fenêtre à part décide
        // SDL réécrit le style de la fenêtre (plein écran…) : sans WS_CLIPCHILDREN,
        // le rendu OpenGL de l'appli peindrait par-dessus la page.
        LONG_PTR st = GetWindowLongPtrW(parent_, GWL_STYLE);
        if (!(st & WS_CLIPCHILDREN)) SetWindowLongPtrW(parent_, GWL_STYLE, st | WS_CLIPCHILDREN);
        RECT r{x, y, x + w, y + h};
        if (r.left == bounds_.left && r.top == bounds_.top && r.right == bounds_.right && r.bottom == bounds_.bottom) return;
        bounds_ = r;
        if (ctrl_) ctrl_->put_Bounds(r);
    }

    void set_visible(bool v) override {
        if (popup_) { visible_ = v; return; }
        if (v == visible_) return;
        visible_ = v;
        if (ctrl_) ctrl_->put_IsVisible(v ? TRUE : FALSE);
    }

    void set_muted(bool m) override {
        muted_ = m;
        if (!wv_) return;
        ICoreWebView2_8* w8 = nullptr;
        if (SUCCEEDED(wv_->QueryInterface(__uuidof(ICoreWebView2_8), reinterpret_cast<void**>(&w8))) && w8) {
            w8->put_IsMuted(m ? TRUE : FALSE);
            w8->Release();
        }
    }

    void add_startup_script(const std::string& js) override {
        if (!wv_) { startup_scripts_.push_back(js); return; }
        auto* h = make_cb<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, HRESULT, LPCWSTR>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
        wv_->AddScriptToExecuteOnDocumentCreated(W(js).c_str(), h);
        h->Release();
    }

    void run_script(const std::string& js) override {
        if (!wv_) return;
        auto* h = make_cb<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
        wv_->ExecuteScript(W(js).c_str(), h);
        h->Release();
    }

    void eval(const std::string& js, std::function<void(const std::string&)> done) override {
        if (!wv_) { done("pas prêt"); return; }
        auto* h = make_cb<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(
            [done](HRESULT hr, LPCWSTR res) -> HRESULT {
                std::string s = FAILED(hr) || !res ? std::string("erreur") : U8(res);
                post_main([done, s] { done(s); });
                return S_OK;
            });
        wv_->ExecuteScript(W(js).c_str(), h);
        h->Release();
    }

    bool detached_fullscreen() const override { return popup_ != nullptr; }

    void set_detached_fullscreen(bool on) override {
        if (on == (popup_ != nullptr)) return;
        if (on) {
            static bool registered = false;
            if (!registered) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = popup_proc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                wc.lpszClassName = L"TubeMasterPleinEcran";
                RegisterClassW(&wc);
                registered = true;
            }
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(parent_, MONITOR_DEFAULTTONEAREST), &mi);
            RECT r = mi.rcMonitor;
            popup_ = CreateWindowExW(WS_EX_TOPMOST, L"TubeMasterPleinEcran", L"TubeMaster",
                                     WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left,
                                     r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!popup_) { diag("plein écran : création de la fenêtre impossible"); return; }
            SetWindowLongPtrW(popup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            if (ctrl_) {
                ctrl_->put_ParentWindow(popup_);
                ctrl_->put_Bounds(RECT{0, 0, r.right - r.left, r.bottom - r.top});
                ctrl_->put_IsVisible(TRUE);
                ctrl_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            SetForegroundWindow(popup_);
            diag("plein écran : fenêtre " + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top));
        } else {
            HWND p = popup_;
            popup_ = nullptr;
            if (ctrl_) {
                ctrl_->put_ParentWindow(parent_);
                ctrl_->put_Bounds(bounds_);
                ctrl_->put_IsVisible(visible_ ? TRUE : FALSE);
            }
            SetWindowLongPtrW(p, GWLP_USERDATA, 0);
            DestroyWindow(p);
            SetForegroundWindow(parent_);
        }
    }

    // Fenêtre plein écran : Échap / F11 / Alt+F4 la referment proprement.
    static LRESULT CALLBACK popup_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<WinWebView*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) {
            if (msg == WM_KEYDOWN && (wp == VK_ESCAPE || wp == VK_F11)) { self->emit(WebKey::Fullscreen); return 0; }
            if (msg == WM_CLOSE) { self->emit(WebKey::Fullscreen); return 0; }
            if (msg == WM_SIZE && self->ctrl_) {
                RECT rc;
                GetClientRect(h, &rc);
                self->ctrl_->put_Bounds(rc);
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void focus() override {
        if (ctrl_) ctrl_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

private:
    void fail(const std::string& e) {
        failed_ = true;
        error_ = e;
    }

    void emit(WebKey k) {
        if (on_key) post_main([cb = on_key, k] { cb(k); });
    }

    void on_controller(ICoreWebView2Controller* c) {
        ctrl_ = c;
        ctrl_->AddRef();
        ctrl_->get_CoreWebView2(&wv_);
        if (!wv_) { fail("Vue web indisponible"); return; }
        ICoreWebView2Settings* s = nullptr;
        if (SUCCEEDED(wv_->get_Settings(&s)) && s) {
            s->put_AreDevToolsEnabled(FALSE);
            s->put_IsStatusBarEnabled(FALSE);
            s->Release();
        }
        ctrl_->put_Bounds(bounds_);
        ctrl_->put_IsVisible(visible_ ? TRUE : FALSE);

        EventRegistrationToken tok;
        // Touches de la télécommande, même quand la page a le focus clavier.
        auto* keys = make_cb<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*,
                             ICoreWebView2AcceleratorKeyPressedEventArgs*>(
            [this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                UINT vk = 0;
                a->get_KeyEventKind(&kind);
                a->get_VirtualKey(&vk);
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                    return S_OK;
                bool ctrl = GetKeyState(VK_CONTROL) < 0;
                bool handled = true;
                switch (vk) {
                case VK_PRIOR: emit(WebKey::PrevChannel); break;
                case VK_NEXT: emit(WebKey::NextChannel); break;
                case VK_F11: emit(WebKey::Fullscreen); break;
                case VK_ESCAPE: handled = false; emit(WebKey::Escape); break;   // la page garde aussi Échap
                default:
                    if (ctrl && vk == 'L') emit(WebKey::ToggleStrip);
                    else if (ctrl && vk == 'M') emit(WebKey::Mute);
                    else if (ctrl && vk == 'F') emit(WebKey::Favorite);
                    else if (ctrl && vk == VK_BACK) emit(WebKey::PrevViewed);
                    else handled = false;
                }
                if (handled) a->put_Handled(TRUE);
                return S_OK;
            });
        ctrl_->add_AcceleratorKeyPressed(keys, &tok);
        keys->Release();

        // Le bouton plein écran du lecteur du site agrandit toute la fenêtre.
        auto* fs = make_cb<ICoreWebView2ContainsFullScreenElementChangedEventHandler, ICoreWebView2*, IUnknown*>(
            [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                BOOL f = FALSE;
                sender->get_ContainsFullScreenElement(&f);
                if (on_fullscreen) post_main([cb = on_fullscreen, f] { cb(f != FALSE); });
                return S_OK;
            });
        wv_->add_ContainsFullScreenElementChanged(fs, &tok);
        fs->Release();

        // Pas de fenêtres surgissantes : les liens s'ouvrent dans la même vue.
        auto* nw = make_cb<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*,
                           ICoreWebView2NewWindowRequestedEventArgs*>(
            [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                LPWSTR uri = nullptr;
                a->put_Handled(TRUE);
                if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                    wv_->Navigate(uri);
                    CoTaskMemFree(uri);
                }
                return S_OK;
            });
        wv_->add_NewWindowRequested(nw, &tok);
        nw->Release();

        // Messages des scripts de la page (double-clic sur la vidéo = plein écran).
        auto* msg = make_cb<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*,
                            ICoreWebView2WebMessageReceivedEventArgs*>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                LPWSTR s = nullptr;
                if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) {
                    if (wcscmp(s, L"fullscreen") == 0) emit(WebKey::Fullscreen);
                    else if (wcscmp(s, L"click") == 0) emit(WebKey::Click);
                    else if (wcscmp(s, L"dblclick") == 0) emit(WebKey::DoubleClick);
                    else if (wcscmp(s, L"wheelup") == 0) emit(WebKey::WheelUp);
                    else if (wcscmp(s, L"wheeldown") == 0) emit(WebKey::WheelDown);
                    else if (wcscmp(s, L"ended") == 0) emit(WebKey::Ended);
                    CoTaskMemFree(s);
                }
                return S_OK;
            });
        wv_->add_WebMessageReceived(msg, &tok);
        msg->Release();

        auto* done = make_cb<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*,
                             ICoreWebView2NavigationCompletedEventArgs*>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                loading_ = false;
                wake_main();
                return S_OK;
            });
        wv_->add_NavigationCompleted(done, &tok);
        done->Release();

        // Adresse affichée : suit aussi la navigation interne du site (history.pushState).
        auto* src = make_cb<ICoreWebView2SourceChangedEventHandler, ICoreWebView2*,
                            ICoreWebView2SourceChangedEventArgs*>(
            [this](ICoreWebView2* w, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                LPWSTR s = nullptr;
                if (SUCCEEDED(w->get_Source(&s)) && s) {
                    std::string u = U8(s);
                    CoTaskMemFree(s);
                    if (on_source) post_main([cb = on_source, u] { cb(u); });
                }
                return S_OK;
            });
        wv_->add_SourceChanged(src, &tok);
        src->Release();

        set_muted(muted_);
        for (auto& js : startup_scripts_) add_startup_script(js);
        startup_scripts_.clear();
        if (!pending_.empty()) {
            std::string u;
            u.swap(pending_);
            navigate(u);
        }
        wake_main();
    }

    HWND parent_;
    HWND popup_ = nullptr;
    HMODULE lib_ = nullptr;
    ICoreWebView2Environment* env_ = nullptr;
    ICoreWebView2Controller* ctrl_ = nullptr;
    ICoreWebView2* wv_ = nullptr;
    RECT bounds_{0, 0, 0, 0};
    bool visible_ = true;
    bool muted_ = false;
    bool loading_ = false;
    bool failed_ = false;
    std::string error_, pending_;
    std::vector<std::string> startup_scripts_;
};

} // namespace

std::unique_ptr<WebView> WebView::create(SDL_Window* parent, const std::string& data_dir, std::string* why) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(parent, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        if (why) *why = "fenêtre Windows introuvable";
        return nullptr;
    }
    auto v = std::make_unique<WinWebView>(info.info.win.window, data_dir);
    if (v->failed()) {
        if (why) *why = v->error();
        return nullptr;
    }
    return v;
}

} // namespace sw

#else   // ── autres systèmes : pas de vue intégrée, la page s'ouvre dans le navigateur

namespace sw {
std::unique_ptr<WebView> WebView::create(SDL_Window*, const std::string&, std::string* why) {
    if (why) *why = "lecteur intégré disponible sous Windows uniquement";
    return nullptr;
}
} // namespace sw

#endif
