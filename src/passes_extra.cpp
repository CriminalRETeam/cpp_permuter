// Passes for rewrites that produced gta2_re matches by hand: a flag set with if/else instead of
// from a comparison, one local reused for two values (or one split in two), and a local's value
// read again from its source at one use.

#include "pass_util.hpp"

#include <algorithm>

namespace perm {

namespace util {

std::string declTypeText(const Func& f, const Stmt& s);
bool initBlocked(const Effects& ie, const Effects& between);
bool simpleSingleDecl(const Stmt& s);
bool insideLoopNotContaining(const Func& f, int tok, const Stmt& s);
std::string ifElseText(const std::string& cond, const std::string& thenBlock,
                       const std::string& elseBlock, const std::string& ind);
bool isTrue(const std::string& s);
bool isFalse(const std::string& s);

namespace {

bool boolish(const Func& f, const Expr& x) {
    if (x.k == Expr::Paren) return boolish(f, *x.kids[0]);
    if (x.k == Expr::Unary) return f.t(x.op) == "!";
    if (x.k != Expr::Binary) return false;
    const std::string& op = f.t(x.op);
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
           op == "&&" || op == "||";
}

// The single "lhs = rhs;" expression of an expression statement, or null.
std::unique_ptr<Expr> plainAssign(const Func& f, const Stmt* s) {
    if (!s || s->kind != SK::Expr || f.t(s->end - 1) != ";") return nullptr;
    auto x = parseExpr(f, s->begin, s->end - 1);
    if (!x || x->k != Expr::Assign || f.t(x->op) != "=") return nullptr;
    return x;
}

bool isLoop(const Stmt& s) { return s.kind == SK::For || s.kind == SK::While || s.kind == SK::Do; }

// Innermost loop holding token tok, or null.
const Stmt* loopOf(const Func& f, int tok) {
    const Stmt* r = nullptr;
    forEachStmt(*f.body, [&](const Stmt& l) {
        if (isLoop(l) && tok >= l.begin && tok < l.end && (!r || l.begin >= r->begin)) r = &l;
    });
    return r;
}

// Whether some loop holds both a and b but not the declaration d: a value stored at one could
// then flow to the other through the back edge.
bool sharedLoop(const Func& f, int a, int b, const Stmt& d) {
    bool r = false;
    forEachStmt(*f.body, [&](const Stmt& l) {
        if (isLoop(l) && a >= l.begin && a < l.end && b >= l.begin && b < l.end &&
            !(d.begin >= l.begin && d.begin < l.end))
            r = true;
    });
    return r;
}

struct LocalDecl {
    const Stmt* s;
    std::string name, type; // type: the declared type with the declarator's * (no spaces)
    std::vector<int> uses;  // after the declaration, to the end of its block
};

std::vector<LocalDecl> localDecls(const Func& f) {
    std::vector<LocalDecl> r;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (!simpleSingleDecl(s)) return;
        const Declarator& d = s.decls[0];
        if (d.isArray || d.isRef) return;
        if (d.init == Declarator::Brace || (d.init != Declarator::None && f.t(d.initBegin) == "{")) return;
        std::string name = f.t(d.nameTok);
        r.push_back({&s, name, norm(declTypeText(f, s) + f.tokText(d.begin, d.nameTok)),
                     usesOf(f, name, s.end, s.parent->end)});
    });
    return r;
}

std::string initText(const Func& f, const Stmt& s) {
    const Declarator& d = s.decls[0];
    return d.init == Declarator::None ? "" : f.tokText(d.initBegin, d.initEnd);
}

} // namespace

// ---------------------------------------------------------------------------
// bool_assign: 'x = a < b;' <-> 'if (a < b) x = 1; else x = 0;'

