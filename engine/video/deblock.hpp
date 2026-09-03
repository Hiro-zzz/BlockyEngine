#pragma once
// The deblocking filter.
//
// Quantisation is done on 4x4 blocks independently, so neighbouring blocks
// land on slightly different answers and the seam between them shows as a
// straight line the picture never had. The filter smooths across those seams
// -- and only those, which is the whole difficulty: it has to soften an
// artefact that looks exactly like an edge without softening the edges that
// belong to the picture.
//
// It decides by asking whether the step across the seam is small enough to
// be quantisation error rather than content. Both thresholds come from the
// quantiser: at a fine QP almost nothing is filtered, at a coarse one a lot
// is, which is right because that is when blocking appears.
//
// ------------------------------------------------------ why an encoder needs it
//
// A decoder-side filter would be no business of ours if every frame were
// intra: intra prediction reads reconstructed samples *before* filtering, so
// the filtered picture would only ever be output. Inter prediction ended
// that. Reference pictures are the filtered ones, so an encoder predicting
// from an unfiltered copy predicts from something the decoder does not have,
// and the two drift apart frame after frame.
//
// So this has to match a decoder exactly, which is why the thresholds are
// transcribed from the specification rather than tuned, and why it is
// verified against a real decoder rather than by eye. It was written a
// version before it was needed, deliberately: on the day P frames arrived
// there was one new thing on trial instead of two.
#include <cstdint>
#include <vector>

namespace blocky {
namespace h264 {

// ------------------------------------------------------------ how hard to filter
//
// Boundary strength is the filter's whole judgement, and it is not about how
// big the step across an edge is -- that is what the thresholds are for. It
// is about how likely the step is to be an artefact, which the format decides
// from what the two sides were coded as:
//
//   4  a macroblock edge with an intra macroblock on either side: prediction
//      restarts here, so a visible seam is the likeliest thing in the format
//   3  an internal edge of an intra macroblock
//   2  either side has coefficients -- there is quantisation error to smooth
//   1  no coefficients on either side, but the two moved differently, so the
//      pieces came from places that did not touch in the reference
//   0  same motion, no coefficients: the two sides are a copy of samples that
//      were already neighbours. There is nothing here that was not there
//      before, and filtering would only blur it.
//
// Strength 0 is why an all-intra encoder cannot pretend inter does not exist:
// most of a still P frame filters at 0, and a filter that treats it as 3
// softens the whole picture and drifts away from every decoder.
struct DeblockPicture {
    int mbWidth = 0, mbHeight = 0;
    int qp = 26;        // luma quantiser, constant across the picture for now
    int qpChroma = 26;

    // One entry per macroblock. An I_PCM macroblock is filtered as though its
    // quantiser were zero, which in practice means not at all -- there is no
    // quantisation error in it to smooth.
    std::vector<uint8_t> pcm;

    // One entry per macroblock. Empty means every macroblock is intra, which
    // is what an I picture passes.
    std::vector<uint8_t> intra;

    // One entry per 4x4 luma block, `mbWidth * 4` to a row: whether the block
    // has any non-zero coefficient. Chroma edges take the strength derived
    // from luma, so there is no chroma equivalent of this.
    std::vector<uint8_t> nonZero;

    // One entry per macroblock, in quarter luma samples -- the unit the
    // format stores vectors in, and the unit the whole-sample threshold for
    // strength 1 is written against. Partitions smaller than the macroblock
    // would need one per 4x4 block; this encoder has none, so a macroblock's
    // internal edges always compare a vector with itself and come out at
    // strength 0 unless there are coefficients.
    std::vector<int16_t> mvx, mvy;
};

// Filters the planes in place. Luma is `width` by `height`; chroma is half of
// each.
void deblock(uint8_t* y, uint8_t* cb, uint8_t* cr, int width, int height,
             const DeblockPicture& picture);

// Exposed so the tests can check the thresholds are monotone, which is what
// catches a transcription error in them.
int deblockAlpha(int indexA);
int deblockBeta(int indexB);
int deblockTc0(int bS, int indexA);   // bS 1..3

} // namespace h264
} // namespace blocky
