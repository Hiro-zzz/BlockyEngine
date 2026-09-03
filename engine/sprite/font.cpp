#include "engine/sprite/font.hpp"

#include <algorithm>
#include <vector>

namespace blocky {
namespace {

constexpr int   kGrid = 16;          // cells across and down the atlas
constexpr float kGapPixels = 1.0f;   // blank column between two glyphs
constexpr float kSpaceAdvance = 4.0f;
constexpr float kInkAlpha = 0.5f;

// ------------------------------------------------------------ built-in font
// Five by seven, drawn rather than encoded, so a wrong pixel is a wrong pixel
// you can see in the source. `test_sprite` dumps the table back out as text,
// which is the only honest way to check a font by eye.
constexpr int kBuiltinWidth = 5;
constexpr int kBuiltinHeight = 7;

struct BuiltinGlyph {
    char code;
    const char* rows;  // kBuiltinHeight rows of kBuiltinWidth, '#' is ink
};

const BuiltinGlyph kBuiltin[] = {
    {'A', ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
    {'B', "####." "#...#" "#...#" "####." "#...#" "#...#" "####."},
    {'C', ".###." "#...#" "#...." "#...." "#...." "#...#" ".###."},
    {'D', "####." "#...#" "#...#" "#...#" "#...#" "#...#" "####."},
    {'E', "#####" "#...." "#...." "####." "#...." "#...." "#####"},
    {'F', "#####" "#...." "#...." "####." "#...." "#...." "#...."},
    {'G', ".###." "#...#" "#...." "#.###" "#...#" "#...#" ".###."},
    {'H', "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},
    {'I', ".###." "..#.." "..#.." "..#.." "..#.." "..#.." ".###."},
    {'J', "..###" "...#." "...#." "...#." "...#." "#..#." ".##.."},
    {'K', "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#"},
    {'L', "#...." "#...." "#...." "#...." "#...." "#...." "#####"},
    {'M', "#...#" "##.##" "#.#.#" "#...#" "#...#" "#...#" "#...#"},
    {'N', "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#" "#...#"},
    {'O', ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
    {'P', "####." "#...#" "#...#" "####." "#...." "#...." "#...."},
    {'Q', ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#"},
    {'R', "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#"},
    {'S', ".####" "#...." "#...." ".###." "....#" "....#" "####."},
    {'T', "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#.."},
    {'U', "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},
    {'V', "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#.."},
    {'W', "#...#" "#...#" "#...#" "#...#" "#.#.#" "##.##" "#...#"},
    {'X', "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#"},
    {'Y', "#...#" "#...#" ".#.#." "..#.." "..#.." "..#.." "..#.."},
    {'Z', "#####" "....#" "...#." "..#.." ".#..." "#...." "#####"},

    {'0', ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###."},
    {'1', "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." ".###."},
    {'2', ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####"},
    {'3', "####." "....#" "....#" ".###." "....#" "....#" "####."},
    {'4', "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#."},
    {'5', "#####" "#...." "####." "....#" "....#" "#...#" ".###."},
    {'6', ".###." "#...#" "#...." "####." "#...#" "#...#" ".###."},
    {'7', "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#..."},
    {'8', ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###."},
    {'9', ".###." "#...#" "#...#" ".####" "....#" "#...#" ".###."},

    {'.', "....." "....." "....." "....." "....." ".##.." ".##.."},
    {',', "....." "....." "....." "....." ".##.." ".##.." ".#..."},
    {':', "....." ".##.." ".##.." "....." ".##.." ".##.." "....."},
    {';', "....." ".##.." ".##.." "....." ".##.." ".##.." ".#..."},
    {'!', "..#.." "..#.." "..#.." "..#.." "..#.." "....." "..#.."},
    {'?', ".###." "#...#" "....#" "...#." "..#.." "....." "..#.."},
    {'-', "....." "....." "....." "#####" "....." "....." "....."},
    {'_', "....." "....." "....." "....." "....." "....." "#####"},
    {'+', "....." "..#.." "..#.." "#####" "..#.." "..#.." "....."},
    {'=', "....." "....." "#####" "....." "#####" "....." "....."},
    {'/', "....#" "....#" "...#." "..#.." ".#..." "#...." "#...."},
    {'(', "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#."},
    {')', ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#..."},
    {'<', "...#." "..#.." ".#..." "#...." ".#..." "..#.." "...#."},
    {'>', ".#..." "..#.." "...#." "....#" "...#." "..#.." ".#..."},
    {'*', "....." "#.#.#" ".###." "#####" ".###." "#.#.#" "....."},
    {'#', ".#.#." ".#.#." "#####" ".#.#." "#####" ".#.#." ".#.#."},
    {'%', "##..#" "##.#." "..#.." ".#..." "#..##" "...##" "....."},
    {0x27, "..#.." "..#.." "....." "....." "....." "....." "....."},  // apostrophe
    {0x22, ".#.#." ".#.#." "....." "....." "....." "....." "....."},  // quote
};

}  // namespace

bool Font::loadFromPng(const uint8_t* bytes, size_t size, std::string* error) {
    if (!texture_.loadFromPng(bytes, size, error)) return false;
    builtin_ = false;
    measureGlyphs();
    return true;
}

bool Font::loadFromSource(const AssetSource& source, const std::string& path,
                          std::string* error) {
    std::vector<uint8_t> bytes;
    if (!source.read(path, bytes, error)) return false;
    return loadFromPng(bytes.data(), bytes.size(), error);
}

void Font::useBuiltin() {
    const int cell = 8;
    ImageU8 atlas(kGrid * cell, kGrid * cell);  // zero-filled, so fully transparent

    for (const BuiltinGlyph& entry : kBuiltin) {
        int code = int(static_cast<unsigned char>(entry.code));
        int originX = (code % kGrid) * cell;
        int originY = (code / kGrid) * cell;

        for (int y = 0; y < kBuiltinHeight; ++y) {
            for (int x = 0; x < kBuiltinWidth; ++x) {
                if (entry.rows[y * kBuiltinWidth + x] != '#') continue;
                atlas.set(originX + x, originY + y, {255, 255, 255, 255});
            }
        }
    }

    texture_.fromImage(atlas);
    builtin_ = true;
    measureGlyphs();
}

void Font::measureGlyphs() {
    cell_ = texture_.width() / kGrid;
    if (cell_ <= 0) cell_ = 8;

    for (int code = 0; code < 256; ++code) {
        int originX = (code % kGrid) * cell_;
        int originY = (code / kGrid) * cell_;

        // An atlas shorter than the full grid simply has no cell here.
        if (originX + cell_ > texture_.width() || originY + cell_ > texture_.height()) {
            blank_[code] = true;
            width_[code] = 0.0f;
            advance_[code] = kSpaceAdvance;
            continue;
        }

        // The file records no widths, so the ink does: walk in from the right
        // until a column turns out to have something in it.
        int ink = 0;
        for (int x = cell_ - 1; x >= 0; --x) {
            bool any = false;
            for (int y = 0; y < cell_; ++y) {
                if (texture_.alphaAt(originX + x, originY + y) >= kInkAlpha) { any = true; break; }
            }
            if (any) { ink = x + 1; break; }
        }

        blank_[code] = ink == 0;
        width_[code] = float(ink);
        advance_[code] = ink == 0 ? kSpaceAdvance : float(ink) + kGapPixels;
    }
}

Font::Glyph Font::glyph(unsigned char code) const {
    Glyph result;
    if (texture_.empty()) return result;

    unsigned char c = code;
    // The built-in table stops at uppercase; serving lowercase from it beats
    // dropping half a label on the floor.
    if (builtin_ && c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');

    result.blank = blank_[c];
    result.width = width_[c];
    result.advance = advance_[c];

    const float atlasWidth = float(texture_.width());
    const float atlasHeight = float(texture_.height());
    const int originX = (c % kGrid) * cell_;
    const int originY = (c / kGrid) * cell_;

    result.uvMin = {float(originX) / atlasWidth, float(originY) / atlasHeight};
    result.uvMax = {(float(originX) + result.width) / atlasWidth,
                    float(originY + cell_) / atlasHeight};
    return result;
}

float Font::measure(const std::string& text) const {
    if (text.empty() || texture_.empty()) return 0.0f;

    float total = 0.0f;
    for (char ch : text) total += glyph(static_cast<unsigned char>(ch)).advance;

    // Drop the gap after the final glyph, so a measured label centres on the
    // ink rather than on the ink plus a column of nothing.
    return std::max(0.0f, total - kGapPixels);
}

} // namespace blocky
