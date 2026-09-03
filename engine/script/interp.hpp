#pragma once
// The evaluator: a tree walker over the parsed program.
//
// A tree walker rather than a bytecode VM, and the reason is the one this
// project keeps arriving at: measure before choosing. A sandbox's scripts run
// on events and over a handful of objects, while the per-frame work -- the
// solver, the mesher, the tracer -- is all C++ that the script never enters.
// A compiler and a VM are perhaps twice this file again, and buy speed in the
// one place nothing spends its time.
//
// The seam is drawn so that judgement can be revisited without argument:
// nothing outside this file knows how a program is executed. `Script` loads
// and calls; the parser produces a tree; a bytecode backend would replace
// `Interpreter` alone.
//
// ------------------------------------------------------------- misbehaviour
//
// Two things a sandbox must survive, because both will happen on the first
// afternoon someone writes a tool:
//
//   - **A runtime error must not take the session down.** Errors are thrown
//     and caught at the call boundary, reported with a line, and the world
//     keeps running.
//   - **A runaway loop must not hang the game.** Every statement costs one
//     unit of a per-call budget; `while true { }` spends it and reports
//     itself instead of freezing the frame.
#include "engine/core/random.hpp"
#include "engine/script/ast.hpp"
#include "engine/script/value.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace blocky::script {

class Interpreter {
public:
    explicit Interpreter(Heap& heap);

    // ------------------------------------------------------------- host side
    void defineNative(const std::string& name, NativeFn fn);
    void defineGlobal(const std::string& name, const Value& value);
    Value* findGlobal(const std::string& name);

    // Whatever the host needs its natives to reach: the physics world, the
    // scene, itself. A plain pointer rather than a capture because NativeFn
    // is a function pointer, which keeps calls allocation-free.
    void* userData = nullptr;

    // Where `print` goes. Defaults to stdout.
    std::function<void(const std::string&)> onPrint;

    // Statements one call may execute before it is stopped. Zero disables the
    // check, which is for tests rather than for a game.
    uint64_t stepBudget = 2000000;

    // ------------------------------------------------------------- running
    // Runs the top level. Handlers declared by `on` are registered as it goes.
    bool run(const Program& program, std::string& error, int& errorLine);

    bool hasHandler(const std::string& event) const;
    bool callHandler(const std::string& event, const std::vector<Value>& args, std::string& error,
                     int& errorLine);

    // ------------------------------------------------- available to natives
    Heap& heap() { return heap_; }
    Rng&  rng() { return rng_; }
    int   currentLine() const { return line_; }

    Value makeString(const std::string& text);
    Value makeList(std::vector<Value> items);
    Value makeMap();

    // A string that will be handed out again for the same text rather than
    // allocated afresh.
    //
    // Only for text that is *constant* -- literals in the source. Strings are
    // immutable in this language, so sharing one is safe, and the win is not
    // small: `pose(p, "head", ...)` in a per-frame handler allocated a string
    // every tick, which on the demo was six hundred objects in ten seconds.
    // Computed strings still allocate, because two of them being equal today
    // says nothing about tomorrow.
    Value internString(const std::string& text);

    // Reports a runtime error at the line being evaluated. Never returns.
    [[noreturn]] void fail(const std::string& message);

    // Argument helpers, so every native does not re-invent the same messages.
    void expectArgCount(const std::vector<Value>& args, size_t count, const char* name);
    double asNumber(const Value& value, const char* name, size_t index);
    Vec3   asVec(const Value& value, const char* name, size_t index);
    const std::string& asString(const Value& value, const char* name, size_t index);

    Value callValue(const Value& callee, const std::vector<Value>& args);

private:
    enum class Flow { Normal, Break, Continue, Return };

    Flow exec(const Stmt& stmt, Env& env, Value& result);
    Flow execBlock(const std::vector<std::unique_ptr<Stmt>>& body, Env& env, Value& result);
    Value eval(const Expr& expr, Env& env);

    Value evalBinary(const Expr& expr, const Value& a, const Value& b);
    Value evalUnary(const Expr& expr, const Value& a);
    Value evalMember(const Expr& expr, const Value& target);
    Value evalIndex(const Expr& expr, const Value& target, const Value& key);
    void  assign(const Expr& target, const Value& value, Env& env);

    void step();

    Heap& heap_;
    Env* globals_ = nullptr;
    Rng rng_{0x5eed1234u};

    std::vector<std::pair<std::string, Value>> handlers_;
    std::unordered_map<std::string, Value> interned_;

    int line_ = 0;
    int depth_ = 0;
    uint64_t steps_ = 0;
};

// Installs print, maths, vectors and the container helpers. The host adds its
// own on top; nothing here knows what a block or a body is.
void installCoreLibrary(Interpreter& interpreter);

} // namespace blocky::script
