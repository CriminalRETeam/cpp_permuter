#include "passes.hpp"

#include "analysis.hpp"
#include "pass_util.hpp"

#include <algorithm>
#include <numeric>
#include <set>
#include <sstream>

namespace perm {

// ---------------------------------------------------------------------------
// Rewriter

Rewriter::Rewriter(const Func& f) : f_(f) {
    size_t n = f.toks.size();
    lead_.resize(n);
    text_.resize(n);
    prefix_.resize(n);
    touched_.assign(n, false);
    for (size_t i = 0; i < n; ++i) {
        lead_[i] = f.toks[i].lead;
        text_[i] = f.toks[i].text;
    }
}

void Rewriter::replace(int b, int e, const std::string& text) {
    for (int i = b; i < e; ++i) {
        if (touched_[i]) overlap_ = true;
        touched_[i] = true;
        if (i != b) lead_[i].clear();
        text_[i].clear();
    }
    if (b < e) text_[b] = text;
    else overlap_ = true;
}

void Rewriter::remove(int b, int e) {
    replace(b, e, "");
    if (overlap_) return;
    std::string& l = lead_[b];
    size_t nl = l.rfind('\n');
    l = nl == std::string::npos ? "" : l.substr(0, nl);
}

void Rewriter::insertBefore(int tok, const std::string& stmtText) {
    prefix_[tok] += stmtText + f_.indentOf(tok);
}

std::string Rewriter::apply() const {
    if (overlap_) return "";
    std::string out;
    out.reserve(f_.text.size() + 256);
    for (size_t i = 0; i < f_.toks.size(); ++i) {
        out += lead_[i];
        out += prefix_[i];
        out += text_[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// helpers (shared with the other pass files through pass_util.hpp)

namespace util {

std::vector<const Stmt*> blocksOf(const Func& f) {
    std::vector<const Stmt*> r;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind == SK::Block) r.push_back(&s);
    });
    return r;
}

std::string norm(const std::string& s) {
    std::string r;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') r += c;
    return r;
}

std::string exprText(const Func& f, const Expr& x) { return f.tokText(x.b, x.e); }

bool isPostfixLike(const Expr& x) {
    return x.k == Expr::Primary || x.k == Expr::Paren || x.k == Expr::Call ||
           x.k == Expr::Index || x.k == Expr::Member || x.k == Expr::Postfix;
}

int exprPrec(const Func& f, const Expr& x) {
    switch (x.k) {
    case Expr::Comma: return 1;
    case Expr::Assign: return 2;
    case Expr::Ternary: return 3;
    case Expr::Binary: return binPrec(f.t(x.op));
    default: return 100;
    }
}

std::string paren(const std::string& s) { return "(" + s + ")"; }

// Text of x for use as an operand of an operator with precedence prec.
std::string operandText(const Func& f, const Expr& x, int prec) {
    std::string s = exprText(f, x);
    return exprPrec(f, x) <= prec ? paren(s) : s;
}

bool insideLoopNotContaining(const Func& f, int tok, const Stmt& s) {
    bool r = false;
    forEachStmt(*f.body, [&](const Stmt& l) {
        if (l.kind != SK::For && l.kind != SK::While && l.kind != SK::Do) return;
        if (tok >= l.begin && tok < l.end && !(s.begin >= l.begin && s.begin < l.end)) r = true;
    });
    return r;
}

std::string declTypeText(const Func& f, const Stmt& s) { return f.tokText(s.typeBegin, s.typeEnd); }

// "T name" for declarator d of s, without its initializer.
std::string declNoInit(const Func& f, const Stmt& s, const Declarator& d) {
    if (&d == &s.decls[0]) return f.tokText(s.typeBegin, d.declEnd);
    std::string t = declTypeText(f, s);
    std::string rest = f.tokText(d.begin, d.declEnd);
    if (f.t(d.begin) == "*" || f.t(d.begin) == "&") return t + rest;
    return t + " " + rest;
}

// Whether moving an initializer with effects ie past code with effects between
// could change what it computes. Locals are tracked exactly; memory is only
// checked for calls, since a byte-for-byte match proves the rewrite anyway.
bool initBlocked(const Effects& ie, const Effects& between) {
    for (auto& r : ie.reads)
        if (between.writes.count(r)) return true;
    if ((ie.call || ie.memRead) && between.call) return true;
    if (ie.call && between.memWrite) return true;
    return false;
}

bool simpleSingleDecl(const Stmt& s) {
    return s.kind == SK::Decl && !s.isStatic && s.decls.size() == 1 && s.parent &&
           s.parent->kind == SK::Block;
}

int indexInParent(const Stmt& s) {
    auto& k = s.parent->kids;
    for (size_t i = 0; i < k.size(); ++i)
        if (k[i].get() == &s) return (int)i;
    return -1;
}

// All orders of n items that keep every conflicting pair in its original order.
void topoOrders(int n, const std::vector<std::vector<bool>>& conf, size_t cap,
                std::vector<std::vector<int>>& out) {
    std::vector<int> cur;
    std::vector<bool> used(n, false);
    std::function<void()> rec = [&]() {
        if (out.size() >= cap) return;
        if ((int)cur.size() == n) {
            out.push_back(cur);
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (used[i]) continue;
            bool ok = true;
            for (int j = 0; j < i && ok; ++j)
                if (conf[i][j] && !used[j]) ok = false;
            if (!ok) continue;
            used[i] = true;
            cur.push_back(i);
            rec();
            cur.pop_back();
            used[i] = false;
        }
    };
    rec();
}

std::vector<int> randomTopo(int n, const std::vector<std::vector<bool>>& conf, Rng& rng) {
    std::vector<int> cur;
    std::vector<bool> used(n, false);
    while ((int)cur.size() < n) {
        std::vector<int> avail;
        for (int i = 0; i < n; ++i) {
            if (used[i]) continue;
            bool ok = true;
            for (int j = 0; j < i && ok; ++j)
                if (conf[i][j] && !used[j]) ok = false;
            if (ok) avail.push_back(i);
        }
        int pick = avail[std::uniform_int_distribution<size_t>(0, avail.size() - 1)(rng)];
        used[pick] = true;
        cur.push_back(pick);
    }
    return cur;
}

// ---------------------------------------------------------------------------
// reorder_saves

struct Run {
    std::vector<const Stmt*> items;
    std::vector<std::vector<bool>> conf;
};

bool isSave(const Func& f, const Stmt& s, const Effects& e) {
    if (e.call || e.memWrite || e.barrier) return false;
    if (s.kind == SK::Decl) return !s.isStatic;
    if (s.kind == SK::Expr) return f.isLocal(s.begin) && f.t(s.begin + 1) == "=";
    return false;
}

std::vector<Run> saveRuns(const Func& f) {
    std::vector<Run> runs;
    for (const Stmt* b : blocksOf(f)) {
        std::vector<const Stmt*> cur;
        std::vector<Effects> effs;
        auto flush = [&]() {
            if (cur.size() >= 2) {
                Run r;
                r.items = cur;
                size_t n = cur.size();
                r.conf.assign(n, std::vector<bool>(n, false));
                for (size_t i = 0; i < n; ++i)
                    for (size_t j = 0; j < n; ++j)
                        if (i != j) r.conf[i][j] = conflicts(effs[i], effs[j]);
                runs.push_back(std::move(r));
            }
            cur.clear();
            effs.clear();
        };
        for (auto& k : b->kids) {
            Effects e = effectsOfStmt(f, *k);
            if (isSave(f, *k, e)) {
                cur.push_back(k.get());
                effs.push_back(e);
            } else {
                flush();
            }
        }
        flush();
    }
    return runs;
}

std::string applyOrders(const Func& f, const std::vector<Run>& runs,
                        const std::vector<std::vector<int>>& orders) {
    Rewriter rw(f);
    for (size_t r = 0; r < runs.size(); ++r) {
        auto& items = runs[r].items;
        auto& ord = orders[r];
        for (size_t k = 0; k < items.size(); ++k)
            if (ord[k] != (int)k)
                rw.replace(items[k]->begin, items[k]->end, f.textOf(*items[ord[k]]));
    }
    return rw.apply();
}

void enumReorderSaves(const Func& f, const EmitFn& emit) {
    std::vector<Run> runs = saveRuns(f);
    if (runs.empty()) return;
    std::vector<std::vector<std::vector<int>>> perRun;
    for (auto& r : runs) {
        std::vector<std::vector<int>> o;
        topoOrders((int)r.items.size(), r.conf, 40320, o);
        perRun.push_back(std::move(o));
    }
    // odometer over the cartesian product of every run's orders
    auto shared = std::make_shared<const std::vector<Run>>(runs);
    std::vector<size_t> idx(runs.size(), 0);
    while (true) {
        std::vector<std::vector<int>> pick;
        bool identity = true;
        for (size_t r = 0; r < runs.size(); ++r) {
            pick.push_back(perRun[r][idx[r]]);
            for (size_t k = 0; k < pick.back().size(); ++k)
                if (pick.back()[k] != (int)k) identity = false;
        }
        if (!identity && !emit([&f, shared, pick]() { return applyOrders(f, *shared, pick); }))
            return;
        size_t r = 0;
        while (r < runs.size() && ++idx[r] == perRun[r].size()) idx[r++] = 0;
        if (r == runs.size()) break;
    }
}

bool randomReorderSaves(const Func& f, Rng& rng, std::string& out) {
    std::vector<Run> runs = saveRuns(f);
    if (runs.empty()) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::vector<std::vector<int>> orders;
        size_t which = std::uniform_int_distribution<size_t>(0, runs.size() - 1)(rng);
        bool changed = false;
        for (size_t r = 0; r < runs.size(); ++r) {
            std::vector<int> id(runs[r].items.size());
            std::iota(id.begin(), id.end(), 0);
            if (r == which) {
                auto o = randomTopo((int)id.size(), runs[r].conf, rng);
                changed = o != id;
                orders.push_back(o);
            } else {
                orders.push_back(id);
            }
        }
        if (!changed) continue;
        out = applyOrders(f, runs, orders);
        return !out.empty();
    }
    return false;
}

// ---------------------------------------------------------------------------
// move_stmt

void enumMoveStmt(const Func& f, const EmitFn& emit) {
    for (const Stmt* b : blocksOf(f)) {
        auto& kids = b->kids;
        size_t n = kids.size();
        if (n < 2) continue;
        std::vector<Effects> effs;
        for (auto& k : kids) effs.push_back(effectsOfStmt(f, *k));
        for (size_t i = 0; i < n; ++i) {
            if (effs[i].barrier || kids[i]->kind == SK::Empty) continue;
            for (int dir = -1; dir <= 1; dir += 2) {
                for (long j = (long)i + dir; j >= 0 && j < (long)n; j += dir) {
                    if (conflictsRelaxed(effs[i], effs[j]) || kids[j]->kind == SK::Empty) break;
                    // move i to position j
                    std::vector<int> order;
                    for (size_t k = 0; k < n; ++k)
                        if (k != i) order.push_back((int)k);
                    order.insert(order.begin() + j, (int)i);
                    size_t lo = std::min<size_t>(i, j), hi = std::max<size_t>(i, j);
                    if (!emit([&f, b, order, lo, hi]() {
                            Rewriter rw(f);
                            for (size_t k = lo; k <= hi; ++k)
                                rw.replace(b->kids[k]->begin, b->kids[k]->end,
                                           f.textOf(*b->kids[order[k]]));
                            return rw.apply();
                        }))
                        return;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// inline_local

void enumInlineLocal(const Func& f, const EmitFn& emit) {
    std::vector<const Stmt*> decls;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (simpleSingleDecl(s)) decls.push_back(&s);
    });
    for (const Stmt* s : decls) {
        const Declarator& d = s->decls[0];
        if (d.isArray) continue;
        std::string name = f.t(d.nameTok);
        std::vector<int> uses = usesOf(f, name, s->end, f.bodyClose);
        if (d.init != Declarator::Assign || f.t(d.initBegin) == "{") {
            if (uses.empty() && d.init == Declarator::None)
                emit([&f, s]() {
                    Rewriter rw(f);
                    rw.remove(s->begin, s->end);
                    return rw.apply();
                });
            continue;
        }
        Effects ie = effectsOf(f, d.initBegin, d.initEnd);
        if (!ie.writes.empty() || ie.memWrite || ie.barrier) continue;
        std::string init = f.tokText(d.initBegin, d.initEnd);
        if (uses.empty()) {
            // dead local: drop it, keeping a call for its side effects
            bool keep = ie.call;
            if (!emit([&f, s, init, keep]() {
                    Rewriter rw(f);
                    if (keep) rw.replace(s->begin, s->end, init + ";");
                    else rw.remove(s->begin, s->end);
                    return rw.apply();
                }))
                return;
            continue;
        }
        bool anyWrite = false;
        for (int u : uses) anyWrite |= isWriteUse(f, u);
        if (anyWrite) continue;
        bool pure = !ie.call;
        if (uses.size() > 1 && !pure) continue;
        Effects between = effectsOf(f, s->end, uses.back());
        bool trivially = pure && !ie.memRead && ie.reads.empty();
        if (!trivially && (initBlocked(ie, between) || (between.barrier && ie.call))) continue;
        bool inLoop = false;
        for (int u : uses) inLoop |= insideLoopNotContaining(f, u, *s);
        if (inLoop && !trivially) continue;

        auto x = parseExpr(f, d.initBegin, d.initEnd);
        bool postfixLike = x && isPostfixLike(*x);
        bool commaFree = x && x->k != Expr::Comma;
        // A use that is a whole operand on its own (an assignment's right side,
        // an argument, a return value, an index) needs no parentheses.
        auto standalone = [&](int u) {
            const std::string& p = f.t(u - 1);
            const std::string& n = f.t(u + 1);
            bool before = isAssignOp(p) || p == "(" || p == "," || p == "return" || p == "[";
            bool after = n == ";" || n == ")" || n == "," || n == "]";
            return before && after && commaFree;
        };
        std::string castT = declTypeText(f, *s) + f.tokText(d.begin, d.nameTok);
        std::vector<int> casts = {0};
        if (uses.size() == 1 && !d.isRef && !s->isConst) casts.push_back(1);
        for (int cast : casts) {
            std::vector<std::string> texts;
            for (int u : uses) {
                if (cast) texts.push_back("((" + castT + ")" + paren(init) + ")");
                else texts.push_back(postfixLike || standalone(u) ? init : paren(init));
            }
            if (!emit([&f, s, uses, texts]() {
                    Rewriter rw(f);
                    rw.remove(s->begin, s->end);
                    for (size_t k = 0; k < uses.size(); ++k) rw.replace(uses[k], uses[k] + 1, texts[k]);
                    return rw.apply();
                }))
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// sink_decl

void enumSinkDecl(const Func& f, const EmitFn& emit) {
    std::vector<const Stmt*> decls;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (simpleSingleDecl(s)) decls.push_back(&s);
    });
    for (const Stmt* s : decls) {
        const Stmt* P = s->parent;
        int di = indexInParent(*s);
        const Declarator& d = s->decls[0];
        std::string name = f.t(d.nameTok);
        std::vector<int> uses = usesOf(f, name, s->end, P->end);
        if (uses.empty()) continue;
        // innermost block holding every use
        const Stmt* B = P;
        forEachStmt(*P, [&](const Stmt& x) {
            if (x.kind == SK::Block && x.begin < uses.front() && uses.back() < x.end &&
                x.end - x.begin < B->end - B->begin)
                B = &x;
        });
        auto itemAt = [&](const Stmt* blk) {
            for (size_t k = 0; k < blk->kids.size(); ++k)
                if (blk->kids[k]->end > uses.front()) return (int)k;
            return -1;
        };
        std::vector<std::pair<const Stmt*, int>> targets;
        int tb = itemAt(B);
        if (tb >= 0 && (B != P || tb > di + 1)) targets.push_back({B, tb});
        int tp = itemAt(P);
        if (B != P && tp > di + 1) targets.push_back({P, tp});
        Effects ie = d.init == Declarator::None ? Effects{} : effectsOf(f, d.initBegin, d.initEnd);
        for (auto [blk, t] : targets) {
            const Stmt& item = *blk->kids[t];
            int at = item.begin;
            if (d.init != Declarator::None) {
                Effects between = effectsOf(f, s->end, at);
                if (initBlocked(ie, between)) continue;
                if (insideLoopNotContaining(f, at, *s)) continue;
            } else if (insideLoopNotContaining(f, at, *s)) {
                // only when the loop body starts by assigning it
                if (!(item.kind == SK::Expr && item.begin == uses.front() && f.t(at + 1) == "="))
                    continue;
            }
            if (!emit([&f, s, at]() {
                    Rewriter rw(f);
                    rw.remove(s->begin, s->end);
                    rw.insertBefore(at, f.textOf(*s));
                    return rw.apply();
                }))
                return;
        }
    }
}

// ---------------------------------------------------------------------------
// hoist_decl

struct Hoist {
    const Stmt* s;
    std::string top;      // declarations to place at the top
    std::string leftover; // what stays in place ("" to remove)
};

bool makeHoist(const Func& f, const Stmt& s, Hoist& h) {
    if (s.kind != SK::Decl || s.isStatic || !s.parent || s.parent->kind != SK::Block) return false;
    h.s = &s;
    for (auto& d : s.decls) {
        if (d.init == Declarator::None) {
            h.top += declNoInit(f, s, d) + ";";
            continue;
        }
        if (d.init != Declarator::Assign || s.isConst || d.isRef || d.isArray ||
            f.t(d.initBegin) == "{")
            return false;
        if (!h.top.empty()) h.top += " ";
        h.top += declNoInit(f, s, d) + ";";
        if (!h.leftover.empty()) h.leftover += " ";
        h.leftover += f.t(d.nameTok) + " = " + f.tokText(d.initBegin, d.initEnd) + ";";
    }
    return true;
}

// First item of blk that isn't a declaration, or -1.
int firstNonDecl(const Stmt& blk) {
    for (size_t k = 0; k < blk.kids.size(); ++k)
        if (blk.kids[k]->kind != SK::Decl && blk.kids[k]->kind != SK::Empty) return (int)k;
    return -1;
}

void applyHoist(Rewriter& rw, const Hoist& h, int at) {
    if (h.leftover.empty()) rw.remove(h.s->begin, h.s->end);
    else rw.replace(h.s->begin, h.s->end, h.leftover);
    rw.insertBefore(at, h.top);
}

void enumHoistDecl(const Func& f, const EmitFn& emit) {
    std::vector<Hoist> hs;
    forEachStmt(*f.body, [&](const Stmt& s) {
        Hoist h;
        if (makeHoist(f, s, h)) hs.push_back(h);
    });
    int topIdx = firstNonDecl(*f.body);
    if (topIdx < 0) return;
    int topTok = f.body->kids[topIdx]->begin;
    std::vector<const Hoist*> toTop;
    for (auto& h : hs) {
        const Stmt* P = h.s->parent;
        bool inTopRun = P == f.body.get() && indexInParent(*h.s) < topIdx;
        if (!inTopRun) {
            toTop.push_back(&h);
            if (!emit([&f, h, topTok]() {
                    Rewriter rw(f);
                    applyHoist(rw, h, topTok);
                    return rw.apply();
                }))
                return;
        }
        if (P != f.body.get()) {
            int bi = firstNonDecl(*P);
            if (bi >= 0 && indexInParent(*h.s) > bi) {
                int at = P->kids[bi]->begin;
                if (!emit([&f, h, at]() {
                        Rewriter rw(f);
                        applyHoist(rw, h, at);
                        return rw.apply();
                    }))
                    return;
            }
        }
    }
    if (toTop.size() >= 2) {
        std::vector<Hoist> all;
        for (auto* h : toTop) all.push_back(*h);
        emit([&f, all, topTok]() {
            Rewriter rw(f);
            for (auto& h : all) applyHoist(rw, h, topTok);
            return rw.apply();
        });
    }
}

// ---------------------------------------------------------------------------
// merge_decl: "T x; ... x = e;" -> "T x = e;"

void enumMergeDecl(const Func& f, const EmitFn& emit) {
    std::vector<const Stmt*> decls;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (simpleSingleDecl(s) && s.decls[0].init == Declarator::None && !s.decls[0].isArray)
            decls.push_back(&s);
    });
    for (const Stmt* s : decls) {
        const Declarator& d = s->decls[0];
        std::string name = f.t(d.nameTok);
        std::vector<int> uses = usesOf(f, name, s->end, s->parent->end);
        if (uses.empty() || f.t(uses[0] + 1) != "=") continue;
        const Stmt* A = nullptr;
        forEachStmt(*f.body, [&](const Stmt& x) {
            if (x.kind == SK::Expr && x.begin == uses[0]) A = &x;
        });
        if (!A || !A->parent || A->parent->kind != SK::Block) continue;
        if (uses.back() >= A->parent->end) continue;
        if (uses.size() > 1 && uses[1] < A->end) continue; // "x = x + 1"
        std::string text = declNoInit(f, *s, d) + " = " + f.tokText(A->begin + 2, A->end - 1) + ";";
        if (!emit([&f, s, A, text]() {
                Rewriter rw(f);
                rw.remove(s->begin, s->end);
                rw.replace(A->begin, A->end, text);
                return rw.apply();
            }))
            return;
    }
}

// ---------------------------------------------------------------------------
// expression passes

void enumSwapOperands(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr> root, Expr& x) {
        if (x.k != Expr::Binary) return true;
        const std::string& op = f.t(x.op);
        if (op != "+" && op != "*" && op != "&" && op != "|" && op != "^" && op != "==" && op != "!=")
            return true;
        int p = binPrec(op);
        const Expr& l = *x.kids[0];
        const Expr& r = *x.kids[1];
        if (norm(exprText(f, l)) == norm(exprText(f, r))) return true; // "x * x"
        bool assoc = op == "+" || op == "*" || op == "&" || op == "|" || op == "^";
        std::string lt = exprText(f, l);
        if (exprPrec(f, l) < p || (exprPrec(f, l) == p && !(assoc && f.t(l.op) == op)))
            lt = paren(lt);
        std::string text = exprText(f, r) + " " + op + " " + lt;
        int b = x.b, e = x.e;
        return emit([&f, b, e, text, root]() {
            Rewriter rw(f);
            rw.replace(b, e, text);
            return rw.apply();
        });
    });
}

void enumFlipCompare(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr> root, Expr& x) {
        if (x.k != Expr::Binary) return true;
        const std::string& op = f.t(x.op);
        std::string m = op == "<" ? ">" : op == ">" ? "<" : op == "<=" ? ">=" : op == ">=" ? "<=" : "";
        if (m.empty()) return true;
        std::string text = exprText(f, *x.kids[1]) + " " + m + " " + operandText(f, *x.kids[0], 10);
        int b = x.b, e = x.e;
        return emit([&f, b, e, text, root]() {
            Rewriter rw(f);
            rw.replace(b, e, text);
            return rw.apply();
        });
    });
}

