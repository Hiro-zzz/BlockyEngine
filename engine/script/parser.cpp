#include "engine/script/parser.hpp"

#include <stdexcept>

namespace blocky::script {
namespace {

struct SyntaxError {
    std::string message;
    int line;
};

class Parser {
public:
    Parser(std::vector<Token> tokens, Program& program)
        : tokens_(std::move(tokens)), program_(program) {}

    void parseProgram() {
        while (!at(TokenKind::End)) program_.statements.push_back(statement());
    }

private:
    // ------------------------------------------------------------- plumbing
    const Token& peek(size_t ahead = 0) const {
        size_t index = position_ + ahead;
        return index < tokens_.size() ? tokens_[index] : tokens_.back();
    }
    bool at(TokenKind kind) const { return peek().kind == kind; }
    int lastConsumedLine() const { return position_ > 0 ? tokens_[position_ - 1].line : 1; }
    const Token& advance() { return tokens_[position_ < tokens_.size() ? position_++ : position_]; }

    bool accept(TokenKind kind) {
        if (!at(kind)) return false;
        advance();
        return true;
    }

    const Token& expect(TokenKind kind, const char* what) {
        if (!at(kind)) {
            throw SyntaxError{std::string("expected ") + what + " but found '" + peek().text + "'",
                              peek().line};
        }
        return advance();
    }

    // Semicolons are allowed and never required; a run of them is nothing.
    void skipSeparators() { while (accept(TokenKind::Semicolon)) {} }

    std::unique_ptr<Expr> makeExpr(ExprKind kind, int line) {
        auto expr = std::make_unique<Expr>();
        expr->kind = kind;
        expr->line = line;
        return expr;
    }

    // ----------------------------------------------------------- statements
    std::unique_ptr<Stmt> statement() {
        skipSeparators();
        int line = peek().line;

        if (accept(TokenKind::Let)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::Let;
            stmt->line = line;
            stmt->name = expect(TokenKind::Identifier, "a name after 'let'").text;
            if (accept(TokenKind::Assign)) stmt->expr = expression();
            skipSeparators();
            return stmt;
        }

        if (at(TokenKind::Fn) && peek(1).kind == TokenKind::Identifier) {
            advance();
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::Function;
            stmt->line = line;
            stmt->name = advance().text;
            stmt->fn = functionRest(stmt->name, line);
            return stmt;
        }

        if (accept(TokenKind::On)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::On;
            stmt->line = line;
            stmt->name = expect(TokenKind::Identifier, "an event name after 'on'").text;

            // `on tick(dt) { }` and `on ready { }` are both allowed: an event
            // that hands you nothing should not make you write empty parens.
            if (at(TokenKind::LParen)) {
                stmt->fn = functionRest(stmt->name, line);
            } else {
                auto decl = std::make_unique<FunctionDecl>();
                decl->name = stmt->name;
                decl->line = line;
                auto block = blockStatement();
                decl->body = block.get();
                stmt->fn = decl.get();
                program_.functions.push_back(std::move(decl));
                ownedBlocks_.push_back(std::move(block));
            }
            return stmt;
        }

        if (accept(TokenKind::If)) return ifRest(line);

        if (accept(TokenKind::While)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::While;
            stmt->line = line;
            stmt->expr = condition();
            stmt->body.push_back(blockStatement());
            return stmt;
        }

        if (accept(TokenKind::For)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::For;
            stmt->line = line;
            stmt->name = expect(TokenKind::Identifier, "a loop variable after 'for'").text;
            expect(TokenKind::In, "'in'");
            stmt->expr = condition();
            stmt->body.push_back(blockStatement());
            return stmt;
        }

        if (accept(TokenKind::Return)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::Return;
            stmt->line = line;

            // `return` alone is legal, so a result is only parsed when
            // something that could start one is on the same line.
            if (!at(TokenKind::RBrace) && !at(TokenKind::End) && !at(TokenKind::Semicolon) &&
                peek().line == line) {
                stmt->expr = expression();
            }
            skipSeparators();
            return stmt;
        }

        if (accept(TokenKind::Break)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::Break;
            stmt->line = line;
            skipSeparators();
            return stmt;
        }

        if (accept(TokenKind::Continue)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->kind = StmtKind::Continue;
            stmt->line = line;
            skipSeparators();
            return stmt;
        }

        if (at(TokenKind::LBrace)) return blockStatement();

        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->line = line;
        stmt->expr = expression();
        skipSeparators();
        return stmt;
    }

