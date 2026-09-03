// Tests for the video layer: the bit writer, emulation prevention, colour
// conversion, the MP4 container, the integer transform, the nine 4x4 intra
// modes, the deblocking filter and the CAVLC tables.
//
// The container is checked by walking the box tree of a real file this code
// wrote, with a parser that knows the layout from the specification rather
// than from mp4.cpp. The check that matters most is the offset table: `stco`
// is the only thing that says where a frame lives, and an offset that is
// wrong by one byte does not produce a slightly wrong picture -- it produces
// a file that stops decoding there, with nothing in the writing of it having
// complained. So the test follows every offset into the file and demands that
// the bytes at the far end are the sample it claims.
//
// What none of this can check by itself is whether a *decoder* agrees, and
// the reason is worth stating: encoder and decoder halves written here are
// self-consistent, so a round trip returns the right answer even when both
// ends disagree with the specification. Two rescaling bugs hid in exactly
// that blind spot. So the structural properties are checked here -- a VLC
// book must be a prefix code, the transform must invert, the filter must
// reproduce the tabulated thresholds -- and conformance is proven outside,
// against a real decoder, by hand. That comparison needs a decoder
// installed, and these tests are meant to run anywhere.
//
// Needs no game files.
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/video/bitstream.hpp"
#include "engine/video/cavlc.hpp"
#include "engine/video/deblock.hpp"
#include "engine/video/encode.hpp"
#include "engine/video/h264.hpp"
#include "engine/video/inter.hpp"
#include "engine/video/intra4x4.hpp"
#include "engine/video/mp4.hpp"
#include "engine/video/transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

// ---------------------------------------------------------------- bitstream
// Renders whatever a writer produced as a string of ones and zeros, so an
// expected code can be written down the way the specification writes it.
std::string codeOf(const std::function<void(BitWriter&)>& write) {
    BitWriter w;
    write(w);
    const size_t length = w.bitCount();
    w.alignTo(false);

    std::string out;
    for (uint8_t byte : w.data()) {
        for (int i = 7; i >= 0; --i) out += ((byte >> i) & 1) ? '1' : '0';
    }
    out.resize(length);
    return out;
}

void testBitstream() {
    std::printf("bitstream\n");

    check(codeOf([](BitWriter& w) { w.u(3, 5); }) == "101", "fixed width writes most significant first");
    check(codeOf([](BitWriter& w) { w.u(3, 0xFF); }) == "111",
          "a value wider than its field is masked, not allowed to bleed");
    check(codeOf([](BitWriter& w) { w.u(1, 1); w.u(1, 0); w.u(1, 1); }) == "101",
          "single bits pack in order");

    // Exp-Golomb, straight out of the table in the specification.
    check(codeOf([](BitWriter& w) { w.ue(0); }) == "1", "ue(0)");
    check(codeOf([](BitWriter& w) { w.ue(1); }) == "010", "ue(1)");
    check(codeOf([](BitWriter& w) { w.ue(2); }) == "011", "ue(2)");
    check(codeOf([](BitWriter& w) { w.ue(3); }) == "00100", "ue(3)");
    check(codeOf([](BitWriter& w) { w.ue(4); }) == "00101", "ue(4)");
    check(codeOf([](BitWriter& w) { w.ue(7); }) == "0001000", "ue(7)");
    // The macroblock type this encoder falls back to when coding would be
    // larger than the raw samples.
    check(codeOf([](BitWriter& w) { w.ue(25); }) == "000011010", "ue(25), which is I_PCM");

    check(codeOf([](BitWriter& w) { w.se(0); }) == "1", "se(0)");
    check(codeOf([](BitWriter& w) { w.se(1); }) == "010", "se(1)");
    check(codeOf([](BitWriter& w) { w.se(-1); }) == "011", "se(-1)");
    check(codeOf([](BitWriter& w) { w.se(2); }) == "00100", "se(2)");
    check(codeOf([](BitWriter& w) { w.se(-2); }) == "00101", "se(-2)");

    // A code whose length crosses a byte boundary, since that is where a bit
    // writer goes wrong if it is going to.
    check(codeOf([](BitWriter& w) { w.ue(300); }).size() == 17, "a large code is 17 bits wide");
    check(codeOf([](BitWriter& w) { w.u(5, 31); w.ue(300); }).size() == 22,
          "and still 17 bits when it starts mid byte");

    {
        BitWriter w;
        w.u(3, 5);
        check(!w.aligned(), "three bits in is not byte aligned");
        w.trailingBits();
        check(w.aligned() && w.data().size() == 1 && w.data()[0] == 0xB0,
              "trailing bits close the byte with a one and then zeros");
    }
    {
        BitWriter w;
        w.u(8, 0xAB);
        check(w.aligned(), "eight bits in is aligned");
        w.trailingBits();
        check(w.data().size() == 2 && w.data()[1] == 0x80,
              "an already aligned writer still gets a whole trailing byte");
    }
    {
        // bytes() must refuse to smear samples across a partial byte.
        BitWriter w;
        w.u(4, 0xF);
        const uint8_t payload[2] = {0x12, 0x34};
        w.bytes(payload, 2);
        check(w.data().size() == 3 && w.data()[1] == 0x12 && w.data()[2] == 0x34,
              "byte payloads align themselves first and land intact");
    }
}

// ------------------------------------------------------------------- escaping
void testEscaping() {
    std::printf("emulation prevention\n");

    struct Case { std::vector<uint8_t> raw, escaped; const char* what; };
    const Case cases[] = {
        {{0, 0, 0}, {0, 0, 3, 0}, "00 00 00 gains an escape"},
        {{0, 0, 1}, {0, 0, 3, 1}, "00 00 01 would be a start code, so it is escaped"},
        {{0, 0, 2}, {0, 0, 3, 2}, "00 00 02 is escaped too"},
        {{0, 0, 3}, {0, 0, 3, 3}, "so is a real 03, or the decoder could not tell them apart"},
        {{0, 0, 4}, {0, 0, 4}, "00 00 04 is left alone"},
        {{1, 2, 3}, {1, 2, 3}, "ordinary bytes are untouched"},
        {{0, 0, 0, 0}, {0, 0, 3, 0, 0}, "a run of zeros escapes once and starts counting again"},
    };
    for (const Case& c : cases) {
        check(rbspEscape(c.raw) == c.escaped, c.what);
        check(rbspUnescape(rbspEscape(c.raw)) == c.raw, "and the round trip is exact");
    }

    // The property that actually matters: nowhere in an escaped stream may
    // the pattern a decoder scans for appear. Exercised on the worst possible
    // input, which is nearly all zeros.
    std::vector<uint8_t> nasty;
    for (int i = 0; i < 400; ++i) nasty.push_back(uint8_t(i % 7 == 0 ? (i / 7) % 5 : 0));
    const std::vector<uint8_t> escaped = rbspEscape(nasty);

    bool startCode = false;
    for (size_t i = 0; i + 2 < escaped.size(); ++i) {
        if (escaped[i] == 0 && escaped[i + 1] == 0 && escaped[i + 2] <= 1) startCode = true;
    }
    check(!startCode, "an escaped stream contains no start code anywhere");
    check(rbspUnescape(escaped) == nasty, "and still round trips on adversarial input");
}

