#pragma once
// Source text to tokens.
//
// Statements need no terminator. Nothing in the grammar lets an expression
// begin with `let`, `if`, `while`, `for`, `return`, `break`, `continue` or
// `on`, so a statement always knows where it ends -- except for one genuine
// ambiguity, `f(x)` on one line followed by `(y)` on the next, which reads
// equally well as a call of a call. The parser resolves it by refusing to
// continue a postfix chain across a line break, which is why every token
// carries its line and why the lexer is the thing that knows about them.
#include <string>
#include <vector>

namespace blocky::script {

enum class TokenKind {
    End,

    Identifier,
    Number,
    String,

    // Keywords
    Let, Fn, If, Else, While, For, In, Return, Break, Continue,
    True, False, Nil, On, And, Or, Not,

    // Punctuation and operators
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Dot, Colon, Semicolon,
    Plus, Minus, Star, Slash, Percent,
    Assign, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;     // identifiers, strings, and the raw text of others
    double number = 0.0;
    int line = 1;
};

struct LexResult {
    std::vector<Token> tokens;
    std::string error;    // empty when the scan succeeded
    int errorLine = 0;
};

LexResult lex(const std::string& source);

const char* tokenName(TokenKind kind);

} // namespace blocky::script
