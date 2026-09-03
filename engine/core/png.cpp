#include "engine/core/png.hpp"
#include "engine/core/deflate.hpp"
#include "engine/core/file.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace blocky {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// ===========================================================================
//  PNG chunk plumbing
// ===========================================================================

constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void appendU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void appendChunk(std::vector<uint8_t>& out, const char tag[4], const uint8_t* payload, size_t size) {
    appendU32BE(out, uint32_t(size));
    size_t crcStart = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (size) out.insert(out.end(), payload, payload + size);
    appendU32BE(out, crc32Bytes(out.data() + crcStart, out.size() - crcStart));
}

int channelsForColorType(int colorType) {
    switch (colorType) {
        case 0: return 1;  // grayscale
        case 2: return 3;  // truecolour
        case 3: return 1;  // palette index
        case 4: return 2;  // grayscale + alpha
        case 6: return 4;  // truecolour + alpha
        default: return 0;
    }
}

uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
    int p = int(a) + int(b) - int(c);
    int pa = std::abs(p - int(a)), pb = std::abs(p - int(b)), pc = std::abs(p - int(c));
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Reverse the per-scanline filters in place, producing raw samples.
bool unfilter(std::vector<uint8_t>& raw, int width, int height, int channels, int bitDepth,
              std::vector<uint8_t>& out, std::string* error) {
    size_t bytesPerLine = (size_t(width) * size_t(channels) * size_t(bitDepth) + 7) / 8;
    size_t bpp = std::max<size_t>(1, size_t(channels) * size_t(bitDepth) / 8);

    if (raw.size() < (bytesPerLine + 1) * size_t(height)) {
        setError(error, "png: image data truncated");
        return false;
    }

    out.assign(bytesPerLine * size_t(height), 0);

    for (int y = 0; y < height; ++y) {
        const uint8_t* src = raw.data() + (bytesPerLine + 1) * size_t(y);
        uint8_t filter = src[0];
        ++src;

        uint8_t* cur = out.data() + bytesPerLine * size_t(y);
        const uint8_t* prior = y > 0 ? out.data() + bytesPerLine * size_t(y - 1) : nullptr;

        for (size_t x = 0; x < bytesPerLine; ++x) {
            uint8_t a = x >= bpp ? cur[x - bpp] : 0;              // left
            uint8_t b = prior ? prior[x] : 0;                     // above
            uint8_t c = (prior && x >= bpp) ? prior[x - bpp] : 0; // above-left

            switch (filter) {
                case 0: cur[x] = src[x]; break;
                case 1: cur[x] = uint8_t(src[x] + a); break;
                case 2: cur[x] = uint8_t(src[x] + b); break;
                case 3: cur[x] = uint8_t(src[x] + uint8_t((int(a) + int(b)) / 2)); break;
                case 4: cur[x] = uint8_t(src[x] + paethPredictor(a, b, c)); break;
                default: setError(error, "png: unknown filter type"); return false;
            }
        }
    }
    return true;
}

// Pull sample `index` out of a scanline packed at `bitDepth` bits per sample.
uint16_t sampleAt(const uint8_t* line, size_t index, int bitDepth) {
    switch (bitDepth) {
        case 8:  return line[index];
        case 16: return uint16_t((line[index * 2] << 8) | line[index * 2 + 1]);
        case 1:  return (line[index >> 3] >> (7 - (index & 7))) & 1u;
        case 2:  return (line[index >> 2] >> (6 - 2 * (index & 3))) & 3u;
        case 4:  return (line[index >> 1] >> (4 - 4 * (index & 1))) & 15u;
        default: return 0;
    }
}

// Scale a `bitDepth`-bit sample up to full 8-bit range.
uint8_t scaleTo8(uint16_t value, int bitDepth) {
    switch (bitDepth) {
        case 8:  return uint8_t(value);
        case 16: return uint8_t(value >> 8);
        case 1:  return value ? 255 : 0;
        case 2:  return uint8_t(value * 85);
        case 4:  return uint8_t(value * 17);
        default: return 0;
    }
}

void appendU16BE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