// --------------------------------------------------------------------- colour
ImageU8 solid(int width, int height, uint8_t r, uint8_t g, uint8_t b) {
    ImageU8 image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) image.set(x, y, {r, g, b, 255});
    }
    return image;
}

bool near(int a, int b, int tolerance = 1) { return a - b <= tolerance && b - a <= tolerance; }

void testColour() {
    std::printf("colour\n");

    {
        const YuvFrame white = rgbToYuv420(solid(16, 16, 255, 255, 255), 16, 16);
        check(white.valid(), "the frame is well formed");
        check(near(white.y[0], 235), "white sits at the top of limited range, not at 255");
        check(near(white.cb[0], 128) && near(white.cr[0], 128), "and is colourless");
    }
    {
        const YuvFrame black = rgbToYuv420(solid(16, 16, 0, 0, 0), 16, 16);
        check(near(black.y[0], 16), "black sits at 16, not at 0");
        check(near(black.cb[0], 128) && near(black.cr[0], 128), "and is colourless too");
    }
    {
        // BT.709 puts pure red at a luma of 0.2126 and its Cr at the extreme.
        const YuvFrame red = rgbToYuv420(solid(16, 16, 255, 0, 0), 16, 16);
        check(near(red.y[0], 63), "red luma follows the BT.709 weights");
        check(near(red.cr[0], 240, 2), "and its Cr runs to the end of the range");
        check(near(red.cb[0], 102, 2), "while Cb falls below the middle");
    }
    {
        // Padding must replicate the edge. Filling with black instead would
        // drag a dark fringe into the samples the crop keeps.
        ImageU8 small = solid(2, 2, 255, 255, 255);
        const YuvFrame padded = rgbToYuv420(small, 16, 16);
        check(padded.width == 16 && padded.height == 16, "the frame is padded to whole macroblocks");
        check(near(padded.y[15], 235) && near(padded.y[15 * 16 + 15], 235),
              "the padding repeats the edge pixel rather than going black");
    }
    {
        const YuvFrame frame = rgbToYuv420(solid(32, 32, 10, 200, 90), 32, 32);
        check(frame.cb.size() == frame.y.size() / 4, "chroma is quarter size, as 4:2:0 requires");
    }
}

