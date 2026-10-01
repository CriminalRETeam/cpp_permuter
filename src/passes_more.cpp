// More passes, from gta2_re's docs/matching_quirks.md and decomp-permuter's
// randomizer: comparisons, chained assignments, scopes, local types and
// arrays, switch case layout, inline getters, temporaries, removals.

#include "pass_util.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace perm {

namespace {
std::vector<Getter> gGetters;
} // namespace

void setGetters(std::vector<Getter> g) { gGetters = std::move(g); }
const std::vector<Getter>& getters() { return gGetters; }

std::vector<Getter> findGetters(const std::string& text) {
    std::vector<Getter> out;
    for (auto& d : listDefinitions(text)) {
        std::string err;
        auto f = parseFunc(text.substr(d.start, d.end - d.start), err);
        if (!f) continue;
        // no parameters
        int po = f->paramOpen, pc = f->paramClose;
        if (!(pc == po + 1 || (pc == po + 2 && f->t(po + 1) == "void"))) continue;
        auto& k = f->body->kids;
        if (k.size() != 1 || k[0]->kind != SK::Return || f->t(k[0]->begin) != "return") continue;
        int b = k[0]->begin + 1, e = k[0]->end - 1;
        if (f->t(b) == "this" && f->t(b + 1) == "->") b += 2;
        if (b >= e || f->toks[b].kind != TokKind::Ident) continue;
        // a plain member path: a.b->c[1].d
        bool path = true;
        for (int i = b; i < e && path; ++i) {
            const Token& t = f->toks[i];
            path = t.kind == TokKind::Ident || t.kind == TokKind::Number || t.text == "." ||
                   t.text == "->" || t.text == "[" || t.text == "]";
        }
        if (!path || isKeyword(f->t(b))) continue;
        std::string name = d.name.substr(d.name.rfind(':') == std::string::npos ? 0 : d.name.rfind(':') + 1);
        out.push_back({name, util::norm(f->tokText(b, e))});
    }
    return out;
}

namespace util {

namespace {

// --- inequalities -----------------------------------------------------------

bool intLiteral(const Func& f, const Expr& x, long long& v, std::string& prefix, std::string& suffix) {
    if (x.k != Expr::Primary || x.e != x.b + 1 || f.toks[x.b].kind != TokKind::Number) return false;
    std::string s = f.t(x.b);
    if (s.find('.') != std::string::npos) return false;
    bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (!hex && (s.find('e') != std::string::npos || s.find('E') != std::string::npos ||
                 s.find('f') != std::string::npos || s.find('F') != std::string::npos))
        return false;
    size_t end = s.size();
    while (end > 0 && std::string("uUlL").find(s[end - 1]) != std::string::npos) end--;
    suffix = s.substr(end);
    prefix = hex ? s.substr(0, 2) : "";
    try {
        v = std::stoll(s.substr(0, end), nullptr, hex ? 16 : (s.size() > 1 && s[0] == '0' ? 8 : 10));
    } catch (...) {
        return false;
    }
    return true;
}

std::string formatLike(long long v, const std::string& prefix, const std::string& suffix) {
    if (prefix.empty()) return std::to_string(v) + suffix;
    char buf[32];
    std::snprintf(buf, sizeof buf, prefix[1] == 'X' ? "%llX" : "%llx", (unsigned long long)v);
    return prefix + buf + suffix;
}

void enumInequalities(const Func& f, const EmitFn& emit) {
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        if (x.k != Expr::Binary) return true;
        const std::string& op = f.t(x.op);
        if (op != "<" && op != ">" && op != "<=" && op != ">=") return true;
        long long v;
        std::string pre, suf;
        bool right = intLiteral(f, *x.kids[1], v, pre, suf);
        if (!right && !intLiteral(f, *x.kids[0], v, pre, suf)) return true;
        // with the constant on the right: a > N == a >= N+1, a >= N == a > N-1, ...
        // on the left the same rules apply to the mirrored operator
        std::string mop = right ? op : (op == "<" ? ">" : op == ">" ? "<" : op == "<=" ? ">=" : "<=");
        long long nv = (mop == ">" || mop == "<=") ? v + 1 : v - 1;
        std::string nop = mop == ">" ? ">=" : mop == ">=" ? ">" : mop == "<" ? "<=" : "<";
        if (nv < 0 && v >= 0) return true; // "x >= 0" -> "x > -1" breaks unsigned x
        if (!right) nop = nop == "<" ? ">" : nop == ">" ? "<" : nop == "<=" ? ">=" : "<=";
        std::string lit = formatLike(nv, pre, suf);
        std::string text = right ? exprText(f, *x.kids[0]) + " " + nop + " " + lit
                                 : lit + " " + nop + " " + exprText(f, *x.kids[1]);
        return emitReplace(f, emit, x.b, x.e, text);
    });
}

