// Passes for rewrites that kept coming up when matching gta2_re functions by
// hand: caching a member in a local (by value, pointer or reference), binding
// a local as a const reference, the forms of '++', a condition in a local and
// power of two multiplies as shifts.

#include "pass_util.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace perm {

namespace {
std::map<std::string, std::string> gFieldTypes;

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
} // namespace

std::map<std::string, std::string> findFieldTypes(const std::string& text) {
    // One "Type name;" declaration per line; names declared with two different
    // types (fields of different classes, locals of inline functions) are
    // marked ambiguous with an empty type.
    static const std::set<std::string> notTypes = {
        "return", "typedef", "delete", "goto", "break", "continue", "using", "friend", "case",
        "default", "public", "private", "protected", "else", "throw", "struct", "class", "enum",
        "union", "namespace", "template", "operator", "do", "virtual", "extern", "EXTERN_GLOBAL",
        "GLOBAL", "DEFINE_GLOBAL"};
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        size_t c = line.find("//");
        if (c != std::string::npos) line.resize(c);
        line = trim(line);
        if (line.empty() || line.back() != ';' || line[0] == '#') continue;
        if (line.find_first_of("(){}=,<>\"'") != std::string::npos) continue;
        line = trim(line.substr(0, line.size() - 1));
        if (!line.empty() && line.back() == ']') continue; // arrays
        size_t e = line.size(), b = e;
        while (b > 0 && identChar(line[b - 1])) b--;
        if (b == e || std::isdigit((unsigned char)line[b])) continue;
        std::string name = line.substr(b), type = trim(line.substr(0, b));
        if (type.empty() || type.find(':') == 0) continue;
        for (const char* q : {"static ", "mutable ", "const ", "volatile "})
            while (type.compare(0, std::string(q).size(), q) == 0) type = trim(type.substr(std::string(q).size()));
        size_t sp = type.find(' ');
        std::string first = sp == std::string::npos ? type : type.substr(0, sp);
        if (type.empty() || notTypes.count(first) || isKeyword(name)) continue;
        // "Ped *p" and "Ped* p" alike
        std::string t;
        for (char ch : type)
            if (ch == '*') {
                while (!t.empty() && t.back() == ' ') t.pop_back();
                t += '*';
            } else if (ch != ' ' || (!t.empty() && t.back() != ' ' && t.back() != '*')) {
                t += ch;
            }
        auto it = out.find(name);
        if (it == out.end()) out[name] = t;
        else if (it->second != t) it->second.clear();
    }
    return out;
}

void setFieldTypes(std::map<std::string, std::string> t) {
    gFieldTypes.clear();
    for (auto& [n, ty] : t)
        if (!ty.empty()) gFieldTypes[n] = ty;
}
const std::map<std::string, std::string>& fieldTypes() { return gFieldTypes; }