// ------------------------------------------------------------------ mp4 boxes
uint32_t beU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Find a box by path, e.g. "moov/trak/mdia/minf/stbl/stco". Returns the range
// of its payload, after the size and tag.
bool findBox(const std::vector<uint8_t>& d, const std::string& path, size_t& begin, size_t& end) {
    size_t scanBegin = 0, scanEnd = d.size();
    size_t at = 0;

    while (at <= path.size()) {
        const size_t slash = path.find('/', at);
        const std::string want = path.substr(at, slash == std::string::npos ? slash : slash - at);

        bool found = false;
        size_t pos = scanBegin;
        while (pos + 8 <= scanEnd) {
            const uint32_t size = beU32(d.data() + pos);
            if (size < 8 || pos + size > scanEnd) return false;
            if (std::memcmp(d.data() + pos + 4, want.data(), 4) == 0) {
                scanBegin = pos + 8;
                scanEnd = pos + size;
                // stsd is a full box that carries a version, flags and an
                // entry count before its children start. Descending into it
                // without stepping over those lands in the middle of a
                // number and finds nothing.
                if (want == "stsd") scanBegin += 8;
                // A visual sample entry has 78 bytes of fixed fields -- size,
                // resolution, compressor name and the rest -- before any
                // child box such as avcC.
                if (want == "avc1") scanBegin += 78;
                found = true;
                break;
            }
            pos += size;
        }
        if (!found) return false;
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    begin = scanBegin;
    end = scanEnd;
    return true;
}

void testContainer() {
    std::printf("mp4 container\n");

    // Three frames, the middle one a repeat of the first so the collapse has
    // something to do, and the last different again.
    const std::string dir = "out/test_video";
    const std::vector<std::string> paths = {dir + "/0000.png", dir + "/0001.png",
                                            dir + "/0002.png", dir + "/0003.png"};
    check(pngSave(paths[0], solid(48, 32, 200, 40, 40), nullptr), "frame 0 written");
    check(pngSave(paths[1], solid(48, 32, 200, 40, 40), nullptr), "frame 1 written");
    check(pngSave(paths[2], solid(48, 32, 40, 180, 90), nullptr), "frame 2 written");
    check(pngSave(paths[3], solid(48, 32, 40, 180, 90), nullptr), "frame 3 written");

    const std::string moviePath = dir + "/clip.mp4";
    Mp4Stats stats;
    std::string error;
    check(encodeMp4(paths, 24, moviePath, 24, &stats, &error), "four frames encode to a movie");
    check(stats.inputFrames == 4, "four frames went in");
    check(stats.samples == 2, "and identical pairs collapsed into two samples");

    std::vector<uint8_t> d;
    check(readFileBytes(moviePath, d, &error), "the movie reads back");

    size_t begin = 0, end = 0;
    check(findBox(d, "ftyp", begin, end), "there is a file type box");
    check(findBox(d, "mdat", begin, end), "and a media data box");
    const size_t mdatBegin = begin, mdatEnd = end;

    check(findBox(d, "moov/mvhd", begin, end), "and a movie header");
    check(findBox(d, "moov/trak/tkhd", begin, end), "and a track header");
    check(findBox(d, "moov/trak/mdia/minf/stbl/stsd", begin, end), "and a sample description");
    check(findBox(d, "moov/trak/mdia/minf/stbl/stsd/avc1", begin, end),
          "which declares an AVC track");
    check(findBox(d, "moov/trak/mdia/minf/stbl/stsd/avc1/avcC", begin, end),
          "carrying the decoder configuration");
    check(end - begin > 8, "and the configuration is not empty");

    // Durations. The collapse must lengthen a sample, not drop time.
    check(findBox(d, "moov/trak/mdia/minf/stbl/stts", begin, end), "there is a time to sample box");
    uint64_t ticks = 0;
    uint32_t samples = 0;
    {
        const uint32_t entries = beU32(d.data() + begin + 4);
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t count = beU32(d.data() + begin + 8 + i * 8);
            const uint32_t delta = beU32(d.data() + begin + 12 + i * 8);
            ticks += uint64_t(count) * delta;
            samples += count;
        }
    }
    check(samples == 2, "two samples are listed");
    // 24 fps, timescale 24000, four frames of 1000 ticks each.
    check(ticks == 4000, "and between them they last the full four frames");

    // Sizes and offsets. This is the part that fails silently if it fails.
    std::vector<uint32_t> sizes, offsets;
    check(findBox(d, "moov/trak/mdia/minf/stbl/stsz", begin, end), "there is a sample size box");
    {
        const uint32_t count = beU32(d.data() + begin + 8);
        for (uint32_t i = 0; i < count; ++i) sizes.push_back(beU32(d.data() + begin + 12 + i * 4));
    }
    check(findBox(d, "moov/trak/mdia/minf/stbl/stco", begin, end), "there is a chunk offset box");
    {
        const uint32_t count = beU32(d.data() + begin + 4);
        for (uint32_t i = 0; i < count; ++i) offsets.push_back(beU32(d.data() + begin + 8 + i * 4));
    }
    check(sizes.size() == 2 && offsets.size() == 2, "both tables have an entry per sample");

    bool offsetsLandRight = true;
    for (size_t i = 0; i < offsets.size() && i < sizes.size(); ++i) {
        if (offsets[i] < mdatBegin || offsets[i] + sizes[i] > mdatEnd) {
            offsetsLandRight = false;
            continue;
        }
        // Every sample begins with the four byte length of the NAL unit that
        // follows, so the bytes at the far end of the offset can be checked
        // against the size the table claims.
        const uint32_t nalLength = beU32(d.data() + offsets[i]);
        if (nalLength + 4 != sizes[i]) offsetsLandRight = false;
        // And that NAL must be a slice: an IDR at nal_ref_idc 3 type 5 for
        // the first sample, a P slice at 2 and 1 for the ones after it.
        const uint8_t expected = i == 0 ? 0x65 : 0x41;
        if (d[offsets[i] + 4] != expected) offsetsLandRight = false;
    }
    check(offsetsLandRight, "every offset lands inside mdat on a sample of the size it claims");

    check(findBox(d, "moov/trak/mdia/minf/stbl/stss", begin, end), "there is a sync sample box");
    // Four frames at 24 fps is a sixth of a second, so the keyframe interval
    // never comes round: the first sample is the only one to seek to, and the
    // table has to say so rather than claiming both.
    check(beU32(d.data() + begin + 4) == 1, "listing the one sample that can be seeked to");
    check(beU32(d.data() + begin + 8) == 1, "which is the first, counting from one");
    check(stats.keyframes == 1, "and the stats agree");

    // Refusals.
    check(!encodeMp4({}, 24, dir + "/none.mp4", 24, nullptr, &error), "no frames is refused");
    check(pngSave(dir + "/odd.png", solid(15, 32, 0, 0, 0), nullptr), "an odd sized frame written");
    check(!encodeMp4({dir + "/odd.png"}, 24, dir + "/odd.mp4", 24, nullptr, &error),
          "4:2:0 cannot represent an odd width, so it is refused rather than fudged");

    H264Encoder encoder;
    check(!encoder.begin(0, 16, 24, 24, &error), "a zero sized frame is refused");
    check(encoder.begin(900, 1200, 24, 24, &error), "a real frame size is accepted");
    check(encoder.codedWidth() == 912 && encoder.codedHeight() == 1200,
          "the coded size rounds up to whole macroblocks");
    check(!encoder.avcc().empty(), "and a decoder configuration is available");
}

