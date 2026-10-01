#include "analysis.hpp"

namespace perm {

int binPrec(const std::string& op) {
    if (op == ",") return 1;
    if (op == "||") return 4;
    if (op == "&&") return 5;
    if (op == "|") return 6;
    if (op == "^") return 7;
    if (op == "&") return 8;
    if (op == "==" || op == "!=") return 9;
    if (op == "<" || op == ">" || op == "<=" || op == ">=") return 10;
    if (op == "<<" || op == ">>") return 11;
    if (op == "+" || op == "-") return 12;
    if (op == "*" || op == "/" || op == "%") return 13;
    if (op == ".*" || op == "->*") return 14;
    return 0;
}

void forEachExpr(Expr& x, const std::function<void(Expr&)>& fn) {
    fn(x);
    for (auto& k : x.kids) forEachExpr(*k, fn);
}

namespace {

// True if token i can end an operand, so a following '*', '&', '-' is binary.
bool operandEnd(const Func& f, int i) {
    if (i < 0) return false;
    const Token& t = f.toks[i];
    switch (t.kind) {
    case TokKind::Number:
    case TokKind::String:
    case TokKind::Char: return true;
    case TokKind::Ident:
        return !isKeyword(t.text) || t.text == "this" || t.text == "true" || t.text == "false" ||
               t.text == "nullptr";
    case TokKind::Punct: return t.text == ")" || t.text == "]" || t.text == "++" || t.text == "--";
    default: return false;
    }
}

struct ExprParser {
    const Func& f;
    const std::vector<Token>& T;
    int pos, end;
    bool fail = false;

    ExprParser(const Func& fn, int b, int e) : f(fn), T(fn.toks), pos(b), end(e) {}

    bool at(const char* s) const {
        return pos < end && T[pos].kind == TokKind::Punct && T[pos].text == s;
    }

    std::unique_ptr<Expr> node(Expr::K k, int b) {
        auto x = std::make_unique<Expr>();
        x->k = k;
        x->b = b;
        return x;
    }

    bool isTypeTok(int i) const {
        const Token& t = T[i];
        if (t.kind == TokKind::Ident) {
            if (isBuiltinType(t.text)) return true;
            if (t.text == "const" || t.text == "volatile" || t.text == "struct" ||
                t.text == "class" || t.text == "union" || t.text == "enum" ||
                t.text == "unsigned" || t.text == "signed")
                return true;
            return !isKeyword(t.text) && !f.locals.count(t.text);
        }
        if (t.kind == TokKind::Punct)
            return t.text == "::" || t.text == "*" || t.text == "&" || t.text == "<" ||
                   t.text == ">" || t.text == ",";
        return false;
    }

    // '(' at pos starts a C-style cast?
    bool isCast() const {
        int close = matchBracket(T, pos);
        if (close < 0 || close >= end || close == pos + 1) return false;
        int idents = 0;
        bool ptr = false, builtin = false;
        for (int i = pos + 1; i < close; ++i) {
            if (!isTypeTok(i)) return false;
            if (T[i].kind == TokKind::Ident) {
                idents++;
                if (isBuiltinType(T[i].text)) builtin = true;
            } else if (T[i].text == "*" || T[i].text == "&") {
                ptr = true;
            }
        }
        if (idents == 0) return false;
        if (close + 1 >= end) return false;
        const Token& n = T[close + 1];
        if (n.kind == TokKind::Punct) {
            const std::string& s = n.text;
            bool ambiguous = s == "-" || s == "+" || s == "*" || s == "&";
            if (ambiguous && !ptr && !builtin && idents == 1) return false;
            return s == "(" || s == "!" || s == "~" || s == "-" || s == "+" || s == "*" ||
                   s == "&" || s == "++" || s == "--" || s == "::";
        }
        if (n.kind == TokKind::Ident) return !isKeyword(n.text) || n.text == "this" ||
                                              n.text == "sizeof" || n.text == "true" ||
                                              n.text == "false" || isBuiltinType(n.text);
        return n.kind != TokKind::PP && n.kind != TokKind::End;
    }

