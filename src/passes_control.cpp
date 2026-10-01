// Control-flow and expression-shape passes. Most of them come straight from
// the VC6 codegen notes in gta2_re's docs/matching_quirks.md.

#include "pass_util.hpp"

#include <algorithm>
#include <cctype>

namespace perm::util {

bool emitReplace(const Func& f, const EmitFn& emit, int b, int e, const std::string& text) {
    return emit([&f, b, e, text]() {
        Rewriter rw(f);
        rw.replace(b, e, text);
        return rw.apply();
    });
}

// Indentation (spaces only) of the line holding token i.
std::string lineInd(const Func& f, int i) { return f.indentOf(i).substr(1); }

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
}

// Statements of a branch: the block's statements, or the statement itself.
std::vector<const Stmt*> branchStmts(const Stmt* s) {
    std::vector<const Stmt*> r;
    if (s->kind == SK::Block)
        for (auto& k : s->kids) r.push_back(k.get());
    else
        r.push_back(s);
    return r;
}

// Text of consecutive statements, re-indented so their first line sits at ind.
std::string stmtsText(const Func& f, const std::vector<const Stmt*>& v, const std::string& ind) {
    if (v.empty()) return "";
    std::string text = f.tokText(v.front()->begin, v.back()->end);
    replaceAll(text, f.indentOf(v.front()->begin), "\n" + ind);
    return text;
}

std::string stmtText(const Func& f, const Stmt* s, const std::string& ind) {
    return stmtsText(f, {s}, ind);
}

// A controlled statement (if/loop body) for a header at indentation ind,
// starting with its line break: a block sits at ind, a single statement one
// level deeper.
std::string bodyText(const Func& f, const Stmt* s, const std::string& ind) {
    std::string in = s->kind == SK::Block ? ind : ind + "    ";
    return "\n" + in + stmtText(f, s, in);
}

// Smallest expression node in x covering tokens [b, e), or null.
const Expr* coveringNode(const Expr& x, int b, int e) {
    if (x.b > b || x.e < e) return nullptr;
    for (auto& k : x.kids)
        if (const Expr* r = coveringNode(*k, b, e)) return r;
    return &x;
}

const Expr* nodeAt(const Expr& x, int b, int e) {
    if (x.b == b && x.e == e) return &x;
    for (auto& k : x.kids)
        if (const Expr* r = nodeAt(*k, b, e)) return r;
    return nullptr;
}

// "{ stmts }" laid out at indentation ind (the brace's own line).
std::string blockText(const Func& f, const std::vector<const Stmt*>& v, const std::string& ind,
                      const std::vector<std::string>& extraBefore,
                      const std::vector<std::string>& extraAfter) {
    std::string in = ind + "    ";
    std::string r = "{";
    for (auto& x : extraBefore) r += "\n" + in + x;
    if (!v.empty()) r += "\n" + in + stmtsText(f, v, in);
    for (auto& x : extraAfter) r += "\n" + in + x;
    return r + "\n" + ind + "}";
}

std::string ifElseText(const std::string& cond, const std::string& thenBlock,
                       const std::string& elseBlock, const std::string& ind) {
    std::string r = "if (" + cond + ")\n" + ind + thenBlock;
    if (!elseBlock.empty()) r += "\n" + ind + "else\n" + ind + elseBlock;
    return r;
}

bool isJump(const Stmt* s) {
    return s && (s->kind == SK::Return || s->kind == SK::Break || s->kind == SK::Continue ||
                 s->kind == SK::Goto);
}

bool hasToken(const Func& f, const Stmt& s, const char* tok) {
    for (int i = s.begin; i < s.end; ++i)
        if (f.t(i) == tok) return true;
    return false;
}

bool isTrue(const std::string& s) { return s == "true" || s == "1" || s == "TRUE"; }
bool isFalse(const std::string& s) { return s == "false" || s == "0" || s == "FALSE"; }
bool isZero(const std::string& s) { return s == "0" || s == "NULL" || s == "nullptr"; }

// Value of "return v;" as text, or "" when s isn't one.
std::string returnValue(const Func& f, const Stmt* s) {
    if (!s || s->kind != SK::Return || f.t(s->begin) != "return" || s->end - 1 <= s->begin + 1)
        return "";
    return f.tokText(s->begin + 1, s->end - 1);
}