    std::unique_ptr<Stmt> ifRest(int line) {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::If;
        stmt->line = line;
        stmt->expr = condition();
        stmt->body.push_back(blockStatement());

        if (accept(TokenKind::Else)) {
            int elseLine = peek().line;
            if (accept(TokenKind::If)) {
                stmt->other.push_back(ifRest(elseLine));
            } else {
                stmt->other.push_back(blockStatement());
            }
        }
        return stmt;
    }

    std::unique_ptr<Stmt> blockStatement() {
        int line = peek().line;
        expect(TokenKind::LBrace, "'{'");

        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Block;
        stmt->line = line;

        skipSeparators();
        while (!at(TokenKind::RBrace)) {
            if (at(TokenKind::End)) throw SyntaxError{"unclosed '{'", line};
            stmt->body.push_back(statement());
            skipSeparators();
        }
        expect(TokenKind::RBrace, "'}'");
        return stmt;
    }

    // Parses `(params) { body }` and records the declaration.
    const FunctionDecl* functionRest(const std::string& name, int line) {
        auto decl = std::make_unique<FunctionDecl>();
        decl->name = name;
        decl->line = line;

        expect(TokenKind::LParen, "'(' after a function name");
        if (!at(TokenKind::RParen)) {
            do {
                decl->params.push_back(expect(TokenKind::Identifier, "a parameter name").text);
            } while (accept(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");

        auto block = blockStatement();
        decl->body = block.get();

        const FunctionDecl* result = decl.get();
        program_.functions.push_back(std::move(decl));
        ownedBlocks_.push_back(std::move(block));
        return result;
    }

    // ---------------------------------------------------------- expressions
    // A condition sits directly before a '{' that opens a block, so a '{' may
    // not start a map literal there. Without this `while x { }` parses as an
    // index into a map and then falls over on the missing body.
    std::unique_ptr<Expr> condition() {
        bool saved = noMapLiteral_;
        noMapLiteral_ = true;
        auto expr = expression();
        noMapLiteral_ = saved;
        return expr;
    }

    std::unique_ptr<Expr> expression() { return assignment(); }

    std::unique_ptr<Expr> assignment() {
        auto left = logicalOr();

        if (at(TokenKind::Assign)) {
            int line = advance().line;
            if (left->kind != ExprKind::Ident && left->kind != ExprKind::Index &&
                left->kind != ExprKind::Member) {
                throw SyntaxError{"this cannot be assigned to", line};
            }

            auto expr = makeExpr(ExprKind::Assign, line);
            expr->a = std::move(left);
            expr->b = assignment();   // right associative: a = b = c
            return expr;
        }
        return left;
    }

    std::unique_ptr<Expr> logicalOr() {
        auto left = logicalAnd();
        while (at(TokenKind::Or)) {
            int line = advance().line;
            auto expr = makeExpr(ExprKind::Logical, line);
            expr->op = TokenKind::Or;
            expr->a = std::move(left);
            expr->b = logicalAnd();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expr> logicalAnd() {
        auto left = equality();
        while (at(TokenKind::And)) {
            int line = advance().line;
            auto expr = makeExpr(ExprKind::Logical, line);
            expr->op = TokenKind::And;
            expr->a = std::move(left);
            expr->b = equality();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expr> binaryLevel(std::unique_ptr<Expr> (Parser::*next)(),
                                      std::initializer_list<TokenKind> ops) {
        auto left = (this->*next)();
        for (;;) {
            bool matched = false;
            for (TokenKind op : ops) {
                if (!at(op)) continue;
                int line = advance().line;
                auto expr = makeExpr(ExprKind::Binary, line);
                expr->op = op;
                expr->a = std::move(left);
                expr->b = (this->*next)();
                left = std::move(expr);
                matched = true;
                break;
            }
            if (!matched) return left;
        }
    }

    std::unique_ptr<Expr> equality() {
        return binaryLevel(&Parser::comparison, {TokenKind::Equal, TokenKind::NotEqual});
    }
    std::unique_ptr<Expr> comparison() {
        return binaryLevel(&Parser::term, {TokenKind::Less, TokenKind::LessEqual,
                                           TokenKind::Greater, TokenKind::GreaterEqual});
    }
    std::unique_ptr<Expr> term() {
        return binaryLevel(&Parser::factor, {TokenKind::Plus, TokenKind::Minus});
    }
    std::unique_ptr<Expr> factor() {
        return binaryLevel(&Parser::unary, {TokenKind::Star, TokenKind::Slash, TokenKind::Percent});
    }

    std::unique_ptr<Expr> unary() {
        if (at(TokenKind::Minus) || at(TokenKind::Not)) {
            TokenKind op = peek().kind;
            int line = advance().line;
            auto expr = makeExpr(ExprKind::Unary, line);
            expr->op = op;
            expr->a = unary();
            return expr;
        }
        return postfix();
    }

    std::unique_ptr<Expr> postfix() {
        auto expr = primary();

        for (;;) {
            // A postfix chain may not cross a line break. `f(x)` on one line
            // and `(y)` on the next are two statements, not a call of a call
            // -- the one place this grammar would otherwise be ambiguous
            // without statement terminators.
            if (peek().line != lastLine_) return expr;

            if (at(TokenKind::LParen)) {
                int line = advance().line;
                auto call = makeExpr(ExprKind::Call, line);
                call->a = std::move(expr);

                bool saved = noMapLiteral_;
                noMapLiteral_ = false;
                if (!at(TokenKind::RParen)) {
                    do { call->items.push_back(expression()); } while (accept(TokenKind::Comma));
                }
                noMapLiteral_ = saved;

                lastLine_ = expect(TokenKind::RParen, "')'").line;
                expr = std::move(call);
                continue;
            }

            if (at(TokenKind::LBracket)) {
                int line = advance().line;
                auto index = makeExpr(ExprKind::Index, line);
                index->a = std::move(expr);

                bool saved = noMapLiteral_;
                noMapLiteral_ = false;
                index->b = expression();
                noMapLiteral_ = saved;

                lastLine_ = expect(TokenKind::RBracket, "']'").line;
                expr = std::move(index);
                continue;
            }

            if (at(TokenKind::Dot)) {
                int line = advance().line;
                auto member = makeExpr(ExprKind::Member, line);
                member->a = std::move(expr);
                const Token& name = expect(TokenKind::Identifier, "a field name after '.'");
                member->text = name.text;
                lastLine_ = name.line;
                expr = std::move(member);
                continue;
            }

            return expr;
        }
    }

    std::unique_ptr<Expr> primary() {
        const Token& token = peek();
        lastLine_ = token.line;

        if (accept(TokenKind::Number)) {
            auto expr = makeExpr(ExprKind::Number, token.line);
            expr->number = token.number;
            return expr;
        }
        if (accept(TokenKind::String)) {
            auto expr = makeExpr(ExprKind::String, token.line);
            expr->text = token.text;
            return expr;
        }
        if (accept(TokenKind::True) || accept(TokenKind::False)) {
            auto expr = makeExpr(ExprKind::Bool, token.line);
            expr->boolean = token.kind == TokenKind::True;
            return expr;
        }
        if (accept(TokenKind::Nil)) return makeExpr(ExprKind::Nil, token.line);

        if (accept(TokenKind::Identifier)) {
            auto expr = makeExpr(ExprKind::Ident, token.line);
            expr->text = token.text;
            return expr;
        }

        if (at(TokenKind::Fn)) {
            int line = advance().line;
            auto expr = makeExpr(ExprKind::Function, line);
            expr->fn = functionRest(std::string(), line);
            lastLine_ = lastConsumedLine();
            return expr;
        }

        if (at(TokenKind::LParen)) {
            advance();
            bool saved = noMapLiteral_;
            noMapLiteral_ = false;
            auto inner = expression();
            noMapLiteral_ = saved;
            lastLine_ = expect(TokenKind::RParen, "')'").line;
            return inner;
        }

        if (at(TokenKind::LBracket)) {
            int line = advance().line;
            auto expr = makeExpr(ExprKind::ListLit, line);

            bool saved = noMapLiteral_;
            noMapLiteral_ = false;
            skipNewlineOnlySeparators();
            if (!at(TokenKind::RBracket)) {
                do {
                    skipNewlineOnlySeparators();
                    if (at(TokenKind::RBracket)) break;   // trailing comma
                    expr->items.push_back(expression());
                    skipNewlineOnlySeparators();
                } while (accept(TokenKind::Comma));
            }
            noMapLiteral_ = saved;

            lastLine_ = expect(TokenKind::RBracket, "']'").line;
            return expr;
        }

        if (at(TokenKind::LBrace) && !noMapLiteral_) {
            int line = advance().line;
            auto expr = makeExpr(ExprKind::MapLit, line);

            bool saved = noMapLiteral_;
            noMapLiteral_ = false;
            skipNewlineOnlySeparators();
            if (!at(TokenKind::RBrace)) {
                do {
                    skipNewlineOnlySeparators();
                    if (at(TokenKind::RBrace)) break;    // trailing comma

                    // A key is a bare name or a string, never an expression:
                    // `{x: 1}` should not depend on whether `x` is in scope.
                    if (at(TokenKind::String)) {
                        expr->keys.push_back(advance().text);
                    } else {
                        expr->keys.push_back(expect(TokenKind::Identifier, "a field name").text);
                    }
                    expect(TokenKind::Colon, "':' after a field name");
                    expr->items.push_back(expression());
                    skipNewlineOnlySeparators();
                } while (accept(TokenKind::Comma));
            }
            noMapLiteral_ = saved;

            lastLine_ = expect(TokenKind::RBrace, "'}'").line;
            return expr;
        }

        // The one place the grammar is genuinely ambiguous, and the message
        // should say so rather than leaving the author to guess. After `if`,
        // `while` or `in`, a '{' opens the body -- there is no way to tell it
        // from a map literal without lookahead that would still be wrong half
        // the time. Parentheses settle it, and inside them maps are allowed
        // again.
        if (at(TokenKind::LBrace) && noMapLiteral_) {
            throw SyntaxError{"a '{' here starts the body, not a map -- write ({...}) to mean a map",
                              token.line};
        }

        throw SyntaxError{"expected an expression but found '" + token.text + "'", token.line};
    }

    // Inside brackets a semicolon is still nothing; newlines are not tokens.
    void skipNewlineOnlySeparators() { while (accept(TokenKind::Semicolon)) {} }

    std::vector<Token> tokens_;
    Program& program_;
    size_t position_ = 0;
    int lastLine_ = 1;
    bool noMapLiteral_ = false;

    // Block statements that belong to a FunctionDecl rather than to the
    // statement list. They are kept alive here for the life of the program.
    std::vector<std::unique_ptr<Stmt>> ownedBlocks_;

public:
    std::vector<std::unique_ptr<Stmt>> takeOwnedBlocks() { return std::move(ownedBlocks_); }
};

}  // namespace

ParseResult parse(const std::string& source) {
    ParseResult result;

    LexResult tokens = lex(source);
    if (!tokens.error.empty()) {
        result.error = tokens.error;
        result.errorLine = tokens.errorLine;
        return result;
    }

    Parser parser(std::move(tokens.tokens), result.program);
    try {
        parser.parseProgram();
    } catch (const SyntaxError& error) {
        result.error = error.message;
        result.errorLine = error.line;
        return result;
    }

    // Function bodies are owned by the program so that a FunctionDecl's
    // `body` pointer stays valid for as long as anything can call it -- and
    // kept out of `statements`, which is the list that actually runs.
    result.program.blocks = parser.takeOwnedBlocks();
    return result;
}

} // namespace blocky::script