// Per scanline, try all five filters and keep whichever produces the smallest
// sum of absolute deviations -- the standard heuristic, and it makes a large
// difference on flat Minecraft-style artwork. Shared by the still encoder and
// by the APNG writer, which need byte-identical output.
std::vector<uint8_t> filterScanlines(const ImageU8& image) {
    const int width = image.width(), height = image.height();
    const size_t stride = size_t(width) * 4;
    const size_t bpp = 4;

    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * size_t(height));

    std::array<std::vector<uint8_t>, 5> candidates;
    for (auto& c : candidates) c.resize(stride);

    for (int y = 0; y < height; ++y) {
        const uint8_t* cur = image.data() + stride * size_t(y);
        const uint8_t* prior = y > 0 ? image.data() + stride * size_t(y - 1) : nullptr;

        for (size_t x = 0; x < stride; ++x) {
            uint8_t a = x >= bpp ? cur[x - bpp] : 0;
            uint8_t b = prior ? prior[x] : 0;
            uint8_t c = (prior && x >= bpp) ? prior[x - bpp] : 0;

            candidates[0][x] = cur[x];
            candidates[1][x] = uint8_t(cur[x] - a);
            candidates[2][x] = uint8_t(cur[x] - b);
            candidates[3][x] = uint8_t(cur[x] - uint8_t((int(a) + int(b)) / 2));
            candidates[4][x] = uint8_t(cur[x] - paethPredictor(a, b, c));
        }

        int best = 0;
        uint64_t bestScore = UINT64_MAX;
        for (int f = 0; f < 5; ++f) {
            uint64_t score = 0;
            for (size_t x = 0; x < stride; ++x) {
                int8_t sv = int8_t(candidates[size_t(f)][x]);
                score += uint64_t(sv < 0 ? -int(sv) : int(sv));
            }
            if (score < bestScore) { bestScore = score; best = f; }
        }

        raw.push_back(uint8_t(best));
        raw.insert(raw.end(), candidates[size_t(best)].begin(), candidates[size_t(best)].end());
    }
    return raw;
}

// Fills IHDR fields shared by the still and animated writers.
void writeIhdr(std::vector<uint8_t>& out, int width, int height) {
    uint8_t ihdr[13];
    ihdr[0] = uint8_t(uint32_t(width) >> 24);  ihdr[1] = uint8_t(uint32_t(width) >> 16);
    ihdr[2] = uint8_t(uint32_t(width) >> 8);   ihdr[3] = uint8_t(uint32_t(width));
    ihdr[4] = uint8_t(uint32_t(height) >> 24); ihdr[5] = uint8_t(uint32_t(height) >> 16);
    ihdr[6] = uint8_t(uint32_t(height) >> 8);  ihdr[7] = uint8_t(uint32_t(height));
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // colour type: truecolour + alpha
    ihdr[10] = 0;  // compression: deflate
    ihdr[11] = 0;  // filter method 0
    ihdr[12] = 0;  // no interlacing
    appendChunk(out, "IHDR", ihdr, sizeof(ihdr));
}

} // namespace

// ===========================================================================
//  Public API
// ===========================================================================

