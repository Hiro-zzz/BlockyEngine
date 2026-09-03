#pragma once
// Values for the engine's own scripting language.
//
// ---------------------------------------------------------------- why a language
//
// Scenes are compiled: each `scenes/<name>.cpp` becomes its own executable,
// and that was the right call for a still frame you render once. A sandbox
// inverts it. There the iteration loop *is* the product -- a tool that needs
// a rebuild and a restart, losing the world it was being tested in, is not a
// tool anyone will iterate on. So the gameplay layer needs to be reloadable
// while the world stays up.
//
// This is deliberately not Lua. Two reasons, and only the second is about
// dependencies:
//
//   - **Vectors are values here.** Every script this engine will ever run does
//     vector arithmetic, and in an embedded language that means an allocation
//     and a metatable dispatch per operation. `Value` carries a Vec3 inline,
//     so `a + b * 2` allocates nothing and branches once. A language shaped
//     around its domain is worth more than a general one, and this is the
//     domain.
//   - Nothing else in this engine is borrowed, and a script VM is smaller
//     than the H.264 encoder that is already here.
//
// ------------------------------------------------------------------ memory
//
// Objects live in a `Heap` that owns them outright and frees them all at
// once. There is no garbage collector: a tree-walking evaluator keeps live
// values in C++ locals, so a collector would need to see the machine stack,
// and that is a real piece of work for a first pass.
//
// The mitigation is in the value layout rather than in a promise. Numbers,
// booleans and **vectors** are stored inline, so the arithmetic a per-frame
// handler actually does allocates nothing at all; only strings, lists, maps
// and closures reach the heap. A handler that builds no containers can run
// for hours. One that builds a list every frame will grow until the script is
// reloaded, which frees the arena -- and reloading is the thing this whole
// layer exists to make cheap.
#include "engine/core/math.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace blocky::script {

struct Obj;
struct Env;
struct FunctionDecl;
class Interpreter;

enum class Type {
    Nil,
    Bool,
    Number,
    Vec,
    Str,
    List,
    Map,
    Function,   // declared in script
    Native,     // implemented in C++
    Handle,     // an opaque id owned by the host: a body, a joint, a prop
};

struct Value;

// A function the host provides. Arguments arrive already evaluated; throwing
// `ScriptError` from one reports a runtime error at the call site.
using NativeFn = Value (*)(Interpreter&, const std::vector<Value>&);

struct Value {
    Type type = Type::Nil;

    bool     boolean = false;
    double   number = 0.0;
    Vec3     vec{};
    int64_t  handle = 0;    // Handle: whatever the host wants it to mean
    uint32_t handleKind = 0;

    Obj* object = nullptr;  // Str, List, Map, Function; owned by the Heap
    NativeFn native = nullptr;

    static Value nil() { return Value{}; }
    static Value boolean_(bool v) { Value o; o.type = Type::Bool; o.boolean = v; return o; }
    static Value num(double v) { Value o; o.type = Type::Number; o.number = v; return o; }
    static Value vector(Vec3 v) { Value o; o.type = Type::Vec; o.vec = v; return o; }
    static Value handleOf(uint32_t kind, int64_t id) {
        Value o; o.type = Type::Handle; o.handleKind = kind; o.handle = id; return o;
    }

    bool isNil() const { return type == Type::Nil; }

    // Only `nil` and `false` are false. A zero number is true, as in Lua --
    // the alternative makes `if count` mean something different from
    // `if count != 0` exactly when a count is zero, which is the case anyone
    // writing that line cares about.
    bool truthy() const { return !(type == Type::Nil || (type == Type::Bool && !boolean)); }
};

// One struct for all four heap kinds rather than a class hierarchy. Script
// objects are few and short-lived; a vtable and four headers would buy
// nothing but ceremony.
struct Obj {
    Type kind = Type::Str;

    std::string text;            // Str
    std::vector<Value> items;    // List

    // Map, kept in insertion order rather than hashed. Iteration order is
    // then a property of the program instead of a property of the build, and
    // a script that walks a map and spawns things stays reproducible -- the
    // same reason the take loop refuses to read a clock.
    std::vector<std::pair<std::string, Value>> fields;

    const FunctionDecl* decl = nullptr;   // Function
    Env* closure = nullptr;               // Function: where it was declared
};

// Lexical scope. Parented, so a closure keeps the chain it was written in.
struct Env {
    Env* parent = nullptr;
    std::vector<std::pair<std::string, Value>> vars;

    Value* find(const std::string& name) {
        for (Env* env = this; env; env = env->parent)
            for (auto& entry : env->vars)
                if (entry.first == name) return &entry.second;
        return nullptr;
    }

    // Declares in *this* scope, shadowing anything outer. A redeclaration in
    // the same scope overwrites rather than erroring: reloading a script
    // re-runs its top level, and a global that refused to be re-declared
    // would make reload a special case.
    void declare(const std::string& name, const Value& value) {
        for (auto& entry : vars)
            if (entry.first == name) { entry.second = value; return; }
        vars.emplace_back(name, value);
    }
};

// Owns every object and scope a script allocates, and frees them together.
class Heap {
public:
    Obj* newObj(Type kind) {
        objects_.push_back(std::make_unique<Obj>());
        objects_.back()->kind = kind;
        return objects_.back().get();
    }

    Env* newEnv(Env* parent) {
        envs_.push_back(std::make_unique<Env>());
        envs_.back()->parent = parent;
        return envs_.back().get();
    }

    void reset() {
        objects_.clear();
        envs_.clear();
    }

    size_t objectCount() const { return objects_.size(); }
    size_t scopeCount() const { return envs_.size(); }

private:
    std::vector<std::unique_ptr<Obj>> objects_;
    std::vector<std::unique_ptr<Env>> envs_;
};

// A runtime failure. Carries the line so the host can name it; a script error
// must never take the session down with it, so the interpreter throws and the
// call boundary catches.
struct ScriptError {
    std::string message;
    int line = 0;
};

const char* typeName(Type type);
std::string toString(const Value& value);
bool valuesEqual(const Value& a, const Value& b);

} // namespace blocky::script
