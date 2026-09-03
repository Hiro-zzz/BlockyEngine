#include "engine/video/inter.hpp"

#include <cstdlib>

namespace blocky {
namespace h264 {
namespace {

inline int clampTo(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }
inline int clip1(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// The six-tap of 8.4.2.2.1, unrounded and unclipped. Every half-sample
// position in the format is this and nothing else; the taps sum to 32, which
// is why a flat area survives it unchanged and a straight ramp lands exactly
// on its own midpoint.
inline int tap6(int a, int b, int c, int d, int e, int f) {
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}

// A half sample, rounded and clipped back to eight bits. The centre position
// does *not* go through this on its way in -- see the header.
inline int half(int intermediate) { return clip1((intermediate + 16) >> 5); }

// The fifteen fractional positions of figure 8-4, laid out as the document
// draws them so the switch below can be read against the page:
//
//     G  a  b  c   H
//     d  e  f  g
//     h  i  j  k   m
//     n  p  q  r
//     M            N
//
// G, H, M are whole samples; b, h, m, s are half samples along one axis; j is
// the centre. Every other position is the rounded average of two of those.
enum {
    kPosA = 1,  kPosB = 2,  kPosC = 3,
    kPosD = 4,  kPosE = 5,  kPosF = 6,  kPosG = 7,
    kPosH = 8,  kPosI = 9,  kPosJ = 10, kPosK = 11,
    kPosN = 12, kPosP = 13, kPosQ = 14, kPosR = 15,
};

} // namespace

void predictLuma(const RefPicture& ref, int x0, int y0, int mvx, int mvy, int size,
                 uint8_t* out) {
    // The integer part floors and the fraction is what is left over, always
    // positive. An arithmetic shift does this for negatives; a division would
    // round towards zero and displace the prediction by a sample on one side
    // of zero only, which reads as a codec that cannot track leftward motion.
    const int xInt = x0 + (mvx >> 2);
    const int yInt = y0 + (mvy >> 2);
    const int xFrac = mvx & 3;
    const int yFrac = mvy & 3;

    const int maxX = ref.width - 1;
    const int maxY = ref.height - 1;

    auto at = [&](int x, int y) {
        return int(ref.y[size_t(clampTo(y, maxY)) * size_t(ref.width) +
                         size_t(clampTo(x, maxX))]);
    };

    // A whole-sample vector needs no filter at all: the prediction is a copy.
    if (xFrac == 0 && yFrac == 0) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) out[y * size + x] = uint8_t(at(xInt + x, yInt + y));
        }
        return;
    }

    // Intermediates, kept unrounded because the centre position is a six-tap
    // over six of them.
    //
    // `vert` holds the vertical six-tap across the columns the block spans
    // plus two either side, which is how far the centre position reaches.
    // `horz` holds the horizontal one and carries one row past the block,
    // because position s sits a row below.
    constexpr int kMax = kMaxPredictionSize;
    int vert[kMax][kMax + 5];
    int horz[kMax + 1][kMax];

    for (int y = 0; y < size; ++y) {
        const int sy = yInt + y;
        for (int c = 0; c < size + 5; ++c) {
            const int sx = xInt - 2 + c;
            vert[y][c] = tap6(at(sx, sy - 2), at(sx, sy - 1), at(sx, sy), at(sx, sy + 1),
                              at(sx, sy + 2), at(sx, sy + 3));
        }
    }
    for (int y = 0; y <= size; ++y) {
        const int sy = yInt + y;
        for (int x = 0; x < size; ++x) {
            const int sx = xInt + x;
            horz[y][x] = tap6(at(sx - 2, sy), at(sx - 1, sy), at(sx, sy), at(sx + 1, sy),
                              at(sx + 2, sy), at(sx + 3, sy));
        }
    }

