#pragma once
// Intertitles in Cyrillic.
//
// A silent film needs cards, and this one is written in Russian -- but Font
// addresses a glyph by a single byte, and the game's ascii.png has no
// Cyrillic in it at all: rows 0-7 are ASCII and everything above is CP437
// box drawing and accented Latin. So the alphabet is drawn here, five by
// seven in the same style as the engine's built-in font, and painted into a
// 16x16 atlas at the CP1251 code points.
//
// CP1251 is the convenient choice and not an arbitrary one: it lays А..Я at
// 0xC0..0xDF and а..я at 0xE0..0xFF, which is exactly the linear range that
// U+0410..U+044F occupies in Unicode. So the transcoder below is one
// subtraction, and the two halves of the alphabet land in two clean rows of
// the atlas.
//
// Only capitals are drawn. Lowercase code points get the same glyphs, the way
// the engine's built-in font serves lowercase Latin from its uppercase forms
// -- a card set in capitals is what a title card wants anyway.
#include "engine/core/image.hpp"
#include "engine/core/png.hpp"
#include "engine/sprite/font.hpp"

#include <string>

namespace film {

using blocky::Font;
using blocky::ImageU8;

namespace type {

// ------------------------------------------------------------------ glyphs
// Five by seven, '#' is ink, drawn rather than encoded so a wrong pixel is a
// pixel you can see in the source.
struct Glyph {
    unsigned char code;
    const char* rows;
};

inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;

// The Cyrillic capitals, in CP1251 order from 0xC0. Several are the Latin
// letter unchanged -- А В Е К М Н О Р С Т Х -- and are still written out in
// full, because a table with holes in it is harder to check than a long one.
inline const Glyph kCyrillic[] = {
    {0xC0, ".###." "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},  // А
    {0xC1, "#####" "#...." "#...." "####." "#...#" "#...#" "####."},  // Б
    {0xC2, "####." "#...#" "#...#" "####." "#...#" "#...#" "####."},  // В
    {0xC3, "#####" "#...." "#...." "#...." "#...." "#...." "#...."},  // Г
    {0xC4, "..##." "..#.#" "..#.#" ".#..#" ".#..#" "#####" "#...#"},  // Д
    {0xC5, "#####" "#...." "#...." "####." "#...." "#...." "#####"},  // Е
    {0xC6, "#.#.#" "#.#.#" ".###." "#####" ".###." "#.#.#" "#.#.#"},  // Ж
    {0xC7, "####." "....#" "....#" ".###." "....#" "....#" "####."},  // З
    {0xC8, "#...#" "#...#" "#..##" "#.#.#" "##..#" "#...#" "#...#"},  // И
    {0xC9, ".###." "....." "#...#" "#..##" "#.#.#" "##..#" "#...#"},  // Й
    {0xCA, "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#"},  // К
    {0xCB, "..###" ".#..#" ".#..#" ".#..#" ".#..#" "#...#" "#...#"},  // Л
    {0xCC, "#...#" "##.##" "#.#.#" "#...#" "#...#" "#...#" "#...#"},  // М
    {0xCD, "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#"},  // Н
    {0xCE, ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###."},  // О
    {0xCF, "#####" "#...#" "#...#" "#...#" "#...#" "#...#" "#...#"},  // П
    {0xD0, "####." "#...#" "#...#" "####." "#...." "#...." "#...."},  // Р
    {0xD1, ".###." "#...#" "#...." "#...." "#...." "#...#" ".###."},  // С
    {0xD2, "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#.."},  // Т
    {0xD3, "#...#" "#...#" ".#.#." "..#.." "..#.." ".#..." "#...."},  // У
    {0xD4, "..#.." ".###." "#.#.#" "#.#.#" "#.#.#" ".###." "..#.."},  // Ф
    {0xD5, "#...#" "#...#" ".#.#." "..#.." ".#.#." "#...#" "#...#"},  // Х
    {0xD6, "#...#" "#...#" "#...#" "#...#" "#...#" "#####" "....#"},  // Ц
    {0xD7, "#...#" "#...#" "#...#" ".####" "....#" "....#" "....#"},  // Ч
    {0xD8, "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#####"},  // Ш
    {0xD9, "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#.#.#" "#####" "....#"},  // Щ
    {0xDA, "##..." ".#..." ".#..." ".###." ".#..#" ".#..#" ".###."},  // Ъ
    {0xDB, "#...#" "#...#" "#...#" "###.#" "#..##" "#..##" "###.#"},  // Ы
    {0xDC, "#...." "#...." "#...." "####." "#...#" "#...#" "####."},  // Ь
    {0xDD, "####." "....#" "....#" ".####" "....#" "....#" "####."},  // Э
    {0xDE, "#..#." "#.#.#" "#.#.#" "###.#" "#.#.#" "#.#.#" "#..#."},  // Ю
    {0xDF, ".####" "#...#" "#...#" ".####" "..#.#" ".#..#" "#...#"},  // Я
};

// Ё sits apart from the run, at 0xA8, exactly as CP1251 puts it.
inline const Glyph kYo = {0xA8, ".#.#." "....." "#####" "#...." "####." "#...." "#####"};

// Digits and the punctuation a card needs. Latin letters are deliberately not
// here: this film's cards are Russian, and a half-populated Latin alphabet
// would be an invitation to write one in by accident and get blanks.
inline const Glyph kCommon[] = {
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
    {'!', "..#.." "..#.." "..#.." "..#.." "..#.." "....." "..#.."},
    {'?', ".###." "#...#" "....#" "...#." "..#.." "....." "..#.."},
    {'-', "....." "....." "....." "#####" "....." "....." "....."},
};

// An em dash, which is what opens a line of speech in Russian. There is no
// ASCII for it, so it takes a code point of its own in the atlas and
// `cp1251` below maps U+2014 onto it.
inline constexpr unsigned char kEmDash = 0x97;
inline const Glyph kDash = {kEmDash, "....." "....." "....." "#####" "....." "....." "....."};

// Guillemets, the Russian quotation marks. Same story: their own code points
// in CP1251, at 0xAB and 0xBB.
inline const Glyph kQuoteOpen  = {0xAB, "....." "..#.#" ".#.#." "#.#.." ".#.#." "..#.#" "....."};
inline const Glyph kQuoteClose = {0xBB, "....." "#.#.." ".#.#." "..#.#" ".#.#." "#.#.." "....."};

// ------------------------------------------------------------------- atlas

inline void paint(ImageU8& atlas, const Glyph& glyph, int cell) {
    const int originX = (int(glyph.code) % 16) * cell;
    const int originY = (int(glyph.code) / 16) * cell;
    for (int y = 0; y < kGlyphHeight; ++y) {
        for (int x = 0; x < kGlyphWidth; ++x) {
            if (glyph.rows[y * kGlyphWidth + x] != '#') continue;
            atlas.set(originX + x, originY + y, {255, 255, 255, 255});
        }
    }
}

// The font the cards are set in. Built from the table above and nothing else,
// so it looks the same whether or not the game is installed -- a card is the
// one thing in this film that must never fall back to something else.
inline Font buildFont() {
    const int cell = 8;
    ImageU8 atlas(16 * cell, 16 * cell);  // zero-filled: fully transparent

    for (const Glyph& g : kCyrillic) {
        paint(atlas, g, cell);
        // The lowercase half of the range gets the same capital, which is how
        // the engine's own built-in font serves lowercase Latin.
        Glyph lower = g;
        lower.code = static_cast<unsigned char>(g.code + 0x20);
        paint(atlas, lower, cell);
    }
    for (const Glyph& g : kCommon) paint(atlas, g, cell);
    paint(atlas, kYo, cell);
    {
        Glyph yoLower = kYo;
        yoLower.code = 0xB8;
        paint(atlas, yoLower, cell);
    }
    paint(atlas, kDash, cell);
    paint(atlas, kQuoteOpen, cell);
    paint(atlas, kQuoteClose, cell);

    // Font reads a PNG, and pngEncode is right here, so the atlas goes out
    // and comes back rather than needing a second way in.
    const std::vector<uint8_t> png = blocky::pngEncode(atlas);
    Font font;
    font.loadFromPng(png.data(), png.size(), nullptr);
    return font;
}

// ------------------------------------------------------------- transcoding
//
// Source files are UTF-8 (the build passes /utf-8), so a Russian string
// literal arrives as two bytes per letter. The font wants one. U+0410..U+044F
// maps onto 0xC0..0xFF by subtracting 0x350, which is the whole conversion;
// Ё, ё, the em dash and the guillemets are the four exceptions worth
// carrying.
inline std::string cp1251(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());

    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);

        if (c < 0x80) { out.push_back(char(c)); ++i; continue; }

        // Two-byte sequence: Cyrillic lives entirely in this range.
        if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            const unsigned int cp =
                ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            i += 2;
            if (cp >= 0x0410 && cp <= 0x044F) {
                out.push_back(char(cp - 0x0410 + 0xC0));
            } else if (cp == 0x0401) {
                out.push_back(char(0xA8));  // Ё
            } else if (cp == 0x0451) {
                out.push_back(char(0xB8));  // ё
            } else if (cp == 0x00AB) {
                out.push_back(char(0xAB));  // «
            } else if (cp == 0x00BB) {
                out.push_back(char(0xBB));  // »
            }
            continue;
        }

        // Three-byte sequence: the em dash and the typographic quotes.
        if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            const unsigned int cp = ((c & 0x0Fu) << 12) |
                                    ((static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu) << 6) |
                                    (static_cast<unsigned char>(utf8[i + 2]) & 0x3Fu);
            i += 3;
            if (cp == 0x2014 || cp == 0x2013) out.push_back(char(kEmDash));
            else if (cp == 0x00AB) out.push_back(char(0xAB));
            else if (cp == 0x00BB) out.push_back(char(0xBB));
            continue;
        }

        // Anything else is dropped rather than guessed at: a wrong byte would
        // come out as some other letter, which is worse than a gap.
        ++i;
    }
    return out;
}

} // namespace type
} // namespace film
