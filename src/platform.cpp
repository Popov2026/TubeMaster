#include "platform.hpp"

#include <cstdio>
#include <filesystem>
#include <iterator>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <unistd.h>
#include <climits>
#include <cstdlib>
#endif

namespace sw {

#ifdef _WIN32
static std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
static std::string U(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string exe_dir() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    if (!n) return ".";
    return std::filesystem::path(buf).parent_path().u8string();
}

void open_external(const std::string& url) {
    ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_folder(const std::string& path) {
    ShellExecuteW(nullptr, L"explore", W(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void show_in_folder(const std::string& file) {
    std::wstring arg = L"/select,\"" + W(file) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

// « Libellé|*.txt;*.csv » → filtres de la boîte de dialogue (+ « Tous »).
static void set_filter(IFileDialog* dlg, const std::string& filter) {
    if (filter.empty()) return;
    size_t bar = filter.find('|');
    static std::wstring label, spec;
    label = W(filter.substr(0, bar));
    spec = W(bar == std::string::npos ? "*.*" : filter.substr(bar + 1));
    COMDLG_FILTERSPEC fs[] = {{label.c_str(), spec.c_str()}, {L"Tous les fichiers", L"*.*"}};
    dlg->SetFileTypes(2, fs);
}

std::string pick_folder(const std::string& title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dlg))) || !dlg)
        return {};
    DWORD o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
    dlg->SetTitle(W(title).c_str());
    std::string out;
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = U(p); CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

std::vector<std::string> pick_open_files(const std::string& title, const std::string& filter, bool multi) {
    std::vector<std::string> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                reinterpret_cast<void**>(&dlg))) || !dlg)
        return out;
    DWORD o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (multi ? FOS_ALLOWMULTISELECT : 0));
    dlg->SetTitle(W(title).c_str());
    set_filter(dlg, filter);
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr)) && arr) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it)) && it) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out.push_back(U(p)); CoTaskMemFree(p); }
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    dlg->Release();
    return out;
}

std::string pick_save_file(const std::string& title, const std::string& def, const std::string& filter) {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog,
                                reinterpret_cast<void**>(&dlg))) || !dlg)
        return {};
    DWORD o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    dlg->SetTitle(W(title).c_str());
    set_filter(dlg, filter);
    dlg->SetDefaultExtension(L"txt");
    dlg->SetFileName(W(def).c_str());
    std::string out;
    if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = U(p); CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    return out;
}

void platform_init() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); }
void platform_shutdown() { CoUninitialize(); }

#else

std::string exe_dir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return std::filesystem::path(buf).parent_path().string();
}

static std::string shell_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    return r + "'";
}

void open_external(const std::string& url) {
    std::string cmd = "xdg-open " + shell_quote(url) + " >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "xdg-open a échoué\n");
}
void open_folder(const std::string& path) { open_external(path); }
void show_in_folder(const std::string& file) { open_external(std::filesystem::path(file).parent_path().string()); }

static std::string run_capture(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string pick_folder(const std::string& title) {
    return run_capture("zenity --file-selection --directory --title=" + shell_quote(title) + " 2>/dev/null");
}
std::vector<std::string> pick_open_files(const std::string& title, const std::string&, bool multi) {
    std::string r = run_capture("zenity --file-selection " + std::string(multi ? "--multiple --separator='\n' " : "") +
                                "--title=" + shell_quote(title) + " 2>/dev/null");
    std::vector<std::string> out;
    size_t a = 0;
    while (a < r.size()) {
        size_t e = r.find('\n', a);
        out.push_back(r.substr(a, e == std::string::npos ? std::string::npos : e - a));
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return out;
}
std::string pick_save_file(const std::string& title, const std::string& def, const std::string&) {
    return run_capture("zenity --file-selection --save --confirm-overwrite --title=" + shell_quote(title) +
                       " --filename=" + shell_quote(def) + " 2>/dev/null");
}
void platform_init() {}
void platform_shutdown() {}

#endif

} // namespace sw
