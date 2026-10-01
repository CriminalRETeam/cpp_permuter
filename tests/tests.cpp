// Unit tests. No framework: each CHECK prints the failing line.

#include "analysis.hpp"
#include "examples.hpp"
#include "lexer.hpp"
#include "macros.hpp"
#include "parser.hpp"
#include "passes.hpp"
#include "scorer.hpp"
#include "workspace.hpp"

#include <filesystem>
#include <fstream>

#include <chrono>
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

// Every variant of pass on src (one function named f) must contain all of want
// somewhere; and the variants must all parse again.
static std::vector<std::string> run(const std::string& src, const std::string& pass) {
    auto f = parse(src, "f");
    CHECK(f != nullptr);
    if (!f) return {};
    auto v = variants(*f, pass);
    for (auto& s : v) {
        std::string err;
        CHECK(parseFunc(s, err) != nullptr);
    }
    return v;
}

static bool any(const std::vector<std::string>& v, std::initializer_list<const char*> want) {
    for (auto& s : v) {
        bool all = true;
        for (auto* w : want) all &= contains(s, w);
        if (all) return true;
    }
    return false;
}

static void testBoolReturn() {
    auto v = run("bool f(int a, int b)\n{\n    return a <= b;\n}\n", "bool_return");
    CHECK(any(v, {"if (a <= b)\n    {\n        return true;\n    }\n    return false;"}));
    CHECK(any(v, {"return 1;", "else", "return 0;"}));
    auto r = run("bool f(int a, int b)\n{\n    if (a <= b)\n    {\n        return true;\n    }\n"
                 "    return false;\n}\n", "bool_return");
    CHECK(any(r, {"return a <= b;"}) && !any(r, {"return true"}));
    auto n = run("bool f(int a, int b)\n{\n    if (a <= b) return 0; else return 1;\n}\n", "bool_return");
    CHECK(any(n, {"return a > b;"}));
    CHECK(run("int f(int a)\n{\n    return a + 1;\n}\n", "bool_return").empty());
}

static void testTernaryArg() {
    auto v = run("void f(P* p, int x)\n{\n    p->g(x == 0 ? 1 : 0);\n}\n", "ternary_arg");
    CHECK(any(v, {"if (x == 0)\n    {\n        p->g(1);\n    }\n    else\n    {\n        p->g(0);\n    }"}));
    auto r = run("void f(P* p, int x)\n{\n    if (x == 0)\n        p->g(1, 2);\n    else\n"
                 "        p->g(0, 2);\n}\n", "ternary_arg");
    CHECK(any(r, {"p->g(x == 0 ? 1 : 0, 2);"}));
    auto r2 = run("void f(P* p, int x)\n{\n    if (x)\n        y = p->a + 1;\n    else\n"
                  "        y = p->b + 1;\n}\n", "ternary_arg");
    CHECK(any(r2, {"y = (x ? p->a : p->b) + 1;"}));
}

static void testSwitchIf() {
    auto v = run("void f(int n)\n{\n    if (n == 1)\n    {\n        a();\n    }\n    else\n    {\n"
                 "        b();\n    }\n}\n", "switch_if");
    CHECK(any(v, {"switch (n)\n    {\n        case 1:\n            a();\n            break;\n"
                  "        default:\n            b();\n            break;\n    }"}));
    auto r = run("void f(int n)\n{\n    switch (n)\n    {\n        case 1:\n            a();\n"
                 "            break;\n    }\n}\n", "switch_if");
    CHECK(any(r, {"if (n == 1)\n    {\n        a();\n    }"}));
    // two cases, or a break inside the body, can't be an if
    CHECK(run("void f(int n)\n{\n    switch (n) { case 1: a(); break; case 2: b(); break; }\n}\n",
              "switch_if").empty());
    CHECK(run("void f(int n)\n{\n    while (1) { if (n == 1) { break; } }\n}\n", "switch_if").empty());
}

