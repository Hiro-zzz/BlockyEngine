#include "engine/video/cavlc.hpp"

#include <cstdlib>

namespace blocky {
namespace h264 {
namespace {

struct Code {
    uint8_t length;
    uint16_t value;
};

constexpr Code kNone{0, 0};

// ---------------------------------------------------------------- coeff_token
// Indexed [trailingOnes][totalCoeff]. A zero length means the combination
// cannot occur -- there cannot be more trailing ones than coefficients.
//
// Three books, chosen by how busy the neighbouring blocks were. The first is
// for quiet neighbourhoods and spends one bit on "no coefficients at all",
// which is most blocks of most frames.
const Code kCoeffToken0[4][17] = {
    {{1,1},{6,5},{8,7},{9,7},{10,7},{11,7},{13,15},{13,11},{13,8},{14,15},{14,11},{15,15},{15,11},{16,15},{16,11},{16,7},{16,4}},
    {kNone,{2,1},{6,4},{8,6},{9,6},{10,6},{11,6},{13,14},{13,10},{14,14},{14,10},{15,14},{15,10},{15,1},{16,14},{16,10},{16,6}},
    {kNone,kNone,{3,1},{7,5},{8,5},{9,5},{10,5},{11,5},{13,13},{13,9},{14,13},{14,9},{15,13},{15,9},{16,13},{16,9},{16,5}},
    {kNone,kNone,kNone,{5,3},{6,3},{7,4},{8,4},{9,4},{10,4},{11,4},{13,12},{14,12},{14,8},{15,12},{15,8},{16,12},{16,8}},
};

const Code kCoeffToken1[4][17] = {
    {{2,3},{6,11},{6,7},{7,7},{8,7},{8,4},{9,7},{11,15},{11,11},{12,15},{12,11},{12,8},{13,15},{13,11},{13,7},{14,9},{14,7}},
    {kNone,{2,2},{5,7},{6,10},{6,6},{7,6},{8,6},{9,6},{11,14},{11,10},{12,14},{12,10},{13,14},{13,10},{14,11},{14,8},{14,6}},
    {kNone,kNone,{3,3},{6,9},{6,5},{7,5},{8,5},{9,5},{11,13},{11,9},{12,13},{12,9},{13,13},{13,9},{13,6},{14,10},{14,5}},
    {kNone,kNone,kNone,{4,5},{4,4},{5,6},{6,8},{6,4},{7,4},{9,4},{11,12},{11,8},{12,12},{13,12},{13,8},{13,1},{14,4}},
};

const Code kCoeffToken2[4][17] = {
    {{4,15},{6,15},{6,11},{6,8},{7,15},{7,11},{7,9},{7,8},{8,15},{8,11},{9,15},{9,11},{9,8},{10,13},{10,9},{10,5},{10,1}},
    {kNone,{4,14},{5,15},{5,12},{5,10},{5,8},{6,14},{6,10},{7,14},{8,14},{8,10},{9,14},{9,10},{9,7},{10,12},{10,8},{10,4}},
    {kNone,kNone,{4,13},{5,14},{5,11},{5,9},{6,13},{6,9},{7,13},{7,10},{8,13},{8,9},{9,13},{9,9},{10,11},{10,7},{10,3}},
    {kNone,kNone,kNone,{4,12},{4,11},{4,10},{4,9},{4,8},{5,13},{6,12},{7,12},{8,12},{8,8},{9,12},{10,10},{10,6},{10,2}},
};

// The chroma DC book: only four coefficients are possible, so it is small and
// entirely its own.
const Code kCoeffTokenChromaDc[4][5] = {
    {{2,1},{6,7},{6,4},{6,3},{6,2}},
    {kNone,{1,1},{6,6},{7,3},{8,3}},
    {kNone,kNone,{3,1},{7,2},{8,2}},
    {kNone,kNone,kNone,{6,5},{7,0}},
};

// ---------------------------------------------------------------- total_zeros
// How many zeros lie before the last coefficient. One book per coefficient
// count, because knowing there are fifteen coefficients in a sixteen slot
// block leaves very little for this field to say.
const Code kTotalZeros[15][16] = {
    {{1,1},{3,3},{3,2},{4,3},{4,2},{5,3},{5,2},{6,3},{6,2},{7,3},{7,2},{8,3},{8,2},{9,3},{9,2},{9,1}},
    {{3,7},{3,6},{3,5},{3,4},{3,3},{4,5},{4,4},{4,3},{4,2},{5,3},{5,2},{6,3},{6,2},{6,1},{6,0},kNone},
    {{4,5},{3,7},{3,6},{3,5},{4,4},{4,3},{3,4},{3,3},{4,2},{5,3},{5,2},{6,1},{5,1},{6,0},kNone,kNone},
    {{5,3},{3,7},{4,5},{4,4},{3,6},{3,5},{3,4},{4,3},{3,3},{4,2},{5,2},{5,1},{5,0},kNone,kNone,kNone},
    {{4,5},{4,4},{4,3},{3,7},{3,6},{3,5},{3,4},{3,3},{4,2},{5,1},{4,1},{5,0},kNone,kNone,kNone,kNone},
    {{6,1},{5,1},{3,7},{3,6},{3,5},{3,4},{3,3},{3,2},{4,1},{3,1},{6,0},kNone,kNone,kNone,kNone,kNone},
    {{6,1},{5,1},{3,5},{3,4},{3,3},{2,3},{3,2},{4,1},{3,1},{6,0},kNone,kNone,kNone,kNone,kNone,kNone},
    {{6,1},{4,1},{5,1},{3,3},{2,3},{2,2},{3,2},{3,1},{6,0},kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{6,1},{6,0},{4,1},{2,3},{2,2},{3,1},{2,1},{5,1},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{5,1},{5,0},{3,1},{2,3},{2,2},{2,1},{4,1},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{4,0},{4,1},{3,1},{3,2},{1,1},{3,3},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{4,0},{4,1},{2,1},{1,1},{3,1},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{3,0},{3,1},{1,1},{2,1},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{2,0},{2,1},{1,1},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{1,0},{1,1},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
};

const Code kTotalZerosChromaDc[3][4] = {
    {{1,1},{2,1},{3,1},{3,0}},
    {{1,1},{2,1},{2,0},kNone},
    {{1,1},{1,0},kNone,kNone},
};

// ------------------------------------------------------------------ run_before
// How many zeros sit immediately before each coefficient. The book narrows as
// the zeros are used up, which is why the count left is the index.
const Code kRunBefore[7][15] = {
    {{1,1},{1,0},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{1,1},{2,1},{2,0},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{2,3},{2,2},{2,1},{2,0},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{2,3},{2,2},{2,1},{3,1},{3,0},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{2,3},{2,2},{3,3},{3,2},{3,1},{3,0},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{2,3},{3,0},{3,1},{3,3},{3,2},{3,5},{3,4},kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone},
    {{3,7},{3,6},{3,5},{3,4},{3,3},{3,2},{3,1},{4,1},{5,1},{6,1},{7,1},{8,1},{9,1},{10,1},{11,1}},
};

// Pattern to codeNum, both columns of Table 9-4. Extracted from the
// specification rather than typed, and checked for being permutations before
// use -- a mapping that is not a bijection would leave some patterns
// unwritable and decode some codeNums two ways.
//
// The two columns disagree almost everywhere, and the ends say why. Intra
// gives its cheapest code to pattern 47, every block coded, because an intra
// macroblock that predicts from an edge nearly always has residual left over.
// Inter gives it to pattern 0, nothing coded, because a macroblock that found
// its match in the previous picture nearly always has nothing left to say.
const uint8_t kCbpToCodeNumIntra[48] = {
     3, 29, 30, 17, 31, 18, 37,  8, 32, 38, 19,  9,
    20, 10, 11,  2, 16, 33, 34, 21, 35, 22, 39,  4,
    36, 40, 23,  5, 24,  6,  7,  1, 41, 42, 43, 25,
    44, 26, 46, 12, 45, 47, 27, 13, 28, 14, 15,  0,
};

const uint8_t kCbpToCodeNumInter[48] = {
     0,  2,  3,  7,  4,  8, 17, 13,  5, 18,  9, 14,
    10, 15, 16, 11,  1, 32, 33, 36, 34, 37, 44, 40,
    35, 45, 38, 41, 39, 42, 43, 19,  6, 24, 25, 20,
    26, 21, 46, 28, 27, 47, 22, 29, 23, 30, 31, 12,
};

void writeCode(BitWriter& w, const Code& code) { w.u(code.length, code.value); }

// A prefix of `zeros` zero bits then a one. The unary part of a level.
void writeLevelPrefix(BitWriter& w, int zeros) {
    for (int i = 0; i < zeros; ++i) w.u(1, 0);
    w.u(1, 1);
}

int maxCoefficients(BlockKind kind) {
    switch (kind) {
        case BlockKind::Luma16:    return 16;
        case BlockKind::LumaAc15:  return 15;
        case BlockKind::ChromaDc4: return 4;
        case BlockKind::ChromaAc15: return 15;
    }
    return 16;
}

} // namespace

int codedBlockPatternCodeNum(int cbp, bool intra) {
    if (cbp < 0 || cbp > 47) return -1;
    return int(intra ? kCbpToCodeNumIntra[cbp] : kCbpToCodeNumInter[cbp]);
}

int coeffTokenTableCount() { return 4; }

bool coeffTokenTable(int table, int trailingOnes, int totalCoeff, VlcCode& out) {
    if (trailingOnes < 0 || trailingOnes > 3) return false;

    if (table == 3) {   // chroma DC
        if (totalCoeff < 0 || totalCoeff > 4) return false;
        const Code& c = kCoeffTokenChromaDc[trailingOnes][totalCoeff];
        if (c.length == 0) return false;
        out = {c.length, c.value};
        return true;
    }

    if (totalCoeff < 0 || totalCoeff > 16) return false;
    const Code* row = table == 0   ? kCoeffToken0[trailingOnes]
                      : table == 1 ? kCoeffToken1[trailingOnes]
                                   : kCoeffToken2[trailingOnes];
    if (row[totalCoeff].length == 0) return false;
    out = {row[totalCoeff].length, row[totalCoeff].value};
    return true;
}

bool totalZerosTable(bool chromaDc, int totalCoeff, int zeros, VlcCode& out) {
    if (chromaDc) {
        if (totalCoeff < 1 || totalCoeff > 3 || zeros < 0 || zeros > 3) return false;
        const Code& c = kTotalZerosChromaDc[totalCoeff - 1][zeros];
        if (c.length == 0) return false;
        out = {c.length, c.value};
        return true;
    }
    if (totalCoeff < 1 || totalCoeff > 15 || zeros < 0 || zeros > 15) return false;
    const Code& c = kTotalZeros[totalCoeff - 1][zeros];
    if (c.length == 0) return false;
    out = {c.length, c.value};
    return true;
}

bool runBeforeTable(int zerosLeft, int run, VlcCode& out) {
    if (zerosLeft < 1 || run < 0 || run > 14) return false;
    const int row = zerosLeft > 6 ? 6 : zerosLeft - 1;
    const Code& c = kRunBefore[row][run];
    if (c.length == 0) return false;
    out = {c.length, c.value};
    return true;
}

int writeResidualBlock(BitWriter& w, const int32_t coefficients[], BlockKind kind, int nC) {
    const int count = maxCoefficients(kind);
    const bool chromaDc = kind == BlockKind::ChromaDc4;

    // Gather the non-zero coefficients and where they sit. Everything after
    // this works on the list rather than the block.
    int levels[16];
    int positions[16];
    int totalCoeff = 0;
    for (int i = 0; i < count; ++i) {
        if (coefficients[i] != 0) {
            levels[totalCoeff] = int(coefficients[i]);
            positions[totalCoeff] = i;
            ++totalCoeff;
        }
    }

    // Trailing ones: coefficients of magnitude one at the high frequency end,
    // at most three. They are so common that they get a sign bit and nothing
    // else.
    int trailingOnes = 0;
    for (int i = totalCoeff - 1; i >= 0 && trailingOnes < 3; --i) {
        if (levels[i] != 1 && levels[i] != -1) break;
        ++trailingOnes;
    }

    // Which code book. The neighbour context only selects it; it is never
    // written, because the decoder can look at the same neighbours.
    int table;
    if (chromaDc)      table = 3;
    else if (nC < 2)   table = 0;
    else if (nC < 4)   table = 1;
    else if (nC < 8)   table = 2;
    else               table = -1;   // eight or more: a flat six bit code

    if (table >= 0) {
        VlcCode token{};
        if (!coeffTokenTable(table, trailingOnes, totalCoeff, token)) return totalCoeff;
        w.u(token.length, token.value);
    } else {
        // In a busy neighbourhood a variable length code buys nothing, so the
        // count is simply written out.
        const uint32_t value =
            totalCoeff == 0 ? 3u : uint32_t(((totalCoeff - 1) << 2) | trailingOnes);
        w.u(6, value);
    }

    if (totalCoeff == 0) return 0;

    // Signs of the trailing ones, highest frequency first.
    for (int i = 0; i < trailingOnes; ++i) {
        w.u(1, levels[totalCoeff - 1 - i] < 0 ? 1u : 0u);
    }

    // The remaining levels, still working down from high frequency. The code
    // widens as it goes: high frequencies are small, low ones need room.
    int suffixLength = (totalCoeff > 10 && trailingOnes < 3) ? 1 : 0;

    for (int i = totalCoeff - trailingOnes - 1; i >= 0; --i) {
        const int level = levels[i];
        int levelCode = level > 0 ? (level - 1) * 2 : (-level - 1) * 2 + 1;

        // The first level after fewer than three trailing ones cannot be one
        // -- if it were, it would have been a trailing one. So the alphabet
        // starts two later and the codes shift down to suit.
        if (i == totalCoeff - trailingOnes - 1 && trailingOnes < 3) levelCode -= 2;

        if (suffixLength == 0) {
            if (levelCode < 14) {
                writeLevelPrefix(w, levelCode);
            } else if (levelCode < 30) {
                writeLevelPrefix(w, 14);
                w.u(4, uint32_t(levelCode - 14));
            } else {
                writeLevelPrefix(w, 15);
                w.u(12, uint32_t(levelCode - 30));
            }
        } else {
            const int prefix = levelCode >> suffixLength;
            if (prefix < 15) {
                writeLevelPrefix(w, prefix);
                w.u(suffixLength, uint32_t(levelCode & ((1 << suffixLength) - 1)));
            } else {
                writeLevelPrefix(w, 15);
                w.u(12, uint32_t(levelCode - (15 << suffixLength)));
            }
        }

        if (suffixLength == 0) suffixLength = 1;
        const int magnitude = std::abs(level);
        if (suffixLength < 6 && magnitude > (3 << (suffixLength - 1))) ++suffixLength;
    }

    // Where the coefficients sit, described as gaps rather than positions.
    const int totalZeros = positions[totalCoeff - 1] + 1 - totalCoeff;

    if (totalCoeff < count) {
        VlcCode zerosCode{};
        if (totalZerosTable(chromaDc, totalCoeff, totalZeros, zerosCode)) {
            w.u(zerosCode.length, zerosCode.value);
        }
    }

    int zerosLeft = totalZeros;
    for (int i = totalCoeff - 1; i > 0 && zerosLeft > 0; --i) {
        const int run = positions[i] - positions[i - 1] - 1;
        VlcCode runCode{};
        if (runBeforeTable(zerosLeft, run, runCode)) w.u(runCode.length, runCode.value);
        zerosLeft -= run;
    }

    return totalCoeff;
}

} // namespace h264
} // namespace blocky