    std::unique_ptr<Expr> parse(int minPrec) {
        auto lhs = unary();
        if (!lhs) return nullptr;
        while (pos < end && !fail) {
            const Token& t = T[pos];
            if (t.kind != TokKind::Punct) break;
            const std::string& op = t.text;
            int b = lhs->b;
            if (isAssignOp(op)) {
                if (minPrec > 2) break;
                int o = pos++;
                auto rhs = parse(2);
                if (!rhs) return nullptr;
                auto x = node(Expr::Assign, b);
                x->op = o;
                x->e = rhs->e;
                x->kids.push_back(std::move(lhs));
                x->kids.push_back(std::move(rhs));
                lhs = std::move(x);
            } else if (op == "?") {
                if (minPrec > 3) break;
                int q = pos++;
                auto mid = parse(1);
                if (!mid || !at(":")) return fail = true, nullptr;
                int c = pos++;
                auto rhs = parse(2);
                if (!rhs) return nullptr;
                auto x = node(Expr::Ternary, b);
                x->op = q;
                x->colon = c;
                x->e = rhs->e;
                x->kids.push_back(std::move(lhs));
                x->kids.push_back(std::move(mid));
                x->kids.push_back(std::move(rhs));
                lhs = std::move(x);
            } else {
                int p = binPrec(op);
                if (p == 0 || p < minPrec) break;
                int o = pos++;
                auto rhs = parse(p + 1);
                if (!rhs) return nullptr;
                auto x = node(op == "," ? Expr::Comma : Expr::Binary, b);
                x->op = o;
                x->e = rhs->e;
                x->kids.push_back(std::move(lhs));
                x->kids.push_back(std::move(rhs));
                lhs = std::move(x);
            }
        }
        return fail ? nullptr : std::move(lhs);
    }

    std::unique_ptr<Expr> unary() {
        if (pos >= end) return fail = true, nullptr;
        const Token& t = T[pos];
        int b = pos;
        if (t.kind == TokKind::Punct) {
            const std::string& s = t.text;
            if (s == "+" || s == "-" || s == "!" || s == "~" || s == "*" || s == "&" ||
                s == "++" || s == "--") {
                pos++;
                auto o = unary();
                if (!o) return nullptr;
                auto x = node(Expr::Unary, b);
                x->op = b;
                x->e = o->e;
                x->kids.push_back(std::move(o));
                return x;
            }
            if (s == "(" && isCast()) {
                int close = matchBracket(T, pos);
                pos = close + 1;
                auto o = unary();
                if (!o) return nullptr;
                auto x = node(Expr::Cast, b);
                x->e = o->e;
                x->kids.push_back(std::move(o));
                return x;
            }
        } else if (t.kind == TokKind::Ident) {
            if (t.text == "sizeof") {
                pos++;
                if (at("(")) {
                    // sizeof(type) or sizeof(expr): treat the group as opaque
                    int close = matchBracket(T, pos);
                    if (close < 0 || close >= end) return fail = true, nullptr;
                    pos = close + 1;
                    auto x = node(Expr::Primary, b);
                    x->e = pos;
                    return postfix(std::move(x));
                }
                auto o = unary();
                if (!o) return nullptr;
                auto x = node(Expr::Unary, b);
                x->op = b;
                x->e = o->e;
                x->kids.push_back(std::move(o));
                return x;
            }
            if (t.text == "new" || t.text == "delete" || t.text == "throw")
                return fail = true, nullptr;
        }
        return postfix(primary());
    }

    std::unique_ptr<Expr> primary() {
        if (pos >= end) return fail = true, nullptr;
        const Token& t = T[pos];
        int b = pos;
        if (t.kind == TokKind::Number || t.kind == TokKind::Char) {
            pos++;
            auto x = node(Expr::Primary, b);
            x->e = pos;
            return x;
        }
        if (t.kind == TokKind::String) {
            while (pos < end && T[pos].kind == TokKind::String) pos++;
            auto x = node(Expr::Primary, b);
            x->e = pos;
            return x;
        }
        if (t.kind == TokKind::Ident || (t.kind == TokKind::Punct && t.text == "::")) {
            const std::string& s = t.text;
            if (s == "static_cast" || s == "reinterpret_cast" || s == "const_cast" ||
                s == "dynamic_cast") {
                pos++;
                if (!at("<")) return fail = true, nullptr;
                int depth = 0;
                while (pos < end) {
                    if (T[pos].text == "<") depth++;
                    if (T[pos].text == ">" && --depth == 0) break;
                    pos++;
                }
                pos++;
                if (!at("(")) return fail = true, nullptr;
                int close = matchBracket(T, pos);
                if (close < 0 || close >= end) return fail = true, nullptr;
                ExprParser inner(f, pos + 1, close);
                auto in = inner.parse(1);
                if (!in || inner.pos != close) return fail = true, nullptr;
                pos = close + 1;
                auto x = node(Expr::Cast, b);
                x->e = pos;
                x->kids.push_back(std::move(in));
                return x;
            }
            if (t.kind == TokKind::Ident && isKeyword(s) && s != "this" && s != "true" &&
                s != "false" && s != "nullptr" && !isBuiltinType(s))
                return fail = true, nullptr;
            if (t.kind == TokKind::Punct) pos++;
            if (pos >= end || T[pos].kind != TokKind::Ident) return fail = true, nullptr;
            pos++;
            while (at("::") && pos + 1 < end) {
                pos++;
                if (at("~")) pos++;
                if (pos >= end || T[pos].kind != TokKind::Ident) return fail = true, nullptr;
                pos++;
            }
            auto x = node(Expr::Primary, b);
            x->e = pos;
            return x;
        }
        if (t.kind == TokKind::Punct && t.text == "(") {
            int close = matchBracket(T, pos);
            if (close < 0 || close >= end) return fail = true, nullptr;
            ExprParser inner(f, pos + 1, close);
            auto in = inner.parse(1);
            if (!in || inner.pos != close) return fail = true, nullptr;
            pos = close + 1;
            auto x = node(Expr::Paren, b);
            x->e = pos;
            x->kids.push_back(std::move(in));
            return x;
        }
        return fail = true, nullptr;
    }

