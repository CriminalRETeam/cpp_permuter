// Unit tests. No framework: each CHECK prints the failing line.

#include "analysis.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "passes.hpp"
#include "scorer.hpp"

#include <iostream>
#include <set>

using namespace perm;

static int gFailures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #c ") failed\n"; \
            gFailures++;                                                             \
        }                                                                            \
    } while (0)

static std::unique_ptr<Func> parse(const std::string& src, const std::string& name) {
    size_t b, e;
    std::string err;
    if (!locateFunction(src, name, b, e, err)) {
        std::cerr << err << "\n";
        return nullptr;
    }
    return parseFunc(src.substr(b, e - b), err);
}

static std::vector<std::string> variants(const Func& f, const std::string& pass) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    findPass(pass)->enumerate(f, [&](Mutation m) {
        std::string s = m();
        if (!s.empty() && seen.insert(s).second) out.push_back(s);
        return true;
    });
    return out;
}

static bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

static void testLexerRoundTrip() {
    std::string src = "#include \"a.h\"\n// c\nint f(int a) { /* x */ return a >>= 2, L\"s\" '\\''; }\n"
                      "#define X(a) \\\n  a\n__asm { mov eax, 1 ; don't }\n";
    auto toks = lex(src);
    std::string back;
    for (auto& t : toks) back += t.lead + t.text;
    CHECK(back == src);
    CHECK(toks[0].kind == TokKind::PP);
    bool sawShift = false;
    for (auto& t : toks) sawShift |= t.text == ">>=";
    CHECK(sawShift);
}

static void testLocate() {
    std::string src =
        "MATCH_FUNC(0x1)\nvoid A::f(int x);\nvoid g() { a.f(1); }\n"
        "MATCH_FUNC(0x2)\nvoid A::f(int x)\n{\n    x++;\n}\n"
        "A::A() : m(1), n(2) { }\nA::~A() { }\n";
    size_t b, e;
    std::string err;
    CHECK(locateFunction(src, "A::f", b, e, err));
    CHECK(src.substr(b, 9) == "A::f(int ");
    CHECK(src.substr(e - 1, 1) == "}");
    CHECK(locateFunction(src, "f", b, e, err)); // unqualified
    CHECK(src.substr(b, 4) == "A::f");
    CHECK(locateFunction(src, "A::A", b, e, err));
    CHECK(locateFunction(src, "A::~A", b, e, err));
    CHECK(!locateFunction(src, "A::nope", b, e, err));
}

static void testParser() {
    std::string src = R"(
void C::f(u8 idx, Foo* p)
{
    Ped* a = p->x;
    s32 i, j = 2;
    unsigned long k;
    const Foo& r = *p;
    a = 1;
    p->x = 2;
    idx * 2;
    Foo::Bar(1);
    for (int n = 0; n < 3; n++) { k += n; }
    if (a) b(); else if (c) d(); else { e(); }
    switch (idx) { case Enum::A: break; default: return; }
label:
    __asm mov eax, 1
    __asm { int 3 }
    do { i--; } while (i);
    WIP_IMPLEMENTED;
}
)";
    auto f = parse(src, "C::f");
    CHECK(f != nullptr);
    if (!f) return;
    auto& k = f->body->kids;
    std::vector<SK> kinds;
    for (auto& s : k) kinds.push_back(s->kind);
    std::vector<SK> want = {SK::Decl, SK::Decl, SK::Decl, SK::Decl, SK::Expr, SK::Expr,
                            SK::Expr, SK::Expr, SK::For, SK::If, SK::Switch, SK::Label,
                            SK::Asm, SK::Asm, SK::Do, SK::Expr};
    CHECK(kinds == want);
    CHECK(f->params == (std::vector<std::string>{"idx", "p"}));
    CHECK(f->locals.count("a") && f->locals.count("i") && f->locals.count("j") &&
          f->locals.count("k") && f->locals.count("r") && f->locals.count("n"));
    CHECK(k[1]->decls.size() == 2);
    CHECK(k[3]->decls[0].isRef);
    CHECK(k[9]->kids.size() == 2 && k[9]->kids[1]->kind == SK::If);
}

static void testEffects() {
    std::string src = "void f(P* p) { int a = p->x; int b = a + 1; p->y = b; g(a); int c = 3; }";
    auto f = parse(src, "f");
    auto& k = f->body->kids;
    Effects e0 = effectsOfStmt(*f, *k[0]), e1 = effectsOfStmt(*f, *k[1]);
    Effects e2 = effectsOfStmt(*f, *k[2]), e3 = effectsOfStmt(*f, *k[3]);
    Effects e4 = effectsOfStmt(*f, *k[4]);
    CHECK(e0.writes.count("a") && e0.memRead && !e0.memWrite && !e0.call);
    CHECK(conflicts(e0, e1)); // b reads a
    CHECK(e2.memWrite && e3.call);
    CHECK(!conflicts(e0, e4));
    CHECK(conflicts(e2, e3));
    CHECK(!conflictsRelaxed(e0, e2));
}