bool pngDecode(const uint8_t* bytes, size_t size, ImageU8& out, std::string* error) {
    if (size < 8 || std::memcmp(bytes, kSignature, 8) != 0) {
        setError(error, "png: bad signature");
        return false;
    }

    int width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
    bool haveHeader = false;
    std::vector<uint8_t> palette;      // RGB triples
    std::vector<uint8_t> paletteAlpha; // parallel alpha, from tRNS
    std::vector<uint8_t> compressed;
    bool haveTransparencyKey = false;
    uint16_t keyR = 0, keyG = 0, keyB = 0;

    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t length = readU32BE(bytes + pos);
        const char* tag = reinterpret_cast<const char*>(bytes + pos + 4);
        const uint8_t* payload = bytes + pos + 8;
        if (pos + 12 + length > size) { setError(error, "png: chunk overruns file"); return false; }

        if (std::memcmp(tag, "IHDR", 4) == 0) {
            if (length < 13) { setError(error, "png: short IHDR"); return false; }
            width     = int(readU32BE(payload));
            height    = int(readU32BE(payload + 4));
            bitDepth  = payload[8];
            colorType = payload[9];
            interlace = payload[12];
            haveHeader = true;

            if (width <= 0 || height <= 0) { setError(error, "png: bad dimensions"); return false; }
            if (interlace != 0) { setError(error, "png: Adam7 interlacing unsupported"); return false; }
            if (channelsForColorType(colorType) == 0) { setError(error, "png: bad colour type"); return false; }
            bool depthOk = (bitDepth == 8 || bitDepth == 16 ||
                            ((bitDepth == 1 || bitDepth == 2 || bitDepth == 4) &&
                             (colorType == 0 || colorType == 3)));
            if (!depthOk) { setError(error, "png: unsupported bit depth"); return false; }
        } else if (std::memcmp(tag, "PLTE", 4) == 0) {
            palette.assign(payload, payload + length);
        } else if (std::memcmp(tag, "tRNS", 4) == 0) {
            if (colorType == 3) {
                paletteAlpha.assign(payload, payload + length);
            } else if (colorType == 0 && length >= 2) {
                haveTransparencyKey = true;
                keyR = keyG = keyB = uint16_t((payload[0] << 8) | payload[1]);
            } else if (colorType == 2 && length >= 6) {
                haveTransparencyKey = true;
                keyR = uint16_t((payload[0] << 8) | payload[1]);
                keyG = uint16_t((payload[2] << 8) | payload[3]);
                keyB = uint16_t((payload[4] << 8) | payload[5]);
            }
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            compressed.insert(compressed.end(), payload, payload + length);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + length;  // length + tag + payload + crc
    }

    if (!haveHeader) { setError(error, "png: missing IHDR"); return false; }
    if (compressed.empty()) { setError(error, "png: missing IDAT"); return false; }
    if (colorType == 3 && palette.empty()) { setError(error, "png: indexed image without PLTE"); return false; }

    std::vector<uint8_t> raw;
    if (!zlibInflate(compressed.data(), compressed.size(), raw, error)) return false;

    int channels = channelsForColorType(colorType);
    std::vector<uint8_t> samples;
    if (!unfilter(raw, width, height, channels, bitDepth, samples, error)) return false;

    size_t bytesPerLine = (size_t(width) * size_t(channels) * size_t(bitDepth) + 7) / 8;
    out.resize(width, height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* line = samples.data() + bytesPerLine * size_t(y);
        for (int x = 0; x < width; ++x) {
            size_t base = size_t(x) * size_t(channels);
            ImageU8::RGBA c{0, 0, 0, 255};

            switch (colorType) {
                case 0: {  // grayscale
                    uint16_t g = sampleAt(line, base, bitDepth);
                    uint8_t v = scaleTo8(g, bitDepth);
                    c = {v, v, v, uint8_t(haveTransparencyKey && g == keyR ? 0 : 255)};
                    break;
                }
                case 2: {  // RGB
                    uint16_t r = sampleAt(line, base + 0, bitDepth);
                    uint16_t g = sampleAt(line, base + 1, bitDepth);
                    uint16_t b = sampleAt(line, base + 2, bitDepth);
                    bool clear = haveTransparencyKey && r == keyR && g == keyG && b == keyB;
                    c = {scaleTo8(r, bitDepth), scaleTo8(g, bitDepth), scaleTo8(b, bitDepth),
                         uint8_t(clear ? 0 : 255)};
                    break;
                }
                case 3: {  // palette index
                    size_t idx = sampleAt(line, base, bitDepth);
                    if (idx * 3 + 2 >= palette.size()) { setError(error, "png: palette index out of range"); return false; }
                    uint8_t a = idx < paletteAlpha.size() ? paletteAlpha[idx] : 255;
                    c = {palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2], a};
                    break;
                }
                case 4: {  // grayscale + alpha
                    uint8_t v = scaleTo8(sampleAt(line, base + 0, bitDepth), bitDepth);
                    uint8_t a = scaleTo8(sampleAt(line, base + 1, bitDepth), bitDepth);
                    c = {v, v, v, a};
                    break;
                }
                case 6: {  // RGBA
                    c = {scaleTo8(sampleAt(line, base + 0, bitDepth), bitDepth),
                         scaleTo8(sampleAt(line, base + 1, bitDepth), bitDepth),
                         scaleTo8(sampleAt(line, base + 2, bitDepth), bitDepth),
                         scaleTo8(sampleAt(line, base + 3, bitDepth), bitDepth)};
                    break;
                }
                default: break;
            }
            out.set(x, y, c);
        }
    }
    return true;
}