// --- chain_assign -------------------------------------------------------------

struct SimpleAssign {
    std::string lhs, rhs;
};

bool simpleAssign(const Func& f, const Stmt& s, SimpleAssign& a) {
    if (s.kind != SK::Expr) return false;
    auto x = parseExpr(f, s.begin, s.end - 1);
    if (!x || x->k != Expr::Assign || f.t(x->op) != "=") return false;
    const Expr& r = *x->kids[1];
    if (r.k == Expr::Assign) return false;
    Effects e = effectsOf(f, r.b, r.e);
    if (e.call || e.memWrite || !e.writes.empty()) return false;
    a = {exprText(f, *x->kids[0]), exprText(f, r)};
    return true;
}

void enumChainAssign(const Func& f, const EmitFn& emit) {
    for (const Stmt* b : blocksOf(f)) {
        auto& k = b->kids;
        for (size_t i = 0; i + 1 < k.size(); ++i) {
            SimpleAssign a0;
            if (!simpleAssign(f, *k[i], a0)) continue;
            std::vector<SimpleAssign> run = {a0};
            size_t j = i + 1;
            SimpleAssign aj;
            while (j < k.size() && simpleAssign(f, *k[j], aj) && norm(aj.rhs) == norm(a0.rhs)) {
                run.push_back(aj);
                j++;
            }
            if (run.size() < 2) continue;
            // two at a time, both orders, and the whole run
            for (size_t p = 0; p + 1 < run.size(); ++p) {
                const SimpleAssign &x = run[p], &y = run[p + 1];
                if (!emitReplace(f, emit, k[i + p]->begin, k[i + p + 1]->end, x.lhs + " = " + y.lhs + " = " + x.rhs + ";"))
                    return;
                if (!emitReplace(f, emit, k[i + p]->begin, k[i + p + 1]->end, y.lhs + " = " + x.lhs + " = " + x.rhs + ";"))
                    return;
            }
            if (run.size() > 2) {
                std::string text;
                for (auto& r : run) text += r.lhs + " = ";
                if (!emitReplace(f, emit, k[i]->begin, k[j - 1]->end, text + a0.rhs + ";")) return;
            }
            i = j - 1;
        }
    }
    // a = b = v;  ->  b = v; a = v;  /  b = v; a = b;
    for (const Stmt* s : stmtsOfKind(f, SK::Expr)) {
        if (!inBlock(*s)) continue;
        auto x = parseExpr(f, s->begin, s->end - 1);
        if (!x || x->k != Expr::Assign || f.t(x->op) != "=" || x->kids[1]->k != Expr::Assign) continue;
        const Expr& in = *x->kids[1];
        if (f.t(in.op) != "=") continue;
        Effects e = effectsOf(f, in.kids[1]->b, in.kids[1]->e);
        std::string a = exprText(f, *x->kids[0]), bb = exprText(f, *in.kids[0]), v = exprText(f, *in.kids[1]);
        std::string ind = "\n" + lineInd(f, s->begin);
        if (!emitReplace(f, emit, s->begin, s->end, bb + " = " + v + ";" + ind + a + " = " + bb + ";")) return;
        if (!e.call && !e.memWrite && e.writes.empty())
            if (!emitReplace(f, emit, s->begin, s->end, bb + " = " + v + ";" + ind + a + " = " + v + ";")) return;
    }
}

// --- scope_block --------------------------------------------------------------

std::set<std::string> declaredIn(const Func& f, const Stmt& s) {
    std::set<std::string> r;
    forEachStmt(s, [&](const Stmt& x) {
        if (x.kind == SK::Decl)
            for (auto& d : x.decls) r.insert(f.t(d.nameTok));
    });
    return r;
}

