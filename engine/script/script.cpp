#include "engine/script/script.hpp"

#include "engine/core/file.hpp"
#include "engine/script/parser.hpp"

namespace blocky::script {

Script::Script() = default;
Script::~Script() = default;

void Script::setError(const std::string& message, int line, const std::string& name) {
    lastError_ = name + ":" + std::to_string(line) + ": " + message;
}

bool Script::build(const std::string& source, const std::string& name) {
    // Parse *first*. Everything below this line is destructive, and a syntax
    // error must not reach it.
    ParseResult parsed = parse(source);
    if (!parsed.error.empty()) {
        setError(parsed.error, parsed.errorLine, name);
        return false;
    }

    auto program = std::make_unique<Program>(std::move(parsed.program));

    // Past this point the previous script is gone. A runtime error in the new
    // top level is therefore not recoverable the way a syntax error is -- the
    // old program's heap is already freed. That is the honest trade: catching
    // it would mean keeping two heaps alive and deciding which handlers won.
    interpreter_.reset();
    heap_.reset();

    interpreter_ = std::make_unique<Interpreter>(heap_);
    if (onPrint) interpreter_->onPrint = onPrint;

    installCoreLibrary(*interpreter_);
    if (onBind) onBind(*interpreter_);

    std::string error;
    int line = 0;
    if (!interpreter_->run(*program, error, line)) {
        setError(error, line, name);
        program_ = std::move(program);   // kept alive: handlers may point into it
        loaded_ = false;
        return false;
    }

    program_ = std::move(program);
    source_ = source;
    name_ = name;
    lastError_.clear();
    loaded_ = true;
    return true;
}

bool Script::loadSource(const std::string& source, const std::string& name) {
    path_.clear();
    return build(source, name);
}

bool Script::loadFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!readFileBytes(path, bytes, &error)) {
        lastError_ = path + ": " + error;
        return false;
    }

    path_ = path;
    std::string source(bytes.begin(), bytes.end());
    return build(source, path);
}

bool Script::reload() {
    if (path_.empty()) {
        lastError_ = "this script was loaded from source and has no file to reload";
        return false;
    }
    return loadFile(path_);
}

bool Script::reloadIfChanged(bool* changed) {
    if (changed) *changed = false;
    if (path_.empty()) return true;

    std::vector<uint8_t> bytes;
    std::string error;
    if (!readFileBytes(path_, bytes, &error)) {
        lastError_ = path_ + ": " + error;
        return false;
    }

    std::string source(bytes.begin(), bytes.end());
    if (source == source_ && loaded_) return true;

    if (changed) *changed = true;
    return build(source, path_);
}

bool Script::call(const std::string& event, const std::vector<Value>& args) {
    if (!loaded_ || !interpreter_) {
        lastError_ = "no script is loaded";
        return false;
    }

    std::string error;
    int line = 0;
    if (!interpreter_->callHandler(event, args, error, line)) {
        setError(error, line, name_);
        return false;
    }
    lastError_.clear();
    return true;
}

bool Script::hasHandler(const std::string& event) const {
    return interpreter_ && interpreter_->hasHandler(event);
}

} // namespace blocky::script
