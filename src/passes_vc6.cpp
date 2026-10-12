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

// ---------------------------------------------------------------------------
// goto_exit: 'break;' -> 'goto loop_exit;' with the label after the loop, and back. A break
// makes the exit a jump-only block whose jumps VC6 retargets; a label with code keeps it.

namespace {

bool isBreakable(const Stmt& s) {
    return s.kind == SK::For || s.kind == SK::While || s.kind == SK::Do || s.kind == SK::Switch;
}

// The innermost loop or switch holding s, or null.
const Stmt* breakTarget(const Stmt* s) {
    for (const Stmt* p = s->parent; p; p = p->parent)
        if (isBreakable(*p)) return p;
    return nullptr;
}

bool usesToken(const Func& f, const std::string& t) {
    for (auto& tk : f.toks)
        if (tk.text == t) return true;
    return false;
}

} // namespace

void enumGotoExit(const Func& f, const EmitFn& emit) {
    std::string label = "loop_exit";
    for (int n = 2; usesToken(f, label); ++n) label = "loop_exit_" + std::to_string(n);
    std::vector<const Stmt*> loops;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind == SK::For || s.kind == SK::While || s.kind == SK::Do) loops.push_back(&s);
    });
    for (const Stmt* L : loops) {
        if (!inBlock(*L)) continue;
        std::vector<const Stmt*> brks;
        forEachStmt(*L, [&](const Stmt& s) {
            if (s.kind == SK::Break && breakTarget(&s) == L) brks.push_back(&s);
        });
        if (brks.empty()) continue;
        auto& sib = L->parent->kids;
        int i = indexInParent(*L);
        int at = i + 1 < (int)sib.size() ? sib[i + 1]->begin : L->parent->end - 1;
        std::string lab = label + (i + 1 < (int)sib.size() ? ":" : ":;");
        // all breaks, then each one alone
        std::vector<std::vector<const Stmt*>> picks{brks};
        if (brks.size() > 1)
            for (auto* b : brks) picks.push_back({b});
        for (auto& pick : picks) {
            if (!emit([&f, pick, at, lab, label]() {
                    Rewriter rw(f);
                    for (auto* b : pick) rw.replace(b->begin, b->end, "goto " + label + ";");
                    rw.insertBefore(at, lab);
                    return rw.apply();
                }))
                return;
        }
    }
    // back: 'goto L;' -> 'break;' when L labels the statement right after the loop it is in
    for (const Stmt* g : stmtsOfKind(f, SK::Goto)) {
        if (g->end - g->begin != 3) continue;
        const Stmt* L = breakTarget(g);
        if (!L || L->kind == SK::Switch || !inBlock(*L)) continue;
        auto& sib = L->parent->kids;
        int i = indexInParent(*L);
        if (i + 1 >= (int)sib.size() || sib[i + 1]->kind != SK::Label) continue;
        if (f.t(sib[i + 1]->begin) != f.t(g->begin + 1)) continue;
        if (!emitReplace(f, emit, g->begin, g->end, "break;")) return;
    }
}

// ---------------------------------------------------------------------------
// tail_return: 'if (c) A else B  return e;' -> 'if (c) { A return e; } else B  return e;' (one
// branch at a time; 'return;' after an if that ends a void function), and back. Which copy of
// a return VC6 keeps when it cross-jumps the tails depends on them.

