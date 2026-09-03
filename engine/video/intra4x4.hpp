#pragma once
// The nine 4x4 intra prediction modes.
//
// I_16x16 predicts a whole macroblock from one flat rule, which suits a sky
// or a wall and is hopeless on an edge: everything the prediction misses
// becomes residual, and an edge misses a lot. I_4x4 predicts each 4x4 block
// separately from its own neighbours, and can point the prediction along the
// edge instead of across it -- there are six directional modes for exactly
// that. On flat, hard-edged artwork the difference is most of the file size.
//
// The cost is signalling: sixteen modes per macroblock instead of one. That
// is what `prev_intra4x4_pred_mode_flag` is for -- a block whose mode matches
// the one predicted from its neighbours spends a single bit, and neighbouring
// blocks usually do agree, because an edge runs through several of them.
//
// ----------------------------------------------------- the above-right trap
//
// Two modes read four samples past the top right corner of the block. Those
// samples are only there if the block up and to the right has already been
// decoded, and inside a macroblock the Z-shaped scan order means that for
// five of the sixteen blocks it has not -- 3, 7, 11, 13 and 15. The format's
// answer is to repeat the last real sample rather than to forbid the mode,
// and `fillAboveRight` below is where that happens. Skipping it does not
// fail loudly: it silently predicts from whatever was in memory.
#include <cstdint>

namespace blocky {
namespace h264 {

// The samples a 4x4 block predicts from. `above` covers p[0..7,-1]: the four
// over the block and the four over the block to its right.
struct Neighbours4x4 {
    uint8_t above[8] = {};
    uint8_t left[4] = {};
    uint8_t corner = 0;      // p[-1,-1]

    bool haveAbove = false;
    bool haveLeft = false;
    bool haveAboveRight = false;

    // The corner is available exactly when both edges are, for every block
    // position: inside a macroblock it is an earlier block, and on an edge it
    // is whichever neighbouring macroblock supplies that edge.
    bool haveCorner() const { return haveAbove && haveLeft; }

    // Repeat p[3,-1] across p[4..7,-1] when the block up and to the right is
    // not there. Call once after filling `above`.
    void fillAboveRight() {
        if (haveAboveRight) return;
        for (int i = 4; i < 8; ++i) above[i] = above[3];
    }
};

// Mode numbering is the specification's, and the order is not arbitrary: the
// most probable mode is the smaller of two neighbours' modes, so the cheap
// modes are deliberately the low numbers.
enum : int {
    kIntra4Vertical = 0,
    kIntra4Horizontal = 1,
    kIntra4Dc = 2,
    kIntra4DiagonalDownLeft = 3,
    kIntra4DiagonalDownRight = 4,
    kIntra4VerticalRight = 5,
    kIntra4HorizontalDown = 6,
    kIntra4VerticalLeft = 7,
    kIntra4HorizontalUp = 8,
    kIntra4ModeCount = 9,
};

// True when `mode` can be used with the neighbours that are actually there.
// DC is the only mode that is always usable, which is what makes it the
// stand-in for an absent neighbour when predicting the mode itself.
bool intra4x4Available(int mode, const Neighbours4x4& n);

// Fills `pred` in raster order. Returns false, leaving `pred` untouched, when
// the mode is not available.
bool predict4x4(int mode, const Neighbours4x4& n, uint8_t pred[16]);

} // namespace h264
} // namespace blocky
