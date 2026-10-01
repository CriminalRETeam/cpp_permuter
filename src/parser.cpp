#include "parser.hpp"

#include <sstream>

namespace perm {

const char* skName(SK k) {
    switch (k) {
    case SK::Block: return "Block";
    case SK::Decl: return "Decl";
    case SK::Expr: return "Expr";
    case SK::If: return "If";
    case SK::For: return "For";
    case SK::While: return "While";
    case SK::Do: return "Do";
    case SK::Switch: return "Switch";
    case SK::Case: return "Case";
    case SK::Default: return "Default";
    case SK::Label: return "Label";
    case SK::Return: return "Return";
    case SK::Break: return "Break";
    case SK::Continue: return "Continue";
    case SK::Goto: return "Goto";
    case SK::Asm: return "Asm";
    case SK::PP: return "PP";
    case SK::Empty: return "Empty";
    case SK::Try: return "Try";
    case SK::Other: return "Other";
    }
    return "?";
}

int matchBracket(const std::vector<Token>& toks, int i) {
    const std::string& o = toks[i].text;
    std::string c = o == "(" ? ")" : o == "[" ? "]" : o == "{" ? "}" : "";
    if (c.empty()) return -1;
    int depth = 0;
    for (int j = i; j < (int)toks.size(); ++j) {
        if (toks[j].kind != TokKind::Punct) continue;
        const std::string& t = toks[j].text;
        if (t == "(" || t == "[" || t == "{") depth++;
        else if (t == ")" || t == "]" || t == "}") {
            depth--;
            if (depth == 0) return t == c ? j : -1;
        }
    }
    return -1;
}

bool Func::isLocal(int i) const {
    if (i < 0 || i >= (int)toks.size() || toks[i].kind != TokKind::Ident) return false;
    if (!locals.count(toks[i].text)) return false;
    if (i > 0) {
        const std::string& p = toks[i - 1].text;
        if (p == "." || p == "->" || p == "::") return false;
    }
    if (i + 1 < (int)toks.size() && toks[i + 1].text == "::") return false;
    return true;
}

std::string Func::indentOf(int i) const {
    for (int k = i; k >= 0; --k) {
        const std::string& lead = toks[k].lead;
        size_t nl = lead.rfind('\n');
        if (nl == std::string::npos) continue;
        std::string ind = "\n";
        for (size_t j = nl + 1; j < lead.size() && (lead[j] == ' ' || lead[j] == '\t'); ++j)
            ind += lead[j];
        return ind;
    }
    return "\n";
}

void forEachStmt(const Stmt& s, const std::function<void(const Stmt&)>& fn) {
    fn(s);
    for (auto& k : s.kids) forEachStmt(*k, fn);
}

// ---------------------------------------------------------------------------
// Locating the function

namespace {

std::vector<std::string> nameTokens(const std::string& qual) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= qual.size()) {
        size_t j = qual.find("::", i);
        std::string comp = qual.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!out.empty()) out.push_back("::");
        if (!comp.empty() && comp[0] == '~') {
            out.push_back("~");
            comp = comp.substr(1);
        }
        out.push_back(comp);
        if (j == std::string::npos) break;
        i = j + 2;
    }
    return out;
}

} // namespace

bool locateFunction(const std::string& src, const std::string& qualName, size_t& start,
                    size_t& end, std::string& err) {
    std::vector<Token> toks = lex(src);
    std::vector<std::string> pat = nameTokens(qualName);
    int n = (int)toks.size();
    int found = 0;
    for (int k = 0; k + (int)pat.size() < n; ++k) {
        bool ok = true;
        for (size_t p = 0; p < pat.size() && ok; ++p) ok = toks[k + p].text == pat[p];
        if (!ok) continue;
        int open = k + (int)pat.size();
        if (toks[open].text != "(") continue;
        // extend backwards over qualifiers the caller left out
        int s = k;
        while (s >= 2 && toks[s - 1].text == "::" && toks[s - 2].kind == TokKind::Ident) s -= 2;
        if (s >= 1 && (toks[s - 1].text == "." || toks[s - 1].text == "->")) continue;
        int close = matchBracket(toks, open);
        if (close < 0) continue;
        int j = close + 1, body = -1;
        while (j < n) {
            const std::string& t = toks[j].text;
            if (toks[j].kind == TokKind::Punct) {
                if (t == "{") {
                    body = j;
                    break;
                }
                if (t == "(" || t == "[") {
                    j = matchBracket(toks, j);
                    if (j < 0) break;
                    j++;
                    continue;
                }
                if (t == ":" || t == "," || t == "::" || t == "*" || t == "&" || t == "<" ||
                    t == ">") {
                    j++;
                    continue;
                }
                break;
            }
            if (toks[j].kind != TokKind::Ident) break;
            j++;
        }
        if (body < 0) continue;
        int bodyEnd = matchBracket(toks, body);
        if (bodyEnd < 0) continue;
        if (found++ == 0) {
            start = toks[s].offset;
            end = toks[bodyEnd].offset + 1;
        }
    }
    if (found == 0) {
        err = "definition of '" + qualName + "' not found";
        return false;
    }
    if (found > 1)
        err = "warning: " + std::to_string(found) + " definitions of '" + qualName +
              "' found, using the first";
    return true;
}

