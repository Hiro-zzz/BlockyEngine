#pragma once
// Getting a finished model out of the editor.
//
// There is no model format in this engine and that is a decision, not a gap:
// maps are code, the prop catalogue is code, and a scene is deliberately not
// a file. So export writes into the two things that already exist rather than
// inventing a third.
//
// ------------------------------------------------------------ source, exact
//
// `emitSource` prints a call to `voxelize::fromLayers`, which is the function
// hand-written models already use and whose own comment says why: a wrong
// voxel is a wrong character you can see in the diff. The editor becomes the
// thing that types it. The result is exact -- every material field survives,
// including the ones a picture cannot carry.
//
// > Colours are written as the **linear** floats the model holds, with the
// > sRGB hex in a trailing comment. Writing `srgbToLinear(0x4a6b8c)` would
// > read better and round-trip badly, and a colour that shifts a little every
// > time it goes through the editor is the kind of bug nobody reports because
// > nobody can point at when it started.
//
// ------------------------------------------------------------- sheet, cheap
//
// `emitLayerSheet` lays the slices out in one PNG. No new codec, no new
// parser, and it opens in any image editor. Its cost is stated rather than
// hidden: **a picture carries colour**, so roughness, metallic and emission
// do not survive the trip. A model that glows must go out as source.
//
// The sheet's own dimensions cannot say how deep the model is -- a 32x32
// sheet is four 16x8 slices or sixteen 8x8 ones. Rather than reserve a
// header row inside the image, which would make it a format instead of a
// picture, the size rides in the **file name**: `mug_8x12x8.png`. It is
// visible, it survives every editor, and it needs no parser beyond reading
// three numbers.
#include "engine/core/image.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/sprite/sprite.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace edit {

struct EmitOptions {
    // The generated function's name: `buildMug` from "mug".
    std::string name = "model";

    // Emitted above the function. A generated file that does not say it is
    // generated gets hand-edited, and the next export silently throws that
    // edit away.
    bool header = true;

    // Written into the header so a regenerated file is byte-identical when
    // the model is. A timestamp here would make every export a diff.
    std::string tool = "BlockyEngine forge";
};

// A C++ identifier from a file name: "iron mug", "iron-mug" and "iron_mug"
// all become `buildIronMug`.
//
// Public rather than hidden inside the emitter, because the caller has to be
// able to *say* what the generated function will be called before it writes
// the file. And separate from the emitter because the emitter's job is then
// to refuse a name it cannot use rather than to quietly repair one -- a
// generated file whose function is not the one that was asked for is a thing
// nobody finds by reading the call site.
std::string identifierFor(const std::string& stem, const std::string& prefix = "build");

// Returns false and fills `error` in two cases: `options.name` is not a
// usable identifier (see `identifierFor`), or the palette will not fit the
// charset -- seventy materials is the limit, because `fromLayers` reads one
// character per column and there are only so many that are not '.' or ' '.
bool emitSource(const VoxelModel& model, std::string& out, const EmitOptions& options = {},
                std::string* error = nullptr);

// The same for a canvas: a call to `pixelart::fromRows`.
//
// A texture written this way stops being an asset file and becomes source,
// which is what the rest of this project already does with maps, palettes and
// the generated block textures -- and the reason `game/` needs no asset files
// at all. The PNG stays the thing you hand to something else; this is the
// thing you keep.
//
// Refuses for the same two reasons `emitSource` does: an unusable name, or
// more than seventy distinct colours. A photograph is not pixel art, and the
// refusal says so rather than emitting four thousand lines nobody can read.
bool emitCanvasSource(const ImageU8& image, std::string& out, const EmitOptions& options = {},
                      std::string* error = nullptr);

// And for a list of sprites: a function returning the `std::vector<Sprite>`
// that scenes currently spell out by hand.
//
// Unlike the other two this has no reader to disagree with. The emitted code
// assigns the engine's own struct field by field, so there is no notation to
// misread -- which means the compiler and a field-by-field comparison are not
// a weaker check here, they are the whole of it.
//
// Textures are left null and the emitted function takes one as a parameter.
// A sprite's texture is a pointer to something the scene owns, and a
// generated file has no business naming it; a dust mote needs none at all.
bool emitSpriteSource(const std::vector<Sprite>& sprites, std::string& out,
                      const EmitOptions& options = {}, std::string* error = nullptr);

// Slices from the bottom up, laid out left to right then down. `columns` of
// zero picks a squarish grid.
ImageU8 emitLayerSheet(const VoxelModel& model, int columns = 0);

// The other direction. `dims` says how to cut the sheet up; get it from the
// file name.
bool readLayerSheet(const ImageU8& sheet, IVec3 dims, VoxelModel& out, int columns = 0,
                    std::string* error = nullptr);

// "mug" plus 8x12x8 becomes "mug_8x12x8.png".
std::string sheetFileName(const std::string& stem, IVec3 dims);

// Reads those three numbers back out of a path. False when the name does not
// carry them, which is a thing to report rather than to guess around.
bool parseSheetFileName(const std::string& path, IVec3& dims);

} // namespace edit
} // namespace blocky