// ---------------------------------------------------------------- transform
// The check this module actually needed, and did not have.
//
// Encoder and decoder must reconstruct identically, because intra prediction
// reads reconstructed neighbours -- so an error in the rescaling does not
// show up as a slightly soft picture. The very first macroblock still comes
// out perfect, because it predicts from nothing; every macroblock after it
// predicts from a wrong neighbour and the error compounds across the frame.
// The symptom is a picture whose top left corner is right and which slides
// into nonsense, which reads like a bitstream fault and is not one.
//
// So the test is a round trip on a flat block: quantise a constant residual,
// rescale it, invert it, and demand the constant back. It is exactly the case
// where the arithmetic has nowhere to hide.
void testTransform() {
    std::printf("transform\n");

    for (int qp : {18, 24, 30, 36}) {
        for (int c : {-48, -7, 5, 40}) {
            // ---- the AC path, one 4x4 block on its own
            int32_t block[16];
            for (int i = 0; i < 16; ++i) block[i] = c;
            h264::forward4x4(block);
            check(block[0] == 16 * c, "a flat block transforms to sixteen times its value");
            bool acZero = true;
            for (int i = 1; i < 16; ++i) {
                if (block[i] != 0) acZero = false;
            }
            check(acZero, "and carries no detail");

            int32_t quantised[16], rescaled[16];
            h264::quantize4x4(block, quantised, qp, true);
            h264::dequantize4x4(quantised, rescaled, qp);
            h264::inverse4x4(rescaled);

            const int tolerance = 1 + (1 << (qp / 6)) / 2;
            bool flat = true;
            for (int i = 0; i < 16; ++i) {
                if (std::abs(rescaled[i] - c) > tolerance) flat = false;
            }
            check(flat, "a flat residual survives the round trip");

            // ---- the I_16x16 DC path, sixteen blocks through the Hadamard
            int32_t dc[16];
            for (int i = 0; i < 16; ++i) dc[i] = 16 * c;
            h264::hadamard4x4(dc);

            int32_t dcQuant[16];
            for (int i = 0; i < 16; ++i) dcQuant[i] = h264::quantizeDcLuma(dc[i], qp, true);

            h264::hadamard4x4(dcQuant);
            h264::dequantizeDcLuma(dcQuant, qp);

            int32_t rebuilt[16] = {0};
            rebuilt[0] = dcQuant[0];
            h264::inverse4x4(rebuilt);
            check(std::abs(rebuilt[0] - c) <= tolerance,
                  "a flat macroblock survives the luma DC round trip");

            // ---- and the chroma DC path, which has its own scaling
            const int qpc = h264::chromaQp(qp);
            int32_t cdc[4];
            for (int i = 0; i < 4; ++i) cdc[i] = 16 * c;
            h264::hadamard2x2(cdc);
            for (int i = 0; i < 4; ++i) cdc[i] = h264::quantizeDcChroma(cdc[i], qpc, true);
            h264::hadamard2x2(cdc);
            h264::dequantizeDcChroma(cdc, qpc);

            int32_t chromaRebuilt[16] = {0};
            chromaRebuilt[0] = cdc[0];
            h264::inverse4x4(chromaRebuilt);
            check(std::abs(chromaRebuilt[0] - c) <= tolerance + 2,
                  "a flat macroblock survives the chroma DC round trip");
        }
    }

    // Detail has to survive too, or the round trip above could be passed by
    // something that only handles constants.
    for (int qp : {18, 24}) {
        int32_t block[16], original[16];
        for (int i = 0; i < 16; ++i) {
            original[i] = block[i] = ((i * 37) % 41) - 20;
        }
        h264::forward4x4(block);
        int32_t q[16], d[16];
        h264::quantize4x4(block, q, qp, true);
        h264::dequantize4x4(q, d, qp);
        h264::inverse4x4(d);

        int worst = 0;
        for (int i = 0; i < 16; ++i) worst = std::max(worst, std::abs(d[i] - original[i]));
        check(worst <= 2 + (1 << (qp / 6)), "a detailed block survives within the step size");
    }
}

// ------------------------------------------------------------- intra 4x4
// Every one of the nine modes is a weighted average of neighbour samples with
// non-negative weights that sum to one. Two consequences follow, and both are
// worth testing because both catch the mistake this code is most prone to --
// an index that runs off the end of an edge.
//
//   Constant in, constant out. If every neighbour holds the same value, every
//   mode must produce exactly that value. A read past the end of an array
//   picks up something else and the constant breaks.
//
//   No mode may invent a sample outside the range of the ones it read.
//
// Neither says anything about a mode predicting in the *right direction*, so
// the directional modes also get their geometry checked directly.
void testIntra4x4() {
    std::printf("intra 4x4\n");

    h264::Neighbours4x4 n;
    n.haveAbove = n.haveLeft = n.haveAboveRight = true;

    for (int value : {7, 63, 128, 200, 254}) {
        for (int i = 0; i < 8; ++i) n.above[i] = uint8_t(value);
        for (int i = 0; i < 4; ++i) n.left[i] = uint8_t(value);
        n.corner = uint8_t(value);

        bool flat = true;
        for (int mode = 0; mode < h264::kIntra4ModeCount; ++mode) {
            uint8_t pred[16];
            if (!h264::predict4x4(mode, n, pred)) { flat = false; continue; }
            for (int i = 0; i < 16; ++i) {
                if (pred[i] != uint8_t(value)) flat = false;
            }
        }
        check(flat, "every mode returns a constant neighbourhood unchanged");
    }

    // Distinct values everywhere, so an out of range read is likely to show.
    for (int i = 0; i < 8; ++i) n.above[i] = uint8_t(40 + i * 11);
    for (int i = 0; i < 4; ++i) n.left[i] = uint8_t(150 + i * 7);
    n.corner = 90;

    int lo = 255, hi = 0;
    for (int i = 0; i < 8; ++i) { lo = std::min(lo, int(n.above[i])); hi = std::max(hi, int(n.above[i])); }
    for (int i = 0; i < 4; ++i) { lo = std::min(lo, int(n.left[i])); hi = std::max(hi, int(n.left[i])); }
    lo = std::min(lo, int(n.corner)); hi = std::max(hi, int(n.corner));

    bool inRange = true;
    for (int mode = 0; mode < h264::kIntra4ModeCount; ++mode) {
        uint8_t pred[16];
        if (!h264::predict4x4(mode, n, pred)) continue;
        for (int i = 0; i < 16; ++i) {
            if (pred[i] < lo || pred[i] > hi) inRange = false;
        }
    }
    check(inRange, "no mode predicts a sample outside the range of what it read");

    // The two modes whose geometry is exact rather than filtered.
    {
        uint8_t pred[16];
        check(h264::predict4x4(h264::kIntra4Vertical, n, pred), "vertical is available");
        bool ok = true;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                if (pred[y * 4 + x] != n.above[x]) ok = false;
            }
        }
        check(ok, "vertical copies the row above into every row");

        check(h264::predict4x4(h264::kIntra4Horizontal, n, pred), "horizontal is available");
        ok = true;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                if (pred[y * 4 + x] != n.left[y]) ok = false;
            }
        }
        check(ok, "horizontal copies the column left into every column");
    }

    // Availability, which is what stops a mode reading samples that are not
    // there at the edges of a picture.
    {
        h264::Neighbours4x4 bare;   // nothing available at all
        check(h264::intra4x4Available(h264::kIntra4Dc, bare),
              "DC works with no neighbours, which is why it is the fallback");
        check(!h264::intra4x4Available(h264::kIntra4Vertical, bare), "vertical needs the row above");
        check(!h264::intra4x4Available(h264::kIntra4Horizontal, bare), "horizontal needs the column left");
        check(!h264::intra4x4Available(h264::kIntra4DiagonalDownRight, bare),
              "the corner modes need both edges");

        uint8_t pred[16];
        check(h264::predict4x4(h264::kIntra4Dc, bare, pred), "DC still predicts");
        bool grey = true;
        for (int i = 0; i < 16; ++i) {
            if (pred[i] != 128) grey = false;
        }
        check(grey, "and with nothing to go on it predicts mid grey");

        h264::Neighbours4x4 aboveOnly;
        aboveOnly.haveAbove = true;
        for (int i = 0; i < 8; ++i) aboveOnly.above[i] = 100;
        check(h264::intra4x4Available(h264::kIntra4Vertical, aboveOnly), "vertical needs only above");
        check(!h264::intra4x4Available(h264::kIntra4Horizontal, aboveOnly),
              "horizontal is still unavailable");
    }

    // The above-right substitution.
    {
        h264::Neighbours4x4 m;
        m.haveAbove = true;
        m.haveAboveRight = false;
        for (int i = 0; i < 4; ++i) m.above[i] = uint8_t(10 + i);
        for (int i = 4; i < 8; ++i) m.above[i] = 200;   // whatever was there
        m.fillAboveRight();
        bool repeated = true;
        for (int i = 4; i < 8; ++i) {
            if (m.above[i] != m.above[3]) repeated = false;
        }
        check(repeated, "an absent above-right block is replaced by the last real sample");
    }
}

