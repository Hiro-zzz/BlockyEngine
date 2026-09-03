#pragma once
// The host's handle on a script: load it, bind to it, call into it, reload it
// while the world stays up.
//
// ------------------------------------------------------------------ reload
//
// Reloading is the point of the whole layer, so the rules it follows are the
// design rather than a detail:
//
//   - **A script that fails to parse changes nothing.** The new source is
//     parsed before anything is thrown away, and a syntax error leaves the
//     previous program running. Someone editing a tool with the world open
//     saves a broken file every few minutes; that must cost them a message,
//     not the session.
//   - **All state lives on the host side.** Bodies, joints, blocks. A script
//     declares behaviour, and the previous program's heap is freed wholesale
//     on reload -- which is also what keeps the absent garbage collector from
//     mattering. A script needing something to survive puts it in the world.
//   - **Bindings are reinstalled, not remembered.** The host supplies
//     `onBind`, called on every load, so there is one place that says what a
//     script can reach and no way for the two loads to disagree.
#include "engine/script/interp.hpp"

#include <memory>
#include <string>

namespace blocky::script {

class Script {
public:
    Script();
    ~Script();

    // Neither copyable nor movable, and the second half is not an oversight:
    // the interpreter holds a reference to the heap that lives in this
    // object, so moving one would leave the interpreter pointing at the
    // corpse. A Script is built where it is used.
    Script(const Script&) = delete;
    Script& operator=(const Script&) = delete;
    Script(Script&&) = delete;
    Script& operator=(Script&&) = delete;

    // Called after the core library is installed and before the top level
    // runs, on every load and reload. Where the host adds its own natives and
    // globals.
    std::function<void(Interpreter&)> onBind;

    // Where `print` goes; forwarded to each interpreter as it is built.
    std::function<void(const std::string&)> onPrint;

    bool loadSource(const std::string& source, const std::string& name = "<source>");
    bool loadFile(const std::string& path);

    // Re-reads the file this was loaded from and loads it again. False when
    // the file cannot be read or does not parse, in which case the script
    // that was already running is untouched.
    bool reload();

    // Reload only when the file on disk differs from what is loaded. Compares
    // contents rather than a timestamp: a script is a page long, reading it
    // costs nothing, and a modification time is a surprisingly unreliable
    // thing to poll across editors and filesystems.
    //
    // `changed` says whether a reload was attempted at all, so a caller can
    // tell "nothing to do" from "tried and failed".
    bool reloadIfChanged(bool* changed = nullptr);

    bool call(const std::string& event, const std::vector<Value>& args = {});
    bool hasHandler(const std::string& event) const;

    bool loaded() const { return loaded_; }
    const std::string& name() const { return name_; }

    // "name:line: message", or empty when the last operation succeeded.
    const std::string& lastError() const { return lastError_; }

    Interpreter* vm() { return interpreter_.get(); }

    size_t heapObjects() const { return heap_.objectCount(); }

private:
    bool build(const std::string& source, const std::string& name);
    void setError(const std::string& message, int line, const std::string& name);

    Heap heap_;
    std::unique_ptr<Interpreter> interpreter_;
    std::unique_ptr<Program> program_;

    std::string path_;      // empty when loaded from source text
    std::string source_;    // what is currently loaded
    std::string name_;
    std::string lastError_;
    bool loaded_ = false;
};

} // namespace blocky::script