std::vector<const Stmt*> stmtsOfKind(const Func& f, SK k) {
    std::vector<const Stmt*> r;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind == k) r.push_back(&s);
    });
    return r;
}

bool inBlock(const Stmt& s) { return s.parent && s.parent->kind == SK::Block; }

// ---------------------------------------------------------------------------
// bool_return: "return a <= b;" <-> "if (a <= b) return true; return false;"

void enumBoolReturn(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::Return)) {
        std::string v = returnValue(f, s);
        if (v.empty()) continue;
        auto x = parseExpr(f, s->begin + 1, s->end - 1);
        if (!x) continue;
        bool boolish = (x->k == Expr::Binary && binPrec(f.t(x->op)) >= 4 &&
                        binPrec(f.t(x->op)) <= 10 && f.t(x->op) != "|" && f.t(x->op) != "^" &&
                        f.t(x->op) != "&") ||
                       (x->k == Expr::Unary && f.t(x->op) == "!");
        if (!boolish) continue;
        std::string ind = lineInd(f, s->begin);
        std::string c = x->k == Expr::Paren ? exprText(f, *x->kids[0]) : v;
        for (auto [t, fl] : {std::pair<const char*, const char*>{"true", "false"}, {"1", "0"}}) {
            std::string ret = std::string("return ") + t + ";";
            std::string retF = std::string("return ") + fl + ";";
            std::string early = ifElseText(c, "{\n" + ind + "    " + ret + "\n" + ind + "}", "", ind) +
                                "\n" + ind + retF;
            std::string withElse = ifElseText(c, "{\n" + ind + "    " + ret + "\n" + ind + "}",
                                              "{\n" + ind + "    " + retF + "\n" + ind + "}", ind);
            if (!emitReplace(f, emit, s->begin, s->end, early)) return;
            if (!emitReplace(f, emit, s->begin, s->end, withElse)) return;
        }
    }
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        std::string a = returnValue(f, singleStmt(s->kids[0].get()));
        if (a.empty()) continue;
        std::string b;
        int end = s->end;
        if (s->kids.size() == 2) {
            b = returnValue(f, singleStmt(s->kids[1].get()));
        } else if (inBlock(*s)) {
            int i = indexInParent(*s);
            if (i + 1 < (int)s->parent->kids.size()) {
                const Stmt* n = s->parent->kids[i + 1].get();
                b = returnValue(f, n);
                end = n->end;
            }
        }
        std::string cond;
        if (isTrue(a) && isFalse(b)) cond = f.tokText(s->condOpen + 1, s->condClose);
        else if (isFalse(a) && isTrue(b)) cond = negate(f, s->condOpen + 1, s->condClose);
        else continue;
        if (!emitReplace(f, emit, s->begin, end, "return " + cond + ";")) return;
    }
}

// ---------------------------------------------------------------------------
// ternary_arg: "f(c ? a : b);" <-> "if (c) f(a); else f(b);"

