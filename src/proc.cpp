#include "proc.hpp"

#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sw {

std::string command_line(const std::string& exe, const std::vector<std::string>& args) {
    auto q = [](const std::string& s) {
        if (!s.empty() && s.find_first_of(" \t\"&|<>^%") == std::string::npos) return s;
        std::string r = "\"";
        for (char c : s) { if (c == '"') r += '\\'; r += c; }
        return r + "\"";
    };
    std::string r = q(exe);
    for (auto& a : args) r += " " + q(a);
    return r;
}

namespace {

// Découpe le flux en lignes (\n, et \r pour les barres de progression).
struct LineSplitter {
    std::string buf;
    template <class F> void feed(const char* d, size_t n, F&& emit) {
        for (size_t i = 0; i < n; ++i) {
            char c = d[i];
            if (c == '\n' || c == '\r') {
                if (!buf.empty()) emit(buf);
                buf.clear();
            } else {
                buf += c;
            }
        }
    }
    template <class F> void flush(F&& emit) {
        if (!buf.empty()) emit(buf);
        buf.clear();
    }
};

} // namespace

#ifdef _WIN32

static std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Règles de CommandLineToArgvW (guillemets, barres obliques inverses).
static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++bs; ++i; }
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; }
        else { r.append(bs, L'\\'); r += a[i]; }
    }
    return r + L"\"";
}

struct Process::Impl {
    HANDLE proc = nullptr, job = nullptr, out_read = nullptr;
};

std::shared_ptr<Process> Process::start(const std::string& exe, const std::vector<std::string>& args,
                                        const std::string& extra_path, LineFn on_line, ExitFn on_exit,
                                        std::string* error) {
    static std::mutex env_m;
    {
        // Environnement hérité : sortie en UTF-8, outils du dossier choisi en tête du PATH.
        std::lock_guard<std::mutex> lk(env_m);
        SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
        SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
        SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");       // progression transmise ligne par ligne
        if (!extra_path.empty()) {
            wchar_t buf[32767];
            DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32767);
            std::wstring cur(buf, n < 32767 ? n : 0), add = W(extra_path);
            if (cur.find(add) == std::wstring::npos) SetEnvironmentVariableW(L"PATH", (add + L";" + cur).c_str());
        }
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { if (error) *error = "tube impossible"; return nullptr; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring cmd = quote_arg(W(exe));
    for (auto& a : args) cmd += L" " + quote_arg(W(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (error) *error = "lancement impossible (erreur " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    // Objet « job » : Stop arrête yt-dlp et tout ce qu'il a lancé (ffmpeg).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::shared_ptr<Process> p(new Process());
    p->impl_ = std::make_unique<Impl>();
    p->impl_->proc = pi.hProcess;
    p->impl_->job = job;
    p->impl_->out_read = rd;
    p->running_ = true;
    std::thread([p, on_line, on_exit] {
        LineSplitter sp;
        char buf[8192];
        DWORD n = 0;
        auto emit = [&](const std::string& l) { if (on_line) on_line(l); };
        while (ReadFile(p->impl_->out_read, buf, sizeof buf, &n, nullptr) && n > 0) sp.feed(buf, n, emit);
        sp.flush(emit);
        WaitForSingleObject(p->impl_->proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(p->impl_->proc, &code);
        p->running_ = false;
        if (on_exit) on_exit(int(code));
    }).detach();
    return p;
}

void Process::kill() {
    if (!impl_ || !running_) return;
    if (impl_->job) TerminateJobObject(impl_->job, 1);
    else TerminateProcess(impl_->proc, 1);
}

Process::~Process() {
    if (!impl_) return;
    if (impl_->out_read) CloseHandle(impl_->out_read);
    if (impl_->proc) CloseHandle(impl_->proc);
    if (impl_->job) CloseHandle(impl_->job);
}

#else

struct Process::Impl {
    pid_t pid = -1;
    int out_read = -1;
};

std::shared_ptr<Process> Process::start(const std::string& exe, const std::vector<std::string>& args,
                                        const std::string& extra_path, LineFn on_line, ExitFn on_exit,
                                        std::string* error) {
    int fd[2];
    if (pipe(fd) != 0) { if (error) *error = "tube impossible"; return nullptr; }
    pid_t pid = fork();
    if (pid < 0) { if (error) *error = "fork impossible"; return nullptr; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]);
        close(fd[1]);
        if (!extra_path.empty()) {
            const char* cur = getenv("PATH");
            std::string np = extra_path + ":" + (cur ? cur : "");
            setenv("PATH", np.c_str(), 1);
        }
        setenv("PYTHONIOENCODING", "utf-8", 1);
        std::vector<char*> av;
        av.push_back(const_cast<char*>(exe.c_str()));
        for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(exe.c_str(), av.data());
        std::fprintf(stderr, "lancement impossible : %s\n", std::strerror(errno));
        _exit(127);
    }
    close(fd[1]);
    std::shared_ptr<Process> p(new Process());
    p->impl_ = std::make_unique<Impl>();
    p->impl_->pid = pid;
    p->impl_->out_read = fd[0];
    p->running_ = true;
    std::thread([p, on_line, on_exit] {
        LineSplitter sp;
        char buf[8192];
        ssize_t n;
        auto emit = [&](const std::string& l) { if (on_line) on_line(l); };
        while ((n = read(p->impl_->out_read, buf, sizeof buf)) > 0) sp.feed(buf, size_t(n), emit);
        sp.flush(emit);
        int st = 0;
        waitpid(p->impl_->pid, &st, 0);
        p->running_ = false;
        if (on_exit) on_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
    }).detach();
    return p;
}

void Process::kill() {
    if (impl_ && running_ && impl_->pid > 0) ::kill(-impl_->pid, SIGTERM);
}

Process::~Process() {
    if (impl_ && impl_->out_read >= 0) close(impl_->out_read);
}

#endif

} // namespace sw
