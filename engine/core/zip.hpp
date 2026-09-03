#pragma once
// Read-only ZIP archive reader, built on our own inflate.
//
// This is what lets the engine open a Minecraft client .jar or a .zip
// resource pack directly, instead of demanding an unpacked directory. ZIP
// entries hold *raw* deflate with no zlib wrapper, which is why deflate.hpp
// exposes rawInflate separately.
//
// Only the two methods that actually occur in game archives are supported:
// stored (0) and deflate (8).
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace blocky {

class ZipArchive {
public:
    // Loads the whole archive into memory. A client jar is around 25 MB, so
    // this is far cheaper than the syscalls that streaming would cost when
    // pulling a thousand small textures out of it.
    bool openFile(const std::string& utf8Path, std::string* error = nullptr);
    void close();

    bool   isOpen()     const { return !data_.empty(); }
    size_t entryCount() const { return names_.size(); }

    // Entry names, in central-directory order. Paths use forward slashes.
    const std::vector<std::string>& names() const { return names_; }

    bool contains(const std::string& name) const { return entries_.count(name) != 0; }

    // Decompress one entry. The CRC-32 recorded in the archive is verified.
    bool read(const std::string& name, std::vector<uint8_t>& out, std::string* error = nullptr) const;

    // Uncompressed size of an entry, or 0 if it is not present.
    uint64_t sizeOf(const std::string& name) const;

private:
    struct Entry {
        uint64_t localHeaderOffset = 0;
        uint64_t compressedSize = 0;
        uint64_t uncompressedSize = 0;
        uint32_t crc32 = 0;
        uint16_t method = 0;
    };

    std::vector<uint8_t> data_;
    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> names_;
};

} // namespace blocky