void enumTailReturn(const Func& f, const EmitFn& emit) {
    // the text starts at the name, so a void function is told by its returns: none with a value
    bool isVoid = true;
    for (const Stmt* r : stmtsOfKind(f, SK::Return)) isVoid &= r->end - r->begin == 2;
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (!inBlock(*s)) continue;
        auto& sib = s->parent->kids;
        int i = indexInParent(*s);
        std::string ret;
        if (i + 1 < (int)sib.size() && sib[i + 1]->kind == SK::Return && i + 2 == (int)sib.size())
            ret = f.textOf(*sib[i + 1]);
        else if (i + 1 == (int)sib.size() && s->parent == f.body.get() && isVoid)
            ret = "return;";
        if (ret.empty()) continue;
        for (size_t k = 0; k < s->kids.size(); ++k) {
            const Stmt* b = s->kids[k].get();
            if (b->kind == SK::If) continue; // else-if chain: the inner if gets its own turn
            auto st = branchStmts(b);
            if (!st.empty() && isJump(st.back())) {
                // drop a copy that equals the trailing return
                if (norm(f.textOf(*st.back())) == norm(ret) && st.size() > 1 &&
                    !emitReplace(f, emit, b->begin, b->end,
                                 blockText(f, {st.begin(), st.end() - 1}, lineInd(f, s->begin))))
                    return;
                continue;
            }
            if (!emitReplace(f, emit, b->begin, b->end, blockText(f, st, lineInd(f, s->begin), {}, {ret})))
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// switch_dup: 'switch (x) { case 1: A; break; ... } S;' -> each case runs S before its break (a
// default that only runs S is added if there is none), and back

void enumSwitchDup(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::Switch)) {
        if (!inBlock(*s) || s->kids.empty() || s->kids[0]->kind != SK::Block) continue;
        const Stmt* body = s->kids[0].get();
        auto& sib = s->parent->kids;
        int i = indexInParent(*s);
        if (i + 1 >= (int)sib.size()) continue;
        const Stmt* n = sib[i + 1].get();
        if (n->kind != SK::Expr) continue;
        std::string stmt = f.textOf(*n);
        std::vector<const Stmt*> brks;
        bool hasDefault = false;
        forEachStmt(*body, [&](const Stmt& x) {
            if (x.kind == SK::Break && breakTarget(&x) == s) brks.push_back(&x);
            if (x.kind == SK::Default && x.parent == body) hasDefault = true;
        });
        const Stmt* last = body->kids.empty() ? nullptr : body->kids.back().get();
        bool fallsOut = last && !isJump(last);
        std::string ind = lineInd(f, s->begin);
        if (!emit([&f, brks, n, stmt, fallsOut, hasDefault, body, ind]() {
                Rewriter rw(f);
                for (auto* b : brks) rw.replace(b->begin, b->end, stmt + "\n" + lineInd(f, b->begin) + "break;");
                std::string tail;
                if (fallsOut) tail += "    " + stmt + "\n" + ind + "    break;\n" + ind;
                if (!hasDefault) tail += "default:\n" + ind + "    " + stmt + "\n" + ind + "    break;\n" + ind;
                if (!tail.empty()) rw.insertBefore(body->end - 1, tail.substr(0, tail.size() - ind.size() - 1));
                rw.remove(n->begin, n->end);
                return rw.apply();
            }))
            return;
    }
    // back: every break of the switch preceded by the same statement -> once after the switch
    for (const Stmt* s : stmtsOfKind(f, SK::Switch)) {
        if (!inBlock(*s) || s->kids.empty() || s->kids[0]->kind != SK::Block) continue;
        const Stmt* body = s->kids[0].get();
        std::vector<const Stmt*> brks;
        forEachStmt(*body, [&](const Stmt& x) {
            if (x.kind == SK::Break && breakTarget(&x) == s) brks.push_back(&x);
        });
        if (brks.size() < 2) continue;
        // values with no case, or a last case that falls out, must not start running it
        bool hasDefault = false;
        for (auto& k : body->kids) hasDefault |= k->kind == SK::Default;
        if (!hasDefault || body->kids.empty() || !isJump(body->kids.back().get())) continue;
        std::string common;
        std::vector<const Stmt*> prev;
        bool ok = true;
        for (auto* b : brks) {
            int k = indexInParent(*b);
            if (k <= 0 || b->parent->kids[k - 1]->kind != SK::Expr) {
                ok = false;
                break;
            }
            const Stmt* p = b->parent->kids[k - 1].get();
            if (common.empty()) common = norm(f.textOf(*p));
            ok &= norm(f.textOf(*p)) == common;
            prev.push_back(p);
        }
        if (!ok) continue;
        std::string text = f.textOf(*prev[0]);
        if (!emit([&f, prev, s, text]() {
                Rewriter rw(f);
                for (auto* p : prev) rw.remove(p->begin, p->end);
                std::string t = f.tokText(s->end - 1, s->end) + "\n" + lineInd(f, s->begin) + text;
                rw.replace(s->end - 1, s->end, t);
                return rw.apply();
            }))
            return;
    }
}

