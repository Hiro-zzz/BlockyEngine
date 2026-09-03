#pragma once
// DEFLATE (RFC 1951) and zlib (RFC 1950), written from scratch.
//
// Two consumers now: PNG wraps deflate in a zlib stream, ZIP archives store
// raw deflate with no wrapper at all. Hence the split into raw and wrapped
// entry points.
#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

// Raw DEFLATE, no header or trailer. This is what a ZIP entry contains.
bool rawInflate(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out,
                std::string* error = nullptr);
std::vector<uint8_t> rawDeflate(const uint8_t* bytes, size_t size);

// zlib-wrapped: two header bytes plus a trailing Adler-32, as PNG uses.
bool zlibInflate(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out,
                 std::string* error = nullptr);
std::vector<uint8_t> zlibDeflate(const uint8_t* bytes, size_t size);

uint32_t crc32Bytes(const uint8_t* bytes, size_t size, uint32_t seed = 0);
uint32_t adler32Bytes(const uint8_t* bytes, size_t size);

} // namespace blocky