void enumScopeBlock(const Func& f, const EmitFn& emit) {
    for (const Stmt* b : blocksOf(f)) {
        auto& k = b->kids;
        // wrap a declaration and the statements using it in their own block
        for (size_t i = 0; i < k.size(); ++i) {
            if (k[i]->kind != SK::Decl || k[i]->isStatic) continue;
            std::set<std::string> names = declaredIn(f, *k[i]);
            size_t j = i;
            bool ok = true;
            for (bool grew = true; grew && ok;) {
                grew = false;
                for (size_t m = j + 1; m < k.size(); ++m) {
                    bool uses = false;
                    for (auto& n : names) uses |= !usesOf(f, n, k[m]->begin, k[m]->end).empty();
                    if (uses) {
                        for (size_t q = j + 1; q <= m; ++q)
                            for (auto& n : declaredIn(f, *k[q])) grew |= names.insert(n).second;
                        j = m;
                    }
                }
            }
            for (size_t q = i; q <= j; ++q)
                if (k[q]->kind == SK::Case || k[q]->kind == SK::Default || k[q]->kind == SK::Label) ok = false;
            if (!ok || (i == 0 && j + 1 == k.size())) continue;
            std::vector<const Stmt*> range;
            for (size_t q = i; q <= j; ++q) range.push_back(k[q].get());
            std::string ind = lineInd(f, k[i]->begin);
            if (!emitReplace(f, emit, k[i]->begin, k[j]->end, blockText(f, range, ind))) return;
        }
        // or take a plain nested block apart
        for (size_t i = 0; i < k.size(); ++i) {
            if (k[i]->kind != SK::Block || k[i]->kids.empty()) continue;
            std::set<std::string> inner = declaredIn(f, *k[i]);
            bool clash = false;
            for (size_t q = 0; q < k.size(); ++q)
                if (q != i && k[q]->kind == SK::Decl)
                    for (auto& d : k[q]->decls) clash |= inner.count(f.t(d.nameTok)) > 0;
            if (clash) continue;
            std::vector<const Stmt*> inside;
            for (auto& s : k[i]->kids) inside.push_back(s.get());
            if (!emitReplace(f, emit, k[i]->begin, k[i]->end, stmtsText(f, inside, lineInd(f, k[i]->begin)))) return;
        }
    }
}

// --- local_type ---------------------------------------------------------------

const std::vector<std::vector<std::string>>& typeFamilies() {
    static const std::vector<std::vector<std::string>> fams = {
        {"s8", "u8", "s16", "u16", "s32", "u32"},
        {"char", "unsigned char", "signed char", "short", "unsigned short", "int", "unsigned int", "long",
         "unsigned long", "unsigned"},
        {"BYTE", "WORD", "DWORD", "INT", "UINT", "SHORT", "USHORT", "LONG", "ULONG"},
    };
    return fams;
}

void enumLocalType(const Func& f, const EmitFn& emit) {
    std::vector<const Stmt*> decls;
    forEachStmt(*f.body, [&](const Stmt& s) {
        if (s.kind == SK::Decl) decls.push_back(&s);
        if (s.forInit) decls.push_back(s.forInit.get());
    });
    for (const Stmt* s : decls) {
        if (s->isStatic) continue;
        int b = s->typeBegin, e = s->typeEnd;
        auto cv = [&](int i) {
            const std::string& t = f.t(i);
            return t == "const" || t == "volatile" || t == "register";
        };
        while (b < e && cv(b)) b++;
        while (e > b && cv(e - 1)) e--;
        if (b >= e) continue;
        std::string core;
        for (int i = b; i < e; ++i) core += (i > b ? " " : "") + f.t(i);
        for (auto& fam : typeFamilies()) {
            if (std::find(fam.begin(), fam.end(), core) == fam.end()) continue;
            for (auto& alt : fam) {
                if (alt == core || alt == "unsigned") continue;
                if (!emitReplace(f, emit, b, e, alt)) return;
            }
        }
    }
}

// --- locals_to_array ----------------------------------------------------------

