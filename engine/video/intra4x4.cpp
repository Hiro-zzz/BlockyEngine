#include "engine/video/intra4x4.hpp"

namespace blocky {
namespace h264 {
namespace {

// Every directional mode is built from one of two filters over the neighbour
// samples: a two tap average along the edge, or a three tap smoothing across
// it. Naming them keeps the mode bodies readable as the geometry they are
// rather than as arithmetic.
inline uint8_t avg2(int a, int b) { return uint8_t((a + b + 1) >> 1); }
inline uint8_t avg3(int a, int b, int c) { return uint8_t((a + 2 * b + c + 2) >> 2); }

} // namespace

bool intra4x4Available(int mode, const Neighbours4x4& n) {
    switch (mode) {
        case kIntra4Dc:
            return true;   // falls back to 128 with no neighbours at all
        case kIntra4Vertical:
        case kIntra4DiagonalDownLeft:
        case kIntra4VerticalLeft:
            return n.haveAbove;
        case kIntra4Horizontal:
        case kIntra4HorizontalUp:
            return n.haveLeft;
        case kIntra4DiagonalDownRight:
        case kIntra4VerticalRight:
        case kIntra4HorizontalDown:
            return n.haveCorner();
        default:
            return false;
    }
}

bool predict4x4(int mode, const Neighbours4x4& n, uint8_t pred[16]) {
    if (!intra4x4Available(mode, n)) return false;

    const uint8_t* a = n.above;   // p[0..7,-1]
    const uint8_t* l = n.left;    // p[-1,0..3]
    const uint8_t  c = n.corner;  // p[-1,-1]

    switch (mode) {
        case kIntra4Vertical:
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) pred[y * 4 + x] = a[x];
            return true;

        case kIntra4Horizontal:
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) pred[y * 4 + x] = l[y];
            return true;

        case kIntra4Dc: {
            int value;
            if (n.haveAbove && n.haveLeft) {
                value = (a[0] + a[1] + a[2] + a[3] + l[0] + l[1] + l[2] + l[3] + 4) >> 3;
            } else if (n.haveAbove) {
                value = (a[0] + a[1] + a[2] + a[3] + 2) >> 2;
            } else if (n.haveLeft) {
                value = (l[0] + l[1] + l[2] + l[3] + 2) >> 2;
            } else {
                value = 128;
            }
            for (int i = 0; i < 16; ++i) pred[i] = uint8_t(value);
            return true;
        }

        case kIntra4DiagonalDownLeft:
            // A 45 degree ramp running down and to the left, which is why it
            // is the mode that needs the samples past the corner.
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    pred[y * 4 + x] = (x == 3 && y == 3)
                                          ? avg3(a[6], a[7], a[7])
                                          : avg3(a[x + y], a[x + y + 1], a[x + y + 2]);
                }
            }
            return true;

        case kIntra4DiagonalDownRight:
            // The other diagonal, which turns the corner: above the diagonal
            // it reads the top edge, below it the left edge, and on it the
            // corner sample itself.
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    if (x > y) {
                        pred[y * 4 + x] = avg3(x - y - 2 >= 0 ? a[x - y - 2] : c,
                                               a[x - y - 1], a[x - y]);
                    } else if (x < y) {
                        pred[y * 4 + x] = avg3(y - x - 2 >= 0 ? l[y - x - 2] : c,
                                               l[y - x - 1], l[y - x]);
                    } else {
                        pred[y * 4 + x] = avg3(a[0], c, l[0]);
                    }
                }
            }
            return true;

        case kIntra4VerticalRight:
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int z = 2 * x - y;
                    const int i = x - (y >> 1);
                    if (z >= 0 && (z & 1) == 0) {
                        pred[y * 4 + x] = avg2(i - 1 >= 0 ? a[i - 1] : c, a[i]);
                    } else if (z >= 0) {
                        pred[y * 4 + x] = avg3(i - 2 >= 0 ? a[i - 2] : c,
                                               i - 1 >= 0 ? a[i - 1] : c, a[i]);
                    } else if (z == -1) {
                        pred[y * 4 + x] = avg3(l[0], c, a[0]);
                    } else {
                        // Only (x,y) = (0,2) lands here with y-3 below zero,
                        // and the sample it wants is the corner.
                        pred[y * 4 + x] = avg3(l[y - 1], l[y - 2], y >= 3 ? l[y - 3] : c);
                    }
                }
            }
            return true;

        case kIntra4HorizontalDown:
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int z = 2 * y - x;
                    const int i = y - (x >> 1);
                    if (z >= 0 && (z & 1) == 0) {
                        pred[y * 4 + x] = avg2(i - 1 >= 0 ? l[i - 1] : c, l[i]);
                    } else if (z >= 0) {
                        pred[y * 4 + x] = avg3(i - 2 >= 0 ? l[i - 2] : c,
                                               i - 1 >= 0 ? l[i - 1] : c, l[i]);
                    } else if (z == -1) {
                        pred[y * 4 + x] = avg3(a[0], c, l[0]);
                    } else {
                        // Mirror of the case above: (2,0) reaches past the
                        // start of the top edge and wants the corner.
                        pred[y * 4 + x] = avg3(a[x - 1], a[x - 2], x >= 3 ? a[x - 3] : c);
                    }
                }
            }
            return true;

        case kIntra4VerticalLeft:
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int i = x + (y >> 1);
                    pred[y * 4 + x] = ((y & 1) == 0) ? avg2(a[i], a[i + 1])
                                                     : avg3(a[i], a[i + 1], a[i + 2]);
                }
            }
            return true;

        default: {
            // Horizontal up: a ramp along the left edge that runs off the
            // bottom, where it flattens into the last sample rather than
            // reading past it.
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const int z = x + 2 * y;
                    const int i = y + (x >> 1);
                    if (z < 5 && (z & 1) == 0)      pred[y * 4 + x] = avg2(l[i], l[i + 1]);
                    else if (z < 5)                 pred[y * 4 + x] = avg3(l[i], l[i + 1], l[i + 2]);
                    else if (z == 5)                pred[y * 4 + x] = avg3(l[2], l[3], l[3]);
                    else                            pred[y * 4 + x] = l[3];
                }
            }
            return true;
        }
    }
}

} // namespace h264
} // namespace blocky
