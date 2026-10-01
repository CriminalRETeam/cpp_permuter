#include "scorer.hpp"

#include "runner.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace perm {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

bool isHex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit((unsigned char)c); });
}

struct RawInsn {
    unsigned addr = 0;
    std::vector<unsigned char> bytes;
    std::string mnem, args;
    std::vector<std::pair<unsigned, std::string>> relocs; // offset, symbol
    std::vector<std::string> relocTypes;
};

// Replaces the first whole-word occurrence of word in s.
bool replaceWord(std::string& s, const std::string& word, const std::string& with) {
    size_t pos = 0;
    while ((pos = s.find(word, pos)) != std::string::npos) {
        bool lb = pos == 0 || !(std::isalnum((unsigned char)s[pos - 1]) || s[pos - 1] == '_');
        size_t e = pos + word.size();
        bool rb = e >= s.size() || !(std::isalnum((unsigned char)s[e]) || s[e] == '_');
        if (lb && rb) {
            s.replace(pos, word.size(), with);
            return true;
        }
        pos = e;
    }
    return false;
}

std::string hexStr(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", v);
    return b;
}

} // namespace

std::vector<Insn> parseObjdump(const std::string& text, const std::string& symbol,
                               bool ignoreRelocNames) {
    std::vector<RawInsn> raw;
    std::istringstream in(text);
    std::string line;
    bool inside = false;
    std::string header = "<" + symbol + ">:";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find(">:") != std::string::npos && line.find('<') != std::string::npos &&
            !line.empty() && line[0] != ' ' && line[0] != '\t') {
            inside = line.find(header) != std::string::npos;
            continue;
        }
        if (!inside) continue;
        std::string t = trim(line);
        if (t.empty()) continue;
        size_t colon = t.find(':');
        if (colon == std::string::npos || !isHex(t.substr(0, colon))) continue;
        unsigned addr = (unsigned)std::stoul(t.substr(0, colon), nullptr, 16);
        std::string rest = trim(t.substr(colon + 1));
        // relocation line: "IMAGE_REL_I386_DIR32\tsym" / "R_386_32\tsym"
        if (rest.rfind("IMAGE_REL_", 0) == 0 || rest.rfind("R_386", 0) == 0 ||
            rest.rfind("R_X86", 0) == 0 || rest.rfind("dir32", 0) == 0 ||
            rest.rfind("DISP32", 0) == 0) {
            size_t sp = rest.find_first_of(" \t");
            std::string type = rest.substr(0, sp);
            std::string sym = sp == std::string::npos ? "" : trim(rest.substr(sp));
            if (!raw.empty()) {
                raw.back().relocs.push_back({addr, sym});
                raw.back().relocTypes.push_back(type);
            }
            continue;
        }
        RawInsn r;
        r.addr = addr;
        // raw bytes are pairs of hex digits separated by single spaces, then a tab
        size_t p = 0;
        while (p + 2 <= rest.size() && isHex(rest.substr(p, 2)) &&
               (p + 2 == rest.size() || rest[p + 2] == ' ' || rest[p + 2] == '\t')) {
            r.bytes.push_back((unsigned char)std::stoul(rest.substr(p, 2), nullptr, 16));
            p += 2;
            if (p < rest.size() && rest[p] == '\t') break;
            while (p < rest.size() && rest[p] == ' ') p++;
            if (p < rest.size() && rest[p] == '\t') break;
        }
        std::string ins = trim(rest.substr(p));
        if (ins.empty()) {
            // continuation of a long byte sequence (binutils)
            if (!raw.empty()) raw.back().bytes.insert(raw.back().bytes.end(), r.bytes.begin(), r.bytes.end());
            continue;
        }
        size_t sp = ins.find_first_of(" \t");
        r.mnem = ins.substr(0, sp);
        r.args = sp == std::string::npos ? "" : trim(ins.substr(sp));
        // prefixes such as "lock", "rep" are printed as their own mnemonic
        raw.push_back(std::move(r));
    }

    std::map<unsigned, int> indexOf;
    for (size_t i = 0; i < raw.size(); ++i) indexOf[raw[i].addr] = (int)i;

    std::vector<Insn> out;
    for (size_t i = 0; i < raw.size(); ++i) {
        RawInsn& r = raw[i];
        std::string args = r.args;
        size_t lt = args.find(" <");
        if (lt != std::string::npos) args = trim(args.substr(0, lt));
        bool branch = r.mnem[0] == 'j' || r.mnem.rfind("call", 0) == 0 || r.mnem.rfind("loop", 0) == 0;
        bool handled = false;
        for (size_t k = 0; k < r.relocs.size(); ++k) {
            std::string sym = ignoreRelocNames ? "<sym>" : r.relocs[k].second;
            bool rel = r.relocTypes[k].find("REL32") != std::string::npos ||
                       r.relocTypes[k].find("PC32") != std::string::npos ||
                       r.relocTypes[k] == "DISP32" || r.relocTypes[k].find("PLT") != std::string::npos;
            if (rel && branch) {
                args = sym;
                handled = true;
                continue;
            }
            unsigned off = r.relocs[k].first - r.addr;
            unsigned addend = 0;
            for (unsigned b = 0; b < 4 && off + b < r.bytes.size(); ++b)
                addend |= (unsigned)r.bytes[off + b] << (8 * b);
            std::string v = hexStr(addend);
            if (!replaceWord(args, v, addend ? sym + "+" + v : sym)) args += " ; " + sym;
        }
        if (branch && !handled && r.relocs.empty() && args.rfind("0x", 0) == 0) {
            unsigned target = (unsigned)std::stoul(args, nullptr, 16);
            auto it = indexOf.find(target);
            args = it != indexOf.end() ? "L" + std::to_string(it->second) : args;
        }
        if (ignoreRelocNames && r.relocs.empty()) {
            // absolute addresses in code taken straight from an executable
            std::string res;
            size_t q = 0;
            while (q < args.size()) {
                if (args.compare(q, 2, "0x") == 0) {
                    size_t e = q + 2;
                    while (e < args.size() && std::isxdigit((unsigned char)args[e])) e++;
                    unsigned long v = std::stoul(args.substr(q + 2, e - q - 2), nullptr, 16);
                    res += v >= 0x400000 && v < 0x10000000 ? "<sym>" : args.substr(q, e - q);
                    q = e;
                } else {
                    res += args[q++];
                }
            }
            args = res;
        }
        out.push_back({r.mnem, args});
    }
    return out;
}