std::string negate(const Func& f, int b, int e) {
    auto x = parseExpr(f, b, e);
    std::string all = f.tokText(b, e);
    if (!x) return "!(" + all + ")";
    if (x->k == Expr::Binary) {
        const std::string& op = f.t(x->op);
        std::string n = op == "==" ? "!=" : op == "!=" ? "==" : op == "<" ? ">=" : op == ">=" ? "<"
                      : op == ">" ? "<=" : op == "<=" ? ">" : "";
        if (!n.empty()) return exprText(f, *x->kids[0]) + " " + n + " " + exprText(f, *x->kids[1]);
    }
    if (x->k == Expr::Unary && f.t(x->op) == "!") {
        const Expr& o = *x->kids[0];
        if (o.k == Expr::Paren) return exprText(f, *o.kids[0]);
        return exprText(f, o);
    }
    if (isPostfixLike(*x)) return "!" + all;
    return "!(" + all + ")";
}

void enumInvertIf(const Func& f, const EmitFn& emit) {
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind != SK::If || s.kids.size() != 2) return;
        const Stmt* th = s.kids[0].get();
        const Stmt* el = s.kids[1].get();
        std::string neg = negate(f, s.condOpen + 1, s.condClose);
        std::string elText = f.textOf(*el);
        if (el->kind == SK::If) {
            std::string ind = f.indentOf(s.begin);
            std::string inner;
            for (char c : elText) inner += c == '\n' ? std::string("\n    ") : std::string(1, c);
            elText = "{" + ind + "    " + inner + ind + "}";
        }
        const Stmt* sp = &s;
        emit([&f, sp, th, el, neg, elText]() {
            Rewriter rw(f);
            rw.replace(sp->condOpen + 1, sp->condClose, neg);
            rw.replace(th->begin, th->end, elText);
            rw.replace(el->begin, el->end, f.textOf(*th));
            return rw.apply();
        });
    });
}