void enumTernaryArg(const Func& f, const EmitFn& emit) {
    for (auto& site : exprSites(f)) {
        const Stmt& s = *site.stmt;
        if (s.kind != SK::Expr && s.kind != SK::Return) continue;
        if (!inBlock(s) && s.parent && s.parent->kind != SK::If) continue;
        auto root = parseExpr(f, site.b, site.e);
        if (!root) continue;
        std::vector<const Expr*> ternaries;
        forEachExpr(*root, [&](Expr& x) {
            if (x.k == Expr::Ternary && &x != root.get()) ternaries.push_back(&x);
        });
        std::string ind = lineInd(f, s.begin);
        for (const Expr* t : ternaries) {
            auto with = [&](const Expr& branch) {
                std::string v = exprText(f, branch);
                if (branch.k == Expr::Comma) v = paren(v);
                return f.tokText(s.begin, t->b) + f.toks[t->b].lead + v + f.toks[t->e].lead +
                       f.tokText(t->e, s.end);
            };
            const Expr& c = *t->kids[0];
            std::string ct = c.k == Expr::Paren ? exprText(f, *c.kids[0]) : exprText(f, c);
            std::string in = ind + "    ";
            std::string text = ifElseText(ct, "{\n" + in + with(*t->kids[1]) + "\n" + ind + "}",
                                          "{\n" + in + with(*t->kids[2]) + "\n" + ind + "}", ind);
            if (!emitReplace(f, emit, s.begin, s.end, text)) return;
        }
    }
    // the other way: two branches that differ in one sub-expression
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (s->kids.size() != 2) continue;
        const Stmt* a = singleStmt(s->kids[0].get());
        const Stmt* b = singleStmt(s->kids[1].get());
        if (!a || !b || a->kind != b->kind || (a->kind != SK::Expr && a->kind != SK::Return)) continue;
        int na = a->end - a->begin, nb = b->end - b->begin;
        int p = 0;
        while (p < na && p < nb && f.t(a->begin + p) == f.t(b->begin + p)) p++;
        int q = 0;
        while (q < na - p && q < nb - p && f.t(a->end - 1 - q) == f.t(b->end - 1 - q)) q++;
        if (p == na && p == nb) continue; // identical
        int ab = a->begin + p, ae = a->end - q, bb = b->begin + p, be = b->end - q;
        if (ab >= ae || bb >= be) continue;
        // widen the difference to whole sub-expressions of both statements
        int skip = a->kind == SK::Return ? 1 : 0;
        auto ta = parseExpr(f, a->begin + skip, a->end - 1);
        auto tb = parseExpr(f, b->begin + skip, b->end - 1);
        if (!ta || !tb) continue;
        const Expr* ea = coveringNode(*ta, ab, ae);
        if (!ea) continue;
        const Expr* eb = nodeAt(*tb, b->begin + (ea->b - a->begin), b->end - (a->end - ea->e));
        if (!eb || ea->k == Expr::Comma || eb->k == Expr::Comma) continue;
        ab = ea->b, ae = ea->e, bb = eb->b, be = eb->e;
        auto c = parseExpr(f, s->condOpen + 1, s->condClose);
        std::string ct = f.tokText(s->condOpen + 1, s->condClose);
        if (!c || exprPrec(f, *c) <= 3) ct = paren(ct);
        std::string tern = ct + " ? " + f.tokText(ab, ae) + " : " + f.tokText(bb, be);
        const std::string& before = f.t(ab - 1);
        const std::string& after = f.t(ae);
        bool standalone = (before == "(" || before == "," || before == "return" || isAssignOp(before)) &&
                          (after == ")" || after == "," || after == ";");
        if (!standalone) tern = paren(tern);
        std::string text = f.tokText(a->begin, ab) + f.toks[ab].lead + tern + f.toks[ae].lead +
                           f.tokText(ae, a->end);
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
    }
}

// ---------------------------------------------------------------------------
// switch_if: "if (x == K) S" <-> "switch (x) { case K: S break; }"