namespace util {

namespace {

std::string freshName(const Func& f, const std::string& base) {
    std::string n = base;
    int i = 0;
    while (f.locals.count(n)) n = base + std::to_string(++i);
    return n;
}

bool isPointerType(const std::string& t) { return !t.empty() && t.back() == '*'; }

bool isScalarType(const std::string& t) {
    static const std::set<std::string> s = {
        "s8", "u8", "s16", "u16", "s32", "u32", "char", "short", "int", "long", "bool", "float",
        "double", "unsigned char", "unsigned short", "unsigned int", "unsigned long", "unsigned",
        "signed char", "BYTE", "WORD", "DWORD", "INT", "UINT", "BOOL", "f32", "Ang16"};
    return s.count(t) > 0;
}

// The block statement holding every statement in v (directly or nested), and
// its child that holds v[0]: the declaration goes before that child.
const Stmt* insertionPoint(const std::vector<const Stmt*>& v) {
    auto chain = [](const Stmt* s) {
        std::vector<const Stmt*> c;
        for (; s; s = s->parent) c.push_back(s);
        std::reverse(c.begin(), c.end());
        return c;
    };
    std::vector<const Stmt*> common = chain(v[0]);
    for (size_t i = 1; i < v.size(); ++i) {
        auto c = chain(v[i]);
        size_t n = 0;
        while (n < common.size() && n < c.size() && common[n] == c[n]) n++;
        common.resize(n);
    }
    // deepest common block, then the next statement down towards v[0]
    auto first = chain(v[0]);
    for (size_t n = common.size(); n-- > 0;) {
        if (common[n]->kind != SK::Block) continue;
        if (n + 1 < first.size()) return first[n + 1];
        return nullptr;
    }
    return nullptr;
}

// --- cache_member -----------------------------------------------------------------

struct MemberUse {
    int b, e;
    const Stmt* stmt;
};

bool hasCall(const Func& f, int b, int e) {
    for (int i = b; i < e; ++i)
        if (f.t(i) == "(" && i > b && f.toks[i - 1].kind == TokKind::Ident) return true;
    return false;
}

bool writtenAt(const Func& f, int b, int e) {
    if (e < (int)f.toks.size() && (isAssignOp(f.t(e)) || f.t(e) == "++" || f.t(e) == "--")) return true;
    if (b > 0 && (f.t(b - 1) == "++" || f.t(b - 1) == "--")) return true;
    if (b > 0 && f.t(b - 1) == "&") {
        // unary & (address taken), not a binary and
        if (b < 2) return true;
        const Token& p = f.toks[b - 2];
        bool operandEnd = p.kind == TokKind::Ident || p.kind == TokKind::Number || p.text == ")" ||
                          p.text == "]";
        if (!operandEnd || isKeyword(p.text)) return true;
    }
    return false;
}

void enumCacheMember(const Func& f, const EmitFn& emit) {
    const auto& types = fieldTypes();
    std::map<std::string, std::vector<MemberUse>> uses;
    std::vector<std::string> order;
    std::set<std::string> written;
    for (auto& site : exprSites(f)) {
        if (!inBlock(*site.stmt) && site.stmt->kind != SK::If && site.stmt->kind != SK::Return)
            continue;
        auto root = parseExpr(f, site.b, site.e);
        if (!root) continue;
        forEachExpr(*root, [&](Expr& x) {
            if (x.k != Expr::Member || x.e - x.b < 3) return;
            const std::string& dot = f.t(x.e - 2);
            if (dot != "->" && dot != ".") return;
            if (hasCall(f, x.b, x.e)) return;
            std::string key = norm(f.tokText(x.b, x.e));
            if (!uses.count(key)) order.push_back(key);
            if (writtenAt(f, x.b, x.e)) written.insert(key);
            uses[key].push_back({x.b, x.e, site.stmt});
        });
    }
    // a member inside a longer cached path is a use of that path too
    for (const std::string& key : order) {
        if (written.count(key)) continue;
        auto& v = uses[key];
        std::sort(v.begin(), v.end(), [](const MemberUse& a, const MemberUse& b) { return a.b < b.b; });
        v.erase(std::unique(v.begin(), v.end(), [](const MemberUse& a, const MemberUse& b) { return a.b == b.b; }),
                v.end());
        const MemberUse& u0 = v[0];
        auto it = types.find(f.t(u0.e - 1));
        if (it == types.end()) continue;
        const std::string& type = it->second;
        // the path's base must not be written anywhere (a local reassigned in a loop)
        bool baseWritten = false;
        for (int i = u0.b; i < u0.e - 2 && !baseWritten; ++i)
            if (f.toks[i].kind == TokKind::Ident && f.locals.count(f.t(i)))
                for (int w : usesOf(f, f.t(i), f.bodyOpen, f.bodyClose))
                    if (isWriteUse(f, w)) baseWritten = true;
        if (baseWritten) continue;
        std::vector<const Stmt*> stmts;
        for (auto& u : v) stmts.push_back(u.stmt);
        const Stmt* at = insertionPoint(stmts);
        if (!at) continue;
        std::string path = f.tokText(u0.b, u0.e);
        std::string name = freshName(f, "cached");
        bool ptrOK = !isPointerType(type) && !isScalarType(type);
        // 0: 'T name = path;'   1: 'T& name = path;'   2: 'T* name = &path;' (structs)
        for (int form = 0; form < 3; ++form) {
            if (form == 2 && !ptrOK) continue;
            if (form == 1 && isPointerType(type)) continue;
            for (int all = 1; all >= 0; --all) {
                if (!all && v.size() < 2) continue;
                std::vector<MemberUse> sel(v.begin(), all ? v.end() : v.begin() + 1);
                std::string decl = form == 0   ? type + " " + name + " = " + path + ";"
                                   : form == 1 ? type + "& " + name + " = " + path + ";"
                                               : type + "* " + name + " = &" + path + ";";
                int atTok = at->begin;
                if (!emit([&f, sel, decl, atTok, name, form]() {
                        Rewriter rw(f);
                        rw.insertBefore(atTok, decl);
                        for (auto& u : sel) {
                            if (form == 2 && u.e < (int)f.toks.size() && f.t(u.e) == ".")
                                rw.replace(u.b, u.e + 1, name + "->");
                            else
                                rw.replace(u.b, u.e, form == 2 ? "(*" + name + ")" : name);
                        }
                        return rw.apply();
                    }))
                    return;
            }
        }
    }
}

// --- ref_local --------------------------------------------------------------------

void enumRefLocal(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::Decl)) {
        if (s->isStatic || s->decls.size() != 1) continue;
        const Declarator& d = s->decls[0];
        if (d.isArray || d.init != Declarator::Assign) continue;
        int amp = -1;
        for (int i = d.begin; i < d.nameTok; ++i)
            if (f.t(i) == "&") amp = i;
        bool isConst = false;
        for (int i = s->typeBegin; i < s->typeEnd; ++i) isConst |= f.t(i) == "const";
        int tb = s->typeBegin, nt = d.nameTok;
        if (amp >= 0) {
            // 'const T& x = e' -> 'T x = e' (and without the const)
            int a = amp;
            if (!emit([&f, a, tb, isConst]() {
                    Rewriter rw(f);
                    rw.replace(a, a + 1, "");
                    if (isConst && f.t(tb) == "const") rw.replace(tb, tb + 1, "");
                    return rw.apply();
                }))
                return;
            if (!isConst && !emit([&f, a, tb]() {
                    Rewriter rw(f);
                    rw.replace(tb, tb, "const ");
                    return rw.apply();
                }))
                return;
            continue;
        }
        bool ptr = false;
        for (int i = d.begin; i < d.nameTok; ++i) ptr |= f.t(i) == "*";
        if (ptr) continue;
        if (!emit([&f, nt, tb, isConst]() {
                Rewriter rw(f);
                rw.replace(nt, nt + 1, "& " + f.t(nt));
                if (!isConst) rw.replace(tb, tb + 1, "const " + f.t(tb));
                return rw.apply();
            }))
            return;
        if (!isConst && !emit([&f, nt]() {
                Rewriter rw(f);
                rw.replace(nt, nt + 1, "& " + f.t(nt));
                return rw.apply();
            }))
            return;
    }
}