void enumLocalsToArray(const Func& f, const EmitFn& emit) {
    for (const Stmt* b : blocksOf(f)) {
        auto& k = b->kids;
        auto eligible = [&](const Stmt& s) {
            if (s.kind != SK::Decl || s.isStatic || s.isConst || s.decls.size() != 1) return false;
            const Declarator& d = s.decls[0];
            return !d.isArray && !d.isRef && d.init != Declarator::Paren && d.init != Declarator::Brace &&
                   !(d.init == Declarator::Assign && f.t(d.initBegin) == "{");
        };
        auto typeKey = [&](const Stmt& s) {
            return norm(f.tokText(s.typeBegin, s.typeEnd) + f.tokText(s.decls[0].begin, s.decls[0].nameTok));
        };
        for (size_t i = 0; i < k.size(); ++i) {
            if (!eligible(*k[i])) continue;
            size_t j = i + 1;
            while (j < k.size() && eligible(*k[j]) && typeKey(*k[j]) == typeKey(*k[i])) j++;
            if (j - i < 2) continue;
            std::vector<const Stmt*> run;
            for (size_t q = i; q < j; ++q) run.push_back(k[q].get());
            // the whole run and each adjacent pair, in order and back to front
            std::vector<std::pair<size_t, size_t>> spans = {{0, run.size()}};
            if (run.size() > 2)
                for (size_t q = 0; q + 1 < run.size(); ++q) spans.push_back({q, q + 2});
            for (auto [lo, hi] : spans) {
                std::vector<const Stmt*> part(run.begin() + lo, run.begin() + hi);
                std::set<std::string> names;
                for (auto* s : part) names.insert(f.t(s->decls[0].nameTok));
                bool selfRef = false;
                for (auto* s : part)
                    if (s->decls[0].init == Declarator::Assign)
                        for (auto& n : names) selfRef |= !usesOf(f, n, s->decls[0].initBegin, s->decls[0].initEnd).empty();
                if (selfRef) continue;
                std::string arr = f.t(part[0]->decls[0].nameTok) + "_arr";
                while (f.locals.count(arr)) arr += "_";
                for (int reversed = 0; reversed < 2; ++reversed) {
                    std::vector<std::string> idx;
                    for (size_t q = 0; q < part.size(); ++q)
                        idx.push_back(arr + "[" + std::to_string(reversed ? part.size() - 1 - q : q) + "]");
                    const Stmt* first = part.front();
                    std::string ind = "\n" + lineInd(f, first->begin);
                    std::string text = f.tokText(first->typeBegin, first->typeEnd) + " " +
                                       f.tokText(first->decls[0].begin, first->decls[0].nameTok) + arr + "[" +
                                       std::to_string(part.size()) + "];";
                    for (size_t q = 0; q < part.size(); ++q)
                        if (part[q]->decls[0].init == Declarator::Assign)
                            text += ind + idx[q] + " = " +
                                    f.tokText(part[q]->decls[0].initBegin, part[q]->decls[0].initEnd) + ";";
                    std::vector<std::pair<int, std::string>> subst;
                    for (size_t q = 0; q < part.size(); ++q)
                        for (int u : usesOf(f, f.t(part[q]->decls[0].nameTok), part.back()->end, f.bodyClose))
                            subst.push_back({u, idx[q]});
                    int pb = part.front()->begin, pe = part.back()->end;
                    if (!emit([&f, pb, pe, text, subst]() {
                            Rewriter rw(f);
                            rw.replace(pb, pe, text);
                            for (auto& [u, t] : subst) rw.replace(u, u + 1, t);
                            return rw.apply();
                        }))
                        return;
                }
            }
            i = j - 1;
        }
    }
}

// --- reorder_cases / split_case_labels ------------------------------------------

struct CaseGroup {
    std::vector<const Stmt*> labels, body;
};

// The case groups of a switch body, or false if it has statements before the
// first label.
bool caseGroups(const Stmt& sw, std::vector<CaseGroup>& out) {
    if (sw.kids.empty() || sw.kids[0]->kind != SK::Block) return false;
    out.clear();
    for (auto& k : sw.kids[0]->kids) {
        bool label = k->kind == SK::Case || k->kind == SK::Default;
        if (label && (out.empty() || !out.back().body.empty())) out.push_back({});
        if (out.empty()) return false;
        (label ? out.back().labels : out.back().body).push_back(k.get());
    }
    return !out.empty();
}

std::string groupText(const Func& f, const CaseGroup& g, bool addBreak) {
    const Stmt* last = g.body.empty() ? g.labels.back() : g.body.back();
    std::string t = f.tokText(g.labels.front()->begin, last->end);
    if (addBreak) t += "\n" + lineInd(f, last->begin) + (g.body.empty() ? "    " : "") + "break;";
    return t;
}