static void testReorderSaves() {
    std::string src = R"(
void G::f(u8 idx)
{
    Ped* t = Alloc();
    W* a = leader->w1;
    W* b = list[idx]->w1;
    W* c = leader->w2;
    W* d = list[idx]->w2;
    t->Copy(leader);
}
)";
    auto f = parse(src, "G::f");
    auto v = variants(*f, "reorder_saves");
    CHECK(v.size() == 23); // 4! - the original
    // dependencies are kept
    std::string src2 = "void f(P* p) { int a = p->x; int b = a + 1; int c = p->z; use(a, b, c); }";
    auto f2 = parse(src2, "f");
    auto v2 = variants(*f2, "reorder_saves");
    CHECK(v2.size() == 2); // abc, acb, cab keep a before b; two are new
    for (auto& s : v2) CHECK(s.find("int a") < s.find("int b"));
}

static void testInlineLocal() {
    std::string src = "void f(P* p) { Q* q = p->q; q->x = 1; int u = 5; }";
    auto f = parse(src, "f");
    auto v = variants(*f, "inline_local");
    bool plain = false, cast = false, dead = false;
    for (auto& s : v) {
        plain |= contains(s, "p->q->x = 1;") && !contains(s, "Q* q");
        cast |= contains(s, "((Q*)(p->q))->x = 1;");
        dead |= !contains(s, "int u");
    }
    CHECK(plain && cast && dead);
    // a field read isn't moved past a call
    std::string src2 = "void f(P* p) { int a = p->x; p->Update(); g = a; }";
    auto v2 = variants(*parse(src2, "f"), "inline_local");
    CHECK(v2.empty());
}

static void testSinkHoistMerge() {
    std::string src = R"(
void f(int n)
{
    int a = 1;
    int b;
    g();
    if (n)
    {
        b = 2;
        use(a, b);
    }
}
)";
    auto f = parse(src, "f");
    auto sink = variants(*f, "sink_decl");
    bool intoBlock = false;
    for (auto& s : sink) intoBlock |= contains(s, "{\n        int b;\n        b = 2;");
    CHECK(intoBlock);
    auto merge = variants(*f, "merge_decl");
    CHECK(merge.size() == 1 && contains(merge[0], "int b = 2;") && !contains(merge[0], "int b;"));

    std::string src2 = R"(
void f(int n)
{
    int a = 1;
    g();
    if (n)
    {
        int b = 2;
        use(a, b);
    }
    int c = 3;
    use(c);
}
)";
    auto hoist = variants(*parse(src2, "f"), "hoist_decl");
    bool all = false;
    for (auto& s : hoist)
        all |= contains(s, "int a = 1;\n    int b;\n    int c;\n    g();") &&
               contains(s, "        b = 2;") && contains(s, "    c = 3;");
    CHECK(all);
}

static void testExprPasses() {
    std::string src = R"(
int f(int a, int b, int* p)
{
    int r = a + b * 2;
    if (a < b) r = 1; else r = 2;
    r = r + 3;
    p[0] += a;
    return a > 0 ? a : b;
}
)";
    auto f = parse(src, "f");
    auto sw = variants(*f, "swap_operands");
    bool s1 = false;
    for (auto& s : sw) s1 |= contains(s, "int r = b * 2 + a;");
    CHECK(s1);
    auto fl = variants(*f, "flip_compare");
    bool f1 = false;
    for (auto& s : fl) f1 |= contains(s, "if (b > a)");
    CHECK(f1);
    auto inv = variants(*f, "invert_if");
    CHECK(inv.size() == 1 && contains(inv[0], "if (a >= b) r = 2; else r = 1;"));
    auto ca = variants(*f, "compound_assign");
    bool c1 = false, c2 = false;
    for (auto& s : ca) {
        c1 |= contains(s, "r += 3;");
        c2 |= contains(s, "p[0] = p[0] + a;");
    }
    CHECK(c1 && c2);
    auto te = variants(*f, "ternary");
    bool t1 = false, t2 = false;
    for (auto& s : te) {
        t1 |= contains(s, "r = a < b ? 1 : 2;");
        t2 |= contains(s, "if (a > 0)") && contains(s, "return a;") && contains(s, "return b;");
    }
    CHECK(t1 && t2);
}