    const int position = yFrac * 4 + xFrac;
    const bool needsCentre = xFrac == 2 && yFrac == 2;
    const bool leansOnCentre = position == kPosF || position == kPosI ||
                               position == kPosK || position == kPosQ;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Column x of the block is column x+2 of `vert`, which begins two
            // to the left. So the half sample under this one is at x+2, and
            // the one under its right neighbour -- position m -- is at x+3.
            const int b = half(horz[y][x]);
            const int s = half(horz[y + 1][x]);
            const int h = half(vert[y][x + 2]);
            const int m = half(vert[y][x + 3]);

            int j = 0;
            if (needsCentre || leansOnCentre) {
                j = clip1((tap6(vert[y][x], vert[y][x + 1], vert[y][x + 2], vert[y][x + 3],
                                vert[y][x + 4], vert[y][x + 5]) +
                           512) >>
                          10);
            }

            int value = 0;
            switch (position) {
                case kPosA: value = (at(xInt + x, yInt + y) + b + 1) >> 1; break;
                case kPosB: value = b; break;
                case kPosC: value = (at(xInt + x + 1, yInt + y) + b + 1) >> 1; break;
                case kPosD: value = (at(xInt + x, yInt + y) + h + 1) >> 1; break;
                case kPosE: value = (b + h + 1) >> 1; break;
                case kPosF: value = (b + j + 1) >> 1; break;
                case kPosG: value = (b + m + 1) >> 1; break;
                case kPosH: value = h; break;
                case kPosI: value = (h + j + 1) >> 1; break;
                case kPosJ: value = j; break;
                case kPosK: value = (m + j + 1) >> 1; break;
                case kPosN: value = (at(xInt + x, yInt + y + 1) + h + 1) >> 1; break;
                case kPosP: value = (h + s + 1) >> 1; break;
                case kPosQ: value = (j + s + 1) >> 1; break;
                default:    value = (m + s + 1) >> 1; break;   // r
            }
            out[y * size + x] = uint8_t(value);
        }
    }
}

void predictChroma(const uint8_t* plane, int width, int height, int x0, int y0, int mvcx,
                   int mvcy, int size, uint8_t* out) {
    // The same floor-and-remainder rule as luma, in eighths rather than
    // quarters. Getting the pair wrong shifts prediction by a sample on one
    // side of zero only.
    const int xInt = x0 + (mvcx >> 3);
    const int yInt = y0 + (mvcy >> 3);
    const int xFrac = mvcx & 7;
    const int yFrac = mvcy & 7;

    const int maxX = width - 1;
    const int maxY = height - 1;

    const int w00 = (8 - xFrac) * (8 - yFrac);
    const int w10 = xFrac * (8 - yFrac);
    const int w01 = (8 - xFrac) * yFrac;
    const int w11 = xFrac * yFrac;

    for (int y = 0; y < size; ++y) {
        const uint8_t* row0 = plane + size_t(clampTo(yInt + y, maxY)) * size_t(width);
        const uint8_t* row1 = plane + size_t(clampTo(yInt + y + 1, maxY)) * size_t(width);
        for (int x = 0; x < size; ++x) {
            const int xa = clampTo(xInt + x, maxX);
            const int xb = clampTo(xInt + x + 1, maxX);
            out[y * size + x] = uint8_t(
                (w00 * row0[xa] + w10 * row0[xb] + w01 * row1[xa] + w11 * row1[xb] + 32) >> 6);
        }
    }
}

int blockSad(const uint8_t* pred, const uint8_t* src, int srcStride, int x0, int y0, int size) {
    int total = 0;
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = src + size_t(y0 + y) * size_t(srcStride) + size_t(x0);
        for (int x = 0; x < size; ++x) total += std::abs(int(pred[y * size + x]) - int(row[x]));
    }
    return total;
}

long long blockSsd(const uint8_t* a, const uint8_t* b, int stride, int x0, int y0, int size) {
    long long total = 0;
    for (int y = 0; y < size; ++y) {
        const uint8_t* row = b + size_t(y0 + y) * size_t(stride) + size_t(x0);
        for (int x = 0; x < size; ++x) {
            const long long d = int(a[y * size + x]) - int(row[x]);
            total += d * d;
        }
    }
    return total;
}

} // namespace h264
} // namespace blocky
