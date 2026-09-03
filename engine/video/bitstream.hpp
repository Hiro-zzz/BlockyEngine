#pragma once
// Bit level writing for H.264 syntax, and the one piece of byte level
// weirdness the format needs on top of it.
//
// H.264 syntax elements are not byte aligned and are not fixed width. Most of
// them are Exp-Golomb codes, which spend one bit per magnitude doubling: a
// zero costs one bit, and a number near a thousand costs nineteen. Writing
// them by hand with shifts is how a codec acquires bugs that only show up on
// one macroblock in ten thousand, so all of it goes through here.
//
// ------------------------------------------------------- emulation prevention
//
// The other half of this file exists because of how H.264 streams are cut
// apart. A decoder finds the boundary between NAL units by scanning for the
// byte pattern 00 00 01, which means that pattern must never occur inside
// one -- and the payload is arbitrary binary, so it certainly would.
//
// The format's answer is to escape it: while writing, any 00 00 followed by
// a byte of 03 or less has an 03 inserted before that byte. The decoder
// strips those back out. Everything written here is therefore in two states,
// and keeping them straight is the whole discipline of this file:
//
//   RBSP  the raw syntax, what the encoder builds
//   EBSP  the escaped bytes, what actually goes in the file
//
// Escaping is a separate function applied once at the end rather than a mode
// on the writer, because a bit writer that escapes as it goes cannot answer
// "how many bits have I written" -- and the alignment rules need that answer.
#include <cstdint>
#include <vector>

namespace blocky {

class BitWriter {
public:
    // Unsigned value, `bits` wide, most significant bit first. u(v) in the
    // specification.
    void u(int bits, uint32_t value);

    void flag(bool value) { u(1, value ? 1u : 0u); }

    // Unsigned Exp-Golomb. ue(v) in the specification: value+1 written in
    // binary, preceded by one fewer zero than that number has bits.
    void ue(uint32_t value);

    // Signed Exp-Golomb. se(v): the values zigzag 0, 1, -1, 2, -2 ... onto
    // the unsigned code, so small magnitudes stay cheap either way.
    void se(int32_t value);

    // Whole bytes. The writer must already be byte aligned -- I_PCM sample
    // data is defined to start on a boundary, and this is where that is
    // enforced rather than assumed.
    void bytes(const uint8_t* data, size_t size);

    // Everything another writer holds, appended bit for bit. Neither side
    // needs to be aligned.
    //
    // This is what lets a macroblock be coded into a scratch writer, measured,
    // and then either kept or thrown away for a cheaper coding of the same
    // block -- a decision that cannot be made before the bits exist.
    void append(const BitWriter& other);

    bool aligned() const { return partialBits_ == 0; }

    // Pad with `value` until the next byte boundary. Does nothing when
    // already aligned, which is what every alignment rule in the spec wants.
    void alignTo(bool value = false);

    // rbsp_trailing_bits: a one, then zeros to the byte boundary. Every NAL
    // ends with this, and it is how a decoder finds where the syntax stopped
    // as opposed to where the padding started.
    void trailingBits();

    size_t bitCount() const { return bytes_.size() * 8 + size_t(partialBits_); }

    const std::vector<uint8_t>& data() const { return bytes_; }

    void clear() {
        bytes_.clear();
        partial_ = 0;
        partialBits_ = 0;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t partial_ = 0;
    int     partialBits_ = 0;
};

// Insert emulation prevention bytes. Any 00 00 followed by 00, 01, 02 or 03
// gains an 03 before that byte.
std::vector<uint8_t> rbspEscape(const std::vector<uint8_t>& rbsp);

// The inverse, for tests: strip the 03 that follows any 00 00. A decoder does
// this before parsing, and a round trip through both is the cheapest proof
// that the escaper did not corrupt anything.
std::vector<uint8_t> rbspUnescape(const std::vector<uint8_t>& ebsp);

} // namespace blocky
