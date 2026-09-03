#pragma once
// Tokens to a syntax tree.
//
// Recursive descent, and it stops at the first syntax error rather than
// trying to recover. Error recovery earns its keep in a compiler that has to
// report on a ten-thousand-line file; here a script is a page long, the edit
// loop is a keystroke, and a second error invented by mis-recovering from the
// first is worse than no second error.
#include "engine/script/ast.hpp"

#include <string>

namespace blocky::script {

struct ParseResult {
    Program program;
    std::string error;     // empty when the parse succeeded
    int errorLine = 0;
};

ParseResult parse(const std::string& source);

} // namespace blocky::script
