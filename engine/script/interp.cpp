#include "engine/script/interp.hpp"

#include <cmath>
#include <cstdio>

namespace blocky::script {
namespace {

constexpr int kMaxDepth = 200;

}  // namespace

Interpreter::Interpreter(Heap& heap) : heap_(heap) {
    globals_ = heap_.newEnv(nullptr);
    onPrint = [](const std::string& text) { std::printf("%s\n", text.c_str()); };
}

// ------------------------------------------------------------- host surface
void Interpreter::defineNative(const std::string& name, NativeFn fn) {
    Value value;
    value.type = Type::Native;
    value.native = fn;
    globals_->declare(name, value);
}

void Interpreter::defineGlobal(const std::string& name, const Value& value) {
    globals_->declare(name, value);
}

Value* Interpreter::findGlobal(const std::string& name) { return globals_->find(name); }

Value Interpreter::makeString(const std::string& text) {
    Value value;
    value.type = Type::Str;
    value.object = heap_.newObj(Type::Str);
    value.object->text = text;
    return value;
}

Value Interpreter::internString(const std::string& text) {
    auto found = interned_.find(text);
    if (found != interned_.end()) return found->second;

    Value value = makeString(text);
    interned_.emplace(text, value);
    return value;
}

Value Interpreter::makeList(std::vector<Value> items) {
    Value value;
    value.type = Type::List;
    value.object = heap_.newObj(Type::List);
    value.object->items = std::move(items);
    return value;
}

Value Interpreter::makeMap() {
    Value value;
    value.type = Type::Map;
    value.object = heap_.newObj(Type::Map);
    return value;
}

void Interpreter::fail(const std::string& message) { throw ScriptError{message, line_}; }

void Interpreter::expectArgCount(const std::vector<Value>& args, size_t count, const char* name) {
    if (args.size() != count) {
        fail(std::string(name) + " takes " + std::to_string(count) + " argument(s), got " +
             std::to_string(args.size()));
    }
}

double Interpreter::asNumber(const Value& value, const char* name, size_t index) {
    if (value.type != Type::Number) {
        fail(std::string(name) + ": argument " + std::to_string(index + 1) +
             " must be a number, got " + typeName(value.type));
    }
    return value.number;
}

Vec3 Interpreter::asVec(const Value& value, const char* name, size_t index) {
    if (value.type != Type::Vec) {
        fail(std::string(name) + ": argument " + std::to_string(index + 1) +
             " must be a vec, got " + typeName(value.type));
    }
    return value.vec;
}

const std::string& Interpreter::asString(const Value& value, const char* name, size_t index) {
    if (value.type != Type::Str || !value.object) {
        fail(std::string(name) + ": argument " + std::to_string(index + 1) +
             " must be a string, got " + typeName(value.type));
    }
    return value.object->text;
}

void Interpreter::step() {
    if (stepBudget == 0) return;
    if (++steps_ > stepBudget) {
        fail("this call ran for " + std::to_string(stepBudget) +
             " statements without finishing -- an endless loop?");
    }
}

// ------------------------------------------------------------------ running
bool Interpreter::run(const Program& program, std::string& error, int& errorLine) {
    steps_ = 0;
    depth_ = 0;

    try {
        Value discard;
        for (const auto& stmt : program.statements) {
            if (exec(*stmt, *globals_, discard) != Flow::Normal) break;
        }
    } catch (const ScriptError& failure) {
        error = failure.message;
        errorLine = failure.line;
        return false;
    }
    return true;
}

bool Interpreter::hasHandler(const std::string& event) const {
    for (const auto& entry : handlers_)
        if (entry.first == event) return true;
    return false;
}

bool Interpreter::callHandler(const std::string& event, const std::vector<Value>& args,
                              std::string& error, int& errorLine) {
    const Value* handler = nullptr;
    for (const auto& entry : handlers_)
        if (entry.first == event) handler = &entry.second;
    if (!handler) return true;   // nothing registered is not a failure

    steps_ = 0;
    depth_ = 0;

    try {
        callValue(*handler, args);
    } catch (const ScriptError& failure) {
        error = failure.message;
        errorLine = failure.line;
        return false;
    }
    return true;
}

Value Interpreter::callValue(const Value& callee, const std::vector<Value>& args) {
    if (callee.type == Type::Native) {
        if (!callee.native) fail("call of an empty function");
        return callee.native(*this, args);
    }

    if (callee.type != Type::Function || !callee.object || !callee.object->decl) {
        fail(std::string("cannot call a ") + typeName(callee.type));
    }

    const FunctionDecl& decl = *callee.object->decl;
    if (args.size() != decl.params.size()) {
        std::string who = decl.name.empty() ? std::string("this function") : "'" + decl.name + "'";
        fail(who + " takes " + std::to_string(decl.params.size()) + " argument(s), got " +
             std::to_string(args.size()));
    }

    if (++depth_ > kMaxDepth) {
        --depth_;
        fail("too many nested calls -- runaway recursion?");
    }

    // Parented on where the function was *written*, not on where it was
    // called: that is what makes a closure keep the variables it captured.
    Env* frame = heap_.newEnv(callee.object->closure ? callee.object->closure : globals_);
    for (size_t i = 0; i < decl.params.size(); ++i) frame->declare(decl.params[i], args[i]);

    Value result;
    if (decl.body) execBlock(decl.body->body, *frame, result);
    --depth_;
    return result;
}

// --------------------------------------------------------------- statements
Interpreter::Flow Interpreter::execBlock(const std::vector<std::unique_ptr<Stmt>>& body, Env& env,
                                         Value& result) {
    for (const auto& stmt : body) {
        Flow flow = exec(*stmt, env, result);
        if (flow != Flow::Normal) return flow;
    }
    return Flow::Normal;
}

Interpreter::Flow Interpreter::exec(const Stmt& stmt, Env& env, Value& result) {
    line_ = stmt.line;
    step();

    switch (stmt.kind) {
        case StmtKind::ExprStmt:
            if (stmt.expr) eval(*stmt.expr, env);
            return Flow::Normal;

        case StmtKind::Let: {
            Value value;
            if (stmt.expr) value = eval(*stmt.expr, env);
            env.declare(stmt.name, value);
            return Flow::Normal;
        }

        case StmtKind::Block: {
            Env* inner = heap_.newEnv(&env);
            return execBlock(stmt.body, *inner, result);
        }

        case StmtKind::If: {
            if (eval(*stmt.expr, env).truthy()) {
                Env* inner = heap_.newEnv(&env);
                return execBlock(stmt.body, *inner, result);
            }
            if (!stmt.other.empty()) {
                Env* inner = heap_.newEnv(&env);
                return execBlock(stmt.other, *inner, result);
            }
            return Flow::Normal;
        }

        case StmtKind::While: {
            while (eval(*stmt.expr, env).truthy()) {
                step();
                Env* inner = heap_.newEnv(&env);
                Flow flow = execBlock(stmt.body, *inner, result);
                if (flow == Flow::Break) break;
                if (flow == Flow::Return) return flow;
            }
            return Flow::Normal;
        }

        case StmtKind::For: {
            Value iterable = eval(*stmt.expr, env);
            std::vector<Value> sequence;

            if (iterable.type == Type::List && iterable.object) {
                sequence = iterable.object->items;   // copied, so the loop survives a push
            } else if (iterable.type == Type::Map && iterable.object) {
                for (const auto& field : iterable.object->fields)
                    sequence.push_back(internString(field.first));
            } else {
                line_ = stmt.line;
                fail(std::string("for expects a list or a map, got ") + typeName(iterable.type));
            }

            for (const Value& item : sequence) {
                step();
                Env* inner = heap_.newEnv(&env);
                inner->declare(stmt.name, item);
                Flow flow = execBlock(stmt.body, *inner, result);
                if (flow == Flow::Break) break;
                if (flow == Flow::Return) return flow;
            }
            return Flow::Normal;
        }

        case StmtKind::Return:
            result = stmt.expr ? eval(*stmt.expr, env) : Value::nil();
            return Flow::Return;

        case StmtKind::Break: return Flow::Break;
        case StmtKind::Continue: return Flow::Continue;

        case StmtKind::Function: {
            Value fn;
            fn.type = Type::Function;
            fn.object = heap_.newObj(Type::Function);
            fn.object->decl = stmt.fn;
            fn.object->closure = &env;
            env.declare(stmt.name, fn);
            return Flow::Normal;
        }

        case StmtKind::On: {
            Value fn;
            fn.type = Type::Function;
            fn.object = heap_.newObj(Type::Function);
            fn.object->decl = stmt.fn;
            fn.object->closure = &env;

            for (auto& entry : handlers_) {
                if (entry.first == stmt.name) { entry.second = fn; return Flow::Normal; }
            }
            handlers_.emplace_back(stmt.name, fn);
            return Flow::Normal;
        }
    }
    return Flow::Normal;
}

// -------------------------------------------------------------- expressions
Value Interpreter::eval(const Expr& expr, Env& env) {
    line_ = expr.line;

    switch (expr.kind) {
        case ExprKind::Number: return Value::num(expr.number);
        case ExprKind::String: return internString(expr.text);
        case ExprKind::Bool: return Value::boolean_(expr.boolean);
        case ExprKind::Nil: return Value::nil();

        case ExprKind::Ident: {
            Value* found = env.find(expr.text);
            if (!found) fail("'" + expr.text + "' is not defined");
            return *found;
        }

        case ExprKind::Unary: return evalUnary(expr, eval(*expr.a, env));

        case ExprKind::Binary: {
            Value a = eval(*expr.a, env);
            Value b = eval(*expr.b, env);
            line_ = expr.line;
            return evalBinary(expr, a, b);
        }

        case ExprKind::Logical: {
            Value a = eval(*expr.a, env);
            // Short circuit, and the result is the *value* rather than a
            // boolean, so `name or "unnamed"` works.
            if (expr.op == TokenKind::And) return a.truthy() ? eval(*expr.b, env) : a;
            return a.truthy() ? a : eval(*expr.b, env);
        }

        case ExprKind::Assign: {
            Value value = eval(*expr.b, env);
            assign(*expr.a, value, env);
            return value;
        }

        case ExprKind::Call: {
            Value callee = eval(*expr.a, env);
            std::vector<Value> args;
            args.reserve(expr.items.size());
            for (const auto& argument : expr.items) args.push_back(eval(*argument, env));
            line_ = expr.line;
            return callValue(callee, args);
        }

        case ExprKind::Index: {
            Value target = eval(*expr.a, env);
            Value key = eval(*expr.b, env);
            line_ = expr.line;
            return evalIndex(expr, target, key);
        }

        case ExprKind::Member: {
            Value target = eval(*expr.a, env);
            line_ = expr.line;
            return evalMember(expr, target);
        }

        case ExprKind::ListLit: {
            std::vector<Value> items;
            items.reserve(expr.items.size());
            for (const auto& item : expr.items) items.push_back(eval(*item, env));
            return makeList(std::move(items));
        }

        case ExprKind::MapLit: {
            Value map = makeMap();
            for (size_t i = 0; i < expr.items.size(); ++i)
                map.object->fields.emplace_back(expr.keys[i], eval(*expr.items[i], env));
            return map;
        }

        case ExprKind::Function: {
            Value fn;
            fn.type = Type::Function;
            fn.object = heap_.newObj(Type::Function);
            fn.object->decl = expr.fn;
            fn.object->closure = &env;
            return fn;
        }
    }
    return Value::nil();
}

Value Interpreter::evalUnary(const Expr& expr, const Value& a) {
    line_ = expr.line;

    if (expr.op == TokenKind::Not) return Value::boolean_(!a.truthy());

    if (expr.op == TokenKind::Minus) {
        if (a.type == Type::Number) return Value::num(-a.number);
        if (a.type == Type::Vec) return Value::vector(-a.vec);
        fail(std::string("cannot negate a ") + typeName(a.type));
    }
    fail("unknown unary operator");
}

Value Interpreter::evalBinary(const Expr& expr, const Value& a, const Value& b) {
    auto bothNumbers = [&] { return a.type == Type::Number && b.type == Type::Number; };

    switch (expr.op) {
        case TokenKind::Equal: return Value::boolean_(valuesEqual(a, b));
        case TokenKind::NotEqual: return Value::boolean_(!valuesEqual(a, b));

        case TokenKind::Plus:
            if (bothNumbers()) return Value::num(a.number + b.number);
            if (a.type == Type::Vec && b.type == Type::Vec) return Value::vector(a.vec + b.vec);
            // A string on either side concatenates, so building a message
            // does not need a conversion at every seam.
            if (a.type == Type::Str || b.type == Type::Str)
                return makeString(toString(a) + toString(b));
            break;

        case TokenKind::Minus:
            if (bothNumbers()) return Value::num(a.number - b.number);
            if (a.type == Type::Vec && b.type == Type::Vec) return Value::vector(a.vec - b.vec);
            break;

        case TokenKind::Star:
            if (bothNumbers()) return Value::num(a.number * b.number);
            if (a.type == Type::Vec && b.type == Type::Number)
                return Value::vector(a.vec * float(b.number));
            if (a.type == Type::Number && b.type == Type::Vec)
                return Value::vector(b.vec * float(a.number));
            // Component-wise, which is what a scale factor per axis wants.
            if (a.type == Type::Vec && b.type == Type::Vec) return Value::vector(a.vec * b.vec);
            break;

        case TokenKind::Slash:
            if (bothNumbers()) {
                if (b.number == 0.0) fail("division by zero");
                return Value::num(a.number / b.number);
            }
            if (a.type == Type::Vec && b.type == Type::Number) {
                if (b.number == 0.0) fail("division by zero");
                return Value::vector(a.vec / float(b.number));
            }
            break;

        case TokenKind::Percent:
            if (bothNumbers()) {
                if (b.number == 0.0) fail("division by zero");
                return Value::num(std::fmod(a.number, b.number));
            }
            break;

        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual: {
            int order = 0;
            if (bothNumbers()) {
                order = a.number < b.number ? -1 : (a.number > b.number ? 1 : 0);
            } else if (a.type == Type::Str && b.type == Type::Str && a.object && b.object) {
                order = a.object->text < b.object->text ? -1 : (a.object->text > b.object->text ? 1 : 0);
            } else {
                break;
            }
            switch (expr.op) {
                case TokenKind::Less: return Value::boolean_(order < 0);
                case TokenKind::LessEqual: return Value::boolean_(order <= 0);
                case TokenKind::Greater: return Value::boolean_(order > 0);
                default: return Value::boolean_(order >= 0);
            }
        }

        default: break;
    }

    fail(std::string("cannot apply '") + tokenName(expr.op) + "' to " + typeName(a.type) + " and " +
         typeName(b.type));
}

Value Interpreter::evalMember(const Expr& expr, const Value& target) {
    if (target.type == Type::Vec) {
        if (expr.text == "x") return Value::num(target.vec.x);
        if (expr.text == "y") return Value::num(target.vec.y);
        if (expr.text == "z") return Value::num(target.vec.z);
        fail("a vec has x, y and z, not '" + expr.text + "'");
    }

    if (target.type == Type::Map && target.object) {
        for (const auto& field : target.object->fields)
            if (field.first == expr.text) return field.second;
        return Value::nil();   // a missing field is nil, not an error
    }

    fail(std::string("cannot read '") + expr.text + "' from a " + typeName(target.type));
}

Value Interpreter::evalIndex(const Expr&, const Value& target, const Value& key) {
    if (target.type == Type::List && target.object) {
        if (key.type != Type::Number) fail(std::string("a list index must be a number, got ") + typeName(key.type));

        double raw = key.number;
        if (raw != std::floor(raw)) fail("a list index must be a whole number");

        long long index = static_cast<long long>(raw);
        if (index < 0 || size_t(index) >= target.object->items.size()) {
            fail("index " + std::to_string(index) + " is outside a list of " +
                 std::to_string(target.object->items.size()));
        }
        return target.object->items[size_t(index)];
    }

    if (target.type == Type::Map && target.object) {
        const std::string& name = asString(key, "map index", 0);
        for (const auto& field : target.object->fields)
            if (field.first == name) return field.second;
        return Value::nil();
    }

    if (target.type == Type::Str && target.object) {
        if (key.type != Type::Number) fail("a string index must be a number");
        long long index = static_cast<long long>(key.number);
        if (index < 0 || size_t(index) >= target.object->text.size())
            fail("index " + std::to_string(index) + " is outside a string of " +
                 std::to_string(target.object->text.size()));
        return makeString(std::string(1, target.object->text[size_t(index)]));
    }

    fail(std::string("cannot index a ") + typeName(target.type));
}

void Interpreter::assign(const Expr& target, const Value& value, Env& env) {
    line_ = target.line;

    if (target.kind == ExprKind::Ident) {
        Value* slot = env.find(target.text);
        // Assigning to something undeclared is an error rather than a
        // declaration. In a language without types, a mistyped name that
        // silently creates a new variable is the single most expensive bug
        // available, and `let` costs three characters.
        if (!slot) fail("'" + target.text + "' is not defined -- use 'let' to declare it");
        *slot = value;
        return;
    }

    if (target.kind == ExprKind::Member) {
        Value object = eval(*target.a, env);
        line_ = target.line;

        if (object.type == Type::Map && object.object) {
            for (auto& field : object.object->fields)
                if (field.first == target.text) { field.second = value; return; }
            object.object->fields.emplace_back(target.text, value);
            return;
        }
        // A vec is a value, not an object: `p.x = 1` would have to write back
        // through whatever produced `p`, and for `f().x = 1` there is nothing
        // to write back to. Rebuild it instead -- `p = vec(1, p.y, p.z)`.
        if (object.type == Type::Vec) fail("a vec is a value and cannot be changed in place");
        fail(std::string("cannot set '") + target.text + "' on a " + typeName(object.type));
    }

    if (target.kind == ExprKind::Index) {
        Value object = eval(*target.a, env);
        Value key = eval(*target.b, env);
        line_ = target.line;

        if (object.type == Type::List && object.object) {
            if (key.type != Type::Number) fail("a list index must be a number");
            long long index = static_cast<long long>(key.number);
            if (index < 0 || size_t(index) >= object.object->items.size())
                fail("index " + std::to_string(index) + " is outside a list of " +
                     std::to_string(object.object->items.size()));
            object.object->items[size_t(index)] = value;
            return;
        }

        if (object.type == Type::Map && object.object) {
            const std::string& name = asString(key, "map index", 0);
            for (auto& field : object.object->fields)
                if (field.first == name) { field.second = value; return; }
            object.object->fields.emplace_back(name, value);
            return;
        }

        fail(std::string("cannot assign into a ") + typeName(object.type));
    }

    fail("this cannot be assigned to");
}

} // namespace blocky::script
