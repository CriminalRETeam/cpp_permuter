// Tolerant C++ tokenizer. Every token keeps the whitespace and comments that
// precede it ("lead"), so joining lead + text over all tokens reproduces the
// input exactly. Preprocessor lines become single PP tokens.
#pragma once

#include <string>
#include <vector>

namespace perm {

enum class TokKind { Ident, Number, String, Char, Punct, PP, End };

struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    std::string lead;   // whitespace/comments before the token
    int line = 0;       // 1-based line of the token text
    size_t offset = 0;  // byte offset of text in the source
};

std::vector<Token> lex(const std::string& src);

// Joins tokens [b, e): lead of b is omitted, leads of the rest are kept.
std::string joinTokens(const std::vector<Token>& toks, int b, int e);

bool isKeyword(const std::string& s);
bool isBuiltinType(const std::string& s);
bool isAssignOp(const std::string& s);

} // namespace perm