// --- incdec -----------------------------------------------------------------------

void enumIncDec(const Func& f, const EmitFn& emit) {
    for (auto& site : exprSites(f)) {
        const Stmt& s = *site.stmt;
        if (s.kind != SK::Expr && s.kind != SK::For) continue;
        auto root = parseExpr(f, site.b, site.e);
        if (!root) continue;
        const Expr* target = nullptr;
        bool inc = true;
        int form = -1; // 0 x++, 1 ++x, 2 x += 1, 3 x = x + 1
        if (root->k == Expr::Postfix) {
            target = root->kids[0].get(), inc = f.t(root->op) == "++", form = 0;
        } else if (root->k == Expr::Unary && (f.t(root->op) == "++" || f.t(root->op) == "--")) {
            target = root->kids[0].get(), inc = f.t(root->op) == "++", form = 1;
        } else if (root->k == Expr::Assign && (f.t(root->op) == "+=" || f.t(root->op) == "-=") &&
                   norm(exprText(f, *root->kids[1])) == "1") {
            target = root->kids[0].get(), inc = f.t(root->op) == "+=", form = 2;
        } else if (root->k == Expr::Assign && f.t(root->op) == "=" && root->kids[1]->k == Expr::Binary) {
            const Expr& r = *root->kids[1];
            const std::string& op = f.t(r.op);
            if ((op == "+" || op == "-") && norm(exprText(f, *r.kids[1])) == "1" &&
                norm(exprText(f, *r.kids[0])) == norm(exprText(f, *root->kids[0])))
                target = root->kids[0].get(), inc = op == "+", form = 3;
        }
        if (!target) continue;
        std::string t = exprText(f, *target);
        std::string pp = inc ? "++" : "--";
        std::string forms[4] = {t + pp, pp + t, t + (inc ? " += 1" : " -= 1"),
                                t + " = " + t + (inc ? " + 1" : " - 1")};
        for (int i = 0; i < 4; ++i)
            if (i != form && !emitReplace(f, emit, site.b, site.e, forms[i])) return;
    }
}