// ------------------------------------------------------------------ deblock
// The thresholds cannot be checked for correctness here -- only a decoder can
// say whether they match the standard. What they can be checked for is the
// shape a threshold table must have, which is what catches a transcription
// error: both rise with the index, and the clipping limit rises with the
// boundary strength, because a stronger boundary is allowed to move samples
// further.
//
// The conformance check is a file: this also writes the encoder's own
// deblocked reconstruction next to the stream that produced it, so the two
// can be compared against a real decoder outside the test.
void testDeblock() {
    std::printf("deblocking\n");

    bool alphaRises = true, betaRises = true;
    for (int i = 0; i < 51; ++i) {
        if (h264::deblockAlpha(i) > h264::deblockAlpha(i + 1)) alphaRises = false;
        if (h264::deblockBeta(i) > h264::deblockBeta(i + 1)) betaRises = false;
    }
    check(alphaRises, "the alpha threshold rises with the quantiser");
    check(betaRises, "so does beta");

    bool off = true;
    for (int i = 0; i < 16; ++i) {
        if (h264::deblockAlpha(i) != 0 || h264::deblockBeta(i) != 0) off = false;
    }
    check(off, "and below index sixteen the filter is off, there being nothing to fix");

    bool tcRises = true, tcWithStrength = true;
    for (int bS = 1; bS <= 3; ++bS) {
        for (int i = 0; i < 51; ++i) {
            if (h264::deblockTc0(bS, i) > h264::deblockTc0(bS, i + 1)) tcRises = false;
        }
    }
    for (int i = 0; i <= 51; ++i) {
        if (!(h264::deblockTc0(1, i) <= h264::deblockTc0(2, i) &&
              h264::deblockTc0(2, i) <= h264::deblockTc0(3, i))) {
            tcWithStrength = false;
        }
    }
    check(tcRises, "the clipping limit rises with the quantiser");
    check(tcWithStrength, "and with the boundary strength");
    check(h264::deblockTc0(0, 40) == 0 && h264::deblockTc0(4, 40) == 0,
          "a strength outside one to three has no limit of its own");

    // ---- an artefact-prone picture, coded coarsely enough that the filter
    // has work to do, kept for comparison against a decoder.
    ImageU8 art(64, 48);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            const bool block = ((x / 7) + (y / 5)) % 2 == 0;
            const int shade = block ? 30 + (x * 3) % 90 : 200 - (y * 4) % 80;
            art.set(x, y, {uint8_t(shade), uint8_t(255 - shade), uint8_t((shade * 2) % 256), 255});
        }
    }

    std::string error;
    H264Encoder encoder;
    check(encoder.begin(64, 48, 24, 32, &error), "the encoder starts at a coarse quantiser");

    const std::vector<uint8_t> sample = encoder.encode(art);
    check(!sample.empty(), "and produces a frame");

    const YuvFrame& recon = encoder.filteredPicture();
    check(recon.valid(), "with a deblocked reconstruction beside it");

    std::vector<uint8_t> raw;
    raw.insert(raw.end(), recon.y.begin(), recon.y.end());
    raw.insert(raw.end(), recon.cb.begin(), recon.cb.end());
    raw.insert(raw.end(), recon.cr.begin(), recon.cr.end());
    check(writeFileBytes("out/test_video/recon.yuv", raw.data(), raw.size(), &error),
          "the reconstruction is written for an outside decoder to be compared against");

    Mp4Writer writer;
    check(writer.begin(64, 48, 24000, encoder.avcc(), &error), "and muxed");
    writer.addSample(sample, 1000);
    check(writer.save("out/test_video/filter.mp4", &error), "into a file next to it");
}

// --------------------------------------------------------------------- inter
// A frame with something moving across a background that does not. The block
// pattern is there so motion compensation has features to lock on to -- a
// smooth gradient would match everywhere and prove nothing.
ImageU8 movingFrame(int width, int height, int shiftX, int shiftY) {
    ImageU8 image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t r = checker ? 40 : 70;
            uint8_t g = checker ? 60 : 90;
            uint8_t b = checker ? 110 : 140;

            // The moving square, with a texture of its own so that a wrong
            // vector costs something visible rather than nothing.
            const int sx = x - shiftX - width / 4;
            const int sy = y - shiftY - height / 4;
            if (sx >= 0 && sx < 24 && sy >= 0 && sy < 24) {
                r = uint8_t(200 - (sx * 5) % 120);
                g = uint8_t(30 + (sy * 7) % 100);
                b = uint8_t(90 + ((sx + sy) * 3) % 140);
            }
            image.set(x, y, {r, g, b, 255});
        }
    }
    return image;
}

// Content that genuinely moves by fractions of a sample. The checkerboard
// above shifts by whole pixels, which a search is free to answer with a whole
// vector -- so it exercises everything about inter prediction except the
// filter. This is smooth and band-limited on purpose: a quarter-sample shift
// of it is a picture the six-tap can actually reach, so an encoder that has
// the filter will use it and one that does not will pay.
ImageU8 smoothFrame(int width, int height, float shiftX, float shiftY) {
    ImageU8 image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = float(x) - shiftX;
            const float v = float(y) - shiftY;
            const float a = std::sin(u * 0.21f) * std::cos(v * 0.17f);
            const float b = std::sin((u + v) * 0.11f);
            image.set(x, y,
                      {uint8_t(std::lround(128.0f + 70.0f * a)),
                       uint8_t(std::lround(120.0f + 60.0f * b)),
                       uint8_t(std::lround(140.0f + 50.0f * a * b)), 255});
        }
    }
    return image;
}