void enumBoolAssign(const Func& f, const EmitFn& emit) {
    std::vector<const Stmt*> exprs = stmtsOfKind(f, SK::Expr);
    for (const Stmt* s : exprs) {
        if (!inBlock(*s) && !(s->parent && s->parent->kind == SK::If)) continue;
        auto x = plainAssign(f, s);
        if (!x || !boolish(f, *x->kids[1])) continue;
        const Expr& c = *x->kids[1];
        std::string ct = c.k == Expr::Paren ? exprText(f, *c.kids[0]) : exprText(f, c);
        std::string lhs = exprText(f, *x->kids[0]);
        std::string ind = lineInd(f, s->begin), in = ind + "    ";
        for (auto [t, fl] : {std::pair<const char*, const char*>{"1", "0"}, {"true", "false"}}) {
            std::string text = ifElseText(ct, "{\n" + in + lhs + " = " + t + ";\n" + ind + "}",
                                          "{\n" + in + lhs + " = " + fl + ";\n" + ind + "}", ind);
            if (!emitReplace(f, emit, s->begin, s->end, text)) return;
            std::string flat = ifElseText(ct, "{\n" + in + lhs + " = " + t + ";\n" + ind + "}", "", ind);
            // 'x = 0; if (c) x = 1;'
            if (!emitReplace(f, emit, s->begin, s->end, lhs + " = " + fl + ";\n" + ind + flat)) return;
        }
    }
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (s->kids.size() != 2) continue;
        auto a = plainAssign(f, singleStmt(s->kids[0].get()));
        auto b = plainAssign(f, singleStmt(s->kids[1].get()));
        if (!a || !b) continue;
        std::string lhs = exprText(f, *a->kids[0]);
        if (norm(lhs) != norm(exprText(f, *b->kids[0]))) continue;
        std::string va = exprText(f, *a->kids[1]), vb = exprText(f, *b->kids[1]);
        auto c = parseExpr(f, s->condOpen + 1, s->condClose);
        if (!c) continue;
        std::string ct = f.tokText(s->condOpen + 1, s->condClose);
        std::string v;
        if (isTrue(va) && isFalse(vb)) v = boolish(f, *c) ? ct : "(" + ct + ") != 0";
        else if (isFalse(va) && isTrue(vb)) v = boolish(f, *c) ? negate(f, s->condOpen + 1, s->condClose) : "(" + ct + ") == 0";
        else continue;
        if (!emitReplace(f, emit, s->begin, s->end, lhs + " = " + v + ";")) return;
    }
}

// ---------------------------------------------------------------------------
// reuse_local: 'T* a = x; use(a); T* b = y; use(b);' -> 'T* a = x; use(a); a = y; use(a);'