bool disassemble(const ScoreConfig& cfg, const std::string& obj, const std::string& symbol,
                 std::vector<Insn>& out, std::string& err) {
    std::string cmd = shellQuote(cfg.objdump) + " -d -r --disassemble-symbols=" +
                      shellQuote(symbol) + " " + shellQuote(obj);
    CmdResult r = runCommand(cmd, 60);
    if (r.status != 0) {
        err = "objdump failed: " + r.output;
        return false;
    }
    out = parseObjdump(r.output, symbol, cfg.ignoreRelocNames);
    if (out.empty()) {
        err = "symbol '" + symbol + "' not found in " + obj;
        return false;
    }
    return true;
}

namespace {

std::string squash(const std::string& s, bool numbers) {
    std::string r;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            r += "%r";
            i++;
            while (i < s.size() && std::isalnum((unsigned char)s[i])) i++;
        } else if (numbers && std::isdigit((unsigned char)s[i])) {
            r += "N";
            while (i < s.size() && (std::isxdigit((unsigned char)s[i]) || s[i] == 'x')) i++;
        } else {
            r += s[i++];
        }
    }
    return r;
}

enum Op { Same, Sub, Del, Ins };

struct Alignment {
    int score = 0;
    std::vector<std::pair<Op, std::pair<int, int>>> steps;
};

int subCost(const ScoreConfig& cfg, const Insn& a, const Insn& b) {
    if (a.mnem != b.mnem) return INT_MAX / 4;
    if (a.args == b.args) return 0;
    if (squash(a.args, false) == squash(b.args, false)) return cfg.penaltyRegalloc;
    return cfg.penaltyArgs;
}

Alignment align(const ScoreConfig& cfg, const std::vector<Insn>& a, const std::vector<Insn>& b) {
    size_t n = a.size(), m = b.size();
    std::vector<int> dp((n + 1) * (m + 1));
    auto at = [&](size_t i, size_t j) -> int& { return dp[i * (m + 1) + j]; };
    for (size_t i = 0; i <= n; ++i) at(i, 0) = (int)i * cfg.penaltyDelete;
    for (size_t j = 0; j <= m; ++j) at(0, j) = (int)j * cfg.penaltyInsert;
    for (size_t i = 1; i <= n; ++i)
        for (size_t j = 1; j <= m; ++j) {
            int best = std::min(at(i - 1, j) + cfg.penaltyDelete, at(i, j - 1) + cfg.penaltyInsert);
            int s = subCost(cfg, a[i - 1], b[j - 1]);
            if (s < INT_MAX / 4) best = std::min(best, at(i - 1, j - 1) + s);
            at(i, j) = best;
        }
    Alignment al;
    size_t i = n, j = m;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0) {
            int s = subCost(cfg, a[i - 1], b[j - 1]);
            if (s < INT_MAX / 4 && at(i, j) == at(i - 1, j - 1) + s) {
                al.steps.push_back({s == 0 ? Same : Sub, {(int)i - 1, (int)j - 1}});
                i--, j--;
                continue;
            }
        }
        if (i > 0 && at(i, j) == at(i - 1, j) + cfg.penaltyDelete) {
            al.steps.push_back({Del, {(int)i - 1, -1}});
            i--;
        } else {
            al.steps.push_back({Ins, {-1, (int)j - 1}});
            j--;
        }
    }
    std::reverse(al.steps.begin(), al.steps.end());
    al.score = at(n, m);
    return al;
}

} // namespace

