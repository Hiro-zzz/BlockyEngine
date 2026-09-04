#pragma once
// Skins built in code instead of read from a file.
//
// The same split `texture_gen.hpp` makes for blocks, and for the same reason:
// the verbs are the engine's, the characters are not. Nothing here knows what
// anybody looks like -- it knows how to fill a part, how to put hair on the
// top of a head and a shoe on the bottom of a leg, and where those rectangles
// are. Who is wearing what is a recipe, and recipes live with whoever owns the
// cast: `game/avatar.cpp` for the one the game draws when a file is missing,
// `scenes/common/skins.hpp` for the people in the scenes.
//
// ------------------------------------------------------------- no pixel table
//
// Every rectangle below is asked of a `Skin` rather than written down. A table
// of literal coordinates here would be a second copy of the layout in
// conventions.md, and the two would agree right up until one of them changed
// -- with the symptom being a fringe drawn across somebody's ear. It also
// means the same recipe paints a classic and a slim character correctly,
// because the arm rectangles it is handed are already the right width.
//
// So a sheet is painted in two moves: `begin` gives a Skin its identity from a
// blank sheet, which is what makes its rectangles answerable, and `finish`
// puts the painted pixels back into that same Skin.
#include "engine/assets/entity/skin.hpp"
#include "engine/core/image.hpp"

#include <string>

namespace blocky::skingen {

using RGBA = ImageU8::RGBA;

// Establishes `skin` from a blank opaque sheet and hands that sheet back to be
// painted. Opaque is what makes it detect as classic -- a slim skin is the one
// with two transparent columns in the arm block -- so a recipe that wants slim
// arms clears those columns and calls this again.
//
// Returns false only if the Skin refuses the blank, which would mean the
// format changed underneath this.
bool begin(Skin& skin, ImageU8& sheet, std::string* error = nullptr);

// Puts the painted sheet back. Kept separate from `begin` so a recipe can hold
// the sheet across as many verbs as it likes without the Skin being rebuilt
// each time.
bool finish(Skin& skin, const ImageU8& sheet);

// ------------------------------------------------------------------- verbs
//
// Deliberately a small pile of them rather than a general drawing library.
// Each one exists because a character needed it, and a character that needs a
// new one gets a new verb -- which keeps this readable as a list of what a
// person is made of rather than as an image toolkit.

// The top or bottom rows of a rectangle. A limb hangs downwards from its
// joint, so the far end of it -- the hand, the shoe -- is the bottom.
SkinRect topRows(const SkinRect& rect, int rows);
SkinRect bottomRows(const SkinRect& rect, int rows);

void fill(ImageU8& sheet, const SkinRect& rect, RGBA colour);

// A little per-texel variation, so a flat fill does not read as plastic. The
// pattern hashes the absolute position rather than the position inside the
// rect: two rects that meet at a seam then carry on the same noise instead of
// showing where one ended.
void scatter(ImageU8& sheet, const SkinRect& rect, int amount);

// Every face of one part, optionally with noise over it.
void part(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int noise = 0);

// The far end of a limb: its bottom face entirely, and the lowest `rows` of
// its four sides. The top face is the shoulder or the hip and is never part of
// a hand or a shoe.
void limbEnd(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int rows);

// The near end, the same way round: the top face and the highest `rows` of the
// sides. Hair on a head, a collar on a torso, a shoulder on a sleeve.
void cap(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int rows,
         bool includeTop = true);

// A horizontal band across every side face of a part, `rows` tall, starting
// `fromTop` rows down. A stripe on a shirt, a cuff, a belt.
void band(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int fromTop, int rows);

// Two eyes and a mouth on an 8x8 head front, in head-front coordinates: `v`
// runs downwards as it does in any image, so row 3 is eye level.
void faceFeatures(ImageU8& sheet, const SkinRect& front, RGBA white, RGBA iris, RGBA mouth);

} // namespace blocky::skingen
