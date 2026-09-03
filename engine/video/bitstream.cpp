#include "engine/video/bitstream.hpp"

namespace blocky {

void BitWriter::u(int bits, uint32_t value) {
    if (bits <= 0) return;
    if (bits > 32) bits = 32;

    // Drop anything above the requested width rather than letting it bleed
    // into the next field, which is the kind of mistake that produces a
    // stream that decodes for a while and then does not.
    if (bits < 32) value &= (uint32_t(1) << bits) - 1u;

    for (int i = bits - 1; i >= 0; --i) {
        partial_ = uint8_t((partial_ << 1) | ((value >> i) & 1u));
        if (++partialBits_ == 8) {
            bytes_.push_back(partial_);
            partial_ = 0;
            partialBits_ = 0;
        }
    }
}

void BitWriter::ue(uint32_t value) {
    // codeNum + 1, written in binary, with leading zeros to match its length.
    // The +1 is what lets zero be represented at all.
    const uint64_t code = uint64_t(value) + 1u;

    int width = 0;
    while ((code >> width) > 1u) ++width;   // floor(log2(code))

    u(width, 0);                            // the leading zeros
    u(width + 1, uint32_t(code));           // the value itself, top bit set
}

void BitWriter::se(int32_t value) {
    // 0 -> 0, 1 -> 1, -1 -> 2, 2 -> 3, -2 -> 4 ...
    const uint32_t mapped = value > 0 ? uint32_t(value) * 2u - 1u : uint32_t(-value) * 2u;
    ue(mapped);
}

void BitWriter::bytes(const uint8_t* data, size_t size) {
    if (!aligned()) {
        // Writing bytes into a half filled byte would silently shift every
        // sample by a few bits. Align first; callers that care have already
        // done so deliberately.
        alignTo(false);
    }
    bytes_.insert(bytes_.end(), data, data + size);
}

void BitWriter::append(const BitWriter& other) {
    if (&other == this) return;
    for (uint8_t byte : other.bytes_) u(8, byte);
    if (other.partialBits_ > 0) u(other.partialBits_, other.partial_);
}

void BitWriter::alignTo(bool value) {
    while (partialBits_ != 0) u(1, value ? 1u : 0u);
}

void BitWriter::trailingBits() {
    u(1, 1);
    alignTo(false);
}

std::vector<uint8_t> rbspEscape(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    out.reserve(rbsp.size() + rbsp.size() / 64 + 8);

    int zeros = 0;
    for (uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

std::vector<uint8_t> rbspUnescape(const std::vector<uint8_t>& ebsp) {
    std::vector<uint8_t> out;
    out.reserve(ebsp.size());

    int zeros = 0;
    for (size_t i = 0; i < ebsp.size(); ++i) {
        const uint8_t byte = ebsp[i];
        if (zeros >= 2 && byte == 0x03) {
            // An escape byte only counts as one when what follows could have
            // been mistaken for a start code. At the very end of a unit there
            // is nothing following, and a trailing 03 is a real byte.
            if (i + 1 < ebsp.size() && ebsp[i + 1] <= 3) {
                zeros = 0;
                continue;
            }
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

} // namespace blocky
