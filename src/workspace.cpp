#include "workspace.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace perm {

namespace {

bool readAll(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Absolute and normalised, but symlinks are kept: a source tree made of
// symlinks (like the mirror itself) must not be resolved back to its origin.
std::string canon(const fs::path& p) { return fs::absolute(p).lexically_normal().string(); }

// Whether the definition starting at byte offset start in src is marked inline.
bool markedInline(const std::vector<Token>& toks, size_t start) {
    int i = 0;
    while (i < (int)toks.size() && toks[i].offset < start) i++;
    for (int k = i - 1; k >= 0; --k) {
        const Token& t = toks[k];
        if (t.kind == TokKind::PP) break;
        if (t.kind == TokKind::Punct && (t.text == ";" || t.text == "}" || t.text == "{")) break;
        if (t.text == "inline" || t.text == "__inline" || t.text == "__forceinline") return true;
    }
    return false;
}

// Argument counts of every call to name in f's body.
std::set<int> callArgCounts(const Func& f, const std::string& name) {
    std::set<int> out;
    for (int i = f.bodyOpen + 1; i < f.bodyClose; ++i) {
        if (f.t(i) != name || f.t(i + 1) != "(") continue;
        int close = matchBracket(f.toks, i + 1);
        if (close < 0) continue;
        int n = close > i + 2 ? 1 : 0, depth = 0;
        for (int j = i + 2; j < close; ++j) {
            const std::string& x = f.t(j);
            if (x == "(" || x == "[" || x == "{") depth++;
            else if (x == ")" || x == "]" || x == "}") depth--;
            else if (x == "," && depth == 0) n++;
        }
        out.insert(n);
    }
    return out;
}

// Whether a definition (text starting at its name) takes n arguments,
// counting defaulted and variadic parameters.
bool takesArgs(const std::string& def, int n) {
    std::vector<Token> t = lex(def);
    int open = -1;
    for (int i = 0; i < (int)t.size(); ++i)
        if (t[i].text == "(") {
            open = i;
            break;
        }
    if (open < 0) return true;
    int close = matchBracket(t, open);
    if (close < 0) return true;
    if (close == open + 1 || (close == open + 2 && t[open + 1].text == "void")) return n == 0;
    int params = 1, defaults = 0, depth = 0;
    bool variadic = false;
    for (int j = open + 1; j < close; ++j) {
        const std::string& x = t[j].text;
        if (x == "(" || x == "[" || x == "{" || x == "<") depth++;
        else if (x == ")" || x == "]" || x == "}" || x == ">") depth--;
        else if (depth == 0 && x == ",") params++;
        else if (depth == 0 && x == "=") defaults++;
        else if (x == "...") variadic = true;
    }
    if (variadic) return n >= params - 1;
    return n >= params - defaults && n <= params;
}

} // namespace

std::set<std::string> calledNames(const Func& f) {
    std::set<std::string> out;
    for (int i = f.bodyOpen + 1; i < f.bodyClose; ++i)
        if (f.toks[i].kind == TokKind::Ident && !isKeyword(f.t(i)) && f.t(i + 1) == "(")
            out.insert(f.t(i));
    return out;
}

std::vector<std::string> includedFiles(const std::string& file, const std::vector<std::string>& dirs) {
    std::vector<std::string> out;
    std::set<std::string> seen = {canon(file)};
    std::vector<std::string> todo = {canon(file)};
    while (!todo.empty()) {
        std::string cur = todo.back();
        todo.pop_back();
        std::string text;
        if (!readAll(cur, text)) continue;
        for (auto& t : lex(text)) {
            if (t.kind != TokKind::PP) continue;
            size_t inc = t.text.find("include");
            size_t q1 = t.text.find('"');
            if (inc == std::string::npos || q1 == std::string::npos || q1 < inc) continue;
            if (t.text.find_first_not_of(" \t", 1) != t.text.find("include")) continue;
            size_t q2 = t.text.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            std::string name = t.text.substr(q1 + 1, q2 - q1 - 1);
            std::vector<fs::path> tries = {fs::path(cur).parent_path() / name};
            for (auto& d : dirs) tries.push_back(fs::path(d) / name);
            for (auto& p : tries) {
                if (!fs::is_regular_file(p)) continue;
                std::string c = canon(p);
                if (seen.insert(c).second) {
                    out.push_back(c);
                    todo.push_back(c);
                }
                break;
            }
        }
    }
    return out;
}

std::vector<Region> inlineCallees(const std::string& srcPath, size_t mainStart, size_t mainEnd,
                                  const Func& main, const std::vector<std::string>& includeDirs,
                                  size_t maxRegions) {
    // calls in the order they first appear
    std::vector<std::string> names;
    std::set<std::string> want = calledNames(main);
    for (int i = main.bodyOpen + 1; i < main.bodyClose; ++i)
        if (want.count(main.t(i)) && main.t(i + 1) == "(" &&
            std::find(names.begin(), names.end(), main.t(i)) == names.end())
            names.push_back(main.t(i));

    std::string src = canon(srcPath);
    std::vector<std::string> files = {src};
    for (auto& h : includedFiles(src, includeDirs)) files.push_back(h);

    // every reachable definition, per called name
    std::vector<std::vector<Region>> perName;
    std::map<std::string, std::string> texts;
    std::map<std::string, std::vector<Token>> toks;
    for (auto& name : names) {
        perName.emplace_back();
        std::set<int> counts = callArgCounts(main, name);
        for (auto& file : files) {
            if (!texts.count(file)) {
                readAll(file, texts[file]);
                toks[file] = lex(texts[file]);
            }
            const std::string& text = texts[file];
            if (text.find(name) == std::string::npos) continue;
            for (auto [b, e] : findDefinitions(text, name)) {
                if (file == src && b == mainStart && e == mainEnd) continue;
                if (file == src && !markedInline(toks[file], b)) continue;
                // skip overloads no call could reach
                bool fits = false;
                for (int n : counts) fits |= takesArgs(text.substr(b, e - b), n);
                if (!fits) continue;
                // the qualified name as written
                std::string q = text.substr(b, text.find('(', b) - b);
                q.erase(std::remove_if(q.begin(), q.end(), ::isspace), q.end());
                perName.back().push_back({file, b, e, q, ""});
            }
        }
    }
    // the first definition of every name before any second overload, so a
    // heavily overloaded name can't use up maxRegions on its own
    std::vector<Region> out;
    for (size_t k = 0;; ++k) {
        bool any = false;
        for (auto& defs : perName) {
            if (k >= defs.size()) continue;
            any = true;
            const Region& r = defs[k];
            bool dup = false;
            for (auto& o : out) dup |= o.file == r.file && o.start < r.end && r.start < o.end;
            if (dup) continue;
            out.push_back(r);
            if (out.size() >= maxRegions) return out;
        }
        if (!any) break;
    }
    return out;
}

namespace {

struct GlobalDecl {
    size_t start = 0, end = 0; // the declaration, through its ';'
    std::string macro;         // e.g. DEFINE_GLOBAL_INIT
    std::vector<std::string> args;
};

std::string trimmed(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// Every "PREFIX...(args)" at the start of a line, for either prefix.
std::vector<GlobalDecl> globalDecls(const std::string& text, const std::string& a, const std::string& b) {
    std::vector<GlobalDecl> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        size_t i = text.find_first_not_of(" \t", pos);
        size_t next = lineEnd + 1;
        if (i != std::string::npos && i < lineEnd &&
            (text.compare(i, a.size(), a) == 0 || text.compare(i, b.size(), b) == 0)) {
            size_t m = i;
            while (m < text.size() && (std::isalnum((unsigned char)text[m]) || text[m] == '_')) m++;
            size_t open = text.find_first_not_of(" \t", m);
            if (open != std::string::npos && text[open] == '(') {
                GlobalDecl d;
                d.start = i;
                d.macro = text.substr(i, m - i);
                int depth = 0;
                size_t argStart = open + 1, j = open;
                for (; j < text.size(); ++j) {
                    char c = text[j];
                    if (c == '(' || c == '{' || c == '[') depth++;
                    else if (c == ')' || c == '}' || c == ']') {
                        if (--depth == 0) break;
                    } else if (c == ',' && depth == 1) {
                        d.args.push_back(trimmed(text.substr(argStart, j - argStart)));
                        argStart = j + 1;
                    }
                }
                if (j < text.size()) {
                    d.args.push_back(trimmed(text.substr(argStart, j - argStart)));
                    size_t e = j + 1;
                    size_t semi = text.find_first_not_of(" \t", e);
                    if (semi != std::string::npos && text[semi] == ';') e = semi + 1;
                    d.end = e;
                    if (d.args.size() >= 2) out.push_back(d);
                    next = std::max(next, text.find('\n', e) == std::string::npos ? text.size() : text.find('\n', e) + 1);
                }
            }
        }
        pos = next;
    }
    return out;
}

} // namespace

std::vector<Region> globalRegions(const std::string& srcPath, const std::string& srcText,
                                  const std::set<std::string>& names, const std::string& definePrefix,
                                  const std::string& externPrefix) {
    std::vector<Region> out;
    std::map<std::string, std::string> definitions; // name -> definition text in a sibling .cpp
    bool siblingsRead = false;
    auto readSiblings = [&]() {
        siblingsRead = true;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(fs::path(srcPath).parent_path(), ec)) {
            if (e.path().extension() != ".cpp" || fs::equivalent(e.path(), srcPath, ec)) continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            for (auto& d : globalDecls(text, definePrefix, definePrefix))
                if (names.count(d.args[1]) && !definitions.count(d.args[1]))
                    definitions[d.args[1]] = text.substr(d.start, d.end - d.start);
        }
    };
    for (auto& d : globalDecls(srcText, definePrefix, externPrefix)) {
        const std::string& name = d.args[1];
        if (!names.count(name)) continue;
        bool array = d.macro.find("_ARRAY") != std::string::npos;
        Region r;
        r.file = srcPath;
        r.start = d.start;
        r.end = d.end;
        r.name = "global " + name;
        if (d.macro.compare(0, definePrefix.size(), definePrefix) == 0) {
            if (array && d.args.size() < 3) continue;
            r.alt = externPrefix + (array ? "_ARRAY" : "") + "(" + d.args[0] + ", " + name +
                    (array ? ", " + d.args[2] : "") + ");";
        } else {
            if (!siblingsRead) readSiblings();
            auto it = definitions.find(name);
            if (it == definitions.end()) continue;
            r.alt = it->second;
        }
        out.push_back(r);
    }
    return out;
}

