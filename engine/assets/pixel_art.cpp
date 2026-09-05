#include "engine/assets/pixel_art.hpp"

#include <algorithm>

namespace blocky {
namespace pixelart {

ImageU8 fromRows(const std::vector<std::string>& rows, const std::vector<PixelKey>& key) {
    // The longest row, not the first. A picture whose width came from row zero
    // would be cropped by one missing character up there, and the crop would
    // look like a drawing mistake rather than a typo.
    size_t width = 0;
    for (const std::string& row : rows) width = std::max(width, row.size());

    ImageU8 image(int(width), int(rows.size()));
    if (width == 0 || rows.empty()) return image;

    // A flat table over the byte, because a key of four entries is looked up
    // once per texel and a linear scan through it is the only thing here that
    // could be called an inner loop.
    ImageU8::RGBA table[256];
    for (ImageU8::RGBA& entry : table) entry = {0, 0, 0, 0};
    for (const PixelKey& entry : key) table[uint8_t(entry.code)] = entry.colour;

    // '.' and ' ' are transparency even if the key names them: they are the
    // notation, and a key that overrode them would make two pictures using the
    // same characters mean different things.
    table[uint8_t('.')] = {0, 0, 0, 0};
    table[uint8_t(' ')] = {0, 0, 0, 0};

    for (size_t y = 0; y < rows.size(); ++y) {
        const std::string& row = rows[y];
        for (size_t x = 0; x < row.size(); ++x) {
            image.set(int(x), int(y), table[uint8_t(row[x])]);
        }
        // Anything past the end of a short row stays the transparent the
        // image was built with.
    }
    return image;
}

} // namespace pixelart
} // namespace blocky
