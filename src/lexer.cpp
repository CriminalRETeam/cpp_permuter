#include "lexer.hpp"

#include <cctype>
#include <cstring>
#include <unordered_set>

namespace perm {

namespace {

const char* kPuncts[] = {
    ">>=", "<<=", "->*", "...", "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
    "&&",  "||",  "+=",  "-=",  "*=", "/=", "%=", "&=", "|=", "^=", ".*", "##",
};

bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '$'; }
bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }

} // namespace

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0, n = s.size();
    int line = 1;
    bool lineStart = true; // only whitespace seen on this line so far
    std::string lead;

    auto advance = [&](size_t to) {
        for (size_t k = i; k < to && k < n; ++k)
            if (s[k] == '\n') line++;
        i = to;
    };

    while (i < n) {
        char c = s[i];
        // whitespace
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            if (c == '\n') lineStart = true;
            lead += c;
            advance(i + 1);
            continue;
        }
        // line continuation outside of PP lines
        if (c == '\\' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {
            lead += c;
            advance(i + 1);
            continue;
        }
        // comments
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            size_t e = s.find('\n', i);
            if (e == std::string::npos) e = n;
            lead.append(s, i, e - i);
            advance(e);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            e = (e == std::string::npos) ? n : e + 2;
            lead.append(s, i, e - i);
            advance(e);
            continue;
        }

        Token t;
        t.line = line;
        t.offset = i;
        t.lead = std::move(lead);
        lead.clear();
        size_t start = i, end = i;

        if (c == '#' && lineStart) {
            // preprocessor line, with backslash continuations
            end = i;
            while (end < n) {
                if (s[end] == '\n') {
                    size_t b = end;
                    if (b > start && s[b - 1] == '\r') b--;
                    if (b > start && s[b - 1] == '\\') {
                        end++;
                        continue;
                    }
                    break;
                }
                // a block comment can span lines inside a directive
                if (s[end] == '/' && end + 1 < n && s[end + 1] == '*') {
                    size_t e = s.find("*/", end + 2);
                    end = (e == std::string::npos) ? n : e + 2;
                    continue;
                }
                end++;
            }
            // keep the trailing \r out of the token text
            while (end > start && (s[end - 1] == '\r')) end--;
            t.kind = TokKind::PP;
        } else if (identStart(c)) {
            end = i;
            while (end < n && identChar(s[end])) end++;
            std::string id = s.substr(start, end - start);
            if (end < n && (s[end] == '"' || s[end] == '\'') &&
                (id == "L" || id == "u" || id == "U" || id == "u8")) {
                char q = s[end];
                end++;
                while (end < n && s[end] != q) {
                    if (s[end] == '\\') end++;
                    end++;
                }
                if (end < n) end++;
                t.kind = q == '"' ? TokKind::String : TokKind::Char;
            } else {
                t.kind = TokKind::Ident;
            }
        } else if (std::isdigit((unsigned char)c) ||
                   (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            end = i;
            bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            while (end < n) {
                char d = s[end];
                if (identChar(d) || d == '.') {
                    end++;
                    continue;
                }
                if ((d == '+' || d == '-') && !hex && end > start &&
                    (s[end - 1] == 'e' || s[end - 1] == 'E')) {
                    end++;
                    continue;
                }
                break;
            }
            t.kind = TokKind::Number;
        } else if (c == '"' || c == '\'') {
            end = i + 1;
            while (end < n && s[end] != c && s[end] != '\n') {
                if (s[end] == '\\') end++;
                end++;
            }
            if (end < n && s[end] == c) end++;
            t.kind = c == '"' ? TokKind::String : TokKind::Char;
        } else {
            t.kind = TokKind::Punct;
            end = i + 1;
            for (const char* p : kPuncts) {
                size_t len = std::strlen(p);
                if (s.compare(i, len, p) == 0) {
                    end = i + len;
                    break;
                }
            }
        }
        t.text = s.substr(start, end - start);
        advance(end);
        lineStart = false;
        out.push_back(std::move(t));
    }
    Token e;
    e.kind = TokKind::End;
    e.lead = std::move(lead);
    e.line = line;
    e.offset = n;
    out.push_back(std::move(e));
    return out;
}

std::string joinTokens(const std::vector<Token>& toks, int b, int e) {
    std::string r;
    for (int i = b; i < e; ++i) {
        if (i != b) r += toks[i].lead;
        r += toks[i].text;
    }
    return r;
}

bool isKeyword(const std::string& s) {
    static const std::unordered_set<std::string> kw = {
        "if", "else", "for", "while", "do", "switch", "case", "default", "return", "break",
        "continue", "goto", "sizeof", "new", "delete", "this", "true", "false", "static",
        "const", "volatile", "register", "extern", "mutable", "inline", "typedef", "struct",
        "class", "union", "enum", "template", "typename", "using", "namespace", "try",
        "catch", "throw", "operator", "static_cast", "reinterpret_cast", "const_cast",
        "dynamic_cast", "__asm", "_asm", "asm", "__try", "__except", "__finally", "__leave",
        "__forceinline", "__inline", "__declspec", "__stdcall", "__cdecl", "__fastcall",
        "__thiscall", "virtual", "public", "private", "protected", "friend", "explicit",
        "auto", "typeid", "nullptr",
    };
    return kw.count(s) || isBuiltinType(s);
}

bool isBuiltinType(const std::string& s) {
    static const std::unordered_set<std::string> t = {
        "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned",
        "bool", "wchar_t", "__int8", "__int16", "__int32", "__int64",
    };
    return t.count(s) > 0;
}

bool isAssignOp(const std::string& s) {
    return s == "=" || s == "+=" || s == "-=" || s == "*=" || s == "/=" || s == "%=" ||
           s == "&=" || s == "|=" || s == "^=" || s == "<<=" || s == ">>=";
}

} // namespace perm