// ---------------------------------------------------------------------------
// Statement parser

namespace {

struct Parser {
    Func& f;
    const std::vector<Token>& T;

    explicit Parser(Func& fn) : f(fn), T(fn.toks) {}

    const std::string& t(int i) const {
        static const std::string empty;
        return i >= 0 && i < (int)T.size() ? T[i].text : empty;
    }
    bool punct(int i, const char* p) const {
        return i >= 0 && i < (int)T.size() && T[i].kind == TokKind::Punct && T[i].text == p;
    }

    // Index of the ';' ending the statement starting at i, or -1 if a '}' at
    // depth 0 (or limit) comes first.
    int findSemi(int i, int limit) const {
        int depth = 0;
        for (int j = i; j < limit; ++j) {
            if (T[j].kind != TokKind::Punct) continue;
            const std::string& s = T[j].text;
            if (s == "(" || s == "[" || s == "{") depth++;
            else if (s == ")" || s == "]" || s == "}") {
                if (depth == 0) return -1;
                depth--;
            } else if (s == ";" && depth == 0) return j;
        }
        return -1;
    }

    // End (exclusive) of an unterminated statement: the next '}' at depth 0.
    int findRecover(int i, int limit) const {
        int depth = 0;
        for (int j = i; j < limit; ++j) {
            if (T[j].kind != TokKind::Punct) continue;
            const std::string& s = T[j].text;
            if (s == "(" || s == "[" || s == "{") depth++;
            else if (s == ")" || s == "]" || s == "}") {
                if (depth == 0) return j > i ? j : i + 1;
                depth--;
            }
        }
        return limit > i ? limit : i + 1;
    }

    std::unique_ptr<Stmt> make(SK k, int b, int e) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->begin = b;
        s->end = e;
        return s;
    }

    std::unique_ptr<Stmt> toSemi(SK k, int i, int limit) {
        int semi = findSemi(i, limit);
        if (semi < 0) return make(SK::Other, i, findRecover(i, limit));
        return make(k, i, semi + 1);
    }

    void adopt(Stmt& p, std::unique_ptr<Stmt> c) {
        c->parent = &p;
        p.kids.push_back(std::move(c));
    }

    std::unique_ptr<Stmt> parseBlock(int open) {
        int close = matchBracket(T, open);
        if (close < 0) close = (int)T.size() - 1;
        auto s = make(SK::Block, open, close + 1);
        int i = open + 1;
        while (i < close) {
            auto c = parseStmt(i, close);
            if (c->end <= i) c->end = i + 1;
            i = c->end;
            adopt(*s, std::move(c));
        }
        return s;
    }

    int matchAngle(int i, int limit) const {
        int depth = 0;
        for (int j = i; j < limit; ++j) {
            const std::string& s = T[j].text;
            if (s == "<") depth++;
            else if (s == ">") {
                if (--depth == 0) return j;
            } else if (s == ">>") {
                depth -= 2;
                if (depth <= 0) return depth == 0 ? j : -1;
            } else if (s == ";" || s == "{" || s == "}" || s == "&&" || s == "||")
                return -1;
        }
        return -1;
    }

