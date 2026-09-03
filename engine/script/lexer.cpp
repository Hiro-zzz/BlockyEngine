#include "engine/script/lexer.hpp"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace blocky::script {
namespace {

const std::unordered_map<std::string, TokenKind>& keywords() {
    static const std::unordered_map<std::string, TokenKind> table = {
        {"let", TokenKind::Let},           {"fn", TokenKind::Fn},
        {"if", TokenKind::If},             {"else", TokenKind::Else},
        {"while", TokenKind::While},       {"for", TokenKind::For},
        {"in", TokenKind::In},             {"return", TokenKind::Return},
        {"break", TokenKind::Break},       {"continue", TokenKind::Continue},
        {"true", TokenKind::True},         {"false", TokenKind::False},
        {"nil", TokenKind::Nil},           {"on", TokenKind::On},
        {"and", TokenKind::And},           {"or", TokenKind::Or},
        {"not", TokenKind::Not},
    };
    return table;
}

bool identifierStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identifierPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

const char* tokenName(TokenKind kind) {
    switch (kind) {
        case TokenKind::End: return "end of file";
        case TokenKind::Identifier: return "identifier";
        case TokenKind::Number: return "number";
        case TokenKind::String: return "string";
        case TokenKind::Let: return "let";
        case TokenKind::Fn: return "fn";
        case TokenKind::If: return "if";
        case TokenKind::Else: return "else";
        case TokenKind::While: return "while";
        case TokenKind::For: return "for";
        case TokenKind::In: return "in";
        case TokenKind::Return: return "return";
        case TokenKind::Break: return "break";
        case TokenKind::Continue: return "continue";
        case TokenKind::True: return "true";
        case TokenKind::False: return "false";
        case TokenKind::Nil: return "nil";
        case TokenKind::On: return "on";
        case TokenKind::And: return "and";
        case TokenKind::Or: return "or";
        case TokenKind::Not: return "not";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::Comma: return ",";
        case TokenKind::Dot: return ".";
        case TokenKind::Colon: return ":";
        case TokenKind::Semicolon: return ";";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Assign: return "=";
        case TokenKind::Equal: return "==";
        case TokenKind::NotEqual: return "!=";
        case TokenKind::Less: return "<";
        case TokenKind::LessEqual: return "<=";
        case TokenKind::Greater: return ">";
        case TokenKind::GreaterEqual: return ">=";
    }
    return "?";
}

LexResult lex(const std::string& source) {
    LexResult result;
    size_t i = 0;
    int line = 1;

    auto fail = [&](const std::string& message) {
        result.error = message;
        result.errorLine = line;
    };

    auto push = [&](TokenKind kind, const std::string& text) {
        Token token;
        token.kind = kind;
        token.text = text;
        token.line = line;
        result.tokens.push_back(token);
    };

    while (i < source.size()) {
        char c = source[i];

        if (c == '\n') { ++line; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }

        if (identifierStart(c)) {
            size_t start = i;
            while (i < source.size() && identifierPart(source[i])) ++i;
            std::string word = source.substr(start, i - start);

            auto found = keywords().find(word);
            push(found != keywords().end() ? found->second : TokenKind::Identifier, word);
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < source.size() && std::isdigit(static_cast<unsigned char>(source[i + 1])))) {
            size_t start = i;
            while (i < source.size() &&
                   (std::isdigit(static_cast<unsigned char>(source[i])) || source[i] == '.'))
                ++i;
            // An exponent, but only when it really is one: `1e3` is a number
            // and `x1e` is not reachable here because identifiers win first.
            if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
                size_t mark = i;
                ++i;
                if (i < source.size() && (source[i] == '+' || source[i] == '-')) ++i;
                if (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) {
                    while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i]))) ++i;
                } else {
                    i = mark;
                }
            }

            std::string text = source.substr(start, i - start);
            char* end = nullptr;
            double value = std::strtod(text.c_str(), &end);
            if (!end || *end != '\0') { fail("malformed number '" + text + "'"); return result; }

            Token token;
            token.kind = TokenKind::Number;
            token.text = text;
            token.number = value;
            token.line = line;
            result.tokens.push_back(token);
            continue;
        }

        if (c == '"') {
            ++i;
            std::string text;
            bool closed = false;
            int startLine = line;

            while (i < source.size()) {
                char ch = source[i];
                if (ch == '"') { ++i; closed = true; break; }
                if (ch == '\n') { ++line; text.push_back(ch); ++i; continue; }
                if (ch == '\\' && i + 1 < source.size()) {
                    char escape = source[i + 1];
                    i += 2;
                    switch (escape) {
                        case 'n': text.push_back('\n'); break;
                        case 't': text.push_back('\t'); break;
                        case '"': text.push_back('"'); break;
                        case '\\': text.push_back('\\'); break;
                        default:
                            fail(std::string("unknown escape '\\") + escape + "'");
                            return result;
                    }
                    continue;
                }
                text.push_back(ch);
                ++i;
            }

            if (!closed) {
                line = startLine;
                fail("unterminated string");
                return result;
            }

            Token token;
            token.kind = TokenKind::String;
            token.text = text;
            token.line = startLine;
            result.tokens.push_back(token);
            continue;
        }

        auto two = [&](char next) { return i + 1 < source.size() && source[i + 1] == next; };

        switch (c) {
            case '(': push(TokenKind::LParen, "("); ++i; continue;
            case ')': push(TokenKind::RParen, ")"); ++i; continue;
            case '{': push(TokenKind::LBrace, "{"); ++i; continue;
            case '}': push(TokenKind::RBrace, "}"); ++i; continue;
            case '[': push(TokenKind::LBracket, "["); ++i; continue;
            case ']': push(TokenKind::RBracket, "]"); ++i; continue;
            case ',': push(TokenKind::Comma, ","); ++i; continue;
            case '.': push(TokenKind::Dot, "."); ++i; continue;
            case ':': push(TokenKind::Colon, ":"); ++i; continue;
            case ';': push(TokenKind::Semicolon, ";"); ++i; continue;
            case '+': push(TokenKind::Plus, "+"); ++i; continue;
            case '-': push(TokenKind::Minus, "-"); ++i; continue;
            case '*': push(TokenKind::Star, "*"); ++i; continue;
            case '/': push(TokenKind::Slash, "/"); ++i; continue;
            case '%': push(TokenKind::Percent, "%"); ++i; continue;
            case '=':
                if (two('=')) { push(TokenKind::Equal, "=="); i += 2; }
                else { push(TokenKind::Assign, "="); ++i; }
                continue;
            case '!':
                if (two('=')) { push(TokenKind::NotEqual, "!="); i += 2; continue; }
                fail("'!' is not an operator here -- use 'not'");
                return result;
            case '<':
                if (two('=')) { push(TokenKind::LessEqual, "<="); i += 2; }
                else { push(TokenKind::Less, "<"); ++i; }
                continue;
            case '>':
                if (two('=')) { push(TokenKind::GreaterEqual, ">="); i += 2; }
                else { push(TokenKind::Greater, ">"); ++i; }
                continue;
            default: break;
        }

        fail(std::string("unexpected character '") + c + "'");
        return result;
    }

    Token end;
    end.kind = TokenKind::End;
    end.line = line;
    result.tokens.push_back(end);
    return result;
}

} // namespace blocky::script