// --- cond_temp ----------------------------------------------------------------------

void enumCondTemp(const Func& f, const EmitFn& emit) {
    std::string name = freshName(f, "cond");
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (!inBlock(*s) || s->condOpen < 0) continue;
        int b = s->condOpen + 1, e = s->condClose;
        if (e - b == 1 && f.isLocal(b)) continue;
        std::string c = f.tokText(b, e);
        int at = s->begin;
        // an 'else if' has no block to put the declaration in
        if (at > 0 && f.t(at - 1) == "else") continue;
        for (const char* type : {"s32", "u8", "bool"}) {
            std::string decl = std::string(type) + " " + name + " = " + c + ";";
            if (!emit([&f, at, b, e, decl, name]() {
                    Rewriter rw(f);
                    rw.insertBefore(at, decl);
                    rw.replace(b, e, name);
                    return rw.apply();
                }))
                return;
        }
    }
}

// --- pow2_shift ---------------------------------------------------------------------

int log2Exact(const std::string& lit) {
    long long v;
    try {
        size_t used = 0;
        bool hex = lit.size() > 2 && lit[0] == '0' && (lit[1] == 'x' || lit[1] == 'X');
        v = std::stoll(lit, &used, hex ? 16 : 10);
        while (used < lit.size() && std::string("uUlL").find(lit[used]) != std::string::npos) used++;
        if (used != lit.size()) return -1;
    } catch (...) {
        return -1;
    }
    if (v < 2 || (v & (v - 1))) return -1;
    int k = 0;
    while ((1LL << k) != v) k++;
    return k;
}

void enumPow2Shift(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, const std::shared_ptr<Expr>&, Expr& x) {
        if (x.k != Expr::Binary && !(x.k == Expr::Assign && f.t(x.op).size() == 2)) return true;
        const std::string& op = f.t(x.op);
        const Expr& r = *x.kids[1];
        if (r.k != Expr::Primary || r.e != r.b + 1 || f.toks[r.b].kind != TokKind::Number) return true;
        const std::string& lit = f.t(r.b);
        std::string lhs = x.k == Expr::Assign ? exprText(f, *x.kids[0])
                                              : operandText(f, *x.kids[0], binPrec("<<"));
        std::string text;
        if (op == "*" || op == "/" || op == "*=" || op == "/=") {
            int k = log2Exact(lit);
            if (k < 0) return true;
            bool mul = op[0] == '*';
            text = x.k == Expr::Assign ? lhs + (mul ? " <<= " : " >>= ") + std::to_string(k)
                                       : paren(lhs + (mul ? " << " : " >> ") + std::to_string(k));
        } else if (op == "<<" || op == "<<=") {
            int k = std::atoi(lit.c_str());
            if (k < 1 || k > 30 || lit.find_first_not_of("0123456789") != std::string::npos) return true;
            std::string m = std::to_string(1LL << k);
            text = x.k == Expr::Assign ? lhs + " *= " + m : paren(operandText(f, *x.kids[0], binPrec("*")) + " * " + m);
        } else {
            return true;
        }
        return emitReplace(f, emit, x.b, x.e, text);
    });
}

} // namespace

void registerManualPasses(std::vector<Pass>& passes) {
    passes.push_back({"cache_member",
                      "Read a member path used in the function into a local first: 'T c = p->f;', "
                      "'T& c = p->f;' or, for a struct, 'T* c = &p->f;' (types from the member "
                      "declarations in the included headers), for all uses or the first",
                      8, enumCacheMember, nullptr});
    passes.push_back({"ref_local",
                      "'T x = e;' <-> 'const T& x = e;' <-> 'T& x = e;' (a named local gets its "
                      "own stack slot, a temporary bound to a reference may share one)",
                      6, enumRefLocal, nullptr});
    passes.push_back({"incdec", "'x++' <-> '++x' <-> 'x += 1' <-> 'x = x + 1' (and '--')", 5, enumIncDec,
                      nullptr});
    passes.push_back({"cond_temp", "Compute an if condition into a local first: 's32 cond = c; if (cond)'",
                      5, enumCondTemp, nullptr});
    passes.push_back({"pow2_shift",
                      "'x * 4' <-> 'x << 2', 'x / 4' -> 'x >> 2' (also '*=' / '<<='): read the "
                      "diff, '/' and '>>' differ for negative values",
                      4, enumPow2Shift, nullptr});
}

} // namespace util

} // namespace perm