    // Tries to read tokens [b, e) (e is the ';' or the end of a for-init) as a
    // declaration.
    bool tryDecl(Stmt& s, int b, int e) {
        int i = b;
        bool isStatic = false, isConst = false;
        auto isStorage = [&](const std::string& x) {
            return x == "static" || x == "const" || x == "volatile" || x == "register" ||
                   x == "extern" || x == "mutable" || x == "typename";
        };
        while (i < e && isStorage(t(i))) {
            if (t(i) == "static") isStatic = true;
            if (t(i) == "const") isConst = true;
            i++;
        }
        if (t(i) == "struct" || t(i) == "class" || t(i) == "union" || t(i) == "enum") i++;
        if (punct(i, "::")) i++;
        if (i >= e) return false;
        if (isBuiltinType(t(i))) {
            while (i < e && isBuiltinType(t(i))) i++;
        } else if (T[i].kind == TokKind::Ident && (t(i) == "auto" || !isKeyword(t(i))) &&
                   !f.locals.count(t(i))) {
            i++;
            while (i < e) {
                if (punct(i, "<")) {
                    int j = matchAngle(i, e);
                    if (j < 0) return false;
                    i = j + 1;
                    continue;
                }
                if (punct(i, "::") && i + 1 < e && T[i + 1].kind == TokKind::Ident) {
                    i += 2;
                    continue;
                }
                break;
            }
        } else {
            return false;
        }
        while (i < e && (t(i) == "const" || t(i) == "volatile")) {
            if (t(i) == "const") isConst = true;
            i++;
        }
        int typeEnd = i;
        std::vector<Declarator> decls;
        while (true) {
            Declarator d;
            d.begin = i;
            while (i < e && (punct(i, "*") || punct(i, "&") || t(i) == "const" ||
                             t(i) == "volatile" || t(i) == "__stdcall" || t(i) == "__cdecl" ||
                             t(i) == "__fastcall" || t(i) == "__thiscall")) {
                if (punct(i, "&")) d.isRef = true;
                i++;
            }
            if (punct(i, "(") && (punct(i + 1, "*") || punct(i + 1, "&"))) {
                // function pointer: (*name)(params)
                int close = matchBracket(T, i);
                if (close < 0 || close >= e) return false;
                int j = i + 1;
                while (j < close && (punct(j, "*") || punct(j, "&") || t(j) == "const")) j++;
                if (j >= close || T[j].kind != TokKind::Ident) return false;
                d.nameTok = j;
                i = close + 1;
                if (!punct(i, "(")) return false;
                int pc = matchBracket(T, i);
                if (pc < 0 || pc >= e) return false;
                i = pc + 1;
            } else if (i < e && T[i].kind == TokKind::Ident && !isKeyword(t(i))) {
                d.nameTok = i++;
            } else {
                return false;
            }
            while (punct(i, "[")) {
                int close = matchBracket(T, i);
                if (close < 0 || close >= e) return false;
                d.isArray = true;
                i = close + 1;
            }
            d.declEnd = i;
            if (punct(i, "=")) {
                i++;
                d.initBegin = i;
                int depth = 0;
                while (i < e) {
                    if (T[i].kind == TokKind::Punct) {
                        const std::string& x = T[i].text;
                        if (x == "(" || x == "[" || x == "{") depth++;
                        else if (x == ")" || x == "]" || x == "}") depth--;
                        else if (x == "," && depth == 0) break;
                    }
                    i++;
                }
                d.initEnd = i;
                d.init = Declarator::Assign;
                if (d.initBegin == d.initEnd) return false;
            } else if (punct(i, "(") || punct(i, "{")) {
                int close = matchBracket(T, i);
                if (close < 0 || close >= e) return false;
                d.init = punct(i, "(") ? Declarator::Paren : Declarator::Brace;
                d.initBegin = i + 1;
                d.initEnd = close;
                i = close + 1;
            }
            d.end = i;
            decls.push_back(d);
            if (i == e) break;
            if (!punct(i, ",")) return false;
            i++;
        }
        s.kind = SK::Decl;
        s.typeBegin = b;
        s.typeEnd = typeEnd;
        s.decls = std::move(decls);
        s.isStatic = isStatic;
        s.isConst = isConst;
        for (auto& d : s.decls) f.locals.insert(t(d.nameTok));
        return true;
    }

