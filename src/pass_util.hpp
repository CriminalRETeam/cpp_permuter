// Helpers shared by the pass implementations (passes.cpp, passes_control.cpp).
// Internal: not part of the permuter's interface.
#pragma once

#include "analysis.hpp"
#include "passes.hpp"

#include <memory>
#include <string>
#include <vector>

namespace perm::util {

std::vector<const Stmt*> blocksOf(const Func& f);
std::string norm(const std::string& s); // without whitespace, for comparing code
std::string exprText(const Func& f, const Expr& x);
bool isPostfixLike(const Expr& x);
int exprPrec(const Func& f, const Expr& x);
std::string paren(const std::string& s);
std::string operandText(const Func& f, const Expr& x, int prec);
int indexInParent(const Stmt& s);
const Stmt* singleStmt(const Stmt* s);
std::string negate(const Func& f, int b, int e);

// passes_control.cpp: rewriting and layout helpers
bool emitReplace(const Func& f, const EmitFn& emit, int b, int e, const std::string& text);
std::string lineInd(const Func& f, int tok);   // indentation (spaces) of tok's line
void replaceAll(std::string& s, const std::string& from, const std::string& to);
std::vector<const Stmt*> branchStmts(const Stmt* s); // a block's statements, or s
// consecutive statements re-indented so their first line sits at ind
std::string stmtsText(const Func& f, const std::vector<const Stmt*>& v, const std::string& ind);
std::string stmtText(const Func& f, const Stmt* s, const std::string& ind);
std::string bodyText(const Func& f, const Stmt* s, const std::string& ind);
std::string blockText(const Func& f, const std::vector<const Stmt*>& v, const std::string& ind,
                      const std::vector<std::string>& extraBefore = {},
                      const std::vector<std::string>& extraAfter = {});
bool isJump(const Stmt* s);
bool hasToken(const Func& f, const Stmt& s, const char* tok);
std::vector<const Stmt*> stmtsOfKind(const Func& f, SK k);
bool inBlock(const Stmt& s);

template <class Fn>
void forEachSiteExpr(const Func& f, Fn fn) {
    for (auto& site : exprSites(f)) {
        auto x = parseExpr(f, site.b, site.e);
        if (!x) continue;
        std::shared_ptr<Expr> root(std::move(x));
        bool stop = false;
        forEachExpr(*root, [&](Expr& e) {
            if (!stop && !fn(site, root, e)) stop = true;
        });
        if (stop) return;
    }
}

// passes_control.cpp
void enumBoolReturn(const Func& f, const EmitFn& emit);
void enumTernaryArg(const Func& f, const EmitFn& emit);
void enumSwitchIf(const Func& f, const EmitFn& emit);
void enumEarlyReturn(const Func& f, const EmitFn& emit);
void enumBranchDup(const Func& f, const EmitFn& emit);
void enumCondSplit(const Func& f, const EmitFn& emit);
void enumExplicitCompare(const Func& f, const EmitFn& emit);
void enumNegateConst(const Func& f, const EmitFn& emit);
void enumLoopForm(const Func& f, const EmitFn& emit);
void enumReassociate(const Func& f, const EmitFn& emit);

// passes_more.cpp
void registerMorePasses(std::vector<Pass>& passes);

} // namespace perm::util