void testInter() {
    std::printf("inter prediction\n");

    // ---- motion compensation, on a plane small enough to check by hand
    {
        const int w = 8, h = 8;
        std::vector<uint8_t> plane(size_t(w) * size_t(h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                plane[size_t(y) * size_t(w) + size_t(x)] = uint8_t(x * 10 + y);
            }
        }

        h264::RefPicture ref;
        ref.y = plane.data();
        ref.cb = plane.data();
        ref.cr = plane.data();
        ref.width = w;
        ref.height = h;

        uint8_t got[16];
        h264::predictLuma(ref, 2, 2, 0, 0, 4, got);
        bool copy = true;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                if (got[y * 4 + x] != plane[size_t(2 + y) * size_t(w) + size_t(2 + x)]) {
                    copy = false;
                }
            }
        }
        check(copy, "a zero vector copies the reference exactly");

        // Four quarters is one whole sample, and the whole-sample path is a
        // copy with no filter in it at all.
        h264::predictLuma(ref, 2, 2, 4, 0, 4, got);
        check(got[0] == plane[size_t(2) * size_t(w) + 3], "and four quarters is one sample over");

        h264::predictLuma(ref, 0, 0, -4, 0, 4, got);
        check(got[0] == plane[0] && got[1] == plane[0],
              "a vector off the left edge repeats the edge sample");

        h264::predictLuma(ref, 0, 0, -400, -400, 4, got);
        bool allCorner = true;
        for (int i = 0; i < 16; ++i) {
            if (got[i] != plane[0]) allCorner = false;
        }
        check(allCorner, "and far outside the picture is the corner, not a read out of bounds");
    }

    // ---- the six-tap, on fields whose answers can be written down
    //
    // A round trip cannot check an interpolator: ours would agree with itself
    // whatever the taps were. These two fields can be checked against
    // arithmetic instead, and between them they pin the filter down.
    {
        const int w = 16, h = 16;
        std::vector<uint8_t> field(size_t(w) * size_t(h));

        h264::RefPicture ref;
        ref.y = field.data();
        ref.cb = field.data();
        ref.cr = field.data();
        ref.width = w;
        ref.height = h;

        // A flat field. The taps sum to 32 and the rounding divides by 32, so
        // a flat field has to come back flat at every one of the sixteen
        // positions -- and a quarter, being the average of two of those, with
        // it. This is what catches a mistyped tap: the symptom otherwise is a
        // picture that quietly gains or loses brightness wherever motion is
        // fractional, compounding down the sequence.
        std::fill(field.begin(), field.end(), uint8_t(137));

        bool flatEverywhere = true;
        for (int fy = 0; fy < 4; ++fy) {
            for (int fx = 0; fx < 4; ++fx) {
                uint8_t got[16];
                h264::predictLuma(ref, 5, 5, 4 + fx, 4 + fy, 4, got);
                for (int i = 0; i < 16; ++i) {
                    if (got[i] != 137) flatEverywhere = false;
                }
            }
        }
        check(flatEverywhere, "every fractional position leaves a flat field flat");

        // A tilted plane, where the filter is exact. The six-tap reproduces a
        // straight line -- that is what taps of (1, -5, 20, 20, -5, 1) over 32
        // are for -- so every position has an answer that follows from the
        // plane rather than from the code: the value at that fraction of the
        // way along, rounded half upwards.
        //
        // The two gradients differ on purpose. With one gradient a filter
        // that confused its axes would still pass.
        //
        // The block sits where its whole filter support does too. A six-tap
        // centred on the last sample of the block reaches three past it, and
        // the centre position reaches three past that again; a support that
        // runs off the edge gets clamped, which is correct behaviour and not
        // a plane any more.
        auto planar = [](int x, int y) { return 10 + 6 * x + 4 * y; };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                field[size_t(y) * size_t(w) + size_t(x)] = uint8_t(planar(x, y));
            }
        }

        bool everyPositionExact = true;
        std::string firstWrong;
        for (int fy = 0; fy < 4; ++fy) {
            for (int fx = 0; fx < 4; ++fx) {
                uint8_t got[16];
                h264::predictLuma(ref, 5, 5, fx, fy, 4, got);

                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        // Twice the true value, so the half-upward rounding
                        // can be written without leaving the integers.
                        const int twice = 2 * planar(5 + x, 5 + y) + 3 * fx + 2 * fy;
                        const int want = (twice + 1) >> 1;
                        if (int(got[y * 4 + x]) != want && firstWrong.empty()) {
                            everyPositionExact = false;
                            firstWrong = "at (" + std::to_string(fx) + "," + std::to_string(fy) +
                                         ") wanted " + std::to_string(want) + ", got " +
                                         std::to_string(int(got[y * 4 + x]));
                        }
                    }
                }
            }
        }
        if (!firstWrong.empty()) std::printf("    %s\n", firstWrong.c_str());
        check(everyPositionExact, "and every position on a plane lands on the plane");
    }

    {
        const int w = 8, h = 8;
        std::vector<uint8_t> plane(size_t(w) * size_t(h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                plane[size_t(y) * size_t(w) + size_t(x)] = uint8_t(x * 10 + y);
            }
        }

        // Chroma vectors are in eighths of a chroma sample, so four eighths
        // is the midpoint between two samples and the answer is their
        // average. This is the case a whole sample luma vector lands in
        // whenever it is odd, which is half the time.
        uint8_t chroma[16];
        h264::predictChroma(plane.data(), w, h, 2, 2, 0, 0, 4, chroma);
        check(chroma[0] == plane[size_t(2) * size_t(w) + 2], "a zero chroma vector copies too");

        const int a = plane[size_t(2) * size_t(w) + 2];
        const int b = plane[size_t(2) * size_t(w) + 3];
        const int c = plane[size_t(2) * size_t(w) + 1];

        h264::predictChroma(plane.data(), w, h, 2, 2, 4, 0, 4, chroma);
        check(chroma[0] == uint8_t((a + b + 1) / 2),
              "a half sample chroma vector averages the two samples it falls between");

        h264::predictChroma(plane.data(), w, h, 2, 2, -4, 0, 4, chroma);
        check(chroma[0] == uint8_t((c + a + 1) / 2),
              "and a negative one floors to its left, rather than rounding towards zero");
    }

    // ---- a real sequence: one IDR, a run of P pictures, one forced IDR
    std::vector<ImageU8> sources;
    std::vector<char> forceKey;

    sources.push_back(movingFrame(96, 64, 0, 0));      // the IDR
    forceKey.push_back(0);
    sources.push_back(movingFrame(96, 64, 0, 0));      // the same picture again
    forceKey.push_back(0);
    // Long enough to take frame_num past sixteen, where its four bits wrap.
    // Wrapping is legal and expected -- a decoder counts modulo the same
    // number -- but it is exactly the kind of thing that works for the first
    // fifteen frames of every test that is too short to reach it.
    for (int t = 1; t <= 18; ++t) {
        sources.push_back(movingFrame(96, 64, (t * 3) % 40, (t * 2) % 24));
        forceKey.push_back(0);
    }
    sources.push_back(movingFrame(96, 64, 15, 10));
    forceKey.push_back(1);

    std::string error;
    H264Encoder encoder;
    check(encoder.begin(96, 64, 24, 26, &error), "the encoder starts");

    Mp4Writer writer;
    check(writer.begin(96, 64, 24000, encoder.avcc(), &error), "and a writer beside it");

    std::vector<size_t> bytes;
    std::vector<char> keyframe;
    std::vector<uint8_t> nalByte;
    std::vector<uint8_t> raw;

    bool everyFrameEncoded = true;
    for (size_t i = 0; i < sources.size(); ++i) {
        const std::vector<uint8_t> sample = encoder.encode(sources[i], forceKey[i] != 0);
        if (sample.size() < 5) {
            everyFrameEncoded = false;
            break;
        }
        bytes.push_back(sample.size());
        keyframe.push_back(encoder.lastFrameWasKeyframe() ? 1 : 0);
        nalByte.push_back(sample[4]);
        writer.addSample(sample, 1000, encoder.lastFrameWasKeyframe());

        const YuvFrame& recon = encoder.filteredPicture();
        raw.insert(raw.end(), recon.y.begin(), recon.y.end());
        raw.insert(raw.end(), recon.cb.begin(), recon.cb.end());
        raw.insert(raw.end(), recon.cr.begin(), recon.cr.end());
    }
    check(everyFrameEncoded, "every frame in the sequence encodes");
    if (!everyFrameEncoded) return;

    check(keyframe.front() == 1, "the first frame is a keyframe, having nothing before it");
    check(keyframe[1] == 0 && keyframe[17] == 0, "the frames after it are not");
    check(keyframe.back() == 1, "and the one that was asked for is");

    // The first byte of each NAL says which kind it is: an IDR slice is
    // nal_ref_idc 3 with type 5, a P slice is 2 with type 1.
    check(nalByte.front() == 0x65, "the first sample is an IDR slice");
    check(nalByte[1] == 0x41, "the second is a P slice");
    check(nalByte.back() == 0x65, "and the forced one is an IDR again");

    // An unchanged picture should cost almost nothing: every macroblock finds
    // itself where it was and skips.
    check(bytes[1] * 20 < bytes[0], "an unchanged frame costs a fraction of coding it again");

    bool movingFramesAreSmaller = true;
    for (size_t i = 2; i + 1 < bytes.size(); ++i) {
        if (bytes[i] >= bytes[0]) movingFramesAreSmaller = false;
    }
    check(movingFramesAreSmaller, "and a moving one still beats coding the picture again");
    check(bytes.back() > bytes[bytes.size() - 2],
          "while a keyframe costs what a fresh picture costs");

    // ---- kept for an outside decoder to disagree with
    //
    // This is the only check here that can prove any of the above conforms.
    // Our reconstruction and our bitstream are two halves of one belief, and
    // a round trip through both returns whatever that belief is. Only a
    // decoder somebody else wrote can say whether it is the format -- and
    // with P pictures the stakes went up, because an encoder that
    // reconstructs differently from the decoder no longer merely produces a
    // wrong macroblock: it predicts the next frame from the wrong picture,
    // and the error compounds down the sequence.
    check(writer.save("out/test_video/inter.mp4", &error), "the sequence muxes into a file");
    check(writeFileBytes("out/test_video/inter.yuv", raw.data(), raw.size(), &error),
          "with every reconstructed picture beside it");

    // ---- a sequence that moves by fractions of a sample
    //
    // The one above is the harder test of everything except the filter, and
    // no test of the filter at all: whole-pixel content gets whole vectors,
    // and an encoder with no interpolation in it would pass every check so
    // far and match every decoder byte for byte. So here is content that
    // moves by three quarters of a sample a frame.
    {
        H264Encoder sub;
        check(sub.begin(96, 64, 24, 26, &error), "a second encoder starts for sub-sample motion");

        Mp4Writer subWriter;
        check(subWriter.begin(96, 64, 24000, sub.avcc(), &error), "and a writer beside it");

        std::vector<uint8_t> subRaw;
        int fractional = 0, inter = 0;
        bool everyFrameEncoded2 = true;

        for (int t = 0; t < 12; ++t) {
            const ImageU8 source = smoothFrame(96, 64, float(t) * 0.75f, float(t) * 0.5f);
            const std::vector<uint8_t> sample = sub.encode(source, false);
            if (sample.size() < 5) {
                everyFrameEncoded2 = false;
                break;
            }
            subWriter.addSample(sample, 1000, sub.lastFrameWasKeyframe());
            fractional += sub.lastFrameFractionalVectors();
            inter += sub.lastFrameInterMacroblocks();

            const YuvFrame& recon = sub.filteredPicture();
            subRaw.insert(subRaw.end(), recon.y.begin(), recon.y.end());
            subRaw.insert(subRaw.end(), recon.cb.begin(), recon.cb.end());
            subRaw.insert(subRaw.end(), recon.cr.begin(), recon.cr.end());
        }
        check(everyFrameEncoded2, "every frame of the sub-sample sequence encodes");

        // Not "most" and not a fixed number: the point is only that the
        // filter ran, so that agreement with a decoder below means something
        // about it. What fraction chose to be fractional is a property of the
        // search, and the search is allowed to change.
        std::printf("    %d of %d inter macroblocks used a fractional vector\n", fractional,
                    inter);
        check(inter > 0, "the sub-sample sequence predicts from the previous picture");
        check(fractional > 0, "and reaches between the samples to do it");

        check(subWriter.save("out/test_video/subpel.mp4", &error),
              "the sub-sample sequence muxes into a file");
        check(writeFileBytes("out/test_video/subpel.yuv", subRaw.data(), subRaw.size(), &error),
              "with every reconstructed picture beside it");
    }
}