void enumSwitchIf(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        auto c = parseExpr(f, s->condOpen + 1, s->condClose);
        if (!c || c->k != Expr::Binary || f.t(c->op) != "==") continue;
        const Expr* v = c->kids[0].get();
        const Expr* k = c->kids[1].get();
        // how much an operand looks like a case label: 2 a literal, 1 an
        // enumerator-like name (qualified or ALL_CAPS), 0 anything else
        auto constness = [&](const Expr& x) {
            if (x.k == Expr::Unary && f.t(x.op) == "-" && x.kids[0]->k == Expr::Primary &&
                f.toks[x.kids[0]->b].kind == TokKind::Number)
                return 2;
            if (x.k != Expr::Primary) return 0;
            const Token& t = f.toks[x.b];
            if (t.kind == TokKind::Number || t.kind == TokKind::Char) return 2;
            if (t.kind != TokKind::Ident || f.isLocal(x.b)) return 0;
            if (x.e - x.b > 1) return 1; // Enum::Value
            bool caps = true;
            for (char ch : t.text) caps &= !std::islower((unsigned char)ch);
            return caps ? 1 : 0;
        };
        int cv = constness(*v), ck = constness(*k);
        if (cv == ck) continue;
        if (cv > ck) std::swap(v, k);
        bool breaks = hasToken(f, *s->kids[0], "break") ||
                      (s->kids.size() == 2 && hasToken(f, *s->kids[1], "break"));
        if (breaks) continue;
        std::string ind = lineInd(f, s->begin), in = ind + "    ", in2 = in + "    ";
        auto caseBody = [&](const Stmt* br) {
            std::string r;
            auto v2 = branchStmts(br);
            if (!v2.empty()) r += "\n" + in2 + stmtsText(f, v2, in2);
            if (v2.empty() || !isJump(v2.back())) r += "\n" + in2 + "break;";
            return r;
        };
        std::string text = "switch (" + exprText(f, *v) + ")\n" + ind + "{\n" + in + "case " +
                           exprText(f, *k) + ":" + caseBody(s->kids[0].get());
        if (s->kids.size() == 2) text += "\n" + in + "default:" + caseBody(s->kids[1].get());
        text += "\n" + ind + "}";
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
    }
    for (const Stmt* s : stmtsOfKind(f, SK::Switch)) {
        if (s->kids.empty() || s->kids[0]->kind != SK::Block) continue;
        auto& kids = s->kids[0]->kids;
        // segments: label index, then statements up to the next label
        std::vector<std::pair<const Stmt*, std::vector<const Stmt*>>> segs;
        bool ok = true;
        for (auto& k : kids) {
            if (k->kind == SK::Case || k->kind == SK::Default) segs.push_back({k.get(), {}});
            else if (segs.empty()) ok = false;
            else segs.back().second.push_back(k.get());
        }
        if (!ok || segs.empty() || segs.size() > 2 || segs[0].first->kind != SK::Case) continue;
        if (segs.size() == 2 && segs[1].first->kind != SK::Default) continue;
        std::vector<std::vector<const Stmt*>> bodies;
        for (size_t i = 0; i < segs.size() && ok; ++i) {
            auto body = segs[i].second;
            bool ends = !body.empty() && (body.back()->kind == SK::Break || isJump(body.back()));
            if (!body.empty() && body.back()->kind == SK::Break) body.pop_back();
            for (auto* b : body)
                if (hasToken(f, *b, "break")) ok = false;
            if (i + 1 < segs.size() && !ends) ok = false; // falls through
            bodies.push_back(body);
        }
        if (!ok) continue;
        const Stmt* label = segs[0].first;
        std::string k = f.tokText(label->begin + 1, label->end - 1);
        std::string v = f.tokText(s->condOpen + 1, s->condClose);
        auto vx = parseExpr(f, s->condOpen + 1, s->condClose);
        if (!vx || exprPrec(f, *vx) <= 9) v = paren(v);
        std::string ind = lineInd(f, s->begin);
        std::string text = ifElseText(v + " == " + k, blockText(f, bodies[0], ind),
                                      bodies.size() == 2 ? blockText(f, bodies[1], ind) : "", ind);
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
    }
}

// ---------------------------------------------------------------------------
// early_return: "if (c) { A; return; } else { B; }" <-> "if (c) { A; return; } B;"

void enumEarlyReturn(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (!inBlock(*s)) continue;
        std::string ind = lineInd(f, s->begin);
        auto thenStmts = branchStmts(s->kids[0].get());
        bool thenJumps = !thenStmts.empty() && isJump(thenStmts.back());
        if (s->kids.size() == 2 && thenJumps) {
            // drop the else
            const Stmt* el = s->kids[1].get();
            std::string body = stmtsText(f, branchStmts(el), ind);
            if (el->kind == SK::Block && el->kids.empty()) body = "";
            const Stmt* sp = s;
            if (!emit([&f, sp, body]() {
                    Rewriter rw(f);
                    if (body.empty()) rw.remove(sp->elseTok, sp->end);
                    else rw.replace(sp->elseTok, sp->end, body);
                    return rw.apply();
                }))
                return;
        } else if (s->kids.size() == 1 && thenJumps) {
            // wrap what follows in an else
            auto& sib = s->parent->kids;
            int i = indexInParent(*s);
            std::vector<const Stmt*> rest;
            bool ok = true;
            for (size_t k = i + 1; k < sib.size(); ++k) {
                SK kk = sib[k]->kind;
                if (kk == SK::Case || kk == SK::Default || kk == SK::Label) ok = false;
                rest.push_back(sib[k].get());
            }
            if (!ok || rest.empty()) continue;
            std::string text = f.textOf(*s) + "\n" + ind + "else\n" + ind + blockText(f, rest, ind);
            if (!emitReplace(f, emit, s->begin, rest.back()->end, text)) return;
        }
    }
}