void enumReorderCases(const Func& f, const EmitFn& emit) {
    for (const Stmt* sw : stmtsOfKind(f, SK::Switch)) {
        std::vector<CaseGroup> gs;
        if (!caseGroups(*sw, gs) || gs.size() < 2) continue;
        bool ok = true;
        for (size_t i = 0; i + 1 < gs.size(); ++i) ok &= !gs[i].body.empty() && isJump(gs[i].body.back());
        if (!ok) continue; // fall-through: order matters
        bool lastOpen = gs.back().body.empty() || !isJump(gs.back().body.back());
        size_t n = gs.size();
        auto emitOrder = [&](const std::vector<size_t>& ord) {
            std::vector<std::pair<std::pair<int, int>, std::string>> edits;
            for (size_t slot = 0; slot < n; ++slot) {
                if (ord[slot] == slot) continue;
                const CaseGroup& g = gs[ord[slot]];
                const CaseGroup& at = gs[slot];
                const Stmt* atLast = at.body.empty() ? at.labels.back() : at.body.back();
                edits.push_back({{at.labels.front()->begin, atLast->end},
                                 groupText(f, g, lastOpen && ord[slot] == n - 1)});
            }
            return emit([&f, edits]() {
                Rewriter rw(f);
                for (auto& [r, t] : edits) rw.replace(r.first, r.second, t);
                return rw.apply();
            });
        };
        std::vector<size_t> ord(n);
        for (size_t i = 0; i < n; ++i) ord[i] = i;
        if (n <= 5) {
            while (std::next_permutation(ord.begin(), ord.end()))
                if (!emitOrder(ord)) return;
        } else {
            for (size_t a = 0; a < n; ++a)
                for (size_t c = 0; c < n; ++c) {
                    if (a == c) continue;
                    std::vector<size_t> o2;
                    for (size_t i = 0; i < n; ++i)
                        if (i != a) o2.push_back(i);
                    o2.insert(o2.begin() + c, a);
                    if (!emitOrder(o2)) return;
                }
        }
    }
}

void enumSplitCaseLabels(const Func& f, const EmitFn& emit) {
    for (const Stmt* sw : stmtsOfKind(f, SK::Switch)) {
        std::vector<CaseGroup> gs;
        if (!caseGroups(*sw, gs)) continue;
        for (size_t i = 0; i < gs.size(); ++i) {
            const CaseGroup& g = gs[i];
            bool closed = !g.body.empty() && isJump(g.body.back());
            // case 1: case 2: body  ->  case 1: body case 2: body
            if (g.labels.size() >= 2 && (closed || i + 1 == gs.size()) && !g.body.empty()) {
                std::string li = lineInd(f, g.labels.front()->begin), bi = lineInd(f, g.body.front()->begin);
                std::string body = stmtsText(f, g.body, bi);
                std::string text;
                for (size_t q = 0; q < g.labels.size(); ++q) {
                    if (q) text += "\n" + li;
                    text += f.textOf(*g.labels[q]) + "\n" + bi + body;
                    if (!closed && q + 1 < g.labels.size()) text += "\n" + bi + "break;";
                }
                if (!emitReplace(f, emit, g.labels.front()->begin, g.body.back()->end, text)) return;
            }
            // two groups with the same body -> one group with both labels
            if (i + 1 < gs.size() && closed && !gs[i + 1].body.empty()) {
                const CaseGroup& h = gs[i + 1];
                if (norm(f.tokText(g.body.front()->begin, g.body.back()->end)) !=
                    norm(f.tokText(h.body.front()->begin, h.body.back()->end)))
                    continue;
                std::string li = lineInd(f, g.labels.front()->begin);
                std::string text = f.tokText(g.labels.front()->begin, g.labels.back()->end);
                for (auto* l : h.labels) text += "\n" + li + f.textOf(*l);
                text += "\n" + lineInd(f, g.body.front()->begin) +
                        stmtsText(f, g.body, lineInd(f, g.body.front()->begin));
                if (!emitReplace(f, emit, g.labels.front()->begin, h.body.back()->end, text)) return;
            }
        }
    }
}

// --- use_getter -----------------------------------------------------------------

