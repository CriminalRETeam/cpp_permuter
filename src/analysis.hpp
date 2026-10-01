// Expression parsing and read/write effects over token ranges.
#pragma once

#include "parser.hpp"

#include <memory>
#include <set>
#include <string>

namespace perm {

struct Expr {
    enum K { Primary, Paren, Call, Index, Member, Postfix, Unary, Cast, Binary, Assign, Ternary, Comma } k;
    int b = 0, e = 0; // token range [b, e)
    int op = -1;      // operator token (Binary/Assign/Unary/Postfix/Ternary '?')
    int colon = -1;   // Ternary ':'
    std::vector<std::unique_ptr<Expr>> kids;
};

// Parses tokens [b, e) as one expression. Returns null if the whole range
// doesn't parse.
std::unique_ptr<Expr> parseExpr(const Func& f, int b, int e);
int binPrec(const std::string& op); // 0 = not a binary operator
void forEachExpr(Expr& x, const std::function<void(Expr&)>& fn);

// Token ranges in the function that hold a complete expression.
struct ExprSite {
    int b, e;
    const Stmt* stmt;
};
std::vector<ExprSite> exprSites(const Func& f);

struct Effects {
    std::set<std::string> reads, writes;
    bool memRead = false, memWrite = false, call = false;
    bool barrier = false; // control flow, labels, asm, preprocessor
};
Effects effectsOf(const Func& f, int b, int e);
Effects effectsOfStmt(const Func& f, const Stmt& s);
bool conflicts(const Effects& a, const Effects& b);
// Like conflicts(), but two statements that only read or write memory (no
// calls) may swap: field stores and loads are assumed not to alias.
bool conflictsRelaxed(const Effects& a, const Effects& b);

// Number of token uses of a local name in [b, e).
std::vector<int> usesOf(const Func& f, const std::string& name, int b, int e);
bool isWriteUse(const Func& f, int tok);

} // namespace perm
