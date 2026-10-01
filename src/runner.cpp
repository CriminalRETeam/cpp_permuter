#include "runner.hpp"

#include <chrono>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace perm {

#ifdef _WIN32

// cmd.exe /S /C "cmd", stdout and stderr into one pipe. The process runs in a
// job object so a timeout kills everything it started.
CmdResult runCommand(const std::string& cmd, int timeoutSec,
                     const std::map<std::string, std::string>& env) {
    CmdResult r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        r.output = "CreatePipe failed";
        return r;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // environment block: ours plus the extra variables
    std::string block;
    if (!env.empty()) {
        LPCH cur = GetEnvironmentStringsA();
        for (LPCH p = cur; *p; p += strlen(p) + 1) {
            std::string kv = p;
            size_t eq = kv.find('=', 1);
            if (eq != std::string::npos && env.count(kv.substr(0, eq))) continue;
            block += kv;
            block += '\0';
        }
        FreeEnvironmentStringsA(cur);
        for (auto& [k, v] : env) {
            block += k + "=" + v;
            block += '\0';
        }
        block += '\0';
    }

    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nul;
    PROCESS_INFORMATION pi{};
    std::string line = "cmd.exe /S /C \"" + cmd + "\"";
    std::vector<char> buf(line.begin(), line.end());
    buf.push_back('\0');
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                             block.empty() ? nullptr : (LPVOID)block.data(), nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(readEnd);
        if (job) CloseHandle(job);
        r.output = "CreateProcess failed";
        return r;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);

    // wait for the process itself, not for the pipe to close: anything it
    // left running would keep the pipe open
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    auto drain = [&]() {
        DWORD avail = 0;
        while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char chunk[8192];
            DWORD got = 0;
            if (!ReadFile(readEnd, chunk, (DWORD)std::min<DWORD>(avail, sizeof chunk), &got, nullptr) || !got) break;
            r.output.append(chunk, got);
        }
    };
    while (true) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) break;
        if (timeoutSec > 0 && std::chrono::steady_clock::now() >= deadline) {
            r.timedOut = true;
            if (job) TerminateJobObject(job, 1);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    drain();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.status = r.timedOut ? -1 : (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readEnd);
    if (job) CloseHandle(job);
    return r;
}

std::string shellQuote(const std::string& s) {
    // for cmd.exe and the usual argv parsing: "..." with \" for quotes
    std::string r = "\"";
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else r += c;
    }
    return r + "\"";
}

#else

CmdResult runCommand(const std::string& cmd, int timeoutSec,
                     const std::map<std::string, std::string>& env) {
    CmdResult r;
    int fds[2];
    if (pipe(fds) != 0) {
        r.output = "pipe() failed";
        return r;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        r.output = "fork() failed";
        return r;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    // Wait for the child itself, not for EOF: daemons it spawns (wineserver)
    // inherit the pipe and can keep it open long after the compile is done.
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    char buf[8192];
    int st = 0;
    bool exited = false;
    auto drain = [&]() {
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof buf)) > 0) r.output.append(buf, (size_t)n);
    };
    while (!exited) {
        pollfd p{fds[0], POLLIN, 0};
        if (poll(&p, 1, 20) > 0 && (p.revents & POLLHUP)) usleep(5000); // closed: don't spin
        drain();
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) exited = true;
        else if (timeoutSec > 0 && std::chrono::steady_clock::now() >= deadline) {
            r.timedOut = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            exited = true;
        }
    }
    drain();
    close(fds[0]);
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (r.timedOut) r.status = -1;
    return r;
}

std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    return r + "'";
}

#endif

std::string expandTemplate(const std::string& tmpl, const std::map<std::string, std::string>& vars) {
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            size_t e = tmpl.find('}', i);
            if (e != std::string::npos) {
                auto it = vars.find(tmpl.substr(i + 1, e - i - 1));
                if (it != vars.end()) {
                    out += shellQuote(it->second);
                    i = e + 1;
                    continue;
                }
            }
        }
        out += tmpl[i++];
    }
    return out;
}

} // namespace perm