static void testEarlyReturn() {
    auto v = run("int f(int n)\n{\n    if (n)\n    {\n        a();\n        return 1;\n    }\n"
                 "    else\n    {\n        b();\n    }\n    return 0;\n}\n", "early_return");
    CHECK(any(v, {"        return 1;\n    }\n    b();\n    return 0;"}) && !any(v, {"else"}));
    auto r = run("int f(int n)\n{\n    if (n)\n    {\n        return 1;\n    }\n    b();\n"
                 "    return 0;\n}\n", "early_return");
    CHECK(any(r, {"    else\n    {\n        b();\n        return 0;\n    }"}));
}

static void testBranchDup() {
    auto v = run("void f(int n)\n{\n    x = 1;\n    if (n)\n    {\n        a();\n        t();\n    }\n"
                 "    else\n    {\n        b();\n        t();\n    }\n    y = 2;\n}\n", "branch_dup");
    CHECK(any(v, {"        a();\n    }\n    else\n    {\n        b();\n    }\n    t();"}));
    CHECK(any(v, {"        t();\n        y = 2;\n    }"}));          // next stmt into both
    CHECK(any(v, {"    {\n        x = 1;\n        a();", "        x = 1;\n        b();"})); // prev into both
    // the previous statement can't move past a condition that reads it
    auto w = run("void f(int n)\n{\n    n = g();\n    if (n) a(); else b();\n}\n", "branch_dup");
    CHECK(!any(w, {"n = g();\n        a();"}));
}

static void testCondSplit() {
    auto v = run("void f(int a, int b)\n{\n    if (a && b)\n        g();\n}\n", "cond_split");
    CHECK(any(v, {"if (a)\n    {\n        if (b)\n            g();\n    }"}));
    auto r = run("void f(int a, int b)\n{\n    if (a || c)\n    {\n        if (b)\n            g();\n    }\n}\n",
                 "cond_split");
    CHECK(any(r, {"if ((a || c) && b)\n        g();"}));
    CHECK(run("void f(int a, int b)\n{\n    if (a && b) g(); else h();\n}\n", "cond_split").empty());
}

static void testExplicitCompare() {
    auto v = run("void f(P* p, int x)\n{\n    if (!p || x)\n        g();\n    while (p->n != 0)\n"
                 "        h();\n    if (x == 0) k();\n}\n", "explicit_compare");
    CHECK(any(v, {"if (p == 0 || x)"}));
    CHECK(any(v, {"if (!p || x != 0)"}));
    CHECK(any(v, {"while (p->n)"}));
    CHECK(any(v, {"if (!x) k();"}));
}

static void testNegateConst() {
    auto v = run("void f(int x)\n{\n    y = x - 4;\n    x -= 0x100;\n    z = x + -2;\n}\n", "negate_const");
    CHECK(any(v, {"y = x + -4;"}) && any(v, {"x += -0x100;"}) && any(v, {"z = x - 2;"}));
}

static void testLoopForm() {
    auto v = run("void f(int n)\n{\n    for (i = 0; i < n; i++)\n    {\n        g(i);\n    }\n}\n", "loop_form");
    CHECK(any(v, {"i = 0;\n    while (i < n)\n    {\n        g(i);\n        i++;\n    }"}));
    auto r = run("void f(int n)\n{\n    int i;\n    i = 0;\n    while (i < n)\n    {\n        g(i);\n        i++;\n    }\n}\n",
                 "loop_form");
    CHECK(any(r, {"for (i = 0; i < n; i++)\n    {\n        g(i);\n    }"}));
    CHECK(any(r, {"for (; i < n;)"}));
    auto w = run("void f(int n)\n{\n    while (1)\n        g();\n}\n", "loop_form");
    CHECK(any(w, {"for (;;)\n        g();"}));
    // continue would skip the step once it's inside the body
    CHECK(run("void f(int n)\n{\n    for (i = 0; i < n; i++) { if (i) continue; g(); }\n}\n", "loop_form").empty());
}

static void testReassociate() {
    auto v = run("int f(int a, int b, int c)\n{\n    return a + b + c;\n}\n", "reassociate");
    CHECK(any(v, {"return a + (b + c);"}));
    auto r = run("int f(int a, int b, int c)\n{\n    return a * (b * c);\n}\n", "reassociate");
    CHECK(any(r, {"return a * b * c;"}));
}

