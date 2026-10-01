// Statement-level parser for one function body.
//
// This is not a C++ front end. It only needs to find statements, blocks,
// local declarations and expressions inside a single function, and it must
// keep going on code it can't fully understand (MSVC extensions, macros,
// types from headers it never sees). Anything it can't classify becomes an
// "Other" statement, which the passes treat as a barrier.
#pragma once

#include "lexer.hpp"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace perm {

enum class SK {
    Block, Decl, Expr, If, For, While, Do, Switch, Case, Default, Label,
    Return, Break, Continue, Goto, Asm, PP, Empty, Try, Other,
};
const char* skName(SK k);

struct Declarator {
    enum Init { None, Assign, Paren, Brace };
    int nameTok = -1;
    int begin = -1, end = -1;         // the whole declarator, init included
    int declEnd = -1;                 // end of the declarator without its init
    int initBegin = -1, initEnd = -1; // init expression (inside parens/braces for Paren/Brace)
    Init init = None;
    bool isArray = false;
    bool isRef = false;
};

struct Stmt {
    SK kind = SK::Other;
    int begin = 0, end = 0; // token range [begin, end)
    std::vector<std::unique_ptr<Stmt>> kids;
    // If: kids[0] = then, kids[1] = else (optional). Loops/Switch: kids[0] = body.
    // Try: the guarded block and the handler blocks.
    int condOpen = -1, condClose = -1; // the '(' and ')' of if/while/for/switch/do-while
    int elseTok = -1;
    // Decl
    int typeBegin = -1, typeEnd = -1;
    std::vector<Declarator> decls;
    bool isStatic = false;
    bool isConst = false;
    // For: the decl in the init clause, if any
    std::unique_ptr<Stmt> forInit;
    Stmt* parent = nullptr;
};

struct Func {
    std::string text; // the function from its (qualified) name to the closing brace
    std::vector<Token> toks;
    int paramOpen = -1, paramClose = -1;
    int bodyOpen = -1, bodyClose = -1;
    std::unique_ptr<Stmt> body;
    std::vector<std::string> params;
    std::set<std::string> locals; // params and every local declared in the body

    std::string tokText(int b, int e) const { return joinTokens(toks, b, e); }
    std::string textOf(const Stmt& s) const { return tokText(s.begin, s.end); }
    const std::string& t(int i) const { return toks[i].text; }
    bool isLocal(int i) const;
    // Indentation of the line holding token i ("\n" + spaces), for inserted statements.
    std::string indentOf(int i) const;
};

// Finds the definition of qualName ("Class::Method", "Method", "Class::~Class")
// in src. On success fills [start, end) with the byte range from the start of
// the qualified name to the closing brace (inclusive).
bool locateFunction(const std::string& src, const std::string& qualName, size_t& start,
                    size_t& end, std::string& err);

// Byte ranges of every definition (with a body) of qualName in src.
std::vector<std::pair<size_t, size_t>> findDefinitions(const std::string& src,
                                                       const std::string& qualName);

struct Definition {
    std::string name; // as written, e.g. "Ped::GetX" or "GetX" inside a class
    size_t start, end;
};
// Every function definition in src (not nested in another function's body).
std::vector<Definition> listDefinitions(const std::string& src);

// Parses the text produced by locateFunction. Returns null and sets err on failure.
std::unique_ptr<Func> parseFunc(const std::string& text, std::string& err);

// Calls fn on every statement in pre-order (the body block included).
void forEachStmt(const Stmt& s, const std::function<void(const Stmt&)>& fn);

// Index of the token closing the bracket at i, or -1.
int matchBracket(const std::vector<Token>& toks, int i);

std::string dumpStmt(const Func& f, const Stmt& s, int depth = 0);

} // namespace perm