void enumUseGetter(const Func& f, const EmitFn& emit) {
    const auto& gs = getters();
    if (gs.empty()) return;
    std::map<std::string, std::vector<const Getter*>> byExpr, byName;
    for (auto& g : gs) {
        byExpr[g.expr].push_back(&g);
        byName[g.name].push_back(&g);
    }
    std::set<std::pair<int, std::string>> done;
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        // obj->path  ->  obj->getter()   (and a bare member path in a method)
        if (x.k == Expr::Member || (x.k == Expr::Primary && !f.isLocal(x.b))) {
            std::string whole = norm(exprText(f, x));
            // try every split "obj" + "->"/"." + "path"
            for (int i = x.b; i < x.e; ++i) {
                bool sep = f.t(i) == "->" || f.t(i) == ".";
                int from = sep ? i + 1 : (i == x.b && x.k == Expr::Primary ? i : -1);
                if (from < 0) continue;
                auto it = byExpr.find(norm(f.tokText(from, x.e)));
                if (it == byExpr.end()) continue;
                std::string obj = sep ? f.tokText(x.b, i) + f.t(i) : "";
                for (auto* g : it->second) {
                    std::string text = obj + g->name + "()";
                    if (!done.insert({x.b, text}).second) continue;
                    if (!emitReplace(f, emit, x.b, x.e, text)) return false;
                }
            }
            (void)whole;
        }
        // obj->getter()  ->  obj->path
        if (x.k == Expr::Call && x.kids.size() == 1) {
            const Expr& callee = *x.kids[0];
            int nameTok = callee.e - 1;
            auto it = byName.find(f.t(nameTok));
            if (it == byName.end()) return true;
            std::string obj = callee.k == Expr::Member ? f.tokText(callee.b, nameTok) : "";
            for (auto* g : it->second) {
                std::string path;
                // the stored path has no spaces; it is valid code as is
                path = g->expr;
                if (!emitReplace(f, emit, x.b, x.e, obj + path)) return false;
            }
        }
        return true;
    });
}

// --- temp_for_expr -------------------------------------------------------------

// Declared types of the locals and parameters: "s32", "Ped*", ...
std::map<std::string, std::string> localTypes(const Func& f) {
    std::map<std::string, std::string> out;
    forEachStmt(*f.body, [&](const Stmt& s) {
        std::vector<const Stmt*> ds;
        if (s.kind == SK::Decl) ds.push_back(&s);
        if (s.forInit) ds.push_back(s.forInit.get());
        for (auto* d : ds) {
            if (d->isStatic) continue;
            int b = d->typeBegin;
            while (b < d->typeEnd && (f.t(b) == "register" || f.t(b) == "const" || f.t(b) == "volatile")) b++;
            for (auto& dc : d->decls)
                if (!dc.isArray && !dc.isRef)
                    out[f.t(dc.nameTok)] = f.tokText(b, d->typeEnd) + f.tokText(dc.begin, dc.nameTok);
        }
    });
    int seg = f.paramOpen + 1, depth = 0;
    for (int i = f.paramOpen + 1; i <= f.paramClose; ++i) {
        const std::string& t = f.t(i);
        if (i < f.paramClose && (t == "(" || t == "[")) depth++;
        if (i < f.paramClose && (t == ")" || t == "]")) depth--;
        if ((t == "," && depth == 0) || i == f.paramClose) {
            int name = -1;
            for (int j = seg; j < i && f.t(j) != "="; ++j)
                if (f.toks[j].kind == TokKind::Ident && !isKeyword(f.t(j))) name = j;
            bool simple = name > seg;
            for (int j = seg; j < name && simple; ++j) simple = f.t(j) != "&" && f.t(j) != "(";
            if (simple && f.locals.count(f.t(name))) {
                int b = seg;
                while (b < name && (f.t(b) == "const" || f.t(b) == "volatile")) b++;
                out[f.t(name)] = f.tokText(b, name);
            }
            seg = i + 1;
        }
    }
    for (auto& [n, t] : out) {
        std::string c = t;
        while (!c.empty() && c.back() == ' ') c.pop_back();
        t = c;
    }
    return out;
}

