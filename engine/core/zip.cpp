#include "engine/core/zip.hpp"

#include "engine/core/deflate.hpp"
#include "engine/core/file.hpp"

#include <cstring>

namespace blocky {
namespace {

constexpr uint32_t kSigEndOfCentralDirectory   = 0x06054b50;
constexpr uint32_t kSigCentralFileHeader       = 0x02014b50;
constexpr uint32_t kSigLocalFileHeader         = 0x04034b50;
constexpr uint32_t kSigZip64EndLocator         = 0x07064b50;
constexpr uint32_t kSigZip64EndOfCentralDir    = 0x06064b50;

constexpr uint16_t kMethodStored  = 0;
constexpr uint16_t kMethodDeflate = 8;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// ZIP is little-endian throughout.
uint16_t readU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t readU64(const uint8_t* p) {
    return uint64_t(readU32(p)) | (uint64_t(readU32(p + 4)) << 32);
}

} // namespace

void ZipArchive::close() {
    data_.clear();
    data_.shrink_to_fit();
    entries_.clear();
    names_.clear();
}

bool ZipArchive::openFile(const std::string& utf8Path, std::string* error) {
    close();

    if (!readFileBytes(utf8Path, data_, error)) return false;
    if (data_.size() < 22) {
        setError(error, "zip: file too small to contain a directory");
        close();
        return false;
    }

    // The end-of-central-directory record sits at the very end, unless a
    // trailing comment pushes it back by up to 64 KiB. Scan backwards.
    const size_t maxCommentLength = 65535;
    size_t searchStart = data_.size() > maxCommentLength + 22 ? data_.size() - maxCommentLength - 22 : 0;

    size_t eocd = 0;
    bool found = false;
    for (size_t i = data_.size() - 22 + 1; i-- > searchStart;) {
        if (readU32(data_.data() + i) == kSigEndOfCentralDirectory) {
            eocd = i;
            found = true;
            break;
        }
    }
    if (!found) {
        setError(error, "zip: end-of-central-directory record not found");
        close();
        return false;
    }

    uint64_t entryCount = readU16(data_.data() + eocd + 10);
    uint64_t directorySize = readU32(data_.data() + eocd + 12);
    uint64_t directoryOffset = readU32(data_.data() + eocd + 16);

    // Any field saturated to all-ones means the real value lives in the Zip64
    // record, which is pointed at by a locator just before the EOCD.
    bool needsZip64 = entryCount == 0xFFFFu || directorySize == 0xFFFFFFFFu ||
                      directoryOffset == 0xFFFFFFFFu;
    if (needsZip64 && eocd >= 20) {
        size_t locator = eocd - 20;
        if (readU32(data_.data() + locator) == kSigZip64EndLocator) {
            uint64_t zip64Offset = readU64(data_.data() + locator + 8);
            if (zip64Offset + 56 <= data_.size() &&
                readU32(data_.data() + zip64Offset) == kSigZip64EndOfCentralDir) {
                entryCount = readU64(data_.data() + zip64Offset + 32);
                directorySize = readU64(data_.data() + zip64Offset + 40);
                directoryOffset = readU64(data_.data() + zip64Offset + 48);
            }
        }
    }

    if (directoryOffset + directorySize > data_.size()) {
        setError(error, "zip: central directory lies outside the file");
        close();
        return false;
    }

    names_.reserve(size_t(entryCount));
    entries_.reserve(size_t(entryCount) * 2);

    size_t pos = size_t(directoryOffset);
    const size_t directoryEnd = size_t(directoryOffset + directorySize);

    for (uint64_t i = 0; i < entryCount; ++i) {
        if (pos + 46 > directoryEnd) {
            setError(error, "zip: central directory truncated");
            close();
            return false;
        }
        const uint8_t* header = data_.data() + pos;
        if (readU32(header) != kSigCentralFileHeader) {
            setError(error, "zip: bad central directory signature");
            close();
            return false;
        }

        Entry entry;
        entry.method = readU16(header + 10);
        entry.crc32 = readU32(header + 16);
        entry.compressedSize = readU32(header + 20);
        entry.uncompressedSize = readU32(header + 24);

        uint16_t nameLength = readU16(header + 28);
        uint16_t extraLength = readU16(header + 30);
        uint16_t commentLength = readU16(header + 32);
        entry.localHeaderOffset = readU32(header + 42);

        if (pos + 46 + nameLength + extraLength + commentLength > directoryEnd) {
            setError(error, "zip: central directory entry overruns");
            close();
            return false;
        }

        std::string name(reinterpret_cast<const char*>(header + 46), nameLength);

        // Zip64 extended information, if any of the 32-bit fields overflowed.
        // The fields appear in a fixed order, but only those that were
        // saturated are present, so they must be consumed in that order.
        if (extraLength > 0) {
            const uint8_t* extra = header + 46 + nameLength;
            size_t offset = 0;
            while (offset + 4 <= extraLength) {
                uint16_t id = readU16(extra + offset);
                uint16_t size = readU16(extra + offset + 2);
                if (offset + 4 + size > extraLength) break;

                if (id == 0x0001) {
                    const uint8_t* field = extra + offset + 4;
                    size_t remaining = size;
                    if (entry.uncompressedSize == 0xFFFFFFFFu && remaining >= 8) {
                        entry.uncompressedSize = readU64(field);
                        field += 8;
                        remaining -= 8;
                    }
                    if (entry.compressedSize == 0xFFFFFFFFu && remaining >= 8) {
                        entry.compressedSize = readU64(field);
                        field += 8;
                        remaining -= 8;
                    }
                    if (entry.localHeaderOffset == 0xFFFFFFFFu && remaining >= 8) {
                        entry.localHeaderOffset = readU64(field);
                    }
                    break;
                }
                offset += 4 + size;
            }
        }

        pos += 46 + nameLength + extraLength + commentLength;

        // Directory entries end in a slash and carry no data.
        if (!name.empty() && name.back() != '/') {
            names_.push_back(name);
            entries_.emplace(std::move(name), entry);
        }
    }

    return true;
}

uint64_t ZipArchive::sizeOf(const std::string& name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? 0 : it->second.uncompressedSize;
}

bool ZipArchive::read(const std::string& name, std::vector<uint8_t>& out, std::string* error) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        setError(error, "zip: no such entry: " + name);
        return false;
    }
    const Entry& entry = it->second;

    // The local header repeats the name and extra fields, and its lengths can
    // differ from the central directory copy -- so the data offset has to be
    // computed from the local header, not guessed.
    if (entry.localHeaderOffset + 30 > data_.size()) {
        setError(error, "zip: local header outside file: " + name);
        return false;
    }
    const uint8_t* local = data_.data() + entry.localHeaderOffset;
    if (readU32(local) != kSigLocalFileHeader) {
        setError(error, "zip: bad local header signature: " + name);
        return false;
    }

    uint16_t localNameLength = readU16(local + 26);
    uint16_t localExtraLength = readU16(local + 28);
    uint64_t dataOffset = entry.localHeaderOffset + 30 + localNameLength + localExtraLength;

    if (dataOffset + entry.compressedSize > data_.size()) {
        setError(error, "zip: entry data outside file: " + name);
        return false;
    }
    const uint8_t* payload = data_.data() + dataOffset;

    out.clear();
    switch (entry.method) {
        case kMethodStored:
            out.assign(payload, payload + entry.compressedSize);
            break;
        case kMethodDeflate:
            out.reserve(size_t(entry.uncompressedSize));
            if (!rawInflate(payload, size_t(entry.compressedSize), out, error)) return false;
            break;
        default:
            setError(error, "zip: unsupported compression method for " + name);
            return false;
    }

    if (out.size() != entry.uncompressedSize) {
        setError(error, "zip: size mismatch for " + name);
        return false;
    }
    if (crc32Bytes(out.data(), out.size()) != entry.crc32) {
        setError(error, "zip: CRC mismatch for " + name);
        return false;
    }
    return true;
}

} // namespace blocky