// ---------------------------------------------------------------------------
// branch_dup: move a statement shared by both branches out of the if, or a
// neighbouring statement into both branches.

void enumBranchDup(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (s->kids.size() != 2 || s->kids[1]->kind == SK::If || !inBlock(*s)) continue;
        std::string ind = lineInd(f, s->begin);
        std::string cond = f.tokText(s->condOpen + 1, s->condClose);
        Effects ce = effectsOf(f, s->condOpen + 1, s->condClose);
        auto th = branchStmts(s->kids[0].get());
        auto el = branchStmts(s->kids[1].get());
        auto movable = [&](const Stmt* x) {
            return x->kind == SK::Expr || x->kind == SK::Empty;
        };
        auto build = [&](std::vector<const Stmt*> t, std::vector<const Stmt*> e,
                         std::vector<std::string> pre, std::vector<std::string> post) {
            return ifElseText(cond, blockText(f, t, ind, pre, post), blockText(f, e, ind, pre, post), ind);
        };
        // shared tail -> after the if
        if (!th.empty() && !el.empty() && movable(th.back()) &&
            norm(f.textOf(*th.back())) == norm(f.textOf(*el.back()))) {
            std::string tail = f.textOf(*th.back());
            std::string text = build({th.begin(), th.end() - 1}, {el.begin(), el.end() - 1}, {}, {}) +
                               "\n" + ind + tail;
            if (!emitReplace(f, emit, s->begin, s->end, text)) return;
        }
        // shared head -> before the if
        if (!th.empty() && !el.empty() && movable(th.front()) &&
            norm(f.textOf(*th.front())) == norm(f.textOf(*el.front())) &&
            !conflictsRelaxed(effectsOfStmt(f, *th.front()), ce)) {
            std::string head = f.textOf(*th.front());
            std::string text = head + "\n" + ind +
                               build({th.begin() + 1, th.end()}, {el.begin() + 1, el.end()}, {}, {});
            if (!emitReplace(f, emit, s->begin, s->end, text)) return;
        }
        auto& sib = s->parent->kids;
        int i = indexInParent(*s);
        // the next statement -> end of both branches
        bool thJumps = !th.empty() && isJump(th.back()), elJumps = !el.empty() && isJump(el.back());
        if (i + 1 < (int)sib.size() && movable(sib[i + 1].get()) && !thJumps && !elJumps) {
            const Stmt* n = sib[i + 1].get();
            std::string text = build(th, el, {}, {f.textOf(*n)});
            if (!emitReplace(f, emit, s->begin, n->end, text)) return;
        }
        // the previous statement -> start of both branches
        if (i > 0 && movable(sib[i - 1].get()) &&
            !conflictsRelaxed(effectsOfStmt(f, *sib[i - 1]), ce)) {
            const Stmt* p = sib[i - 1].get();
            std::string text = build(th, el, {f.textOf(*p)}, {});
            if (!emitReplace(f, emit, p->begin, s->end, text)) return;
        }
    }
}

// ---------------------------------------------------------------------------
// cond_split: "if (a && b) S" <-> "if (a) { if (b) S }"

void enumCondSplit(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::If)) {
        if (s->kids.size() != 1) continue;
        std::string ind = lineInd(f, s->begin), in = ind + "    ";
        auto c = parseExpr(f, s->condOpen + 1, s->condClose);
        if (c && c->k == Expr::Binary && f.t(c->op) == "&&") {
            const Stmt* th = s->kids[0].get();
            std::string inner = "if (" + exprText(f, *c->kids[1]) + ")" + bodyText(f, th, in);
            std::string text = "if (" + exprText(f, *c->kids[0]) + ")\n" + ind + "{\n" + in + inner +
                               "\n" + ind + "}";
            if (!emitReplace(f, emit, s->begin, s->end, text)) return;
        }
        const Stmt* inner = singleStmt(s->kids[0].get());
        if (inner && inner->kind == SK::If && inner->kids.size() == 1) {
            auto a = parseExpr(f, s->condOpen + 1, s->condClose);
            auto b = parseExpr(f, inner->condOpen + 1, inner->condClose);
            if (!a || !b) continue;
            std::string text = "if (" + operandText(f, *a, 4) + " && " + operandText(f, *b, 5) +
                               ")" + bodyText(f, inner->kids[0].get(), ind);
            if (!emitReplace(f, emit, s->begin, s->end, text)) return;
        }
    }
}