void enumTempForExpr(const Func& f, const EmitFn& emit) {
    auto types = localTypes(f);
    int n = 0;
    std::string tmp = "tmp";
    while (f.locals.count(tmp)) tmp = "tmp" + std::to_string(++n);
    for (auto& site : exprSites(f)) {
        const Stmt& s = *site.stmt;
        if (!inBlock(s) || s.kind == SK::While || s.kind == SK::For || s.kind == SK::Do) continue;
        auto root = parseExpr(f, site.b, site.e);
        if (!root) continue;
        std::vector<std::pair<const Expr*, std::string>> picks;
        forEachExpr(*root, [&](Expr& x) {
            if (x.k == Expr::Cast && f.t(x.b) == "(") {
                int close = matchBracket(f.toks, x.b);
                if (close > 0 && close < x.e) picks.push_back({&x, f.tokText(x.b + 1, close)});
            } else if (x.k == Expr::Primary && f.isLocal(x.b) && x.e == x.b + 1 && !isWriteUse(f, x.b) &&
                       types.count(f.t(x.b))) {
                picks.push_back({&x, types[f.t(x.b)]});
            } else if (x.k == Expr::Unary && f.t(x.op) == "*" && x.kids[0]->k == Expr::Primary &&
                       f.isLocal(x.kids[0]->b) && types.count(f.t(x.kids[0]->b))) {
                std::string t = types[f.t(x.kids[0]->b)];
                if (!t.empty() && t.back() == '*') {
                    t.pop_back();
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    // "*p" as an assignment target isn't a value to copy
                    if (!isAssignOp(f.t(x.e))) picks.push_back({&x, t});
                }
            }
        });
        for (auto& [x, type] : picks) {
            if (x->b == site.b && x->e == site.e && s.kind == SK::Expr) continue;
            std::string decl = type + " " + tmp + " = " + exprText(f, *x) + ";";
            int b = x->b, e = x->e, at = s.begin;
            std::string name = tmp;
            if (!emit([&f, b, e, at, decl, name]() {
                    Rewriter rw(f);
                    rw.insertBefore(at, decl);
                    rw.replace(b, e, name);
                    return rw.apply();
                }))
                return;
        }
    }
}

// --- remove_stmt ----------------------------------------------------------------

void enumRemoveStmt(const Func& f, const EmitFn& emit) {
    for (const Stmt* s : stmtsOfKind(f, SK::Expr)) {
        if (!inBlock(*s)) continue;
        const Stmt* sp = s;
        if (!emit([&f, sp]() {
                Rewriter rw(f);
                rw.remove(sp->begin, sp->end);
                return rw.apply();
            }))
            return;
    }
}

} // namespace

// --- named_op ---------------------------------------------------------------------

namespace {
std::vector<OpAlias> gOpAliases;
}

} // namespace util

void setOpAliases(std::vector<OpAlias> a) { util::gOpAliases = std::move(a); }
const std::vector<OpAlias>& opAliases() { return util::gOpAliases; }

namespace util {

// x as the object of a method call: "a.f()" needs parens unless x is postfix-like.
static std::string objectText(const Func& f, const Expr& x) {
    std::string s = exprText(f, x);
    return isPostfixLike(x) ? s : paren(s);
}

void enumNamedOp(const Func& f, const EmitFn& emit) {
    const auto& aliases = opAliases();
    if (aliases.empty()) return;
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        // a * b  ->  a.Multiply(b),   -a  ->  a.Negate()
        if (x.k == Expr::Binary || (x.k == Expr::Unary && f.t(x.op) == "-")) {
            std::string op = x.k == Expr::Unary ? "neg" : f.t(x.op);
            for (auto& a : aliases) {
                if (a.op != op) continue;
                std::string text = objectText(f, *x.kids[0]) + "." + a.method + "(" +
                                   (x.k == Expr::Binary ? exprText(f, *x.kids[1]) : "") + ")";
                if (!emitReplace(f, emit, x.b, x.e, text)) return false;
            }
        }
        // a.Multiply(b)  ->  (a * b),   a.Negate()  ->  (-a)
        if (x.k == Expr::Call && x.kids[0]->k == Expr::Member) {
            const Expr& callee = *x.kids[0];
            const Expr& obj = *callee.kids[0];
            if (f.t(obj.e) != ".") return true;
            const std::string& name = f.t(callee.e - 1);
            for (auto& a : aliases) {
                if (a.method != name) continue;
                std::string text;
                if (a.op == "neg" && x.kids.size() == 1)
                    text = "(-" + operandText(f, obj, 99) + ")";
                else if (a.op != "neg" && x.kids.size() == 2)
                    text = "(" + operandText(f, obj, binPrec(a.op) - 1) + " " + a.op + " " +
                           operandText(f, *x.kids[1], binPrec(a.op)) + ")";
                if (!text.empty() && !emitReplace(f, emit, x.b, x.e, text)) return false;
            }
        }
        return true;
    });
}

// --- cast_operand ------------------------------------------------------------------

static const char* const kCastTypes[] = {"u32", "s32", "u16", "s16", "u8", "s8"};

static bool isIntegerType(std::string t) {
    t = norm(t);
    static const std::set<std::string> ints = {
        "u32", "s32", "u16", "s16", "u8", "s8", "int", "unsignedint", "unsigned", "short", "unsignedshort",
        "char", "unsignedchar", "signedchar", "long", "unsignedlong", "char_type", "DWORD", "WORD", "BYTE",
        "BOOL", "bool", "size_t"};
    return ints.count(t) != 0;
}