static void testExprParser() {
    std::string src = "void f(int a) { x = (Foo*)a->b + (a) - c[1] * sizeof(int) ? y : z; }";
    auto f = parse(src, "f");
    auto& s = *f->body->kids[0];
    auto x = parseExpr(*f, s.begin, s.end - 1);
    CHECK(x && x->k == Expr::Assign && x->kids[1]->k == Expr::Ternary);
}

static void testScorer() {
    std::string dump = R"(
t.obj:	file format coff-i386

Disassembly of section .text:

00000000 <?foo@@YAHPAUS@@@Z>:
       0: 57                           	pushl	%edi
       2: 8b 4c 24 0c                  	movl	0xc(%esp), %ecx
       d: 7e 06                        	jle	0x15 <?foo@@YAHPAUS@@@Z+0x15>
       f: 89 3d 04 00 00 00            	movl	%edi, 0x4
			00000011:  IMAGE_REL_I386_DIR32	?g@@3HA
      15: e8 00 00 00 00               	calll	0x1a <?foo@@YAHPAUS@@@Z+0x1a>
			00000016:  IMAGE_REL_I386_REL32	?f@S@@QAEXXZ
      1a: c3                           	retl

00000020 <?other@@YAXXZ>:
      20: c3                           	retl
)";
    auto in = parseObjdump(dump, "?foo@@YAHPAUS@@@Z", false);
    CHECK(in.size() == 6);
    if (in.size() == 6) {
        CHECK(in[2].full() == "jle L4");
        CHECK(in[3].full() == "movl %edi, ?g@@3HA+0x4");
        CHECK(in[4].full() == "calll ?f@S@@QAEXXZ");
    }
    ScoreConfig sc;
    CHECK(scoreInsns(sc, in, in) == 0);
    auto regs = in;
    regs[1].args = "0xc(%esp), %edx";
    CHECK(scoreInsns(sc, in, regs) == sc.penaltyRegalloc);
    auto moved = in;
    std::swap(moved[0], moved[1]);
    CHECK(scoreInsns(sc, in, moved) == sc.penaltyReorder);
    auto fewer = in;
    fewer.pop_back();
    CHECK(scoreInsns(sc, in, fewer) == sc.penaltyDelete);

    std::vector<std::string> syms = {"?Promote@G@@QAEXE@Z", "??0G@@QAE@XZ", "_ZN1G7PromoteEh", "_cfunc"};
    CHECK(guessSymbol(syms, "G::Promote") == "?Promote@G@@QAEXE@Z");
    CHECK(guessSymbol(syms, "G::G") == "??0G@@QAE@XZ");
    CHECK(guessSymbol({"_ZN1G7PromoteEh"}, "G::Promote") == "_ZN1G7PromoteEh");
    CHECK(guessSymbol(syms, "cfunc") == "_cfunc");
}

static void testPassGroups() {
    std::vector<PassGroup> g;
    std::string err;
    CHECK(parsePassSpecs({}, g, err) && g.size() == allPasses().size());
    CHECK(parsePassSpecs({"reorder_saves,invert_if", "swap_operands+flip_compare"}, g, err));
    CHECK(g.size() == 3 && g[2].passes.size() == 2 && g[2].name() == "swap_operands+flip_compare");
    CHECK(!parsePassSpecs({"reorder_saves+nope"}, g, err) && contains(err, "nope"));

    std::string src = R"(
void G::f(u8 idx)
{
    W* a = leader->w1;
    W* b = list[idx]->w1;
    W* c = leader->w2;
    t->Copy(leader);
    if (idx < 3) x(); else y();
}
)";
    size_t b, e;
    CHECK(locateFunction(src, "G::f", b, e, err));
    std::string text = src.substr(b, e - b);
    parsePassSpecs({"reorder_saves+invert_if"}, g, err);
    std::set<std::string> combos;
    enumerateGroup(g[0], text, [&](const std::string& s) {
        combos.insert(s);
        return true;
    });
    // 5 new orders, 1 inversion, and each order with the inversion
    CHECK(combos.size() == 5 + 1 + 5 * 1);
    bool both = false;
    for (auto& s : combos)
        both |= s.find("W* c") < s.find("W* a") && contains(s, "if (idx >= 3) y(); else x();");
    CHECK(both);
    // stopping early
    int n = 0;
    enumerateGroup(g[0], text, [&](const std::string&) { return ++n < 3; });
    CHECK(n == 3);

    Rng rng(7);
    std::string out;
    CHECK(randomGroupMutation(g[0], text, rng, out) && out != text);
    CHECK(contains(out, "if (idx >= 3)")); // invert_if always has something to do
}

int main() {
    testPassGroups();
    testLexerRoundTrip();
    testLocate();
    testParser();
    testEffects();
    testReorderSaves();
    testInlineLocal();
    testSinkHoistMerge();
    testExprPasses();
    testExprParser();
    testScorer();
    if (gFailures) {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