// ---------------------------------------------------------------------------
// dead_wrap: 'for (...) B  ...  return;' -> 'while (1) { for (...) B ... return; }' (or 'if (1)'),
// and back. The never-taken exit edge of the wrapper changes which successor VC6's RPO walk visits
// first, so code reached by a goto out of the loop can be laid out before the loop's normal tail
// (gta2_re Object_2C::IntegrateHorizontalMovementAndCollisions_524630).

namespace {

const Stmt* continueTarget(const Stmt* s) {
    for (const Stmt* p = s->parent; p; p = p->parent)
        if (p->kind == SK::For || p->kind == SK::While || p->kind == SK::Do) return p;
    return nullptr;
}

bool isConstTrue(const Func& f, const Stmt& s) {
    if (s.condOpen < 0 || s.condClose != s.condOpen + 2) return false;
    std::string c = f.t(s.condOpen + 1);
    return c == "1" || c == "true";
}

} // namespace

void enumDeadWrap(const Func& f, const EmitFn& emit) {
    std::vector<const Stmt*> loops;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind == SK::For || s.kind == SK::While || s.kind == SK::Do) loops.push_back(&s);
    });
    for (const Stmt* L : loops) {
        if (!inBlock(*L) || isConstTrue(f, *L)) continue;
        auto& sib = L->parent->kids;
        int i = indexInParent(*L);
        int j = i;
        while (j < (int)sib.size() && sib[j]->kind != SK::Return && sib[j]->kind != SK::Goto) ++j;
        if (j >= (int)sib.size()) continue;
        int b = sib[i]->begin, e = sib[j]->end;
        bool ok = true;
        for (int k = i; k <= j && ok; ++k)
            forEachStmt(*sib[k], [&](const Stmt& s) {
                const Stmt* t = s.kind == SK::Break ? breakTarget(&s)
                              : s.kind == SK::Continue ? continueTarget(&s) : &s;
                if (!t || t->begin < b || t->end > e) ok = false;
            });
        if (!ok) continue;
        int at = j + 1 < (int)sib.size() ? sib[j + 1]->begin : L->parent->end - 1;
        for (const char* head : {"while (1) {", "if (1) {"}) {
            if (!emit([&f, b, at, head]() {
                    Rewriter rw(f);
                    rw.insertBefore(b, head);
                    rw.insertBefore(at, "}");
                    return rw.apply();
                }))
                return;
        }
    }
    // back: unwrap 'while (1) { ...; return; }' / 'if (1) { ... }' that nothing breaks out of
    for (SK k : {SK::While, SK::If}) {
        for (const Stmt* w : stmtsOfKind(f, k)) {
            if (!inBlock(*w) || !isConstTrue(f, *w) || w->kids.size() != 1) continue;
            auto st = branchStmts(w->kids[0].get());
            if (st.empty()) continue;
            if (k == SK::While && st.back()->kind != SK::Return && st.back()->kind != SK::Goto) continue;
            bool ok = true;
            if (k == SK::While)
                forEachStmt(*w->kids[0], [&](const Stmt& s) {
                    if ((s.kind == SK::Break && breakTarget(&s) == w) ||
                        (s.kind == SK::Continue && continueTarget(&s) == w))
                        ok = false;
                });
            if (!ok) continue;
            if (!emitReplace(f, emit, w->begin, w->end, stmtsText(f, st, lineInd(f, w->begin)))) return;
        }
    }
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
    passes.push_back({"goto_exit",
                      "'break;' <-> 'goto loop_exit;' with the label after the loop (all breaks or one): "
                      "a break's exit is a jump-only block whose jumps VC6 retargets",
                      5, enumGotoExit, nullptr});
    passes.push_back({"tail_return",
                      "Copy the return after an if into the end of one branch ('return;' for an if "
                      "that ends a void function), or drop such a copy: which tail VC6's "
                      "cross-jumping keeps",
                      5, enumTailReturn, nullptr});
    passes.push_back({"switch_dup",
                      "Run the statement after a switch in each case before its break (adding a "
                      "default for it if there is none), or hoist a statement every break shares out "
                      "of the switch: per-case tails",
                      4, enumSwitchDup, nullptr});
    passes.push_back({"dead_wrap",
                      "Wrap a loop and the statements after it up to a return/goto in 'while (1) { }' "
                      "or 'if (1) { }', or unwrap one: the dead exit edge changes VC6's RPO block order",
                      3, enumDeadWrap, nullptr});
}

} // namespace util

} // namespace perm
