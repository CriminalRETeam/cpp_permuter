#include "runner.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace perm {

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
