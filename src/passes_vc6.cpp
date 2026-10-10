// Passes for VC6 codegen effects found while matching gta2_re by hand: the order of && / ||
// operands (which test is laid out first), a while loop written as a guarded do-while, and
// no-op tree nodes (redundant parentheses, an f32 cast of an f32 value) that move the x87
// scheduler's 81-node window breaks (see gta2_re Scripts/x87_sched/README.md).

#include "pass_util.hpp"

namespace perm {

namespace util {

namespace {

// Whether evaluating x can't change anything: no assignment, ++/-- or call. Calls are allowed
// only when allowCalls (inline getters are the usual case; read the diff).
bool sideEffectFree(const Func& f, Expr& x, bool allowCalls) {
    bool ok = true;
    forEachExpr(x, [&](Expr& e) {
        if (e.k == Expr::Assign || e.k == Expr::Postfix) ok = false;
        if (e.k == Expr::Unary && (f.t(e.op) == "++" || f.t(e.op) == "--")) ok = false;
        if (e.k == Expr::Call && !allowCalls) ok = false;
    });
    return ok;
}

bool isFloatType(const std::string& t) {
    std::string n = norm(t);
    if (n.rfind("const", 0) == 0) n = n.substr(5);
    return n == "f32" || n == "float" || n == "double" || n == "f64";
}

// The declared type of a plain local, member path or implicit member, or "".
std::string valueType(const Func& f, const Expr& x, const std::map<std::string, std::string>& locals) {
    if (x.k == Expr::Primary && x.e == x.b + 1) {
        auto it = locals.find(f.t(x.b));
        if (it != locals.end()) return it->second;
        if (f.isLocal(x.b)) return "";
        auto ft = fieldTypes().find(f.t(x.b));
        return ft == fieldTypes().end() ? "" : ft->second;
    }
    if (x.k == Expr::Member) {
        auto ft = fieldTypes().find(f.t(x.e - 1));
        return ft == fieldTypes().end() ? "" : ft->second;
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// swap_logical: 'a && b' -> 'b && a' (and ||) when neither operand has side effects

void enumSwapLogical(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        if (x.k != Expr::Binary) return true;
        const std::string& op = f.t(x.op);
        if (op != "&&" && op != "||") return true;
        Expr& l = *x.kids[0];
        Expr& r = *x.kids[1];
        if (!sideEffectFree(f, l, true) || !sideEffectFree(f, r, true)) return true;
        int p = binPrec(op);
        // 'a && b && c' parses as '(a && b) && c'; the swap gives 'c && a && b' (same tree shape
        // VC6 lays out the same way as 'c && (a && b)')
        std::string lt = exprPrec(f, l) < p ? paren(exprText(f, l)) : exprText(f, l);
        std::string rt = exprPrec(f, r) <= p ? paren(exprText(f, r)) : exprText(f, r);
        return emitReplace(f, emit, x.b, x.e, rt + " " + op + " " + lt);
    });
}

// ---------------------------------------------------------------------------
// do_while: 'while (c) B' <-> 'if (c) { do B while (c); }'

void enumDoWhile(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::While)) {
        if (!inBlock(*s) || s->kids.empty()) continue;
        std::string c = f.tokText(s->condOpen + 1, s->condClose);
        auto cx = parseExpr(f, s->condOpen + 1, s->condClose);
        if (!cx || !sideEffectFree(f, *cx, true)) continue;
        if (hasToken(f, *s->kids[0], "continue")) continue; // continue would skip the test
        std::string ind = lineInd(f, s->begin);
        std::string in = ind + "    ";
        auto body = branchStmts(s->kids[0].get());
        std::string text = "if (" + c + ")\n" + ind + "{\n" + in + "do\n" + in + blockText(f, body, in) +
                           " while (" + c + ");\n" + ind + "}";
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
        // the bare do-while, for loops that always run at least once
        text = "do\n" + ind + blockText(f, body, ind) + " while (" + c + ");";
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
    }
    // back: 'if (c) { do B while (c); }' -> 'while (c) B'
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (!inBlock(*s) || s->kids.size() != 1) continue;
        const Stmt* d = singleStmt(s->kids[0].get());
        if (!d || d->kind != SK::Do || d->kids.empty()) continue;
        std::string c = f.tokText(s->condOpen + 1, s->condClose);
        if (norm(c) != norm(f.tokText(d->condOpen + 1, d->condClose))) continue;
        std::string ind = lineInd(f, s->begin);
        std::string text = "while (" + c + ")\n" + ind + blockText(f, branchStmts(d->kids[0].get()), ind);
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
    }
}

// ---------------------------------------------------------------------------
// noop_node: add or drop parentheses that change nothing but the expression tree, and an
// f32 cast of an f32 value

void enumNoopNode(const Func& f, const EmitFn& emit) {
    auto locals = localTypes(f);
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        if (x.k == Expr::Binary || x.k == Expr::Assign) {
            int p = x.k == Expr::Assign ? 2 : binPrec(f.t(x.op));
            for (size_t k = 0; k < x.kids.size(); ++k) {
                const Expr& c = *x.kids[k];
                if (c.k == Expr::Binary && exprPrec(f, c) > p &&
                    !emitReplace(f, emit, c.b, c.e, paren(exprText(f, c))))
                    return false;
                if (c.k == Expr::Paren && !c.kids.empty()) {
                    const Expr& in = *c.kids[0];
                    bool safe = isPostfixLike(in) || in.k == Expr::Unary || in.k == Expr::Cast ||
                                (in.k == Expr::Binary && exprPrec(f, in) > p) ||
                                (x.k == Expr::Assign && k == 1 && in.k != Expr::Comma);
                    if (safe && !emitReplace(f, emit, c.b, c.e, exprText(f, in))) return false;
                }
            }
        }
        // '(f32)v' -> 'v' for an f32 local or member
        if (x.k == Expr::Cast && f.t(x.b) == "(" && !x.kids.empty()) {
            int close = matchBracket(f.toks, x.b);
            if (close > 0 && close < x.e && isFloatType(f.tokText(x.b + 1, close)) &&
                isFloatType(valueType(f, *x.kids[0], locals)) &&
                !emitReplace(f, emit, x.b, x.e, exprText(f, *x.kids[0])))
                return false;
        }
        // 'v' -> '(f32)v'
        if ((x.k == Expr::Primary || x.k == Expr::Member) && !isWriteUse(f, x.e - 1) &&
            !(x.b > 0 && f.t(x.b - 1) == ")") && isFloatType(valueType(f, x, locals)) &&
            !emitReplace(f, emit, x.b, x.e, "(f32)" + exprText(f, x)))
            return false;
        return true;
    });
}

void registerVc6Passes(std::vector<Pass>& passes) {
    passes.push_back({"swap_logical",
                      "'a && b' <-> 'b && a' (and ||) when neither side has side effects (calls are "
                      "allowed: read the diff): which test VC6 lays out first",
                      6, enumSwapLogical, nullptr});
    passes.push_back({"do_while",
                      "'while (c) B' <-> 'if (c) { do B while (c); }', and 'while (c) B' -> "
                      "'do B while (c);' (only if the loop always runs once: read the diff)",
                      4, enumDoWhile, nullptr});
    passes.push_back({"noop_node",
                      "Add or drop parentheses that change nothing, or an '(f32)' cast of an f32 "
                      "value: no-op tree nodes move the x87 scheduler's 81-node windows",
                      5, enumNoopNode, nullptr});
}

} // namespace util

} // namespace perm
