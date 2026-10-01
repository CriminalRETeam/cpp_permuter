// Running shell commands with a timeout.
#pragma once

#include <map>
#include <string>

namespace perm {

struct CmdResult {
    int status = -1; // exit code, or -1 if it didn't exit normally
    bool timedOut = false;
    std::string output; // stdout and stderr together
};

CmdResult runCommand(const std::string& cmd, int timeoutSec);

std::string shellQuote(const std::string& s);

// Replaces {key} in tmpl with the shell-quoted value.
std::string expandTemplate(const std::string& tmpl, const std::map<std::string, std::string>& vars);

} // namespace perm
