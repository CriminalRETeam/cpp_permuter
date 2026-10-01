#include "macros.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace perm {

// A template: text pieces and macro calls.
struct MacroExpander::Node {
    struct Piece {
        std::string text;                         // when name is empty
        std::string name;                         // PERM_...
        std::vector<std::shared_ptr<Node>> args;
        std::vector<std::string> rawArgs;         // the arguments as written
    };
    std::vector<Piece> pieces;
};

namespace {

using Node = MacroExpander::Node;

// Skips a string/char literal or comment starting at i; returns the index after it.
size_t skipLiteral(const std::string& s, size_t i) {
    char c = s[i];
    if (c == '"' || c == '\'') {
        size_t j = i + 1;
        while (j < s.size() && s[j] != c && s[j] != '\n') j += s[j] == '\\' ? 2 : 1;
        return std::min(s.size(), j + 1);
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
        size_t e = s.find('\n', i);
        return e == std::string::npos ? s.size() : e;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
        size_t e = s.find("*/", i + 2);
        return e == std::string::npos ? s.size() : e + 2;
    }
    return i;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::shared_ptr<Node> parse(const std::string& s, bool& has, bool& randomize, std::string& err);

// Splits the text between the parentheses of a macro call at top-level commas.
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (size_t i = 0; i < s.size();) {
        size_t j = skipLiteral(s, i);
        if (j != i) {
            cur += s.substr(i, j - i);
            i = j;
            continue;
        }
        if (s.compare(i, 3, "(,)") == 0) {
            cur += "(,)"; // unescaped after parsing the argument
            i += 3;
            continue;
        }
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
        i++;
    }
    out.push_back(cur);
    return out;
}

void unescape(Node& n) {
    for (auto& p : n.pieces) {
        if (p.name.empty()) {
            size_t pos;
            while ((pos = p.text.find("(,)")) != std::string::npos) p.text.replace(pos, 3, ",");
        }
        for (auto& a : p.args) unescape(*a);
    }
}

std::shared_ptr<Node> parse(const std::string& s, bool& has, bool& randomize, std::string& err) {
    auto n = std::make_shared<Node>();
    std::string text;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = skipLiteral(s, i);
        if (j != i) {
            text += s.substr(i, j - i);
            i = j;
            continue;
        }
        bool wordStart = i == 0 || !(std::isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_');
        if (wordStart && s.compare(i, 5, "PERM_") == 0) {
            size_t e = i + 5;
            while (e < s.size() && (std::isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
            std::string name = s.substr(i, e - i);
            size_t open = e;
            while (open < s.size() && (s[open] == ' ' || s[open] == '\t')) open++;
            if (open < s.size() && s[open] == '(') {
                // find the matching ')'
                int depth = 0;
                size_t k = open;
                for (; k < s.size();) {
                    size_t l = skipLiteral(s, k);
                    if (l != k) {
                        k = l;
                        continue;
                    }
                    if (s.compare(k, 3, "(,)") == 0) {
                        k += 3;
                        continue;
                    }
                    if (s[k] == '(') depth++;
                    if (s[k] == ')' && --depth == 0) break;
                    k++;
                }
                if (k >= s.size()) {
                    err = name + ": no closing parenthesis";
                    return n;
                }
                if (!text.empty()) n->pieces.push_back({text, "", {}, {}});
                text.clear();
                Node::Piece p;
                p.name = name;
                p.rawArgs = splitArgs(s.substr(open + 1, k - open - 1));
                for (auto& a : p.rawArgs) {
                    if (name == "PERM_IGNORE") {
                        auto t = std::make_shared<Node>();
                        t->pieces.push_back({a, "", {}, {}});
                        p.args.push_back(t);
                    } else {
                        p.args.push_back(parse(a, has, randomize, err));
                    }
                }
                has = true;
                if (name == "PERM_RANDOMIZE") randomize = true;
                static const std::set<std::string> known = {
                    "PERM_GENERAL", "PERM_LINESWAP", "PERM_LINESWAP_TEXT", "PERM_INT",
                    "PERM_ONCE", "PERM_VAR", "PERM_RANDOMIZE", "PERM_FORCE_SAMELINE",
                    "PERM_IGNORE", "PERM_PRETEND"};
                if (!known.count(name)) err = "unknown macro " + name;
                n->pieces.push_back(std::move(p));
                i = k + 1;
                continue;
            }
        }
        text += s[i++];
    }
    if (!text.empty()) n->pieces.push_back({text, "", {}, {}});
    return n;
}

void collectOnce(const Node& n, std::map<std::string, int>& counts) {
    for (auto& p : n.pieces) {
        if (p.name == "PERM_ONCE") counts[trim(p.rawArgs[0])]++;
        for (auto& a : p.args) collectOnce(*a, counts);
    }
}

struct Eval {
    const std::function<int(int)>& choose;
    std::map<std::string, std::string> vars;
    std::map<std::string, int> onceTotal, onceSeen, onceChosen;

    int pick(int n) {
        if (n <= 1) return 0;
        int c = choose(n);
        return std::max(0, std::min(n - 1, c));
    }

    std::string run(const Node& n) {
        std::string out;
        for (auto& p : n.pieces) out += p.name.empty() ? p.text : call(p);
        return out;
    }

    std::string call(const Node::Piece& p) {
        const std::string& m = p.name;
        auto arg = [&](size_t i) { return i < p.args.size() ? run(*p.args[i]) : std::string(); };
        if (m == "PERM_GENERAL") return arg(pick((int)p.args.size()));
        if (m == "PERM_RANDOMIZE" || m == "PERM_IGNORE") return arg(0);
        if (m == "PERM_PRETEND") return "";
        if (m == "PERM_INT") {
            long lo = std::stol(trim(arg(0))), hi = std::stol(trim(arg(1)));
            if (hi < lo) return std::to_string(lo);
            return std::to_string(lo + pick((int)(hi - lo + 1)));
        }
        if (m == "PERM_FORCE_SAMELINE") {
            std::string s = arg(0), r;
            for (char c : s) r += c == '\n' ? ' ' : c;
            return r;
        }
        if (m == "PERM_LINESWAP" || m == "PERM_LINESWAP_TEXT") {
            std::string s = arg(0);
            std::vector<std::string> lines;
            size_t b = 0;
            while (b <= s.size()) {
                size_t e = s.find('\n', b);
                std::string l = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
                if (!trim(l).empty()) lines.push_back(l);
                if (e == std::string::npos) break;
                b = e + 1;
            }
            std::string out = "\n";
            while (!lines.empty()) {
                int k = pick((int)lines.size());
                out += lines[k] + "\n";
                lines.erase(lines.begin() + k);
            }
            return out;
        }
        if (m == "PERM_VAR") {
            std::string name = trim(arg(0));
            if (p.args.size() >= 2) {
                vars[name] = arg(1);
                return "";
            }
            return vars[name];
        }
        if (m == "PERM_ONCE") {
            std::string key = trim(p.rawArgs[0]);
            int idx = onceSeen[key]++;
            if (!onceChosen.count(key)) onceChosen[key] = pick(onceTotal[key]);
            if (idx != onceChosen[key]) return "";
            return p.args.size() >= 2 ? arg(1) : arg(0);
        }
        return "";
    }
};

} // namespace

MacroExpander::MacroExpander(const std::string& text) {
    root_ = parse(text, hasMacros_, randomize_, error_);
    unescape(*root_);
}

std::string MacroExpander::expand(const std::function<int(int)>& choose) const {
    Eval ev{choose, {}, {}, {}, {}};
    collectOnce(*root_, ev.onceTotal);
    return ev.run(*root_);
}

std::string MacroExpander::first() const {
    return expand([](int) { return 0; });
}

void MacroExpander::enumerate(size_t cap, const std::function<bool(const std::string&)>& emit) const {
    // walk the tree of choices: replay a prefix, take option 0 after it, then
    // advance the last choice that still has options left
    std::vector<int> prefix;
    std::set<std::string> seen;
    size_t produced = 0;
    while (true) {
        std::vector<int> taken, counts;
        std::string out = expand([&](int n) {
            size_t i = taken.size();
            int c = i < prefix.size() ? prefix[i] : 0;
            taken.push_back(c);
            counts.push_back(n);
            return c;
        });
        if (seen.insert(out).second) {
            if (!emit(out) || ++produced >= cap) return;
        }
        int j = (int)taken.size() - 1;
        while (j >= 0 && taken[j] + 1 >= counts[j]) j--;
        if (j < 0) return;
        prefix.assign(taken.begin(), taken.begin() + j);
        prefix.push_back(taken[j] + 1);
    }
}

} // namespace perm
