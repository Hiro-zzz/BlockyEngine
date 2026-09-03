#include "engine/video/deblock.hpp"

#include <algorithm>
#include <cstdlib>

namespace blocky {
namespace h264 {
namespace {

// Transcribed from Tables 8-14 and 8-15 and checked before use: both
// thresholds must rise with the index, and tc0 must rise with the boundary
// strength. Below index 16 the filter is off entirely -- at a fine enough
// quantiser there is no blocking to remove and every edge is real.
const uint8_t kAlpha[52] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,  0,
    4,   4,   5,   6,   7,   8,   9,  10,  12,  13,  15,  17,  20, 22, 25, 28,
    32,  36,  40,  45,  50,  56,  63,  71,  80,  90, 101, 113, 127, 144, 162, 182,
    203, 226, 255, 255,
};

const uint8_t kBeta[52] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  6,  6,  7,  7,  8,  8,
    9,  9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18,
};

const uint8_t kTc0[3][52] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
     1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8,
     9, 10, 11, 13},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
     1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 8, 10, 11,
     12, 13, 15, 17},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
     1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16,
     18, 20, 23, 25},
};

inline int clamp51(int v) { return v < 0 ? 0 : (v > 51 ? 51 : v); }
inline uint8_t clip8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

// One line of eight samples across an edge. `q` points at q0 and `step` is the
// distance between neighbouring samples *across* the edge, so the same code
// filters a vertical edge horizontally and a horizontal edge vertically.
//
// Every output is computed from the unmodified inputs before anything is
// written back: the filter is defined on the samples as they arrive, and
// writing p0 before reading it for q0 would quietly change the answer.
void filterLine(uint8_t* q, int step, int bS, int alpha, int beta, int tc0, bool chroma) {
    const int p0 = q[-step], p1 = q[-2 * step], p2 = q[-3 * step], p3 = q[-4 * step];
    const int q0 = q[0], q1 = q[step], q2 = q[2 * step], q3 = q[3 * step];

    // The decision: a step small enough to be quantisation error, with both
    // sides flat enough to be a flat area rather than texture.
    if (std::abs(p0 - q0) >= alpha) return;
    if (std::abs(p1 - p0) >= beta) return;
    if (std::abs(q1 - q0) >= beta) return;

    const int ap = std::abs(p2 - p0);
    const int aq = std::abs(q2 - q0);

    if (bS < 4) {
        const int tc = chroma ? tc0 + 1 : tc0 + (ap < beta ? 1 : 0) + (aq < beta ? 1 : 0);
        const int delta = clip3(-tc, tc, ((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3));

        q[-step] = clip8(p0 + delta);
        q[0] = clip8(q0 - delta);

        if (!chroma && ap < beta) {
            q[-2 * step] =
                clip8(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
        }
        if (!chroma && aq < beta) {
            q[step] = clip8(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
        }
        return;
    }

    // Strength four only happens on a macroblock edge next to an intra
    // macroblock, where the step is most likely to be an artefact of the
    // prediction restarting rather than anything in the picture. It reaches
    // three samples deep instead of two.
    if (chroma) {
        q[-step] = clip8((2 * p1 + p0 + q1 + 2) >> 2);
        q[0] = clip8((2 * q1 + q0 + p1 + 2) >> 2);
        return;
    }

    const bool wide = std::abs(p0 - q0) < ((alpha >> 2) + 2);

    if (ap < beta && wide) {
        q[-step] = clip8((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        q[-2 * step] = clip8((p2 + p1 + p0 + q0 + 2) >> 2);
        q[-3 * step] = clip8((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
    } else {
        q[-step] = clip8((2 * p1 + p0 + q1 + 2) >> 2);
    }

    if (aq < beta && wide) {
        q[0] = clip8((q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3);
        q[step] = clip8((q2 + q1 + q0 + p0 + 2) >> 2);
        q[2 * step] = clip8((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
    } else {
        q[0] = clip8((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

} // namespace

int deblockAlpha(int indexA) { return kAlpha[clamp51(indexA)]; }
int deblockBeta(int indexB) { return kBeta[clamp51(indexB)]; }
int deblockTc0(int bS, int indexA) {
    if (bS < 1 || bS > 3) return 0;
    return kTc0[bS - 1][clamp51(indexA)];
}

void deblock(uint8_t* y, uint8_t* cb, uint8_t* cr, int width, int height,
             const DeblockPicture& picture) {
    // The picture's height is implied by the macroblock grid; the parameter
    // is here so a caller cannot pass planes and dimensions that disagree
    // without it being visible at the call site.
    (void)height;

    const int chromaWidth = width / 2;
    const int mbWidth = picture.mbWidth;
    const int mbHeight = picture.mbHeight;
    const int blocksWide = mbWidth * 4;

    auto mbIndex = [&](int mbx, int mby) { return size_t(mby) * size_t(mbWidth) + size_t(mbx); };

    auto isPcm = [&](int mbx, int mby) {
        const size_t i = mbIndex(mbx, mby);
        return i < picture.pcm.size() && picture.pcm[i] != 0;
    };
    // An empty intra list means an I picture: everything in it is intra.
    auto isIntra = [&](int mbx, int mby) {
        const size_t i = mbIndex(mbx, mby);
        return picture.intra.empty() || (i < picture.intra.size() && picture.intra[i] != 0);
    };
    auto qpOf = [&](int mbx, int mby) { return isPcm(mbx, mby) ? 0 : picture.qp; };
    auto qpcOf = [&](int mbx, int mby) { return isPcm(mbx, mby) ? 0 : picture.qpChroma; };

    auto coded = [&](int bx, int by) {
        const size_t i = size_t(by) * size_t(blocksWide) + size_t(bx);
        return i < picture.nonZero.size() && picture.nonZero[i] != 0;
    };
    auto motion = [&](int mbx, int mby, int& mx, int& my) {
        const size_t i = mbIndex(mbx, mby);
        mx = i < picture.mvx.size() ? int(picture.mvx[i]) : 0;
        my = i < picture.mvy.size() ? int(picture.mvy[i]) : 0;
    };

    // The strength for one 4x4 block boundary, given the blocks on either
    // side in global 4x4 coordinates.
    auto strength = [&](int pbx, int pby, int qbx, int qby, bool mbEdge) {
        const int pMbx = pbx / 4, pMby = pby / 4;
        const int qMbx = qbx / 4, qMby = qby / 4;

        if (isIntra(pMbx, pMby) || isIntra(qMbx, qMby)) return mbEdge ? 4 : 3;
        if (coded(pbx, pby) || coded(qbx, qby)) return 2;

        // One reference picture, so "different reference" cannot happen and
        // only the vectors are left. A whole sample apart is the threshold,
        // which the specification writes as four quarter-samples because that
        // is the unit vectors are stored in.
        int pmx, pmy, qmx, qmy;
        motion(pMbx, pMby, pmx, pmy);
        motion(qMbx, qMby, qmx, qmy);
        if (std::abs(pmx - qmx) >= 4 || std::abs(pmy - qmy) >= 4) return 1;
        return 0;
    };

    // Macroblock by macroblock, and inside each one every vertical edge before
    // any horizontal one. The order is not a detail: each edge is filtered
    // using the samples the previous edges left behind.
    for (int mby = 0; mby < mbHeight; ++mby) {
        for (int mbx = 0; mbx < mbWidth; ++mbx) {
            const int px = mbx * 16, py = mby * 16;
            const int cx = mbx * 8, cy = mby * 8;

            // ---- vertical edges, filtered across horizontally
            for (int edge = 0; edge < 16; edge += 4) {
                if (edge == 0 && mbx == 0) continue;

                const bool mbEdge = edge == 0;
                const int qbx = mbx * 4 + edge / 4;
                const int pbx = qbx - 1;

                const int qpA = mbEdge ? qpOf(mbx - 1, mby) : qpOf(mbx, mby);
                const int index = clamp51(((qpA + qpOf(mbx, mby) + 1) >> 1));
                const int alpha = kAlpha[index], beta = kBeta[index];

                const int qpcA = mbEdge ? qpcOf(mbx - 1, mby) : qpcOf(mbx, mby);
                const int cIndex = clamp51(((qpcA + qpcOf(mbx, mby) + 1) >> 1));
                const int cAlpha = kAlpha[cIndex], cBeta = kBeta[cIndex];

                // Strength is decided per four lines, not per edge: inside a
                // P picture two 4x4 blocks either side of the same edge can
                // differ in whether they have coefficients.
                for (int group = 0; group < 4; ++group) {
                    const int by = mby * 4 + group;
                    const int bS = strength(pbx, by, qbx, by, mbEdge);
                    if (bS == 0 || alpha == 0) continue;

                    const int tc0 = bS < 4 ? kTc0[bS - 1][index] : 0;
                    for (int i = 0; i < 4; ++i) {
                        filterLine(y + size_t(py + group * 4 + i) * size_t(width) +
                                       size_t(px + edge),
                                   1, bS, alpha, beta, tc0, false);
                    }
                }

                // Chroma has half the samples, so only the edges at 0 and 8
                // in luma have a chroma counterpart. A chroma row covers two
                // luma rows, so it takes the strength of the luma block those
                // fall in.
                if ((edge == 0 || edge == 8) && cAlpha != 0) {
                    const int ce = edge / 2;
                    for (int i = 0; i < 8; ++i) {
                        const int by = mby * 4 + i / 2;
                        const int bS = strength(pbx, by, qbx, by, mbEdge);
                        if (bS == 0) continue;

                        const int cTc0 = bS < 4 ? kTc0[bS - 1][cIndex] : 0;
                        const size_t at = size_t(cy + i) * size_t(chromaWidth) + size_t(cx + ce);
                        filterLine(cb + at, 1, bS, cAlpha, cBeta, cTc0, true);
                        filterLine(cr + at, 1, bS, cAlpha, cBeta, cTc0, true);
                    }
                }
            }

            // ---- horizontal edges, filtered across vertically
            for (int edge = 0; edge < 16; edge += 4) {
                if (edge == 0 && mby == 0) continue;

                const bool mbEdge = edge == 0;
                const int qby = mby * 4 + edge / 4;
                const int pby = qby - 1;

                const int qpA = mbEdge ? qpOf(mbx, mby - 1) : qpOf(mbx, mby);
                const int index = clamp51(((qpA + qpOf(mbx, mby) + 1) >> 1));
                const int alpha = kAlpha[index], beta = kBeta[index];

                const int qpcA = mbEdge ? qpcOf(mbx, mby - 1) : qpcOf(mbx, mby);
                const int cIndex = clamp51(((qpcA + qpcOf(mbx, mby) + 1) >> 1));
                const int cAlpha = kAlpha[cIndex], cBeta = kBeta[cIndex];

                for (int group = 0; group < 4; ++group) {
                    const int bx = mbx * 4 + group;
                    const int bS = strength(bx, pby, bx, qby, mbEdge);
                    if (bS == 0 || alpha == 0) continue;

                    const int tc0 = bS < 4 ? kTc0[bS - 1][index] : 0;
                    for (int i = 0; i < 4; ++i) {
                        filterLine(y + size_t(py + edge) * size_t(width) +
                                       size_t(px + group * 4 + i),
                                   width, bS, alpha, beta, tc0, false);
                    }
                }

                if ((edge == 0 || edge == 8) && cAlpha != 0) {
                    const int ce = edge / 2;
                    for (int i = 0; i < 8; ++i) {
                        const int bx = mbx * 4 + i / 2;
                        const int bS = strength(bx, pby, bx, qby, mbEdge);
                        if (bS == 0) continue;

                        const int cTc0 = bS < 4 ? kTc0[bS - 1][cIndex] : 0;
                        const size_t at = size_t(cy + ce) * size_t(chromaWidth) + size_t(cx + i);
                        filterLine(cb + at, chromaWidth, bS, cAlpha, cBeta, cTc0, true);
                        filterLine(cr + at, chromaWidth, bS, cAlpha, cBeta, cTc0, true);
                    }
                }
            }
        }
    }
}

} // namespace h264
} // namespace blocky