std::string spliceRegions(const std::string& original,
                          std::vector<std::pair<const Region*, const std::string*>> parts) {
    std::sort(parts.begin(), parts.end(),
              [](auto& a, auto& b) { return a.first->start < b.first->start; });
    std::string out;
    size_t pos = 0;
    for (auto& [r, text] : parts) {
        out.append(original, pos, r->start - pos);
        out += *text;
        pos = r->end;
    }
    out.append(original, pos, std::string::npos);
    return out;
}

bool Mirror::create(const std::string& root, const std::string& dir,
                    const std::vector<std::string>& realFiles, std::string& err) {
    root_ = canon(root);
    dir_ = fs::absolute(dir).string();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_, ec);
    if (ec) {
        err = "can't create " + dir_ + ": " + ec.message();
        return false;
    }
    // Each entry becomes a symlink. Where symlinks aren't allowed (Windows
    // without Developer Mode) a directory is mirrored entry by entry and a
    // file becomes a hard link, or a copy across volumes. The mirror never
    // writes to these; files it changes are replaced by copies below.
    std::function<bool(const fs::path&, const fs::path&)> linkEntries = [&](const fs::path& from,
                                                                            const fs::path& to) {
        for (auto& e : fs::directory_iterator(from, ec)) {
            fs::path dst = to / e.path().filename();
            std::error_code lec;
            bool dir = fs::is_directory(e.path(), lec);
            if (dir) fs::create_directory_symlink(e.path(), dst, lec);
            else fs::create_symlink(e.path(), dst, lec);
            if (!lec) continue;
            if (dir) {
                fs::create_directory(dst, lec);
                if (lec || !linkEntries(e.path(), dst)) {
                    if (err.empty()) err = "can't mirror " + e.path().string() + ": " + lec.message();
                    return false;
                }
                continue;
            }
            lec.clear();
            fs::create_hard_link(e.path(), dst, lec);
            if (lec) {
                lec.clear();
                fs::copy_file(e.path(), dst, lec);
            }
            if (lec) {
                err = "can't mirror " + e.path().string() + ": " + lec.message();
                return false;
            }
        }
        return true;
    };
    if (!linkEntries(root_, dir_)) return false;
    for (auto& f : realFiles) {
        fs::path rel = fs::path(canon(f)).lexically_relative(root_);
        if (rel.empty() || *rel.begin() == "..") {
            err = f + " is outside " + root_ + " (use --mirror-root)";
            return false;
        }
        // replace symlinked directories on the way with real ones
        fs::path cur = dir_, orig = root_;
        for (auto it = rel.begin(); it != rel.end(); ++it) {
            cur /= *it;
            orig /= *it;
            if (std::next(it) == rel.end()) break;
            if (fs::is_symlink(cur)) {
                fs::remove(cur, ec);
                fs::create_directory(cur, ec);
                if (!linkEntries(orig, cur)) return false;
            }
        }
        fs::remove(cur, ec);
        fs::copy_file(orig, cur, ec);
        if (ec) {
            err = "can't copy " + orig.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::string Mirror::map(const std::string& original) const {
    return (fs::path(dir_) / fs::path(canon(original)).lexically_relative(root_)).string();
}

} // namespace perm