void enumCastOperand(const Func& f, const EmitFn& emit) {
    auto types = localTypes(f);
    forEachSiteExpr(f, [&](const ExprSite&, std::shared_ptr<Expr>, Expr& x) {
        if (x.k != Expr::Binary) return true;
        const std::string& op = f.t(x.op);
        static const std::set<std::string> ops = {"/", "%", ">>", "<", ">", "<=", ">=", "+", "-", "*", "==", "!="};
        if (!ops.count(op)) return true;
        for (int side = 0; side < 2; ++side) {
            const Expr& o = *x.kids[side];
            if (o.k == Expr::Primary && f.toks[o.b].kind == TokKind::Number) continue;
            // Only integers: a local of another type (Fix16, a pointer) or a call result
            // would just fail to compile.
            if (o.k == Expr::Call) continue;
            if (o.k == Expr::Primary && f.isLocal(o.b)) {
                auto it = types.find(f.t(o.b));
                if (it != types.end() && !isIntegerType(it->second)) continue;
            }
            const Expr* inner = &o;
            std::string have;
            if (o.k == Expr::Cast && f.t(o.b) == "(") {
                // an existing integer cast: drop it or change its type
                int close = matchBracket(f.toks, o.b);
                have = norm(f.tokText(o.b + 1, close));
                bool known = false;
                for (auto* t : kCastTypes) known |= have == t;
                if (!known) continue;
                inner = o.kids[0].get();
                if (!emitReplace(f, emit, o.b, o.e, operandText(f, *inner, binPrec(op))))
                    return false;
            }
            for (auto* t : kCastTypes) {
                if (have == t) continue;
                std::string text = "(" + std::string(t) + ")" + objectText(f, *inner);
                if (!emitReplace(f, emit, o.b, o.e, text)) return false;
            }
        }
        return true;
    });
}

void registerMorePasses(std::vector<Pass>& passes) {
    passes.push_back({"inequalities", "'x > 4' <-> 'x >= 5', 'x < 4' <-> 'x <= 3' (integer constants)", 6,
                      enumInequalities, nullptr});
    passes.push_back({"chain_assign",
                      "'a = v; b = v;' <-> 'a = b = v;' (and 'b = v; a = b;'): which register holds "
                      "the shared value",
                      8, enumChainAssign, nullptr});
    passes.push_back({"scope_block",
                      "Wrap a declaration and the statements using it in their own { } block (VC6 "
                      "shares stack slots between blocks), or take a nested block apart",
                      8, enumScopeBlock, nullptr});
    passes.push_back({"local_type", "Change a local's integer type: s32 <-> u32 <-> s16 <-> u8 ... (or the "
                                    "builtin / Windows equivalents)",
                      8, enumLocalType, nullptr});
    passes.push_back({"locals_to_array",
                      "Turn consecutive locals of one type into an array, in order or back to front "
                      "(stack slot order)",
                      4, enumLocalsToArray, nullptr});
    passes.push_back({"reorder_cases", "Reorder a switch's case groups (case bodies are laid out in source order)",
                      8, enumReorderCases, nullptr});
    passes.push_back({"split_case_labels",
                      "'case 1: case 2: S' <-> 'case 1: S case 2: S' (a byte index table vs one entry "
                      "per case)",
                      5, enumSplitCaseLabels, nullptr});
    passes.push_back({"use_getter",
                      "'p->field.x' <-> 'p->get_x()' using the inline getters in the headers the "
                      "source includes",
                      8, enumUseGetter, nullptr});
    passes.push_back({"temp_for_expr",
                      "Compute a cast, a local or '*p' into a new local first (types from the "
                      "declarations; also 'copy through a local to get a spill')",
                      8, enumTempForExpr, nullptr});
    passes.push_back({"named_op",
                      "'a * b' <-> 'a.Multiply_408680(b)', '-a' <-> 'a.Negate_4086A0()' for the "
                      "operators given with --op-alias: whether VC6 inlines the operator",
                      8, enumNamedOp, nullptr});
    passes.push_back({"cast_operand",
                      "Cast an operand of '/ % >> < > + - * == ...' to an integer type, or drop or "
                      "change its cast: signed vs unsigned div, shifts, compares and byte maths",
                      6, enumCastOperand, nullptr});
    passes.push_back({"remove_stmt", "Remove an expression statement", 2, enumRemoveStmt, nullptr});
}

} // namespace util

} // namespace perm