void enumReuseLocal(const Func& f, const EmitFn& emit) {
    auto decls = localDecls(f);
    for (auto& A : decls) {
        for (auto& B : decls) {
            if (&A == &B || A.type != B.type || B.s->begin <= A.s->end) continue;
            // B lies in A's scope, A is dead by then and isn't read again around a loop
            if (B.s->begin < A.s->parent->begin || B.s->parent->end > A.s->parent->end) continue;
            if (!A.uses.empty() && A.uses.back() > B.s->begin) continue;
            if (!usesOf(f, B.name, f.bodyOpen, B.s->begin).empty()) continue;
            bool loop = false;
            for (int u : A.uses) loop |= sharedLoop(f, u, B.s->begin, *A.s);
            if (loop) continue;
            std::vector<int> buses = usesOf(f, B.name, B.s->end, B.s->parent->end);
            std::string init = initText(f, *B.s);
            const Stmt* bs = B.s;
            std::string an = A.name;
            if (!emit([&f, bs, buses, init, an]() {
                    Rewriter rw(f);
                    if (init.empty()) rw.remove(bs->begin, bs->end);
                    else rw.replace(bs->begin, bs->end, an + " = " + init + ";");
                    for (int u : buses) rw.replace(u, u + 1, an);
                    return rw.apply();
                }))
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// split_local: 'a = y;' that starts a new life of a -> 'T a2 = y;' with the later uses renamed

void enumSplitLocal(const Func& f, const EmitFn& emit) {
    auto decls = localDecls(f);
    for (auto& A : decls) {
        for (const Stmt* s : stmtsOfKind(f, SK::Expr)) {
            if (!inBlock(*s) || s->begin <= A.s->end || s->end > A.s->parent->end) continue;
            auto x = plainAssign(f, s);
            if (!x || x->kids[0]->k != Expr::Primary || f.t(x->kids[0]->b) != A.name) continue;
            // every later use sits in s's block, and nothing earlier is read again around a loop
            std::vector<int> after, before;
            for (int u : A.uses) {
                if (u >= s->end) after.push_back(u);
                else if (u < s->begin) before.push_back(u);
            }
            if (after.empty()) continue;
            if (after.back() >= s->parent->end) continue;
            bool bad = false;
            for (int u : before) bad |= sharedLoop(f, u, s->begin, *A.s);
            for (int u : after) bad |= loopOf(f, u) != loopOf(f, s->begin) && sharedLoop(f, u, s->begin, *A.s);
            if (bad) continue;
            std::string nn = A.name + "_2";
            if (f.locals.count(nn)) continue;
            const Declarator& d = A.s->decls[0];
            std::string decl = declTypeText(f, *A.s);
            for (int i = d.begin; i < d.nameTok; ++i) decl += f.t(i); // the declarator's '*'s
            decl += " ";
            std::string text = decl + nn + " = " + exprText(f, *x->kids[1]) + ";";
            if (!emit([&f, s, after, text, nn]() {
                    Rewriter rw(f);
                    rw.replace(s->begin, s->end, text);
                    for (int u : after) rw.replace(u, u + 1, nn);
                    return rw.apply();
                }))
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// inline_use: 'T x = p->f; ... g(x);' -> '... g(p->f);' at one use, keeping the local

void enumInlineUse(const Func& f, const EmitFn& emit) {
    auto decls = localDecls(f);
    for (auto& A : decls) {
        const Declarator& d = A.s->decls[0];
        if (d.init != Declarator::Assign || A.uses.size() < 2) continue;
        bool written = false;
        for (int u : A.uses) written |= isWriteUse(f, u);
        if (written) continue;
        Effects ie = effectsOf(f, d.initBegin, d.initEnd);
        if (ie.call || !ie.writes.empty() || ie.memWrite || ie.barrier) continue;
        auto x = parseExpr(f, d.initBegin, d.initEnd);
        if (!x || x->k == Expr::Comma) continue;
        std::string init = f.tokText(d.initBegin, d.initEnd);
        std::string text = isPostfixLike(*x) ? init : paren(init);
        bool trivially = !ie.memRead && ie.reads.empty();
        for (int u : A.uses) {
            // up to the start of u's statement: the call u is an argument of runs after it
            int st = u;
            while (st > A.s->end && f.t(st - 1) != ";" && f.t(st - 1) != "{" && f.t(st - 1) != "}") st--;
            Effects between = effectsOf(f, A.s->end, st);
            if (!trivially && (initBlocked(ie, between) || (ie.memRead && between.memWrite) || between.barrier))
                continue;
            if (!trivially && insideLoopNotContaining(f, u, *A.s)) continue;
            if (!emitReplace(f, emit, u, u + 1, text)) return;
        }
    }
}

void registerExtraPasses(std::vector<Pass>& passes) {
    passes.push_back({"bool_assign",
                      "'x = a < b;' <-> 'if (a < b) x = 1; else x = 0;' (or 'x = 0; if (a < b) x = 1;'), "
                      "and 'if (a & 8) x = 1; else x = 0;' -> 'x = (a & 8) != 0;'",
                      5, enumBoolAssign, nullptr});
    passes.push_back({"reuse_local",
                      "Reuse a dead local of the same type for a later one: 'T* b = y;' -> 'a = y;' with "
                      "b's uses renamed (one variable or two decides which register or slot holds it)",
                      6, enumReuseLocal, nullptr});
    passes.push_back({"split_local",
                      "Give a local's later value its own variable: 'a = y;' -> 'T a_2 = y;' with the "
                      "uses after it renamed (the reverse of reuse_local)",
                      5, enumSplitLocal, nullptr});
    passes.push_back({"inline_use",
                      "Read a local's (side effect free) initializer again at one of its uses instead of "
                      "the local: 'T x = p->f; a(x); b(x);' -> 'T x = p->f; a(x); b(p->f);'",
                      6, enumInlineUse, nullptr});
}

} // namespace util

} // namespace perm