// ---------------------------------------------------------------- vlc tables
// A variable length code book has to be a prefix code: no entry may be the
// start of another, or a decoder reading left to right cannot tell where one
// symbol ends. That property is what makes the tables checkable without a
// decoder -- almost any mistyped length or value destroys it, and a table
// transcribed wrong is otherwise a bug that shows up as a stream which
// decodes correctly right until it does not.
bool isPrefixCode(const std::vector<h264::VlcCode>& book, std::string& clash) {
    for (size_t i = 0; i < book.size(); ++i) {
        for (size_t j = i + 1; j < book.size(); ++j) {
            const h264::VlcCode& a = book[i].length <= book[j].length ? book[i] : book[j];
            const h264::VlcCode& b = book[i].length <= book[j].length ? book[j] : book[i];
            if (a.length == 0 || b.length == 0) continue;
            if (uint16_t(b.value >> (b.length - a.length)) == a.value) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "len %d value %d is a prefix of len %d value %d",
                              a.length, a.value, b.length, b.value);
                clash = buf;
                return false;
            }
        }
    }
    return true;
}

void testVlcTables() {
    std::printf("cavlc tables\n");

    std::string clash;

    for (int table = 0; table < h264::coeffTokenTableCount(); ++table) {
        std::vector<h264::VlcCode> book;
        const int limit = table == 3 ? 4 : 16;
        for (int t1 = 0; t1 <= 3; ++t1) {
            for (int tc = 0; tc <= limit; ++tc) {
                h264::VlcCode code{};
                if (h264::coeffTokenTable(table, t1, tc, code)) book.push_back(code);
            }
        }
        char what[64];
        std::snprintf(what, sizeof(what), "coeff_token book %d is a prefix code", table);
        const bool ok = isPrefixCode(book, clash);
        if (!ok) std::printf("        %s\n", clash.c_str());
        check(ok, what);

        // Every combination that can occur must be present. A missing entry
        // is a hole the encoder would fall through in silence.
        bool complete = true;
        for (int tc = 0; tc <= limit; ++tc) {
            for (int t1 = 0; t1 <= 3 && t1 <= tc; ++t1) {
                h264::VlcCode code{};
                if (!h264::coeffTokenTable(table, t1, tc, code)) complete = false;
            }
        }
        std::snprintf(what, sizeof(what), "coeff_token book %d has every legal entry", table);
        check(complete, what);
    }

    for (int pass = 0; pass < 2; ++pass) {
        // Each of the two mappings must be a bijection, or some patterns
        // would be unwritable and some codeNums would decode two ways.
        const bool intra = pass == 0;
        bool seen[48] = {false};
        bool bijection = true;
        for (int cbp = 0; cbp < 48; ++cbp) {
            const int codeNum = h264::codedBlockPatternCodeNum(cbp, intra);
            if (codeNum < 0 || codeNum > 47 || seen[codeNum]) bijection = false;
            else seen[codeNum] = true;
        }
        check(bijection, intra ? "intra coded_block_pattern maps 0..47 onto 0..47 one for one"
                               : "and so does the inter mapping");
        check(h264::codedBlockPatternCodeNum(-1, intra) < 0 &&
                  h264::codedBlockPatternCodeNum(48, intra) < 0,
              "and both refuse a pattern that cannot exist");
    }

    {
        // The two columns are not the same table, and the cheapest codeword
        // in each says what its side of the format expects: an intra
        // macroblock nearly always has residual everywhere, an inter one
        // nearly always has none.
        check(h264::codedBlockPatternCodeNum(47, true) == 0,
              "intra spends codeNum 0 on every block coded");
        check(h264::codedBlockPatternCodeNum(0, false) == 0,
              "and inter spends it on nothing coded");

        int differences = 0;
        for (int cbp = 0; cbp < 48; ++cbp) {
            if (h264::codedBlockPatternCodeNum(cbp, true) !=
                h264::codedBlockPatternCodeNum(cbp, false)) {
                ++differences;
            }
        }
        check(differences >= 40, "and the two disagree almost everywhere");
    }

    for (int tc = 1; tc <= 15; ++tc) {
        std::vector<h264::VlcCode> book;
        for (int z = 0; z <= 16 - tc; ++z) {
            h264::VlcCode code{};
            if (h264::totalZerosTable(false, tc, z, code)) book.push_back(code);
        }
        check(int(book.size()) == 17 - tc, "total_zeros has an entry per possible count");
        const bool ok = isPrefixCode(book, clash);
        if (!ok) std::printf("        total_zeros %d: %s\n", tc, clash.c_str());
        check(ok, "total_zeros book is a prefix code");
    }

    for (int tc = 1; tc <= 3; ++tc) {
        std::vector<h264::VlcCode> book;
        for (int z = 0; z <= 4 - tc; ++z) {
            h264::VlcCode code{};
            if (h264::totalZerosTable(true, tc, z, code)) book.push_back(code);
        }
        check(int(book.size()) == 5 - tc, "chroma total_zeros is complete");
        check(isPrefixCode(book, clash), "chroma total_zeros is a prefix code");
    }

    for (int left = 1; left <= 7; ++left) {
        std::vector<h264::VlcCode> book;
        const int maxRun = left > 6 ? 14 : left;
        for (int run = 0; run <= maxRun; ++run) {
            h264::VlcCode code{};
            if (h264::runBeforeTable(left, run, code)) book.push_back(code);
        }
        check(int(book.size()) == maxRun + 1, "run_before has an entry per possible run");
        const bool ok = isPrefixCode(book, clash);
        if (!ok) std::printf("        run_before %d: %s\n", left, clash.c_str());
        check(ok, "run_before book is a prefix code");
    }
}

} // namespace

int main() {
    testBitstream();
    testEscaping();
    testColour();
    testTransform();
    testIntra4x4();
    testDeblock();
    testInter();
    testVlcTables();
    testContainer();

    if (gFailures > 0) {
        std::printf("\n%d video test(s) failed\n", gFailures);
        return 1;
    }
    std::printf("\nall video tests passed\n");
    return 0;
}