// The one expression statement inside s (s itself, or a block holding just it).
const Stmt* singleStmt(const Stmt* s) {
    while (s && s->kind == SK::Block) {
        if (s->kids.size() != 1) return nullptr;
        s = s->kids[0].get();
    }
    return s;
}

void enumTernary(const Func& f, const EmitFn& emit) {
    auto emitReplace = [&](int b, int e, std::string text) {
        return emit([&f, b, e, text]() {
            Rewriter rw(f);
            rw.replace(b, e, text);
            return rw.apply();
        });
    };
    auto branch = [&](const Expr& x) {
        return x.k == Expr::Assign || x.k == Expr::Comma ? paren(exprText(f, x)) : exprText(f, x);
    };
    auto condText = [&](const Expr& c) {
        return c.k == Expr::Paren ? exprText(f, *c.kids[0]) : exprText(f, c);
    };
    // ternary -> if/else
    for (auto& site : exprSites(f)) {
        const Stmt& s = *site.stmt;
        if (s.kind != SK::Expr && s.kind != SK::Return) continue;
        auto x = parseExpr(f, site.b, site.e);
        if (!x) continue;
        std::string ind = f.indentOf(s.begin), in = ind + "    ";
        if (s.kind == SK::Expr && x->k == Expr::Assign && f.t(x->op) == "=" &&
            x->kids[1]->k == Expr::Ternary) {
            const Expr& t = *x->kids[1];
            Effects le = effectsOf(f, x->kids[0]->b, x->kids[0]->e);
            if (le.call || le.memWrite) continue;
            std::string L = exprText(f, *x->kids[0]);
            std::string text = "if (" + condText(*t.kids[0]) + ")" + ind + "{" + in + L + " = " +
                               exprText(f, *t.kids[1]) + ";" + ind + "}" + ind + "else" + ind +
                               "{" + in + L + " = " + exprText(f, *t.kids[2]) + ";" + ind + "}";
            if (!emitReplace(s.begin, s.end, text)) return;
        } else if (s.kind == SK::Return && x->k == Expr::Ternary) {
            std::string c = condText(*x->kids[0]);
            std::string a = exprText(f, *x->kids[1]), b = exprText(f, *x->kids[2]);
            std::string withElse = "if (" + c + ")" + ind + "{" + in + "return " + a + ";" + ind +
                                   "}" + ind + "else" + ind + "{" + in + "return " + b + ";" +
                                   ind + "}";
            std::string early = "if (" + c + ")" + ind + "{" + in + "return " + a + ";" + ind +
                                "}" + ind + "return " + b + ";";
            if (!emitReplace(s.begin, s.end, withElse)) return;
            if (!emitReplace(s.begin, s.end, early)) return;
        }
    }
    // if/else -> ternary
    std::vector<const Stmt*> ifs;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind == SK::If) ifs.push_back(&s);
    });
    for (const Stmt* s : ifs) {
        auto c = parseExpr(f, s->condOpen + 1, s->condClose);
        if (!c) continue;
        std::string ct = c->k == Expr::Assign || c->k == Expr::Ternary || c->k == Expr::Comma
                             ? paren(exprText(f, *c))
                             : exprText(f, *c);
        const Stmt* th = singleStmt(s->kids[0].get());
        const Stmt* el = s->kids.size() == 2 ? singleStmt(s->kids[1].get()) : nullptr;
        if (!th) continue;
        if (el && th->kind == SK::Expr && el->kind == SK::Expr) {
            auto a = parseExpr(f, th->begin, th->end - 1);
            auto b = parseExpr(f, el->begin, el->end - 1);
            if (!a || !b || a->k != Expr::Assign || b->k != Expr::Assign) continue;
            if (f.t(a->op) != "=" || f.t(b->op) != "=") continue;
            std::string L = exprText(f, *a->kids[0]);
            if (norm(L) != norm(exprText(f, *b->kids[0]))) continue;
            std::string text = L + " = " + ct + " ? " + branch(*a->kids[1]) + " : " +
                               branch(*b->kids[1]) + ";";
            if (!emitReplace(s->begin, s->end, text)) return;
            continue;
        }
        if (th->kind != SK::Return || f.t(th->begin) != "return") continue;
        auto a = parseExpr(f, th->begin + 1, th->end - 1);
        if (!a) continue;
        const Stmt* other = el;
        int end = s->end;
        if (!other && s->parent && s->parent->kind == SK::Block) {
            int i = indexInParent(*s);
            if (i >= 0 && i + 1 < (int)s->parent->kids.size()) {
                other = s->parent->kids[i + 1].get();
                end = other->end;
            }
        }
        if (!other || other->kind != SK::Return || f.t(other->begin) != "return") continue;
        auto b = parseExpr(f, other->begin + 1, other->end - 1);
        if (!b) continue;
        std::string text = "return " + ct + " ? " + branch(*a) + " : " + branch(*b) + ";";
        if (!emitReplace(s->begin, end, text)) return;
    }
}