// ---------------------------------------------------------------------------
// explicit_compare: "if (x)" <-> "if (x != 0)", "if (!x)" <-> "if (x == 0)"

void enumExplicitCompare(const Func& f, const EmitFn& emit) {
    std::vector<std::pair<int, int>> conds;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if ((s.kind == SK::If || s.kind == SK::While || s.kind == SK::Do) && s.condOpen >= 0)
            conds.push_back({s.condOpen + 1, s.condClose});
    });
    for (auto [b, e] : conds) {
        auto root = parseExpr(f, b, e);
        if (!root) continue;
        bool stop = false;
        // ctx: precedence of the operator the node is an operand of (0 = whole condition)
        std::function<void(const Expr&, int)> walk = [&](const Expr& x, int ctx) {
            if (stop) return;
            auto replaceWith = [&](const std::string& t, int prec) {
                std::string text = prec <= ctx ? paren(t) : t;
                if (!emitReplace(f, emit, x.b, x.e, text)) stop = true;
            };
            if (x.k == Expr::Binary && (f.t(x.op) == "&&" || f.t(x.op) == "||")) {
                int p = binPrec(f.t(x.op));
                walk(*x.kids[0], p);
                walk(*x.kids[1], p);
                return;
            }
            if (x.k == Expr::Paren) {
                walk(*x.kids[0], 0);
                return;
            }
            if (x.k == Expr::Unary && f.t(x.op) == "!") {
                replaceWith(operandText(f, *x.kids[0], 9) + " == 0", 9);
                return;
            }
            if (x.k == Expr::Binary && (f.t(x.op) == "!=" || f.t(x.op) == "==") &&
                isZero(norm(exprText(f, *x.kids[1])))) {
                const Expr& o = *x.kids[0];
                if (f.t(x.op) == "!=") replaceWith(operandText(f, o, ctx == 100 ? 99 : ctx), 100);
                else replaceWith("!" + (isPostfixLike(o) ? exprText(f, o) : paren(exprText(f, o))), 99);
                return;
            }
            if (isPostfixLike(x) || x.k == Expr::Cast)
                replaceWith(operandText(f, x, 9) + " != 0", 9);
        };
        walk(*root, 0);
        if (stop) return;
    }
}

// ---------------------------------------------------------------------------
// negate_const: "x - 4" <-> "x + -4", "x -= 4" <-> "x += -4"

void enumNegateConst(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        if (x.k != Expr::Binary && x.k != Expr::Assign) return true;
        const std::string& op = f.t(x.op);
        const Expr& r = *x.kids[1];
        std::string lt = exprText(f, *x.kids[0]);
        bool num = r.k == Expr::Primary && f.toks[r.b].kind == TokKind::Number;
        bool negNum = r.k == Expr::Unary && f.t(r.op) == "-" && r.kids[0]->k == Expr::Primary &&
                      f.toks[r.kids[0]->b].kind == TokKind::Number;
        std::string text;
        if (op == "-" && num) text = lt + " + -" + exprText(f, r);
        else if (op == "+" && negNum) text = lt + " - " + exprText(f, *r.kids[0]);
        else if (op == "-=" && num) text = lt + " += -" + exprText(f, r);
        else if (op == "+=" && negNum) text = lt + " -= " + exprText(f, *r.kids[0]);
        else return true;
        return emitReplace(f, emit, x.b, x.e, text);
    });
}

// ---------------------------------------------------------------------------
// loop_form: for <-> while