    std::unique_ptr<Expr> postfix(std::unique_ptr<Expr> x) {
        if (!x) return nullptr;
        while (pos < end && !fail) {
            int b = x->b;
            if (at("(")) {
                int close = matchBracket(T, pos);
                if (close < 0 || close >= end) return fail = true, nullptr;
                auto c = node(Expr::Call, b);
                c->kids.push_back(std::move(x));
                int p = pos + 1;
                while (p < close) {
                    ExprParser a(f, p, close);
                    auto arg = a.parse(2);
                    if (!arg) return fail = true, nullptr;
                    c->kids.push_back(std::move(arg));
                    p = a.pos;
                    if (p < close) {
                        if (T[p].text != ",") return fail = true, nullptr;
                        p++;
                    }
                }
                pos = close + 1;
                c->e = pos;
                x = std::move(c);
            } else if (at("[")) {
                int close = matchBracket(T, pos);
                if (close < 0 || close >= end) return fail = true, nullptr;
                ExprParser a(f, pos + 1, close);
                auto idx = a.parse(1);
                if (!idx || a.pos != close) return fail = true, nullptr;
                auto c = node(Expr::Index, b);
                c->kids.push_back(std::move(x));
                c->kids.push_back(std::move(idx));
                pos = close + 1;
                c->e = pos;
                x = std::move(c);
            } else if (at(".") || at("->")) {
                pos++;
                if (at("~")) pos++;
                if (pos >= end || T[pos].kind != TokKind::Ident) return fail = true, nullptr;
                pos++;
                while (at("::") && pos + 1 < end && T[pos + 1].kind == TokKind::Ident) pos += 2;
                auto c = node(Expr::Member, b);
                c->kids.push_back(std::move(x));
                c->e = pos;
                x = std::move(c);
            } else if (at("++") || at("--")) {
                auto c = node(Expr::Postfix, b);
                c->op = pos++;
                c->kids.push_back(std::move(x));
                c->e = pos;
                x = std::move(c);
            } else {
                break;
            }
        }
        return fail ? nullptr : std::move(x);
    }
};

} // namespace

std::unique_ptr<Expr> parseExpr(const Func& f, int b, int e) {
    if (b >= e) return nullptr;
    ExprParser p(f, b, e);
    auto x = p.parse(1);
    if (!x || p.pos != e) return nullptr;
    return x;
}

std::vector<ExprSite> exprSites(const Func& f) {
    std::vector<ExprSite> out;
    auto addDecl = [&](const Stmt& s) {
        for (auto& d : s.decls)
            if (d.init == Declarator::Assign && f.t(d.initBegin) != "{")
                out.push_back({d.initBegin, d.initEnd, &s});
    };
    forEachStmt(*f.body, [&](const Stmt& s) {
        switch (s.kind) {
        case SK::Expr:
            if (s.end - 1 > s.begin && f.t(s.end - 1) == ";") out.push_back({s.begin, s.end - 1, &s});
            break;
        case SK::Return:
            if (f.t(s.begin) == "return" && f.t(s.end - 1) == ";" && s.end - 1 > s.begin + 1)
                out.push_back({s.begin + 1, s.end - 1, &s});
            break;
        case SK::Decl: addDecl(s); break;
        case SK::If:
        case SK::While:
        case SK::Switch:
        case SK::Do:
            if (s.condOpen >= 0) out.push_back({s.condOpen + 1, s.condClose, &s});
            break;
        case SK::For: {
            int depth = 0, segStart = s.condOpen + 1, seg = 0;
            for (int i = s.condOpen + 1; i <= s.condClose; ++i) {
                const std::string& x = f.t(i);
                if (i < s.condClose && (x == "(" || x == "[" || x == "{")) depth++;
                else if (i < s.condClose && (x == ")" || x == "]" || x == "}")) depth--;
                if ((x == ";" && depth == 0) || i == s.condClose) {
                    if (seg == 0 && s.forInit) addDecl(*s.forInit);
                    else if (i > segStart) out.push_back({segStart, i, &s});
                    segStart = i + 1;
                    seg++;
                }
            }
            break;
        }
        default: break;
        }
    });
    return out;
}

bool isWriteUse(const Func& f, int i) {
    int n = (int)f.toks.size();
    const std::string& next = i + 1 < n ? f.t(i + 1) : "";
    const std::string& prev = i > 0 ? f.t(i - 1) : "";
    bool prevUnary = i < 2 || !operandEnd(f, i - 2);
    if (isAssignOp(next) && !(prev == "*" && prevUnary)) return true;
    if (next == "++" || next == "--" || prev == "++" || prev == "--") return true;
    if (prev == "&" && prevUnary) return true; // address taken
    return false;
}

Effects effectsOf(const Func& f, int b, int e) {
    Effects r;
    for (int i = b; i < e; ++i) {
        const Token& t = f.toks[i];
        if (t.kind == TokKind::PP) {
            r.barrier = true;
            continue;
        }
        if (t.kind == TokKind::Ident) {
            const std::string& x = t.text;
            if (x == "return" || x == "break" || x == "continue" || x == "goto" || x == "throw" ||
                x == "__asm" || x == "_asm" || x == "asm" || x == "case" || x == "default" ||
                x == "__leave") {
                r.barrier = true;
                continue;
            }
            if (x == "new" || x == "delete") {
                r.call = true;
                continue;
            }
            if (f.isLocal(i)) {
                if (isWriteUse(f, i)) {
                    r.writes.insert(x);
                    if (f.t(i + 1) != "=") r.reads.insert(x);
                } else {
                    r.reads.insert(x);
                }
                continue;
            }
            if (isKeyword(x)) continue;
            if (i + 1 < (int)f.toks.size() && f.t(i + 1) == "(") {
                r.call = true;
                continue;
            }
            r.memRead = true; // member, global or something we can't see
            continue;
        }
        if (t.kind != TokKind::Punct) continue;
        const std::string& x = t.text;
        if (x == "->" || x == "[") r.memRead = true;
        else if (x == "*" && !operandEnd(f, i - 1)) r.memRead = true;
        else if (isAssignOp(x)) {
            bool plainLocal = f.isLocal(i - 1) &&
                              !(i >= 2 && (f.t(i - 2) == "*" || f.t(i - 2) == "&") &&
                                !operandEnd(f, i - 3));
            if (!plainLocal) r.memWrite = true;
        } else if (x == "++" || x == "--") {
            bool local = f.isLocal(i + 1) || (f.isLocal(i - 1));
            if (!local) r.memWrite = true;
        }
    }
    return r;
}

Effects effectsOfStmt(const Func& f, const Stmt& s) {
    Effects r = effectsOf(f, s.begin, s.end);
    switch (s.kind) {
    case SK::Return:
    case SK::Break:
    case SK::Continue:
    case SK::Goto:
    case SK::Label:
    case SK::Case:
    case SK::Default:
    case SK::Asm:
    case SK::PP:
    case SK::Other:
    case SK::Try: r.barrier = true; break;
    case SK::Decl:
        if (s.isStatic) r.barrier = true;
        for (auto& d : s.decls) {
            r.writes.insert(f.t(d.nameTok));
            r.reads.erase(f.t(d.nameTok));
        }
        break;
    default: break;
    }
    return r;
}

bool conflicts(const Effects& a, const Effects& b) {
    if (a.barrier || b.barrier) return true;
    for (auto& w : a.writes)
        if (b.reads.count(w) || b.writes.count(w)) return true;
    for (auto& w : b.writes)
        if (a.reads.count(w)) return true;
    bool aw = a.call || a.memWrite, bw = b.call || b.memWrite;
    if (aw && (bw || b.memRead)) return true;
    if (bw && a.memRead) return true;
    return false;
}

bool conflictsRelaxed(const Effects& a, const Effects& b) {
    if (a.barrier || b.barrier) return true;
    for (auto& w : a.writes)
        if (b.reads.count(w) || b.writes.count(w)) return true;
    for (auto& w : b.writes)
        if (a.reads.count(w)) return true;
    if (a.call && (b.call || b.memWrite || b.memRead)) return true;
    if (b.call && (a.memWrite || a.memRead)) return true;
    return false;
}

std::vector<int> usesOf(const Func& f, const std::string& name, int b, int e) {
    std::vector<int> r;
    for (int i = b; i < e; ++i)
        if (f.toks[i].kind == TokKind::Ident && f.toks[i].text == name && f.isLocal(i)) r.push_back(i);
    return r;
}

} // namespace perm