void enumCompoundAssign(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr> root, Expr& x) {
        if (x.k != Expr::Assign) return true;
        const std::string& op = f.t(x.op);
        const Expr& L = *x.kids[0];
        const Expr& R = *x.kids[1];
        std::string lt = exprText(f, L);
        Effects le = effectsOf(f, L.b, L.e);
        if (le.call || le.memWrite) return true;
        std::string text;
        if (op == "=") {
            if (R.k != Expr::Binary) return true;
            const std::string& bop = f.t(R.op);
            static const char* ok[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"};
            if (std::find_if(std::begin(ok), std::end(ok), [&](const char* o) { return bop == o; }) ==
                std::end(ok))
                return true;
            if (norm(exprText(f, *R.kids[0])) != norm(lt)) return true;
            text = lt + " " + bop + "= " + exprText(f, *R.kids[1]);
        } else {
            std::string bop = op.substr(0, op.size() - 1);
            text = lt + " = " + lt + " " + bop + " " + operandText(f, R, binPrec(bop));
        }
        int b = x.b, e = x.e;
        return emit([&f, b, e, text, root]() {
            Rewriter rw(f);
            rw.replace(b, e, text);
            return rw.apply();
        });
    });
}

} // namespace util

// ---------------------------------------------------------------------------

const std::vector<Pass>& allPasses() {
    using namespace util;
    static const std::vector<Pass> passes = {
        {"reorder_saves",
         "Permute runs of consecutive local saves (declarations and assignments to locals "
         "that don't call or store) in every order that keeps dependencies",
         30, enumReorderSaves, randomReorderSaves},
        {"move_stmt",
         "Move a statement up or down past neighbours it doesn't depend on (locals and calls "
         "are respected, memory aliasing is not)",
         20,
         enumMoveStmt, nullptr},
        {"inline_local",
         "Remove a local: substitute its initializer into its uses (plain and with a cast), "
         "or drop it if unused",
         15, enumInlineLocal, nullptr},
        {"sink_decl", "Move a declaration into the innermost block holding all its uses, or to "
                      "just before its first use",
         15, enumSinkDecl, nullptr},
        {"hoist_decl",
         "Move declarations to the top of their block or of the function (one, or all of "
         "them), splitting the initializer into an assignment",
         15, enumHoistDecl, nullptr},
        {"merge_decl", "Join 'T x; ... x = e;' into 'T x = e;' at the assignment", 10,
         enumMergeDecl, nullptr},
        {"swap_operands", "Swap the operands of a commutative operator (+ * & | ^ == !=)", 15,
         enumSwapOperands, nullptr},
        {"flip_compare", "Mirror a comparison: 'a < b' -> 'b > a'", 10, enumFlipCompare, nullptr},
        {"invert_if", "Negate an if condition and swap the then/else branches", 10, enumInvertIf,
         nullptr},
        {"ternary", "Turn 'x = c ? a : b' / 'return c ? a : b' into if/else and back", 8,
         enumTernary, nullptr},
        {"compound_assign", "'x = x op y' <-> 'x op= y'", 8, enumCompoundAssign, nullptr},
        {"bool_return",
         "'return a <= b;' <-> 'if (a <= b) return true; return false;' (with and without "
         "else, true/false and 1/0)",
         8, enumBoolReturn, nullptr},
        {"ternary_arg",
         "'f(c ? a : b);' <-> 'if (c) f(a); else f(b);': a ternary inside a statement, and two "
         "branches that differ in one sub-expression",
         8, enumTernaryArg, nullptr},
        {"switch_if", "'if (x == K) S else D' <-> 'switch (x) { case K: S break; default: D }'", 6,
         enumSwitchIf, nullptr},
        {"early_return",
         "'if (c) { ...return; } else { B }' <-> 'if (c) { ...return; } B' (also break, "
         "continue, goto)",
         10, enumEarlyReturn, nullptr},
        {"branch_dup",
         "Hoist a statement both branches start or end with out of the if, or copy the "
         "statement before/after an if/else into both branches",
         10, enumBranchDup, nullptr},
        {"cond_split", "'if (a && b) S' <-> 'if (a) { if (b) S }'", 8, enumCondSplit, nullptr},
        {"explicit_compare", "'if (x)' <-> 'if (x != 0)', 'if (!x)' <-> 'if (x == 0)'", 8,
         enumExplicitCompare, nullptr},
        {"negate_const", "'x - 4' <-> 'x + -4', 'x -= 4' <-> 'x += -4'", 6, enumNegateConst, nullptr},
        {"loop_form",
         "for <-> while: 'for (i; c; s) B' <-> 'i; while (c) { B s; }', 'while (c)' <-> "
         "'for (; c;)'",
         6, enumLoopForm, nullptr},
        {"reassociate", "'a + b + c' <-> 'a + (b + c)' (for + * & | ^)", 6, enumReassociate,
         nullptr},
    };
    return passes;
}

