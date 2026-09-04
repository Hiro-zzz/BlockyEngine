#pragma once
// Motion compensation: the half of inter prediction that reads samples.
//
// A P macroblock says "I look like this piece of the previous picture, moved
// by this much". Everything expensive about video compression follows from
// that one sentence being usually true.
//
// ---------------------------------------------------------- quarter samples
//
// Luma vectors are in quarter samples, because half the motion in a picture
// is not a whole number of pixels and rounding the vector sends the whole
// difference into the residual.
//
// A fractional position is not a blend of its neighbours. Half-sample
// positions are the six-tap filter of 8.4.2.2.1 -- (1, -5, 20, 20, -5, 1)/32,
// applied along the axis being halved -- and quarter samples are the rounded
// average of the two nearest positions that filter produced. The filter is
// the format, to the bit: an encoder whose interpolation differs from the
// decoder's by one least significant bit predicts the next frame from a
// picture the decoder does not have, and the two drift further apart with
// every frame after that.
//
// The centre position, where both axes are halved, is the trap. It is a
// six-tap over six *intermediate* values of the other axis' six-tap, taken
// before they are rounded and clipped down to samples. Rounding them first is
// a different filter -- close enough to look right on a still, wrong enough
// to drift over a sequence.
//
// Chroma is separate and simpler. In 4:2:0 a chroma sample covers two luma
// samples, so the chroma vector is the luma one measured in eighths of a
// chroma sample -- numerically the same number, because a quarter of a luma
// sample is an eighth of a chroma one. That lands between samples nearly
// always, and chroma resolves it with the bilinear filter of 8.4.2.2.2 rather
// than a six-tap.
//
// ------------------------------------------------------------ off the edge
//
// Vectors may point outside the picture; the format says so and it matters,
// because content entering from a border is exactly where motion estimation
// wants to look. The decoder clamps the sample coordinate to the picture, so
// the edge row repeats outwards forever. This has to clamp identically --
// against the *coded* size, padding included, not the display size -- and it
// has to clamp every tap of the six, not just the position they are centred
// on.
#include <cstdint>

namespace blocky {
namespace h264 {

// The largest block predicted in one call. A macroblock, until partitions
// smaller than one exist.
inline constexpr int kMaxPredictionSize = 16;

// A reference picture: the deblocked reconstruction of an earlier frame.
// Luma is `width` by `height`; both chroma planes are half of each.
struct RefPicture {
    const uint8_t* y = nullptr;
    const uint8_t* cb = nullptr;
    const uint8_t* cr = nullptr;
    int width = 0, height = 0;

    bool valid() const { return y && cb && cr && width > 0 && height > 0; }
};

// Luma prediction for a `size` by `size` block whose top left corner is at
// (x0, y0), displaced by a vector in **quarter samples**. Out is `size*size`
// samples in raster order. `size` may not exceed kMaxPredictionSize.
void predictLuma(const RefPicture& ref, int x0, int y0, int mvx, int mvy, int size,
                 uint8_t* out);

// Chroma prediction for one plane. `plane` is `width` by `height` chroma
// samples; (x0, y0) and `size` are in chroma samples. The vector is in
// eighths of a chroma sample -- which is the luma vector in quarter samples,
// the same number unchanged.
void predictChroma(const uint8_t* plane, int width, int height, int x0, int y0, int mvcx,
                   int mvcy, int size, uint8_t* out);

// Sum of absolute differences between a prediction and the source, and sum of
// squared differences for the decisions that have to weigh distortion against
// bits rather than compare two codings of the same quality.
int      blockSad(const uint8_t* pred, const uint8_t* src, int srcStride, int x0, int y0,
                  int size);
long long blockSsd(const uint8_t* a, const uint8_t* b, int stride, int x0, int y0, int size);

} // namespace h264
} // namespace blocky
