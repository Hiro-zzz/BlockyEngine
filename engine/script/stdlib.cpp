// The core library: everything a script can do before the host binds
// anything of its own.
//
// Nothing here knows what a block, a body or a joint is. That separation is
// the same one the engine draws everywhere else -- `core` knows nothing about
// voxels -- and it means the language can be tested without a world.
//
// `rand` draws from the interpreter's own generator, seeded to a constant. A
// script that spawns a hundred crates at random therefore spawns the *same*
// hundred every run, which is the property the take loop guards so carefully
// upstairs: reproducible by construction rather than by luck.
#include "engine/script/interp.hpp"

#include <algorithm>
#include <cmath>

namespace blocky::script {
namespace {

Value nativePrint(Interpreter& vm, const std::vector<Value>& args) {
    std::string line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) line += " ";
        line += toString(args[i]);
    }
    if (vm.onPrint) vm.onPrint(line);
    return Value::nil();
}

Value nativeType(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "type");
    return vm.makeString(typeName(args[0].type));
}

Value nativeStr(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "str");
    return vm.makeString(toString(args[0]));
}

Value nativeNum(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "num");
    if (args[0].type == Type::Number) return args[0];

    const std::string& text = vm.asString(args[0], "num", 0);
    try {
        size_t used = 0;
        double value = std::stod(text, &used);
        while (used < text.size() && std::isspace(static_cast<unsigned char>(text[used]))) ++used;
        if (used != text.size()) return Value::nil();
        return Value::num(value);
    } catch (...) {
        // Unparseable gives nil rather than an error: asking whether a string
        // is a number is a normal thing to want, and `if num(s) == nil` says
        // it without a second function.
        return Value::nil();
    }
}

Value nativeAssert(Interpreter& vm, const std::vector<Value>& args) {
    if (args.empty() || args.size() > 2) vm.expectArgCount(args, 1, "assert");
    if (args[0].truthy()) return args[0];

    std::string message = args.size() == 2 ? toString(args[1]) : "assertion failed";
    vm.fail(message);
}

// ------------------------------------------------------------- containers
Value nativeLen(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "len");
    const Value& value = args[0];

    if (value.type == Type::List && value.object) return Value::num(double(value.object->items.size()));
    if (value.type == Type::Map && value.object) return Value::num(double(value.object->fields.size()));
    if (value.type == Type::Str && value.object) return Value::num(double(value.object->text.size()));
    vm.fail(std::string("len wants a list, map or string, got ") + typeName(value.type));
}

Value nativePush(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "push");
    if (args[0].type != Type::List || !args[0].object)
        vm.fail(std::string("push wants a list, got ") + typeName(args[0].type));

    args[0].object->items.push_back(args[1]);
    return args[0];
}

Value nativePop(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "pop");
    if (args[0].type != Type::List || !args[0].object)
        vm.fail(std::string("pop wants a list, got ") + typeName(args[0].type));
    if (args[0].object->items.empty()) vm.fail("pop from an empty list");

    Value last = args[0].object->items.back();
    args[0].object->items.pop_back();
    return last;
}

Value nativeKeys(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "keys");
    if (args[0].type != Type::Map || !args[0].object)
        vm.fail(std::string("keys wants a map, got ") + typeName(args[0].type));

    std::vector<Value> out;
    out.reserve(args[0].object->fields.size());
    for (const auto& field : args[0].object->fields) out.push_back(vm.makeString(field.first));
    return vm.makeList(std::move(out));
}

Value nativeHas(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "has");
    if (args[0].type != Type::Map || !args[0].object)
        vm.fail(std::string("has wants a map, got ") + typeName(args[0].type));

    const std::string& name = vm.asString(args[1], "has", 1);
    for (const auto& field : args[0].object->fields)
        if (field.first == name) return Value::boolean_(true);
    return Value::boolean_(false);
}

Value nativeRemove(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "remove");
    if (args[0].type != Type::Map || !args[0].object)
        vm.fail(std::string("remove wants a map, got ") + typeName(args[0].type));

    const std::string& name = vm.asString(args[1], "remove", 1);
    auto& fields = args[0].object->fields;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].first != name) continue;
        Value value = fields[i].second;
        fields.erase(fields.begin() + long(i));
        return value;
    }
    return Value::nil();
}

Value nativeRange(Interpreter& vm, const std::vector<Value>& args) {
    if (args.empty() || args.size() > 3) vm.fail("range takes 1 to 3 arguments");

    double from = 0.0, to = 0.0, stride = 1.0;
    if (args.size() == 1) {
        to = vm.asNumber(args[0], "range", 0);
    } else {
        from = vm.asNumber(args[0], "range", 0);
        to = vm.asNumber(args[1], "range", 1);
        if (args.size() == 3) stride = vm.asNumber(args[2], "range", 2);
    }
    if (stride == 0.0) vm.fail("range needs a non-zero step");

    std::vector<Value> out;
    if (stride > 0.0) {
        for (double v = from; v < to; v += stride) out.push_back(Value::num(v));
    } else {
        for (double v = from; v > to; v += stride) out.push_back(Value::num(v));
    }
    return vm.makeList(std::move(out));
}

// ---------------------------------------------------------------- vectors
Value nativeVec(Interpreter& vm, const std::vector<Value>& args) {
    if (args.size() == 1) {
        float s = float(vm.asNumber(args[0], "vec", 0));
        return Value::vector(Vec3{s, s, s});
    }
    vm.expectArgCount(args, 3, "vec");
    return Value::vector(Vec3{float(vm.asNumber(args[0], "vec", 0)),
                              float(vm.asNumber(args[1], "vec", 1)),
                              float(vm.asNumber(args[2], "vec", 2))});
}