const Pass* findPass(const std::string& name) {
    for (auto& p : allPasses())
        if (p.name == name) return &p;
    return nullptr;
}

bool randomMutation(const Pass& p, const Func& f, Rng& rng, std::string& out) {
    if (p.random) return p.random(f, rng, out);
    // reservoir sample one mutation without materialising every text
    Mutation chosen;
    size_t seen = 0;
    p.enumerate(f, [&](Mutation m) {
        seen++;
        if (std::uniform_int_distribution<size_t>(0, seen - 1)(rng) == 0) chosen = std::move(m);
        return seen < 20000;
    });
    if (!chosen) return false;
    out = chosen();
    return !out.empty();
}

// ---------------------------------------------------------------------------
// pass groups

std::string PassGroup::name() const {
    std::string r;
    for (auto* p : passes) r += (r.empty() ? "" : "+") + p->name;
    return r;
}

int PassGroup::weight() const {
    if (weightOverride >= 0) return weightOverride;
    int w = 0;
    for (auto* p : passes) w = std::max(w, p->weight);
    return w;
}

bool parsePassSpecs(const std::vector<std::string>& specs, std::vector<PassGroup>& out,
                    std::string& err) {
    out.clear();
    bool none = false;
    auto addAll = [&]() {
        for (auto& p : allPasses()) out.push_back({{&p}});
    };
    if (specs.empty()) {
        addAll();
        return true;
    }
    for (auto& spec : specs) {
        std::stringstream groups(spec);
        std::string g;
        while (std::getline(groups, g, ',')) {
            g.erase(std::remove(g.begin(), g.end(), ' '), g.end());
            if (g.empty()) continue;
            if (g == "all") {
                addAll();
                continue;
            }
            if (g == "none") {
                none = true;
                continue;
            }
            PassGroup grp;
            std::stringstream names(g);
            std::string n;
            while (std::getline(names, n, '+')) {
                const Pass* p = findPass(n);
                if (!p) {
                    err = "unknown pass '" + n + "' (see --list-passes)";
                    return false;
                }
                grp.passes.push_back(p);
            }
            if (!grp.passes.empty()) out.push_back(grp);
        }
    }
    if (out.empty() && !none) {
        err = "no passes given";
        return false;
    }
    return true;
}

