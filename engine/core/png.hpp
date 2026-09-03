#pragma once
// Self-contained PNG codec: no zlib, no stb, no libpng.
//
// Reading covers everything a Minecraft resource pack or a player skin can
// throw at us: colour types 0/2/3/4/6, bit depths 1/2/4/8/16, tRNS keys and
// palette alpha. Adam7 interlacing is rejected rather than half-supported.
//
// Writing emits 8-bit RGBA with a fixed-Huffman deflate stream.
#include "engine/core/deflate.hpp"
#include "engine/core/image.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

// Decode a PNG held in memory. Returns false and fills `error` on failure.
bool pngDecode(const uint8_t* bytes, size_t size, ImageU8& out, std::string* error = nullptr);

// Read a PNG from disk.
bool pngLoad(const std::string& path, ImageU8& out, std::string* error = nullptr);

// Encode to an in-memory PNG byte stream.
std::vector<uint8_t> pngEncode(const ImageU8& image);

// Write a PNG to disk, creating parent directories as needed.
bool pngSave(const std::string& path, const ImageU8& image, std::string* error = nullptr);

// Convenience: tonemap an HDR framebuffer and write it out in one call.
bool pngSave(const std::string& path, const Image& hdr, const ToneParams& params = {},
             std::string* error = nullptr);

// --------------------------------------------------------------------- APNG
// Animated PNG: the same file format with three more chunk types.
//
// It earns its place here for two reasons. It is the only animation container
// this project can write without acquiring a dependency -- the image data is
// byte for byte what pngEncode already produces, and the compressor is
// untouched. And its frame delay is a rational, so 24 fps is exactly 1/24 and
// a frame held on twos is one frame with a delay of 2/24 rather than two
// copies. Animation on twos therefore costs half the render *and* half the
// file, which is a pleasant thing for a format to reward.
//
// Frames are appended one at a time and only the compressed bytes are kept,
// so a long take costs the size of the finished file rather than the size of
// every frame decoded.
class ApngWriter {
public:
    // `plays` is how many times a viewer should loop it; 0 means forever.
    bool begin(int width, int height, int plays = 0, std::string* error = nullptr);

    // Encode and append. `delayNum / delayDen` is the frame's duration in
    // seconds -- {1, 24} for one frame at 24 fps, {2, 24} for a held pair.
    bool addFrame(const ImageU8& image, int delayNum, int delayDen,
                  std::string* error = nullptr);

    // Append a frame that is already a complete PNG, reusing its compressed
    // image data verbatim. This is what makes assembling a take out of a
    // rendered sequence cost file reads and nothing else: no decode, no
    // re-filter, no second pass through deflate.
    bool addEncodedFrame(const uint8_t* pngBytes, size_t size, int delayNum, int delayDen,
                         std::string* error = nullptr);

    bool save(const std::string& path, std::string* error = nullptr);

    int  frameCount() const { return frames_; }
    bool started() const { return started_; }

private:
    std::vector<uint8_t> out_;
    size_t   actlOffset_ = 0;   // patched with the frame count on save
    uint32_t sequence_ = 0;
    int      width_ = 0, height_ = 0;
    int      frames_ = 0;
    bool     started_ = false;

    bool appendFrameData(const uint8_t* zlibStream, size_t size, int delayNum, int delayDen,
                         std::string* error);
};

} // namespace blocky