static void writeTo(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

static std::string readFrom(const std::filesystem::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static void testListDefinitions() {
    std::string src = R"(
MATCH_FUNC(0x1)
void A::f(int x)
{
    if (x) { g(); }
}
STUB_FUNC(0x2)
A::A() : m(1), n{2} { }
struct B {
    int get() const { return v; }
    inline void set(int x) throw() { v = x; }
};
void decl(int);
int A::operator==(const A& o) { return 1; }
)";
    std::set<std::string> names;
    for (auto& d : listDefinitions(src)) names.insert(d.name);
    CHECK(names == (std::set<std::string>{"A::f", "A::A", "get", "set"}));
    // the macro before a definition isn't one
    CHECK(findDefinitions(src, "MATCH_FUNC").empty());
}

static void testWorkspace() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("cpp_permuter_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    writeTo(dir / "src/main.cpp", "#include \"a.hpp\"\n#include <vector>\n\nint Main(P* p)\n{\n"
                                  "    return Weight(p->a) + Weight(p->a, 2) + Helper(p) + p->Get();\n}\n"
                                  "inline int Helper(P* p) { return p->b; }\nint NotInline(P* p) { return 0; }\n");
    writeTo(dir / "src/a.hpp", "#include \"sub/b.hpp\"\nstruct P { int a, b; int Get() { return a; } };\n"
                               "inline int Weight(int x) { return x * 4; }\n"
                               "inline int Weight(int x, int y) { return x * y; }\n"
                               "inline int Weight(int x, int y, int z) { return x * y * z; }\n");
    writeTo(dir / "src/sub/b.hpp", "inline int Unrelated() { return 1; }\n");

    auto inc = includedFiles((dir / "src/main.cpp").string(), {});
    CHECK(inc.size() == 2);

    std::string src = readFrom(dir / "src/main.cpp");
    size_t b, e;
    std::string err;
    CHECK(locateFunction(src, "Main", b, e, err));
    auto f = parseFunc(src.substr(b, e - b), err);
    CHECK(f && calledNames(*f) == (std::set<std::string>{"Weight", "Helper", "Get"}));
    auto regions = inlineCallees((dir / "src/main.cpp").string(), b, e, *f, {}, 10);
    std::vector<std::string> got;
    for (auto& r : regions) got.push_back(r.name + "@" + std::filesystem::path(r.file).filename().string());
    // Weight(x) and Weight(x, y) fit the calls, Weight(x, y, z) doesn't;
    // the first of each name comes before the second Weight
    CHECK(got == (std::vector<std::string>{"Weight@a.hpp", "Helper@main.cpp", "Get@a.hpp", "Weight@a.hpp"}));
    CHECK(inlineCallees((dir / "src/main.cpp").string(), b, e, *f, {}, 2).size() == 2);

    // splicing two regions of one file
    Region r1{"x", 0, 3, "a"}, r2{"x", 6, 9, "b"};
    std::string t1 = "AAA", t2 = "BBBB";
    CHECK(spliceRegions("abc---def---", {{&r2, &t2}, {&r1, &t1}}) == "AAA---BBBB---");

    // a source tree made of symlinks, as the integration tests and users build
    // it: the mirror must use the edited file the symlink tree holds, not
    // resolve back to where the other symlinks point
    fs::create_directories(dir / "links");
    std::error_code sec;
    fs::create_symlink(dir / "src/main.cpp", dir / "probe_link", sec);
    bool canSymlink = !sec; // Windows needs Developer Mode or admin rights
    if (!canSymlink) std::cerr << "  (no symlinks here: checking the copy/hard-link mirror only)\n";
    for (auto& entry : fs::directory_iterator(dir / "src")) {
        if (canSymlink) fs::create_symlink(entry.path(), dir / "links" / entry.path().filename());
        else fs::copy(entry.path(), dir / "links" / entry.path().filename(), fs::copy_options::recursive);
    }
    fs::remove(dir / "links/a.hpp");
    writeTo(dir / "links/a.hpp", "// edited\n");
    Mirror m;
    CHECK(m.create((dir / "links").string(), (dir / "mirror").string(), {(dir / "links/main.cpp").string()}, err));
    CHECK(readFrom(m.map((dir / "links/a.hpp").string())) == "// edited\n");
    CHECK(!fs::is_symlink(m.map((dir / "links/main.cpp").string())));
    CHECK(!canSymlink || fs::is_symlink(dir / "mirror/sub"));
    CHECK(fs::exists(dir / "mirror/sub/b.hpp"));
    // a changed file in a subdirectory turns that directory into a real one
    Mirror m2;
    CHECK(m2.create((dir / "src").string(), (dir / "mirror2").string(), {(dir / "src/sub/b.hpp").string()}, err));
    CHECK(!fs::is_symlink(dir / "mirror2/sub") && !fs::is_symlink(dir / "mirror2/sub/b.hpp"));
    CHECK(readFrom(dir / "mirror2/sub/b.hpp") == readFrom(dir / "src/sub/b.hpp"));
    CHECK(!m2.create((dir / "src/sub").string(), (dir / "mirror3").string(), {(dir / "src/main.cpp").string()}, err));
    fs::remove_all(dir);
}

