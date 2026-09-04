#pragma once
// All engine paths are UTF-8. On Windows the narrow CRT functions go through
// the ANSI codepage, which mangles any non-ASCII path -- and this project
// already lives under one. So every file touch converts to UTF-16 first.
#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out,
                   std::string* error = nullptr);

bool writeFileBytes(const std::string& utf8Path, const uint8_t* bytes, size_t size,
                    std::string* error = nullptr);

bool fileExists(const std::string& utf8Path);
bool isDirectory(const std::string& utf8Path);

// Delete a file. True if it is gone afterwards, including when it was never
// there in the first place. Frame sequences are addressed by name and a bad
// frame is meant to be re-rendered by removing it, so the engine has to be
// able to remove one.
bool removeFile(const std::string& utf8Path);

struct DirEntry {
    std::string name;  // leaf name only
    bool isDirectory = false;
};

// Immediate children of a directory. "." and ".." are excluded.
bool listDirectory(const std::string& utf8Path, std::vector<DirEntry>& out);

// Create `utf8Path` and every missing parent directory. Succeeds if it exists.
bool createDirectories(const std::string& utf8Path);

// Create the parent directory chain of a file path.
bool createParentDirectories(const std::string& utf8FilePath);

// The directory part of a path, without the trailing separator ("" if none).
std::string parentPath(const std::string& utf8Path);

} // namespace blocky