bool pngLoad(const std::string& path, ImageU8& out, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes, error)) return false;
    return pngDecode(bytes.data(), bytes.size(), out, error);
}

std::vector<uint8_t> pngEncode(const ImageU8& image) {
    std::vector<uint8_t> raw = filterScanlines(image);

    std::vector<uint8_t> out(kSignature, kSignature + 8);
    writeIhdr(out, image.width(), image.height());

    std::vector<uint8_t> idat = zlibDeflate(raw.data(), raw.size());
    appendChunk(out, "IDAT", idat.data(), idat.size());
    appendChunk(out, "IEND", nullptr, 0);
    return out;
}

bool pngSave(const std::string& path, const ImageU8& image, std::string* error) {
    if (image.empty()) { setError(error, "png: refusing to write an empty image"); return false; }
    if (!createParentDirectories(path)) { setError(error, "png: cannot create directory for " + path); return false; }
    std::vector<uint8_t> bytes = pngEncode(image);
    return writeFileBytes(path, bytes.data(), bytes.size(), error);
}

bool pngSave(const std::string& path, const Image& hdr, const ToneParams& params, std::string* error) {
    return pngSave(path, tonemapToU8(hdr, params), error);
}

// --------------------------------------------------------------------- APNG

bool ApngWriter::begin(int width, int height, int plays, std::string* error) {
    if (width <= 0 || height <= 0) {
        setError(error, "apng: refusing to start an empty animation");
        return false;
    }
    out_.assign(kSignature, kSignature + 8);
    writeIhdr(out_, width, height);

    // acTL has to precede the first frame, but the frame count is not known
    // until the last one has been appended -- so the chunk goes down now with
    // a zero in it and is patched, crc and all, in save().
    actlOffset_ = out_.size();
    if (plays < 0) plays = 0;
    uint8_t actl[8] = {0, 0, 0, 0,
                       uint8_t(uint32_t(plays) >> 24), uint8_t(uint32_t(plays) >> 16),
                       uint8_t(uint32_t(plays) >> 8),  uint8_t(uint32_t(plays))};
    appendChunk(out_, "acTL", actl, sizeof(actl));

    width_ = width;
    height_ = height;
    frames_ = 0;
    sequence_ = 0;
    started_ = true;
    return true;
}

bool ApngWriter::appendFrameData(const uint8_t* zlibStream, size_t size, int delayNum,
                                 int delayDen, std::string* error) {
    if (!started_) { setError(error, "apng: begin() has not been called"); return false; }
    if (size == 0)  { setError(error, "apng: frame has no image data"); return false; }

    delayNum = std::clamp(delayNum, 0, 65535);
    delayDen = std::clamp(delayDen, 1, 65535);

    uint8_t fctl[26];
    size_t o = 0;
    auto put32 = [&](uint32_t v) {
        fctl[o++] = uint8_t(v >> 24); fctl[o++] = uint8_t(v >> 16);
        fctl[o++] = uint8_t(v >> 8);  fctl[o++] = uint8_t(v);
    };
    auto put16 = [&](uint16_t v) { fctl[o++] = uint8_t(v >> 8); fctl[o++] = uint8_t(v); };

    put32(sequence_++);
    put32(uint32_t(width_));
    put32(uint32_t(height_));
    put32(0);  // x offset
    put32(0);  // y offset
    put16(uint16_t(delayNum));
    put16(uint16_t(delayDen));
    fctl[o++] = 0;  // dispose: leave this frame up, the next one covers it
    fctl[o++] = 0;  // blend: replace rather than composite -- frames are full size
    appendChunk(out_, "fcTL", fctl, sizeof(fctl));

    if (frames_ == 0) {
        // The first frame doubles as the still image, which is what every
        // viewer that does not animate PNGs will show. So it goes in as a
        // plain IDAT and carries no sequence number of its own.
        appendChunk(out_, "IDAT", zlibStream, size);
    } else {
        std::vector<uint8_t> fdat;
        fdat.reserve(size + 4);
        appendU32BE(fdat, sequence_++);
        fdat.insert(fdat.end(), zlibStream, zlibStream + size);
        appendChunk(out_, "fdAT", fdat.data(), fdat.size());
    }

    ++frames_;
    return true;
}