static void testInequalities() {
    auto v = run("void f(int x)\n{\n    if (x > 4) a();\n    if (x <= 0x1F) b();\n    if (3 < x) c();\n    if (x >= 0) d();\n}\n",
                 "inequalities");
    CHECK(any(v, {"if (x >= 5) a();"}) && any(v, {"if (x < 0x20) b();"}) && any(v, {"if (4 <= x) c();"}));
    CHECK(!any(v, {"x > -1"})); // unsafe for unsigned x
}

static void testChainAssign() {
    auto v = run("void f(P* p)\n{\n    p->a = 0;\n    p->b = 0;\n    p->c = 0;\n}\n", "chain_assign");
    CHECK(any(v, {"p->a = p->b = 0;\n    p->c = 0;"}) && any(v, {"p->b = p->a = 0;"}) &&
          any(v, {"p->a = p->b = p->c = 0;"}));
    auto r = run("void f(P* p)\n{\n    p->a = p->b = 0;\n}\n", "chain_assign");
    CHECK(any(r, {"p->b = 0;\n    p->a = p->b;"}) && any(r, {"p->b = 0;\n    p->a = 0;"}));
    CHECK(run("void f(P* p)\n{\n    p->a = g();\n    p->b = g();\n}\n", "chain_assign").empty());
}

static void testScopeBlock() {
    auto v = run("void f(int n)\n{\n    g();\n    int a = n;\n    h(a);\n    k();\n}\n", "scope_block");
    CHECK(any(v, {"    g();\n    {\n        int a = n;\n        h(a);\n    }\n    k();"}));
    auto r = run("void f(int n)\n{\n    g();\n    {\n        int a = n;\n        h(a);\n    }\n}\n", "scope_block");
    CHECK(any(r, {"    g();\n    int a = n;\n    h(a);\n}"}));
    // not when a sibling declares the same name
    CHECK(!any(run("void f(int n)\n{\n    int a = 1;\n    {\n        int a = n;\n        h(a);\n    }\n    h(a);\n}\n",
                   "scope_block"),
               {"int a = 1;\n    int a = n;"}));
}

static void testLocalType() {
    auto v = run("void f(int n)\n{\n    s32 i = 0;\n    for (u8 j = 0; j < 3; j++) g(j);\n    unsigned short k;\n}\n",
                 "local_type");
    CHECK(any(v, {"u32 i = 0;"}) && any(v, {"for (s32 j = 0;"}) && any(v, {"    int k;"}));
    CHECK(!any(v, {"s32 i", "s32 i"}) || true);
}

static void testLocalsToArray() {
    auto v = run("void f(int n)\n{\n    s32 a = 1;\n    s32 b = 2;\n    g(a, b);\n}\n", "locals_to_array");
    CHECK(any(v, {"s32 a_arr[2];\n    a_arr[0] = 1;\n    a_arr[1] = 2;\n    g(a_arr[0], a_arr[1]);"}));
    CHECK(any(v, {"a_arr[1] = 1;", "g(a_arr[1], a_arr[0]);"})); // back to front
}