    std::unique_ptr<Stmt> parseCond(SK k, int i, int limit) {
        // keyword ( cond ) stmt
        if (!punct(i + 1, "(")) return toSemi(SK::Other, i, limit);
        int close = matchBracket(T, i + 1);
        if (close < 0 || close >= limit) return make(SK::Other, i, findRecover(i, limit));
        auto s = make(k, i, close + 1);
        s->condOpen = i + 1;
        s->condClose = close;
        if (close + 1 >= limit) return s;
        auto body = parseStmt(close + 1, limit);
        s->end = body->end;
        adopt(*s, std::move(body));
        return s;
    }

    std::unique_ptr<Stmt> parseStmt(int i, int limit) {
        const Token& tk = T[i];
        const std::string& x = tk.text;
        if (tk.kind == TokKind::PP) return make(SK::PP, i, i + 1);
        if (tk.kind == TokKind::Punct) {
            if (x == "{") return parseBlock(i);
            if (x == ";") return make(SK::Empty, i, i + 1);
            return toSemi(SK::Expr, i, limit);
        }
        if (tk.kind != TokKind::Ident) return toSemi(SK::Expr, i, limit);

        if (x == "if") {
            auto s = parseCond(SK::If, i, limit);
            if (s->kind == SK::If && !s->kids.empty() && s->end < limit && t(s->end) == "else" &&
                s->end + 1 < limit) {
                s->elseTok = s->end;
                auto e = parseStmt(s->end + 1, limit);
                s->end = e->end;
                adopt(*s, std::move(e));
            }
            return s;
        }
        if (x == "while") return parseCond(SK::While, i, limit);
        if (x == "switch") return parseCond(SK::Switch, i, limit);
        if (x == "for") {
            auto s = parseCond(SK::For, i, limit);
            if (s->kind == SK::For) {
                int semi = findSemi(s->condOpen + 1, s->condClose);
                if (semi > s->condOpen + 1) {
                    auto init = make(SK::Expr, s->condOpen + 1, semi + 1);
                    if (tryDecl(*init, s->condOpen + 1, semi)) s->forInit = std::move(init);
                }
            }
            return s;
        }
        if (x == "do") {
            auto s = make(SK::Do, i, i + 1);
            auto body = parseStmt(i + 1, limit);
            int w = body->end;
            adopt(*s, std::move(body));
            if (t(w) == "while" && punct(w + 1, "(")) {
                int close = matchBracket(T, w + 1);
                if (close > 0 && close < limit) {
                    s->condOpen = w + 1;
                    s->condClose = close;
                    s->end = punct(close + 1, ";") ? close + 2 : close + 1;
                    return s;
                }
            }
            s->kind = SK::Other;
            s->end = w;
            s->kids.clear();
            return s;
        }
        if (x == "case") {
            int depth = 0;
            for (int j = i + 1; j < limit; ++j) {
                if (punct(j, "(") || punct(j, "[")) depth++;
                else if (punct(j, ")") || punct(j, "]")) depth--;
                else if (punct(j, ":") && depth == 0) return make(SK::Case, i, j + 1);
            }
            return make(SK::Other, i, i + 1);
        }
        if (x == "default" && punct(i + 1, ":")) return make(SK::Default, i, i + 2);
        if (x == "return" || x == "throw") return toSemi(SK::Return, i, limit);
        if (x == "break") return toSemi(SK::Break, i, limit);
        if (x == "continue") return toSemi(SK::Continue, i, limit);
        if (x == "goto") return toSemi(SK::Goto, i, limit);
        if (x == "__asm" || x == "_asm" || x == "asm") {
            int e;
            if (punct(i + 1, "{")) {
                e = matchBracket(T, i + 1);
                e = e < 0 ? limit : e + 1;
            } else {
                e = i + 1;
                int line = tk.line;
                while (e < limit && T[e].line == line && !punct(e, "}") && !punct(e, ";")) e++;
            }
            if (e < limit && punct(e, ";")) e++;
            return make(SK::Asm, i, e);
        }
        if (x == "try" || x == "__try") {
            auto s = make(SK::Try, i, i + 1);
            int j = i + 1;
            if (!punct(j, "{")) return toSemi(SK::Other, i, limit);
            auto b = parseBlock(j);
            j = b->end;
            adopt(*s, std::move(b));
            while (j < limit && (t(j) == "catch" || t(j) == "__except" || t(j) == "__finally")) {
                int k = j + 1;
                if (punct(k, "(")) {
                    k = matchBracket(T, k);
                    if (k < 0) break;
                    k++;
                }
                if (!punct(k, "{")) break;
                auto h = parseBlock(k);
                j = h->end;
                adopt(*s, std::move(h));
            }
            s->end = j;
            return s;
        }
        if (x == "typedef" || x == "using" || x == "template" || x == "namespace")
            return toSemi(SK::Other, i, limit);
        if ((x == "struct" || x == "class" || x == "union" || x == "enum") &&
            (punct(i + 1, "{") || punct(i + 2, "{") || punct(i + 2, ":")))
            return toSemi(SK::Other, i, limit);
        if (!isKeyword(x) && punct(i + 1, ":")) return make(SK::Label, i, i + 2);

        int semi = findSemi(i, limit);
        if (semi < 0) return make(SK::Other, i, findRecover(i, limit));
        auto s = make(SK::Expr, i, semi + 1);
        tryDecl(*s, i, semi);
        return s;
    }
};