void enumerateGroup(const PassGroup& g, const std::string& text,
                    const std::function<bool(const std::string&)>& emit) {
    std::set<std::string> seen = {text};
    bool stop = false;
    std::function<void(const std::string&, size_t)> rec = [&](const std::string& cur, size_t from) {
        for (size_t j = from; j < g.passes.size() && !stop; ++j) {
            std::string err;
            auto f = parseFunc(cur, err);
            if (!f) return;
            std::vector<std::string> outs;
            g.passes[j]->enumerate(*f, [&](Mutation m) {
                std::string s = m();
                if (!s.empty() && seen.insert(s).second) outs.push_back(std::move(s));
                return true;
            });
            for (auto& s : outs) {
                if (stop) return;
                if (!emit(s)) {
                    stop = true;
                    return;
                }
                rec(s, j + 1);
            }
        }
    };
    rec(text, 0);
}

bool randomGroupMutation(const PassGroup& g, const std::string& text, Rng& rng, std::string& out) {
    std::string cur = text;
    for (auto* p : g.passes) {
        std::string err;
        auto f = parseFunc(cur, err);
        if (!f) break;
        std::string next;
        if (randomMutation(*p, *f, rng, next) && next != cur) cur = next;
    }
    if (cur == text) return false;
    out = cur;
    return true;
}

} // namespace perm