int scoreInsns(const ScoreConfig& cfg, const std::vector<Insn>& target, const std::vector<Insn>& cand) {
    Alignment al = align(cfg, target, cand);
    // a deletion and an insertion of the same instruction is a reordering
    std::multiset<std::string> dels, ins;
    for (auto& s : al.steps) {
        if (s.first == Del) dels.insert(target[s.second.first].full());
        if (s.first == Ins) ins.insert(cand[s.second.second].full());
    }
    int score = al.score;
    for (auto& d : dels) {
        auto it = ins.find(d);
        if (it == ins.end()) continue;
        ins.erase(it);
        score -= cfg.penaltyDelete + cfg.penaltyInsert - cfg.penaltyReorder;
    }
    return score;
}

std::string diffInsns(const ScoreConfig& cfg, const std::vector<Insn>& target,
                      const std::vector<Insn>& cand) {
    Alignment al = align(cfg, target, cand);
    std::ostringstream o;
    char buf[512];
    std::snprintf(buf, sizeof buf, "   %-44s %s\n", "TARGET", "CANDIDATE");
    o << buf;
    for (auto& s : al.steps) {
        std::string l = s.second.first >= 0 ? target[s.second.first].full() : "";
        std::string r = s.second.second >= 0 ? cand[s.second.second].full() : "";
        const char* mark = s.first == Same ? "  " : s.first == Sub ? "r " : s.first == Del ? "- " : "+ ";
        std::snprintf(buf, sizeof buf, "%s %-44s %s\n", mark, l.c_str(), r.c_str());
        o << buf;
    }
    return o.str();
}

std::vector<std::string> listSymbols(const ScoreConfig& cfg, const std::string& obj, std::string& err) {
    std::vector<std::string> out;
    CmdResult r = runCommand(shellQuote(cfg.objdump) + " -t " + shellQuote(obj), 60);
    if (r.status != 0) {
        err = r.output;
        return out;
    }
    std::istringstream in(r.output);
    std::string line;
    while (std::getline(in, line)) {
        // COFF: "[ 5](sec  1)(fl 0x00)(ty  20)(scl   2) (nx 0) 0x00000000 ?foo@@YAHXZ"
        // ELF:  "00000000 g     F .text  0000002a foo"
        if (line.find("(sec") != std::string::npos || line.find(" F ") != std::string::npos ||
            line.find(" g ") != std::string::npos) {
            size_t sp = line.find_last_of(" \t");
            if (sp != std::string::npos) out.push_back(line.substr(sp + 1));
        }
    }
    return out;
}

std::string guessSymbol(const std::vector<std::string>& syms, const std::string& qualName) {
    std::vector<std::string> comps;
    size_t i = 0;
    while (true) {
        size_t j = qualName.find("::", i);
        comps.push_back(qualName.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 2;
    }
    std::string last = comps.back();
    std::vector<std::string> prefixes;
    // MSVC
    {
        // ctors/dtors are named by their class: "??0G@ns@@" for ns::G::G
        std::string p;
        if (!last.empty() && last[0] == '~') p = "??1";
        else if (comps.size() >= 2 && comps[comps.size() - 2] == last) p = "??0";
        else p = "?" + last + "@";
        for (size_t k = comps.size() - 1; k-- > 0;) p += comps[k] + "@";
        prefixes.push_back(p + "@");
    }
    // Itanium
    if (comps.size() == 1) {
        prefixes.push_back("_Z" + std::to_string(last.size()) + last);
    } else {
        std::string p = "_ZN";
        for (auto& c : comps) {
            if (!c.empty() && c[0] == '~') p += "D";
            else p += std::to_string(c.size()) + c;
        }
        prefixes.push_back(p);
    }
    for (auto& p : prefixes)
        for (auto& s : syms)
            if (s.rfind(p, 0) == 0) return s;
    for (auto& s : syms)
        if (s == last || s == "_" + last) return s;
    return "";
}

} // namespace perm