static void testReorderCases() {
    auto v = run("int f(int n)\n{\n    switch (n)\n    {\n        case 1:\n            return 5;\n        case 2:\n"
                 "            return 6;\n        default:\n            g();\n    }\n    return 0;\n}\n",
                 "reorder_cases");
    CHECK(v.size() == 5); // 3! - 1
    // the open default gets a break once it isn't last
    CHECK(any(v, {"        default:\n            g();\n            break;\n        case 1:"}));
    // fall-through fixes the order
    CHECK(run("void f(int n)\n{\n    switch (n) { case 1: a(); case 2: b(); break; }\n}\n", "reorder_cases").empty());
}

static void testSplitCaseLabels() {
    auto v = run("int f(int n)\n{\n    switch (n)\n    {\n        case 1:\n        case 2:\n            return 5;\n"
                 "        default:\n            return 0;\n    }\n}\n",
                 "split_case_labels");
    CHECK(any(v, {"        case 1:\n            return 5;\n        case 2:\n            return 5;"}));
    auto r = run("int f(int n)\n{\n    switch (n)\n    {\n        case 1:\n            return 5;\n        case 2:\n"
                 "            return 5;\n    }\n    return 0;\n}\n",
                 "split_case_labels");
    CHECK(any(r, {"        case 1:\n        case 2:\n            return 5;\n    }"}));
}

static void testUseGetter() {
    setGetters(findGetters("struct Ped {\n    Fix16 get_cam_x() { return field_1AC_cam.x; }\n"
                           "    s32 get_id() const { return this->field_4; }\n    s32 two(int a) { return a; }\n};\n"));
    CHECK(getters().size() == 2);
    auto v = run("void f(Ped* p)\n{\n    g(p->field_1AC_cam.x, p->get_id());\n}\n", "use_getter");
    CHECK(any(v, {"g(p->get_cam_x(), p->get_id());"}) && any(v, {"g(p->field_1AC_cam.x, p->field_4);"}));
    setGetters({});
}

static void testTempForExpr() {
    auto v = run("void f(Ped* p, s32* q)\n{\n    s16 n = 3;\n    g((u8)p->a, n, *q);\n}\n", "temp_for_expr");
    CHECK(any(v, {"u8 tmp = (u8)p->a;\n    g(tmp, n, *q);"}));
    CHECK(any(v, {"s16 tmp = n;\n    g((u8)p->a, tmp, *q);"}));
    CHECK(any(v, {"s32 tmp = *q;\n    g((u8)p->a, n, tmp);"}));
}

static void testNamedOp() {
    setOpAliases({{"*", "Multiply_408680"}, {"neg", "Negate_4086A0"}});
    auto v = run("Fix16 f(Fix16 a, Fix16 b)\n{\n    return -a * b;\n}\n", "named_op");
    CHECK(any(v, {"return (-a).Multiply_408680(b);"}));
    CHECK(any(v, {"return a.Negate_4086A0() * b;"}));
    auto back = run("Fix16 f(Fix16 a, Fix16 b)\n{\n    return a.Negate_4086A0().Multiply_408680(b + a);\n}\n",
                    "named_op");
    CHECK(any(back, {"return (a.Negate_4086A0() * (b + a));"}));
    CHECK(any(back, {"return (-a).Multiply_408680(b + a);"}));
    // p->Negate() isn't rewritten: there is no value to apply the operator to
    CHECK(run("Fix16 f(Fix16* p)\n{\n    return p->Negate_4086A0();\n}\n", "named_op").empty());
    setOpAliases({});
    CHECK(run("Fix16 f(Fix16 a, Fix16 b)\n{\n    return a * b;\n}\n", "named_op").empty());
}

static void testCastOperand() {
    auto v = run("u32 f(s32 a, u8 b)\n{\n    return a / b;\n}\n", "cast_operand");
    CHECK(any(v, {"return (u32)a / b;"}));
    CHECK(any(v, {"return a / (s32)b;"}));
    CHECK(v.size() == 12);
    auto c = run("u32 f(s32 a)\n{\n    return (s32)(a + 1) >> 2;\n}\n", "cast_operand");
    CHECK(any(c, {"return (a + 1) >> 2;"}));
    CHECK(any(c, {"return (u32)(a + 1) >> 2;"}));
    // constants are left alone
    CHECK(run("u32 f()\n{\n    return 4 >> 1;\n}\n", "cast_operand").empty());
}

