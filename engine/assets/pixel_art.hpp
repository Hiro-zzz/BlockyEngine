#pragma once
// An image authored by hand, in code.
//
// The counterpart of `voxelize::fromLayers`, one dimension down and there for
// the same reason. Models in this project are code, maps are code, block
// textures are code (`texgen`'s verbs plus a recipe), and a hand-drawn
// texture had no such form -- it could only be a PNG next to the source. Now
// it can be the source: a wrong texel is a wrong character, in a diff, in a
// review, in a `git blame`.
//
// This is the reader. `edit::emitCanvasSource` is the writer, and the two are
// deliberately not a pair invented together: this one is usable by hand, and
// the editor's job is to type it for you.
//
//   ImageU8 mark = pixelart::fromRows({
//       "..##..",
//       ".####.",
//       "..##..",
//   }, {{'#', {255, 170, 40, 255}}});
//
// Rows run top to bottom, columns left to right, exactly as they read on the
// screen. `'.'` and `' '` are transparent; any character not in the key is
// transparent too, so a sketch with notes in it still builds.
//
// Short rows are padded with transparency rather than refused. A row longer
// than the first sets the width for nobody -- the width is the longest row,
// so a picture cannot be silently cropped by a typo in its first line.
#include "engine/core/image.hpp"

#include <string>
#include <vector>

namespace blocky {
namespace pixelart {

struct PixelKey {
    char code = '#';
    ImageU8::RGBA colour{255, 255, 255, 255};
};

ImageU8 fromRows(const std::vector<std::string>& rows, const std::vector<PixelKey>& key);

} // namespace pixelart
} // namespace blocky
