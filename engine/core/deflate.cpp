#include "engine/core/deflate.hpp"

#include <algorithm>
#include <array>

namespace blocky {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void appendU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

// ===========================================================================
//  Checksums
// ===========================================================================

const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();
    return table;
}

// ===========================================================================
//  Inflate (RFC 1951)
// ===========================================================================

constexpr int kMaxBits = 15;

// Canonical Huffman table in the compact "puff" form: how many codes exist of
// each bit length, plus the symbols sorted by (length, symbol).
struct Huffman {
    std::array<int16_t, kMaxBits + 1> count{};
    std::vector<int16_t> symbol;
};

void huffmanBuild(Huffman& h, const int16_t* lengths, int n) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) h.count[size_t(lengths[i])]++;
    h.count[0] = 0;

    std::array<int, kMaxBits + 2> offsets{};
    offsets[1] = 0;
    for (int len = 1; len <= kMaxBits; ++len) offsets[size_t(len) + 1] = offsets[size_t(len)] + h.count[size_t(len)];

    h.symbol.assign(size_t(n), 0);
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h.symbol[size_t(offsets[size_t(lengths[i])]++)] = int16_t(i);
    }
}

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Read `n` bits LSB-first. Sets failed() past the end of the stream.
    uint32_t bits(int n) {
        while (bitCount_ < n) {
            if (pos_ >= size_) { failed_ = true; return 0; }
            bitBuffer_ |= uint32_t(data_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        uint32_t value = bitBuffer_ & ((1u << n) - 1u);
        bitBuffer_ >>= n;
        bitCount_ -= n;
        return value;
    }

    // Huffman codes are packed most-significant-bit first, one bit at a time.
    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= int(bits(1));
            if (failed_) return -1;
            int count = h.count[size_t(len)];
            if (code - first < count) return h.symbol[size_t(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }

    void alignToByte() { bitBuffer_ = 0; bitCount_ = 0; }

    bool   failed() const { return failed_; }
    size_t pos()    const { return pos_; }
    size_t size()   const { return size_; }

    // Copy `n` raw bytes; only valid straight after alignToByte().
    bool copyBytes(std::vector<uint8_t>& out, size_t n) {
        if (pos_ + n > size_) { failed_ = true; return false; }
        out.insert(out.end(), data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return true;
    }
    bool readRawU16(uint16_t& value) {
        if (pos_ + 2 > size_) { failed_ = true; return false; }
        value = uint16_t(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return true;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0, pos_ = 0;
    uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
    bool failed_ = false;
};

// Length/distance code tables from RFC 1951 section 3.2.5.
constexpr int16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
    51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int16_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
    3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int32_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int16_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void buildFixedTables(Huffman& litLen, Huffman& dist) {
    int16_t lengths[288];
    for (int i = 0;   i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    huffmanBuild(litLen, lengths, 288);

    int16_t distLengths[30];
    for (int i = 0; i < 30; ++i) distLengths[i] = 5;
    huffmanBuild(dist, distLengths, 30);
}

bool inflateBlockBody(BitReader& br, const Huffman& litLen, const Huffman& dist,
                      std::vector<uint8_t>& out, std::string* error) {
    for (;;) {
        int symbol = br.decode(litLen);
        if (symbol < 0) { setError(error, "deflate: bad literal/length code"); return false; }

        if (symbol < 256) {
            out.push_back(uint8_t(symbol));
            continue;
        }
        if (symbol == 256) return true;  // end of block

        symbol -= 257;
        if (symbol >= 29) { setError(error, "deflate: invalid length symbol"); return false; }
        int length = kLengthBase[symbol] + int(br.bits(kLengthExtra[symbol]));

        int distSymbol = br.decode(dist);
        if (distSymbol < 0 || distSymbol >= 30) {
            setError(error, "deflate: invalid distance symbol");
            return false;
        }
        size_t distance = size_t(kDistBase[distSymbol]) + size_t(br.bits(kDistExtra[distSymbol]));
        if (br.failed()) { setError(error, "deflate: truncated stream"); return false; }
        if (distance > out.size()) { setError(error, "deflate: distance before start"); return false; }

        // Overlapping copies are legal and common; copy byte by byte.
        size_t from = out.size() - distance;
        for (int i = 0; i < length; ++i) out.push_back(out[from + size_t(i)]);
    }
}

bool inflateDynamicTables(BitReader& br, Huffman& litLen, Huffman& dist, std::string* error) {
    int hlit  = int(br.bits(5)) + 257;
    int hdist = int(br.bits(5)) + 1;
    int hclen = int(br.bits(4)) + 4;
    if (hlit > 286 || hdist > 30) { setError(error, "deflate: bad table sizes"); return false; }

    static constexpr int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int16_t codeLengths[19] = {};
    for (int i = 0; i < hclen; ++i) codeLengths[kOrder[i]] = int16_t(br.bits(3));

    Huffman codeTable;
    huffmanBuild(codeTable, codeLengths, 19);

    std::vector<int16_t> lengths(size_t(hlit + hdist), 0);
    for (int i = 0; i < hlit + hdist;) {
        int symbol = br.decode(codeTable);
        if (symbol < 0) { setError(error, "deflate: bad code-length code"); return false; }

        if (symbol < 16) {
            lengths[size_t(i++)] = int16_t(symbol);
        } else if (symbol == 16) {
            if (i == 0) { setError(error, "deflate: repeat with no previous length"); return false; }
            int16_t prev = lengths[size_t(i - 1)];
            int repeat = 3 + int(br.bits(2));
            while (repeat-- && i < hlit + hdist) lengths[size_t(i++)] = prev;
        } else if (symbol == 17) {
            int repeat = 3 + int(br.bits(3));
            while (repeat-- && i < hlit + hdist) lengths[size_t(i++)] = 0;
        } else {
            int repeat = 11 + int(br.bits(7));
            while (repeat-- && i < hlit + hdist) lengths[size_t(i++)] = 0;
        }
        if (br.failed()) { setError(error, "deflate: truncated table"); return false; }
    }

    huffmanBuild(litLen, lengths.data(), hlit);
    huffmanBuild(dist, lengths.data() + hlit, hdist);
    return true;
}

bool rawInflateImpl(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out, std::string* error) {
    BitReader br(bytes, size);
    bool final = false;

    while (!final) {
        final = br.bits(1) != 0;
        uint32_t type = br.bits(2);
        if (br.failed()) { setError(error, "deflate: truncated block header"); return false; }

        if (type == 0) {                       // stored
            br.alignToByte();
            uint16_t len = 0, nlen = 0;
            if (!br.readRawU16(len) || !br.readRawU16(nlen)) {
                setError(error, "deflate: truncated stored header");
                return false;
            }
            if (uint16_t(~len) != nlen) { setError(error, "deflate: stored length mismatch"); return false; }
            if (!br.copyBytes(out, len)) { setError(error, "deflate: truncated stored block"); return false; }
        } else if (type == 1 || type == 2) {   // fixed or dynamic Huffman
            Huffman litLen, dist;
            if (type == 1) {
                buildFixedTables(litLen, dist);
            } else if (!inflateDynamicTables(br, litLen, dist, error)) {
                return false;
            }
            if (!inflateBlockBody(br, litLen, dist, out, error)) return false;
        } else {
            setError(error, "deflate: reserved block type");
            return false;
        }
    }
    return true;
}

// ===========================================================================
//  Deflate (fixed Huffman + greedy LZ77)
// ===========================================================================

class BitWriter {
public:
    void bits(uint32_t value, int n) {
        for (int i = 0; i < n; ++i) {
            bitBuffer_ |= ((value >> i) & 1u) << bitCount_;
            if (++bitCount_ == 8) flushByte();
        }
    }
    // Huffman codes go out most-significant-bit first.
    void code(uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bitBuffer_ |= ((value >> i) & 1u) << bitCount_;
            if (++bitCount_ == 8) flushByte();
        }
    }
    void finish() { if (bitCount_ > 0) flushByte(); }

    std::vector<uint8_t> take() { finish(); return std::move(out_); }

private:
    void flushByte() { out_.push_back(uint8_t(bitBuffer_)); bitBuffer_ = 0; bitCount_ = 0; }

    std::vector<uint8_t> out_;
    uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
};

// Fixed-table literal/length code for symbol `sym`, per RFC 1951 3.2.6.
void writeFixedSymbol(BitWriter& bw, int sym) {
    if (sym < 144)      bw.code(uint32_t(0x30 + sym), 8);
    else if (sym < 256) bw.code(uint32_t(0x190 + sym - 144), 9);
    else if (sym < 280) bw.code(uint32_t(sym - 256), 7);
    else                bw.code(uint32_t(0xC0 + sym - 280), 8);
}

int lengthSymbolFor(int length) {
    for (int i = 28; i >= 0; --i) {
        if (length >= kLengthBase[i]) return i;
    }
    return 0;
}
int distSymbolFor(int distance) {
    for (int i = 29; i >= 0; --i) {
        if (distance >= kDistBase[i]) return i;
    }
    return 0;
}

constexpr int kWindowSize = 32768;
constexpr int kMinMatch   = 3;
constexpr int kMaxMatch   = 258;
constexpr int kHashBits   = 15;
constexpr int kHashSize   = 1 << kHashBits;
constexpr int kMaxChain   = 64;  // ratio/speed tradeoff

inline uint32_t hash3(const uint8_t* p) {
    return ((uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | uint32_t(p[2])) * 2654435761u) >> (32 - kHashBits);
}

std::vector<uint8_t> rawDeflateImpl(const uint8_t* data, size_t size) {
    BitWriter bw;
    bw.bits(1, 1);  // BFINAL: a single block covers the whole stream
    bw.bits(1, 2);  // BTYPE = fixed Huffman

    std::vector<int32_t> head(size_t(kHashSize), -1);
    std::vector<int32_t> prev(size > 0 ? size : 1, -1);

    size_t pos = 0;
    while (pos < size) {
        int bestLength = 0;
        size_t bestDistance = 0;

        if (pos + kMinMatch <= size) {
            uint32_t h = hash3(data + pos);
            int32_t candidate = head[h];
            int chain = kMaxChain;
            size_t limit = pos > size_t(kWindowSize) ? pos - size_t(kWindowSize) : 0;

            while (candidate >= 0 && size_t(candidate) >= limit && chain-- > 0) {
                size_t c = size_t(candidate);
                // Cheap reject before the full compare.
                if (data[c + size_t(bestLength)] == data[pos + size_t(bestLength)]) {
                    size_t maxLen = std::min<size_t>(size_t(kMaxMatch), size - pos);
                    size_t len = 0;
                    while (len < maxLen && data[c + len] == data[pos + len]) ++len;
                    if (int(len) > bestLength) {
                        bestLength = int(len);
                        bestDistance = pos - c;
                        if (bestLength >= kMaxMatch) break;
                    }
                }
                candidate = prev[c];
            }

            prev[pos] = head[h];
            head[h] = int32_t(pos);
        }

        if (bestLength >= kMinMatch) {
            int lenSym = lengthSymbolFor(bestLength);
            writeFixedSymbol(bw, 257 + lenSym);
            bw.bits(uint32_t(bestLength - kLengthBase[lenSym]), kLengthExtra[lenSym]);

            int distSym = distSymbolFor(int(bestDistance));
            bw.code(uint32_t(distSym), 5);  // fixed distance codes are 5 bits
            bw.bits(uint32_t(int(bestDistance) - kDistBase[distSym]), kDistExtra[distSym]);

            // Register hashes for the bytes the match swallowed, so later
            // positions can still find them.
            for (int i = 1; i < bestLength; ++i) {
                size_t p = pos + size_t(i);
                if (p + kMinMatch <= size) {
                    uint32_t h = hash3(data + p);
                    prev[p] = head[h];
                    head[h] = int32_t(p);
                }
            }
            pos += size_t(bestLength);
        } else {
            writeFixedSymbol(bw, int(data[pos]));
            ++pos;
        }
    }

    writeFixedSymbol(bw, 256);  // end of block
    return bw.take();
}

} // namespace

uint32_t crc32Bytes(const uint8_t* bytes, size_t size, uint32_t seed) {
    const auto& table = crcTable();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32Bytes(const uint8_t* bytes, size_t size) {
    uint32_t a = 1, b = 0;
    // 5552 is the largest run that cannot overflow the 32-bit accumulator.
    while (size > 0) {
        size_t block = std::min<size_t>(size, 5552);
        for (size_t i = 0; i < block; ++i) {
            a += bytes[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        bytes += block;
        size -= block;
    }
    return (b << 16) | a;
}

bool zlibInflate(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out, std::string* error) {
    if (size < 6) { setError(error, "zlib: stream too short"); return false; }

    uint8_t cmf = bytes[0], flg = bytes[1];
    if ((cmf & 0x0F) != 8) { setError(error, "zlib: not deflate"); return false; }
    if (((uint32_t(cmf) << 8) | flg) % 31 != 0) { setError(error, "zlib: bad header check"); return false; }
    if (flg & 0x20) { setError(error, "zlib: preset dictionary unsupported"); return false; }

    out.clear();
    if (!rawInflateImpl(bytes + 2, size - 2 - 4, out, error)) return false;

    uint32_t expected = readU32BE(bytes + size - 4);
    if (adler32Bytes(out.data(), out.size()) != expected) {
        setError(error, "zlib: adler32 mismatch");
        return false;
    }
    return true;
}

std::vector<uint8_t> zlibDeflate(const uint8_t* bytes, size_t size) {
    std::vector<uint8_t> out;
    out.push_back(0x78);  // CM = deflate, CINFO = 32K window
    out.push_back(0x01);  // FCHECK chosen so (0x78 << 8 | 0x01) % 31 == 0

    std::vector<uint8_t> compressed = rawDeflateImpl(bytes, size);
    out.insert(out.end(), compressed.begin(), compressed.end());
    appendU32BE(out, adler32Bytes(bytes, size));
    return out;
}


bool rawInflate(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    return rawInflateImpl(bytes, size, out, error);
}

std::vector<uint8_t> rawDeflate(const uint8_t* bytes, size_t size) {
    return rawDeflateImpl(bytes, size);
}

} // namespace blocky
