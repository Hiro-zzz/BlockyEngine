#pragma once
// CAVLC: context-adaptive variable length coding, the entropy stage of
// Baseline H.264.
//
// This is where the compression actually happens. Everything before it --
// prediction, transform, quantisation -- exists to turn a block of pixels
// into a short list of small numbers with a lot of zeros at the end; CAVLC is
// what turns that list into few bits.
//
// It is adaptive in two ways, and both are the point:
//
//   Across blocks. The code used for "how many coefficients are there" is
//   chosen by how many the neighbours above and to the left had. A detailed
//   area and a flat area get different code books without a single bit being
//   spent to say which, because the decoder can see the same neighbours.
//
//   Within a block. Coefficients are written from the highest frequency
//   backwards, and the code for magnitudes widens as it goes. High
//   frequencies are almost always 0 or 1 and the low ones can be large, so
//   walking backwards means the cheap codes are used where they fit.
//
// ------------------------------------------------------------- on the tables
//
// Most of the implementation is tables out of the specification, and a
// transcription error in one of them produces a stream that decodes correctly
// right up until it does not. The structural defence is in the tests: a
// variable length code book has to be a *prefix code*, no entry the prefix of
// another, or decoding is ambiguous. Almost any mistyped length or value
// breaks that property, and the test checks every table for it.
#include "engine/video/bitstream.hpp"

#include <cstdint>

namespace blocky {
namespace h264 {

// How many coefficients a block can hold, which also selects which flavour of
// the tables applies.
enum class BlockKind {
    Luma16,       // a whole 4x4 block, 16 coefficients
    LumaAc15,     // the AC part of an I_16x16 block, DC handled separately
    ChromaDc4,    // the four DC coefficients of one chroma plane
    ChromaAc15,   // the AC part of a chroma block
};

// Writes one block of coefficients, already in zig-zag order.
//
// `nC` is the neighbour context: the average of the coefficient counts of the
// block above and the block to the left, computed by the caller because only
// it knows the geometry. It is ignored for ChromaDc4, which has a table of
// its own.
//
// Returns the number of non-zero coefficients, which the caller records so it
// can be the neighbour context for the blocks after it.
int writeResidualBlock(BitWriter& w, const int32_t coefficients[], BlockKind kind, int nC);

// coded_block_pattern is written as me(v): an ordinary Exp-Golomb code over a
// codeNum that the standard maps to the pattern through a table. The mapping
// is a permutation of 0..47, which is what the tests check of it -- a table
// that is not a bijection cannot be a mapping at all.
//
// There are two such permutations and they are not interchangeable: Table 9-4
// gives one column for macroblocks predicted Intra_4x4 and another for inter
// ones. They differ in the obvious way -- an intra macroblock nearly always
// has residual everywhere, so pattern 47 gets the one-bit codeNum, while an
// inter macroblock nearly always has none, so pattern 0 does. Using the wrong
// column produces a stream that decodes, and decodes to the wrong blocks.
//
// Returns -1 for a pattern outside 0..47.
int codedBlockPatternCodeNum(int cbp, bool intra);

// Exposed for the tests, which check that every code book is a prefix code.
struct VlcCode {
    uint8_t length = 0;
    uint16_t value = 0;
};
int  coeffTokenTableCount();
bool coeffTokenTable(int table, int trailingOnes, int totalCoeff, VlcCode& out);
bool totalZerosTable(bool chromaDc, int totalCoeff, int zeros, VlcCode& out);
bool runBeforeTable(int zerosLeft, int run, VlcCode& out);

} // namespace h264
} // namespace blocky
