#pragma once
// The syntax tree.
//
// One struct per category rather than one per node kind, with a tag and the
// union of the fields. A hierarchy with fifteen subclasses would need a cast
// or a visitor at every step of the evaluator, and buy nothing: the tree is
// small, short-lived, and read by exactly one function.
#include "engine/script/lexer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace blocky::script {

struct Expr;
struct Stmt;

// Shared by named declarations and by anonymous `fn(...) { }` literals, so a
// closure and a top-level function are the same thing to the interpreter.
struct FunctionDecl {
    std::string name;                 // empty for a literal
    std::vector<std::string> params;
    const Stmt* body = nullptr;       // a Block
    int line = 0;
};

enum class ExprKind {
    Number, String, Bool, Nil,
    Ident,
    Unary,        // op a
    Binary,       // a op b
    Logical,      // a and/or b, short-circuiting
    Call,         // a(items...)
    Index,        // a[b]
    Member,       // a.text
    ListLit,      // [items...]
    MapLit,       // {keys[i]: items[i]}
    Function,     // fn(params) { body }
    Assign,       // a = b, where a is Ident, Index or Member
};

struct Expr {
    ExprKind kind = ExprKind::Nil;
    int line = 0;

    double number = 0.0;
    bool boolean = false;
    std::string text;                 // identifier, string literal, member name
    TokenKind op = TokenKind::End;

    std::unique_ptr<Expr> a;
    std::unique_ptr<Expr> b;
    std::vector<std::unique_ptr<Expr>> items;   // call arguments, list, map values
    std::vector<std::string> keys;              // map keys

    const FunctionDecl* fn = nullptr;
};

enum class StmtKind {
    ExprStmt,
    Let,
    Block,
    If,
    While,
    For,          // for name in expr { body }
    Return,
    Break,
    Continue,
    Function,     // fn name(params) { body }
    On,           // on name(params) { body }
};

struct Stmt {
    StmtKind kind = StmtKind::Block;
    int line = 0;

    std::string name;                            // let / for / function / on
    std::unique_ptr<Expr> expr;                  // condition, initialiser, iterable, result
    std::vector<std::unique_ptr<Stmt>> body;     // block, loop body, then-branch
    std::vector<std::unique_ptr<Stmt>> other;    // else-branch

    const FunctionDecl* fn = nullptr;            // Function and On
};

struct Program {
    // The top level, run once when the script loads.
    std::vector<std::unique_ptr<Stmt>> statements;

    // Every FunctionDecl lives here, so the tree can point at them freely and
    // they all die together with the program on reload.
    std::vector<std::unique_ptr<FunctionDecl>> functions;

    // Function and handler bodies. They are kept apart from `statements` for
    // a reason worth stating: anything in that list gets *executed* when the
    // script loads, so parking a function body there would run its contents
    // at load time, once, outside any call.
    std::vector<std::unique_ptr<Stmt>> blocks;
};

} // namespace blocky::script
