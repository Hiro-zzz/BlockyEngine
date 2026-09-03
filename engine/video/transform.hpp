#pragma once
// The H.264 transform and quantisation stage.
//
// H.264 does not use a real DCT. It uses a 4x4 integer approximation chosen
// so that the inverse can be computed exactly in 16-bit integer arithmetic
// with nothing but adds and shifts -- which is what makes decoders agree with
// each other bit for bit. An encoder that used a floating point DCT and a
// decoder that used this one would drift apart, and the drift would compound
// through every intra prediction that reads a reconstructed neighbour.
//
// The consequence for this file: **the inverse path here is not a convenience
// for testing, it is part of encoding.** Every macroblock must be
// reconstructed exactly the way a decoder will reconstruct it, because the
// next macroblock predicts from those samples. Reconstruct differently and
// the two drift, silently, getting worse down the frame.
//
// Blocks are 16 ints in raster order throughout. Coefficients live in the
// same layout and are only put into zig-zag order at the point they are
// written out.
#include <cstdint>

namespace blocky {
namespace h264 {

// Zig-zag: coefficient scan position -> index in raster order. Low
// frequencies first, which is what puts the zeros together at the end where
// the entropy coder can spend almost nothing on them.
extern const int kZigZag4x4[16];

// Chroma QP is derived from luma QP, and above 29 it stops following it --
// chroma is given progressively finer quantisation than luma at the same
// nominal QP because the eye notices colour blocking sooner.
int chromaQp(int lumaQp);

// ---------------------------------------------------------------- transforms
// The core residual transform. Forward is the encoder's own; inverse is
// specified exactly and must match a decoder's to the bit.
void forward4x4(int32_t block[16]);
void inverse4x4(int32_t block[16]);

// Hadamard, for the DC coefficients that I_16x16 collects out of its sixteen
// luma blocks and the four of each chroma plane. A flat area then costs one
// coefficient for the whole macroblock instead of sixteen.
void hadamard4x4(int32_t block[16]);
void hadamard2x2(int32_t block[4]);

// ------------------------------------------------------------- quantisation
// `intra` only changes the rounding offset: intra blocks round more
// generously because they have no previous frame to hide an error behind.
void quantize4x4(const int32_t in[16], int32_t out[16], int qp, bool intra);
void dequantize4x4(const int32_t in[16], int32_t out[16], int qp);

// The DC blocks are quantised at one step coarser and dequantised by their
// own rules, both of which fold the extra factor the Hadamard introduced.
int32_t quantizeDcLuma(int32_t value, int qp, bool intra);
void    dequantizeDcLuma(int32_t block[16], int qp);
int32_t quantizeDcChroma(int32_t value, int qp, bool intra);
void    dequantizeDcChroma(int32_t block[4], int qp);

} // namespace h264
} // namespace blocky