Value nativeLength(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "length");
    return Value::num(double(length(vm.asVec(args[0], "length", 0))));
}

Value nativeNormalize(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "normalize");
    return Value::vector(normalize(vm.asVec(args[0], "normalize", 0)));
}

Value nativeDot(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "dot");
    return Value::num(double(dot(vm.asVec(args[0], "dot", 0), vm.asVec(args[1], "dot", 1))));
}

Value nativeCross(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "cross");
    return Value::vector(cross(vm.asVec(args[0], "cross", 0), vm.asVec(args[1], "cross", 1)));
}

Value nativeDistance(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "distance");
    Vec3 a = vm.asVec(args[0], "distance", 0);
    Vec3 b = vm.asVec(args[1], "distance", 1);
    return Value::num(double(length(a - b)));
}

// ------------------------------------------------------------------- maths
Value nativeAbs(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "abs");
    return Value::num(std::fabs(vm.asNumber(args[0], "abs", 0)));
}
Value nativeFloor(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "floor");
    return Value::num(std::floor(vm.asNumber(args[0], "floor", 0)));
}
Value nativeCeil(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "ceil");
    return Value::num(std::ceil(vm.asNumber(args[0], "ceil", 0)));
}
Value nativeRound(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "round");
    return Value::num(std::floor(vm.asNumber(args[0], "round", 0) + 0.5));
}
Value nativeSqrt(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "sqrt");
    double value = vm.asNumber(args[0], "sqrt", 0);
    if (value < 0.0) vm.fail("sqrt of a negative number");
    return Value::num(std::sqrt(value));
}
Value nativeSin(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "sin");
    return Value::num(std::sin(vm.asNumber(args[0], "sin", 0)));
}
Value nativeCos(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "cos");
    return Value::num(std::cos(vm.asNumber(args[0], "cos", 0)));
}
Value nativeAtan2(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "atan2");
    return Value::num(std::atan2(vm.asNumber(args[0], "atan2", 0), vm.asNumber(args[1], "atan2", 1)));
}
Value nativePow(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "pow");
    return Value::num(std::pow(vm.asNumber(args[0], "pow", 0), vm.asNumber(args[1], "pow", 1)));
}
Value nativeMin(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "min");
    return Value::num(std::min(vm.asNumber(args[0], "min", 0), vm.asNumber(args[1], "min", 1)));
}
Value nativeMax(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "max");
    return Value::num(std::max(vm.asNumber(args[0], "max", 0), vm.asNumber(args[1], "max", 1)));
}
Value nativeClamp(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 3, "clamp");
    double value = vm.asNumber(args[0], "clamp", 0);
    double low = vm.asNumber(args[1], "clamp", 1);
    double high = vm.asNumber(args[2], "clamp", 2);
    if (low > high) vm.fail("clamp was given a low bound above its high bound");
    return Value::num(std::min(std::max(value, low), high));
}
Value nativeRadians(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "radians");
    return Value::num(vm.asNumber(args[0], "radians", 0) * 3.14159265358979323846 / 180.0);
}
Value nativeDegrees(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "degrees");
    return Value::num(vm.asNumber(args[0], "degrees", 0) * 180.0 / 3.14159265358979323846);
}

Value nativeRand(Interpreter& vm, const std::vector<Value>& args) {
    if (args.empty()) return Value::num(double(vm.rng().nextFloat()));
    vm.expectArgCount(args, 2, "rand");
    double low = vm.asNumber(args[0], "rand", 0);
    double high = vm.asNumber(args[1], "rand", 1);
    return Value::num(low + double(vm.rng().nextFloat()) * (high - low));
}

}  // namespace

void installCoreLibrary(Interpreter& vm) {
    vm.defineNative("print", nativePrint);
    vm.defineNative("type", nativeType);
    vm.defineNative("str", nativeStr);
    vm.defineNative("num", nativeNum);
    vm.defineNative("assert", nativeAssert);

    vm.defineNative("len", nativeLen);
    vm.defineNative("push", nativePush);
    vm.defineNative("pop", nativePop);
    vm.defineNative("keys", nativeKeys);
    vm.defineNative("has", nativeHas);
    vm.defineNative("remove", nativeRemove);
    vm.defineNative("range", nativeRange);

    vm.defineNative("vec", nativeVec);
    vm.defineNative("length", nativeLength);
    vm.defineNative("normalize", nativeNormalize);
    vm.defineNative("dot", nativeDot);
    vm.defineNative("cross", nativeCross);
    vm.defineNative("distance", nativeDistance);

    vm.defineNative("abs", nativeAbs);
    vm.defineNative("floor", nativeFloor);
    vm.defineNative("ceil", nativeCeil);
    vm.defineNative("round", nativeRound);
    vm.defineNative("sqrt", nativeSqrt);
    vm.defineNative("sin", nativeSin);
    vm.defineNative("cos", nativeCos);
    vm.defineNative("atan2", nativeAtan2);
    vm.defineNative("pow", nativePow);
    vm.defineNative("min", nativeMin);
    vm.defineNative("max", nativeMax);
    vm.defineNative("clamp", nativeClamp);
    vm.defineNative("radians", nativeRadians);
    vm.defineNative("degrees", nativeDegrees);
    vm.defineNative("rand", nativeRand);

    vm.defineGlobal("pi", Value::num(3.14159265358979323846));
}

} // namespace blocky::script