void enumLoopForm(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::For)) {
        if (!inBlock(*s) || s->kids.empty()) continue;
        std::vector<std::string> parts;
        int depth = 0, segStart = s->condOpen + 1;
        for (int i = s->condOpen + 1; i <= s->condClose; ++i) {
            const std::string& x = f.t(i);
            if (i < s->condClose && (x == "(" || x == "[")) depth++;
            if (i < s->condClose && (x == ")" || x == "]")) depth--;
            if ((x == ";" && depth == 0) || i == s->condClose) {
                parts.push_back(i > segStart ? f.tokText(segStart, i) : "");
                segStart = i + 1;
            }
        }
        if (parts.size() != 3) continue;
        const Stmt* body = s->kids[0].get();
        if (!parts[2].empty() && hasToken(f, *body, "continue")) continue;
        std::string ind = lineInd(f, s->begin);
        std::string text;
        if (!parts[0].empty()) text += parts[0] + ";\n" + ind;
        std::vector<std::string> post;
        if (!parts[2].empty()) post.push_back(parts[2] + ";");
        text += "while (" + (parts[1].empty() ? std::string("1") : parts[1]) + ")\n" + ind +
                blockText(f, branchStmts(body), ind, {}, post);
        if (!emitReplace(f, emit, s->begin, s->end, text)) return;
    }
    for (const Stmt* s : stmtsOfKind(f, SK::While)) {
        if (!inBlock(*s) || s->kids.empty()) continue;
        std::string c = f.tokText(s->condOpen + 1, s->condClose);
        std::string ind = lineInd(f, s->begin);
        const Stmt* body = s->kids[0].get();
        std::string head = norm(c) == "1" || norm(c) == "true" ? "for (;;)" : "for (; " + c + ";)";
        if (!emitReplace(f, emit, s->begin, s->end, head + bodyText(f, body, ind))) return;
        // "init; while (c) { ...; step; }" -> "for (init; c; step) { ... }"
        int i = indexInParent(*s);
        if (i <= 0 || body->kind != SK::Block || body->kids.empty()) continue;
        const Stmt* init = s->parent->kids[i - 1].get();
        const Stmt* step = body->kids.back().get();
        if ((init->kind != SK::Expr && init->kind != SK::Decl) || step->kind != SK::Expr) continue;
        if (hasToken(f, *body, "continue")) continue;
        Effects ie = effectsOfStmt(f, *init), se = effectsOfStmt(f, *step);
        bool shared = false;
        for (auto& w : ie.writes) shared |= se.writes.count(w) > 0;
        if (!shared) continue;
        std::vector<const Stmt*> rest;
        for (size_t k = 0; k + 1 < body->kids.size(); ++k) rest.push_back(body->kids[k].get());
        std::string text = "for (" + f.tokText(init->begin, init->end - 1) + "; " + c + "; " +
                           f.tokText(step->begin, step->end - 1) + ")\n" + ind + blockText(f, rest, ind);
        if (!emitReplace(f, emit, init->begin, s->end, text)) return;
    }
}

// ---------------------------------------------------------------------------
// reassociate: "a + b + c" <-> "a + (b + c)"

void enumReassociate(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        if (x.k != Expr::Binary) return true;
        const std::string& op = f.t(x.op);
        if (op != "+" && op != "*" && op != "&" && op != "|" && op != "^") return true;
        const Expr& l = *x.kids[0];
        const Expr& r = *x.kids[1];
        if (l.k == Expr::Binary && f.t(l.op) == op) {
            std::string text = exprText(f, *l.kids[0]) + " " + op + " (" + exprText(f, *l.kids[1]) +
                               " " + op + " " + exprText(f, r) + ")";
            if (!emitReplace(f, emit, x.b, x.e, text)) return false;
        }
        if (r.k == Expr::Paren && r.kids[0]->k == Expr::Binary && f.t(r.kids[0]->op) == op) {
            const Expr& in = *r.kids[0];
            std::string text = exprText(f, l) + " " + op + " " + exprText(f, *in.kids[0]) + " " + op +
                               " " + operandText(f, *in.kids[1], binPrec(op));
            if (!emitReplace(f, emit, x.b, x.e, text)) return false;
        }
        return true;
    });
}

} // namespace perm::util
