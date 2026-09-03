#include "engine/video/transform.hpp"

#include <cstdlib>

namespace blocky {
namespace h264 {
namespace {

// Multiplication factors and rescaling factors, indexed by QP modulo six and
// by which of three position classes a coefficient sits in. The whole design
// of H.264 quantisation is that QP splits into a remainder that picks a table
// row and a quotient that becomes a shift, so a step of six in QP is exactly
// a doubling of the step size.
const int kMF[6][3] = {
    {13107, 5243, 8066},
    {11916, 4660, 7490},
    {10082, 4194, 6554},
    { 9362, 3647, 5825},
    { 8192, 3355, 5243},
    { 7282, 2893, 4559},
};

const int kV[6][3] = {
    {10, 16, 13},
    {11, 18, 14},
    {13, 20, 16},
    {14, 23, 18},
    {16, 25, 20},
    {18, 29, 23},
};

// Which of the three classes each raster position belongs to. The corners of
// the even/even lattice share one factor, the odd/odd centres another, and
// everything else a third -- an artefact of the transform's own scaling.
const int kClass[16] = {
    0, 2, 0, 2,
    2, 1, 2, 1,
    0, 2, 0, 2,
    2, 1, 2, 1,
};

const int kChromaQpTable[52] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 29, 30,
    31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38,
    39, 39, 39, 39,
};

} // namespace

const int kZigZag4x4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

int chromaQp(int lumaQp) {
    if (lumaQp < 0) lumaQp = 0;
    if (lumaQp > 51) lumaQp = 51;
    return kChromaQpTable[lumaQp];
}

void forward4x4(int32_t block[16]) {
    // Rows, then columns. The multiplies by two are shifts; there is no
    // multiplication anywhere in the transform itself.
    for (int i = 0; i < 4; ++i) {
        int32_t* d = block + i * 4;
        const int32_t a0 = d[0] + d[3];
        const int32_t a1 = d[1] + d[2];
        const int32_t a2 = d[1] - d[2];
        const int32_t a3 = d[0] - d[3];

        d[0] = a0 + a1;
        d[1] = 2 * a3 + a2;
        d[2] = a0 - a1;
        d[3] = a3 - 2 * a2;
    }
    for (int i = 0; i < 4; ++i) {
        const int32_t a0 = block[i] + block[12 + i];
        const int32_t a1 = block[4 + i] + block[8 + i];
        const int32_t a2 = block[4 + i] - block[8 + i];
        const int32_t a3 = block[i] - block[12 + i];

        block[i]      = a0 + a1;
        block[4 + i]  = 2 * a3 + a2;
        block[8 + i]  = a0 - a1;
        block[12 + i] = a3 - 2 * a2;
    }
}

void inverse4x4(int32_t block[16]) {
    // Specified exactly, down to the shifts. This has to match every decoder
    // in the world, and the encoder relies on it matching.
    for (int i = 0; i < 4; ++i) {
        int32_t* d = block + i * 4;
        const int32_t e0 = d[0] + d[2];
        const int32_t e1 = d[0] - d[2];
        const int32_t e2 = (d[1] >> 1) - d[3];
        const int32_t e3 = d[1] + (d[3] >> 1);

        d[0] = e0 + e3;
        d[1] = e1 + e2;
        d[2] = e1 - e2;
        d[3] = e0 - e3;
    }
    for (int i = 0; i < 4; ++i) {
        const int32_t e0 = block[i] + block[8 + i];
        const int32_t e1 = block[i] - block[8 + i];
        const int32_t e2 = (block[4 + i] >> 1) - block[12 + i];
        const int32_t e3 = block[4 + i] + (block[12 + i] >> 1);

        block[i]      = (e0 + e3 + 32) >> 6;
        block[4 + i]  = (e1 + e2 + 32) >> 6;
        block[8 + i]  = (e1 - e2 + 32) >> 6;
        block[12 + i] = (e0 - e3 + 32) >> 6;
    }
}

void hadamard4x4(int32_t block[16]) {
    // Its own inverse up to a scale factor, which is why the same routine
    // serves both directions and the scaling is folded into quantisation.
    for (int i = 0; i < 4; ++i) {
        int32_t* d = block + i * 4;
        const int32_t a0 = d[0] + d[3];
        const int32_t a1 = d[1] + d[2];
        const int32_t a2 = d[1] - d[2];
        const int32_t a3 = d[0] - d[3];

        d[0] = a0 + a1;
        d[1] = a3 + a2;
        d[2] = a0 - a1;
        d[3] = a3 - a2;
    }
    for (int i = 0; i < 4; ++i) {
        const int32_t a0 = block[i] + block[12 + i];
        const int32_t a1 = block[4 + i] + block[8 + i];
        const int32_t a2 = block[4 + i] - block[8 + i];
        const int32_t a3 = block[i] - block[12 + i];

        block[i]      = a0 + a1;
        block[4 + i]  = a3 + a2;
        block[8 + i]  = a0 - a1;
        block[12 + i] = a3 - a2;
    }
}

void hadamard2x2(int32_t block[4]) {
    const int32_t a = block[0], b = block[1], c = block[2], d = block[3];
    block[0] = a + b + c + d;
    block[1] = a - b + c - d;
    block[2] = a + b - c - d;
    block[3] = a - b - c + d;
}

void quantize4x4(const int32_t in[16], int32_t out[16], int qp, bool intra) {
    const int qbits = 15 + qp / 6;
    const int row = qp % 6;
    // Intra rounds at a third of the step, inter at a sixth. Intra is more
    // generous because there is no previous frame to absorb the error.
    const int32_t offset = int32_t((int64_t(1) << qbits) / (intra ? 3 : 6));

    for (int i = 0; i < 16; ++i) {
        const int32_t value = in[i];
        const int32_t magnitude = value < 0 ? -value : value;
        const int64_t scaled = (int64_t(magnitude) * kMF[row][kClass[i]] + offset) >> qbits;
        out[i] = value < 0 ? -int32_t(scaled) : int32_t(scaled);
    }
}

// The rescaling factors below are the bare normAdjust values, without the
// factor of sixteen that the specification's LevelScale folds in from a flat
// weighting matrix. That choice fixes every shift in this file, and getting
// it wrong is not subtle: the reconstruction comes out sixteen times too
// small, the first macroblock still decodes correctly because it predicts
// from nothing, and everything after it drifts.
//
// The scaling that makes the round trip exact, checked against a constant
// residual: forward gives 16c for a flat 4x4, quantisation divides by
// 2^(15+QP/6), so rescaling has to multiply by V << (QP/6) for the inverse
// transform's own division by 64 to land back on c.
void dequantize4x4(const int32_t in[16], int32_t out[16], int qp) {
    const int shift = qp / 6;
    const int row = qp % 6;

    for (int i = 0; i < 16; ++i) {
        out[i] = (in[i] * kV[row][kClass[i]]) << shift;
    }
}

int32_t quantizeDcLuma(int32_t value, int qp, bool intra) {
    // Two bits coarser than the AC path, not one. One of them is the extra
    // factor the 4x4 Hadamard leaves behind; the other is the halving that
    // the forward DC transform is defined to include and that a plain
    // Hadamard does not do.
    //
    // Leaving that second bit out costs exactly a factor of two, and the
    // shape of the failure is worth remembering: the picture decodes, the
    // geometry is perfect, and every flat area comes back with twice the
    // contrast it should have around the prediction.
    const int qbits = 15 + qp / 6 + 2;
    const int32_t offset = int32_t((int64_t(1) << qbits) / (intra ? 3 : 6));

    const int32_t magnitude = value < 0 ? -value : value;
    const int64_t scaled = (int64_t(magnitude) * kMF[qp % 6][0] + offset) >> qbits;
    return value < 0 ? -int32_t(scaled) : int32_t(scaled);
}

void dequantizeDcLuma(int32_t block[16], int qp) {
    // The 4x4 Hadamard multiplies a flat block by sixteen and quantisation
    // spends one extra bit on that, so the DC path ends up eight times
    // stronger than the AC one and gives that eight back here.
    // The specification writes this as (f * 16V) >> (6 - QP/6), which with a
    // bare V is a left shift by QP/6 and a right shift by two.
    const int shift = qp / 6;
    const int32_t factor = kV[qp % 6][0];

    for (int i = 0; i < 16; ++i) {
        const int32_t scaled = block[i] * factor;
        if (shift >= 2) {
            block[i] = scaled << (shift - 2);
        } else {
            block[i] = (scaled + (1 << (1 - shift))) >> (2 - shift);
        }
    }
}

int32_t quantizeDcChroma(int32_t value, int qp, bool intra) {
    const int qbits = 15 + qp / 6 + 1;
    const int32_t offset = int32_t((int64_t(1) << qbits) / (intra ? 3 : 6));

    const int32_t magnitude = value < 0 ? -value : value;
    const int64_t scaled = (int64_t(magnitude) * kMF[qp % 6][0] + offset) >> qbits;
    return value < 0 ? -int32_t(scaled) : int32_t(scaled);
}

void dequantizeDcChroma(int32_t block[4], int qp) {
    // The 2x2 Hadamard only multiplies by four, so after the same extra
    // quantisation bit the chroma DC path is two times strong rather than
    // eight.
    const int32_t factor = kV[qp % 6][0];
    for (int i = 0; i < 4; ++i) {
        block[i] = ((block[i] * factor) << (qp / 6)) >> 1;
    }
}

} // namespace h264
} // namespace blocky