bool ApngWriter::addFrame(const ImageU8& image, int delayNum, int delayDen, std::string* error) {
    if (!started_) { setError(error, "apng: begin() has not been called"); return false; }
    if (image.width() != width_ || image.height() != height_) {
        setError(error, "apng: every frame must match the size given to begin()");
        return false;
    }
    std::vector<uint8_t> raw = filterScanlines(image);
    std::vector<uint8_t> z = zlibDeflate(raw.data(), raw.size());
    return appendFrameData(z.data(), z.size(), delayNum, delayDen, error);
}

bool ApngWriter::addEncodedFrame(const uint8_t* pngBytes, size_t size, int delayNum,
                                 int delayDen, std::string* error) {
    if (!started_) { setError(error, "apng: begin() has not been called"); return false; }
    if (size < 8 || std::memcmp(pngBytes, kSignature, 8) != 0) {
        setError(error, "apng: frame is not a PNG");
        return false;
    }

    std::vector<uint8_t> idat;
    bool haveHeader = false;
    size_t pos = 8;

    while (pos + 12 <= size) {
        const uint32_t length = readU32BE(pngBytes + pos);
        const uint8_t* tag = pngBytes + pos + 4;
        if (length > size || pos + 12 + length > size) {
            setError(error, "apng: truncated frame");
            return false;
        }
        const uint8_t* payload = pngBytes + pos + 8;

        if (std::memcmp(tag, "IHDR", 4) == 0) {
            if (length < 13) { setError(error, "apng: short IHDR in frame"); return false; }
            const int w = int(readU32BE(payload));
            const int h = int(readU32BE(payload + 4));
            if (w != width_ || h != height_) {
                setError(error, "apng: every frame must match the size given to begin()");
                return false;
            }
            // Reusing the compressed stream verbatim only works if the frame
            // was encoded exactly the way this writer would have encoded it.
            if (payload[8] != 8 || payload[9] != 6 || payload[12] != 0) {
                setError(error, "apng: frames must be 8-bit RGBA and not interlaced");
                return false;
            }
            haveHeader = true;
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            idat.insert(idat.end(), payload, payload + length);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + length;
    }

    if (!haveHeader)  { setError(error, "apng: frame has no IHDR"); return false; }
    if (idat.empty()) { setError(error, "apng: frame has no IDAT"); return false; }
    return appendFrameData(idat.data(), idat.size(), delayNum, delayDen, error);
}

bool ApngWriter::save(const std::string& path, std::string* error) {
    if (!started_ || frames_ == 0) {
        setError(error, "apng: refusing to write an animation with no frames");
        return false;
    }

    // Patch the frame count into acTL. Payload starts eight bytes into the
    // chunk, and the crc covers the tag and the payload together.
    uint8_t* chunk = out_.data() + actlOffset_;
    const uint32_t count = uint32_t(frames_);
    chunk[8]  = uint8_t(count >> 24);
    chunk[9]  = uint8_t(count >> 16);
    chunk[10] = uint8_t(count >> 8);
    chunk[11] = uint8_t(count);
    const uint32_t crc = crc32Bytes(chunk + 4, 12);
    chunk[16] = uint8_t(crc >> 24);
    chunk[17] = uint8_t(crc >> 16);
    chunk[18] = uint8_t(crc >> 8);
    chunk[19] = uint8_t(crc);

    std::vector<uint8_t> file = out_;
    appendChunk(file, "IEND", nullptr, 0);

    if (!createParentDirectories(path)) {
        setError(error, "apng: cannot create directory for " + path);
        return false;
    }
    return writeFileBytes(path, file.data(), file.size(), error);
}

} // namespace blocky
