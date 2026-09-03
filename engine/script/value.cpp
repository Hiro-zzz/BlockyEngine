#include "engine/script/value.hpp"

#include <cmath>
#include <cstdio>

namespace blocky::script {

const char* typeName(Type type) {
    switch (type) {
        case Type::Nil: return "nil";
        case Type::Bool: return "bool";
        case Type::Number: return "number";
        case Type::Vec: return "vec";
        case Type::Str: return "string";
        case Type::List: return "list";
        case Type::Map: return "map";
        case Type::Function: return "function";
        case Type::Native: return "function";
        case Type::Handle: return "handle";
    }
    return "?";
}

namespace {

// Whole numbers print without a decimal point. Scripts count things far more
// often than they measure them, and "spawned 5 crates" reads better than
// "spawned 5.000000 crates".
std::string formatNumber(double value) {
    char buffer[64];
    if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e15) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    }
    return buffer;
}

}  // namespace

std::string toString(const Value& value) {
    switch (value.type) {
        case Type::Nil: return "nil";
        case Type::Bool: return value.boolean ? "true" : "false";
        case Type::Number: return formatNumber(value.number);
        case Type::Vec: {
            return "(" + formatNumber(value.vec.x) + ", " + formatNumber(value.vec.y) + ", " +
                   formatNumber(value.vec.z) + ")";
        }
        case Type::Str: return value.object ? value.object->text : std::string();
        case Type::List: {
            if (!value.object) return "[]";
            std::string out = "[";
            for (size_t i = 0; i < value.object->items.size(); ++i) {
                if (i) out += ", ";
                out += toString(value.object->items[i]);
            }
            return out + "]";
        }
        case Type::Map: {
            if (!value.object) return "{}";
            std::string out = "{";
            for (size_t i = 0; i < value.object->fields.size(); ++i) {
                if (i) out += ", ";
                out += value.object->fields[i].first + ": " + toString(value.object->fields[i].second);
            }
            return out + "}";
        }
        case Type::Function:
        case Type::Native: return "<function>";
        case Type::Handle: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "<handle %u:%lld>", value.handleKind,
                          static_cast<long long>(value.handle));
            return buffer;
        }
    }
    return "?";
}

bool valuesEqual(const Value& a, const Value& b) {
    if (a.type != b.type) return false;

    switch (a.type) {
        case Type::Nil: return true;
        case Type::Bool: return a.boolean == b.boolean;
        case Type::Number: return a.number == b.number;
        case Type::Vec: return a.vec == b.vec;
        case Type::Str: return a.object && b.object && a.object->text == b.object->text;
        case Type::Handle: return a.handleKind == b.handleKind && a.handle == b.handle;

        // Lists and maps compare by identity, not by contents. Deep equality
        // would make `==` recursive and, for two lists that contain each
        // other, non-terminating; a script that wants contents can walk them.
        case Type::List:
        case Type::Map:
        case Type::Function: return a.object == b.object;
        case Type::Native: return a.native == b.native;
    }
    return false;
}

} // namespace blocky::script