void setParents(Stmt& s) {
    for (auto& k : s.kids) {
        k->parent = &s;
        setParents(*k);
    }
}

} // namespace

std::unique_ptr<Func> parseFunc(const std::string& text, std::string& err) {
    auto f = std::make_unique<Func>();
    f->text = text;
    f->toks = lex(text);
    auto& T = f->toks;
    int n = (int)T.size();
    for (int i = 0; i < n; ++i)
        if (T[i].text == "(" && T[i].kind == TokKind::Punct) {
            f->paramOpen = i;
            break;
        }
    if (f->paramOpen < 0) {
        err = "no parameter list";
        return nullptr;
    }
    f->paramClose = matchBracket(T, f->paramOpen);
    if (f->paramClose < 0) {
        err = "unbalanced parameter list";
        return nullptr;
    }
    for (int i = f->paramClose + 1; i < n; ++i) {
        if (T[i].text == "(") {
            i = matchBracket(T, i);
            if (i < 0) break;
            continue;
        }
        if (T[i].text == "{") {
            f->bodyOpen = i;
            break;
        }
    }
    if (f->bodyOpen < 0 || (f->bodyClose = matchBracket(T, f->bodyOpen)) < 0) {
        err = "no function body";
        return nullptr;
    }

    // parameter names: the last plain identifier of each parameter
    int segStart = f->paramOpen + 1, depth = 0;
    for (int i = f->paramOpen + 1; i <= f->paramClose; ++i) {
        const std::string& x = T[i].text;
        if (x == "(" || x == "[") depth++;
        if ((x == ")" || x == "]") && i != f->paramClose) depth--;
        if ((x == "," && depth == 0) || i == f->paramClose) {
            std::string name;
            int d2 = 0;
            for (int j = segStart; j < i; ++j) {
                if (T[j].text == "(" || T[j].text == "[") d2++;
                if (T[j].text == ")" || T[j].text == "]") d2--;
                if (T[j].text == "=" && d2 == 0) break;
                if (T[j].kind == TokKind::Ident && !isKeyword(T[j].text)) name = T[j].text;
            }
            // a lone type ("Foo") has no name
            int idents = 0;
            for (int j = segStart; j < i; ++j)
                if (T[j].kind == TokKind::Ident && T[j].text != "const" && T[j].text != "struct")
                    idents++;
            if (!name.empty() && idents >= 2) {
                f->params.push_back(name);
                f->locals.insert(name);
            }
            segStart = i + 1;
        }
    }

    Parser p(*f);
    f->body = p.parseBlock(f->bodyOpen);
    setParents(*f->body);
    return f;
}

std::string dumpStmt(const Func& f, const Stmt& s, int depth) {
    std::ostringstream o;
    std::string head = f.tokText(s.begin, std::min(s.end, s.begin + 8));
    for (char& c : head)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    o << std::string(depth * 2, ' ') << skName(s.kind) << " L" << f.toks[s.begin].line << ": "
      << head;
    if (s.kind == SK::Decl) {
        o << "   [";
        for (auto& d : s.decls) o << " " << f.t(d.nameTok);
        o << " ]";
    }
    o << "\n";
    if (s.forInit) o << dumpStmt(f, *s.forInit, depth + 1);
    for (auto& k : s.kids) o << dumpStmt(f, *k, depth + 1);
    return o.str();
}

} // namespace perm
