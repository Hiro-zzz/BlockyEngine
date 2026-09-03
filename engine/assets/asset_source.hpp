#pragma once
// Where asset bytes come from. One abstraction, two entirely separate
// consumers above it: the block texture library and the entity skin loader.
//
// A source is either an unpacked directory (a resource pack folder, a
// scratch directory of your own art) or a zip/jar (a client jar, a packed
// resource pack). Callers never need to know which.
#include "engine/core/zip.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace blocky {

class AssetSource {
public:
    // Open a directory. `root` is the folder that *contains* `assets/`.
    bool openDirectory(const std::string& utf8Path, std::string* error = nullptr);

    // Open a .jar or .zip.
    bool openArchive(const std::string& utf8Path, std::string* error = nullptr);

    // Open whichever the path is, deciding by whether it is a directory.
    bool open(const std::string& utf8Path, std::string* error = nullptr);

    void close();
    bool isOpen() const { return kind_ != Kind::None; }

    // Human-readable description of what got opened, for logging.
    const std::string& description() const { return description_; }

    // Path is always resource-pack relative and slash-separated, e.g.
    // "assets/minecraft/textures/block/stone.png".
    bool exists(const std::string& path) const;
    bool read(const std::string& path, std::vector<uint8_t>& out, std::string* error = nullptr) const;

    // Entry names under a prefix. Directory sources answer this by walking
    // the filesystem, archives by filtering the central directory.
    std::vector<std::string> list(const std::string& prefix) const;

    // Look for a Minecraft installation in the usual places and return the
    // newest client jar found, or an empty string. Nothing in the engine
    // requires this to succeed.
    static std::string findClientJar();

private:
    enum class Kind { None, Directory, Archive };

    Kind kind_ = Kind::None;
    std::string root_;
    std::string description_;
    std::unique_ptr<ZipArchive> archive_;
};

} // namespace blocky