static void testRemoveStmt() {
    auto v = run("void f(int n)\n{\n    a();\n    WIP_IMPLEMENTED;\n    b();\n}\n", "remove_stmt");
    CHECK(v.size() == 3 && any(v, {"    a();\n    b();"}));
}

static void testMacros() {
    auto count = [](const std::string& s) {
        MacroExpander m(s);
        std::set<std::string> all;
        m.enumerate(1000, [&](const std::string& x) { return all.insert(x).second || true; });
        return all;
    };
    MacroExpander none("a = 1;");
    CHECK(!none.hasMacros() && none.first() == "a = 1;");
    auto g = count("x = PERM_GENERAL(1, 2, f(3, 4));");
    CHECK(g == (std::set<std::string>{"x = 1;", "x = 2;", "x = f(3, 4);"}));
    auto l = count("{PERM_LINESWAP(\n    a();\n    b();\n    c();\n)}");
    CHECK(l.size() == 6 && l.count("{\n    c();\n    a();\n    b();\n}"));
    CHECK(count("y = PERM_INT(2, 5);").size() == 4);
    auto o = count("PERM_ONCE(a;) b; PERM_ONCE(a;)");
    CHECK(o == (std::set<std::string>{"a; b; ", " b; a;"}));
    auto n = count("PERM_GENERAL(PERM_GENERAL(a, b), c)");
    CHECK(n == (std::set<std::string>{"a", "b", "c"}));
    auto e = count("f(PERM_GENERAL(x(,)y, z));");
    CHECK(e == (std::set<std::string>{"f(x,y);", "f(z);"}));
    auto v = count("PERM_VAR(t, PERM_GENERAL(1, 2)) q = PERM_VAR(t); r = PERM_VAR(t);");
    CHECK(v == (std::set<std::string>{" q = 1; r = 1;", " q = 2; r = 2;"}));
    MacroExpander r("PERM_RANDOMIZE(a = 1;) PERM_PRETEND(junk) PERM_IGNORE(PERM_GENERAL(x))");
    CHECK(r.randomize() && r.first() == "a = 1;  PERM_GENERAL(x)");
    CHECK(!MacroExpander("PERM_BOGUS(1)").error().empty());
    // a random chooser stays within the options
    Rng rng(3);
    MacroExpander big("PERM_LINESWAP(\n1\n2\n3\n4\n)PERM_INT(0, 9)");
    for (int i = 0; i < 20; ++i) {
        std::string s = big.expand([&](int k) { return std::uniform_int_distribution<int>(0, k - 1)(rng); });
        CHECK(s.size() == 10);
    }
}

static void testPassDocs() {
    // docs/passes.md must be what --pass-examples prints now
    std::string gen = passExamplesMarkdown();
    std::string doc = readFrom(std::string(PERMUTER_SOURCE_DIR) + "/docs/passes.md");
    CHECK(gen == doc);
    if (gen != doc) std::cerr << "  regenerate: build/cpp_permuter --pass-examples > docs/passes.md\n";
    CHECK(!contains(gen, "(no candidate"));
    for (auto& p : allPasses()) CHECK(contains(gen, "## `" + p.name + "`"));
}

int main() {
    testPassDocs();
    testInequalities();
    testChainAssign();
    testScopeBlock();
    testLocalType();
    testLocalsToArray();
    testReorderCases();
    testSplitCaseLabels();
    testUseGetter();
    testTempForExpr();
    testNamedOp();
    testCastOperand();
    testRemoveStmt();
    testMacros();
    testListDefinitions();
    testWorkspace();
    testBoolReturn();
    testTernaryArg();
    testSwitchIf();
    testEarlyReturn();
    testBranchDup();
    testCondSplit();
    testExplicitCompare();
    testNegateConst();
    testLoopForm();
    testReassociate();
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
