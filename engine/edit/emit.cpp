#include "engine/edit/emit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace blocky {
namespace edit {

namespace {

// Every printable character `fromLayers` does not already spend on emptiness.
// Seventy of them, which is the export's material limit and the reason the
// function can fail.
const char kCharset[] =
    "#*+=%@&$"
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz";

constexpr size_t kCharsetSize = sizeof(kCharset) - 1;

// Nine significant digits is what a float needs to survive text and come
// back the same. Fewer would make an exported model drift a little on every
// trip through the editor, which is exactly the kind of change nobody can
// date.
std::string literal(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", double(value));

    std::string text(buffer);
    // `1f` is not a float literal and `1.0f` is. A value that printed as a
    // whole number needs the point put back before the suffix.
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
        text += ".0";
    }
    return text + "f";
}

std::string vecLiteral(Vec3 v) {
    return "Vec3{" + literal(v.x) + ", " + literal(v.y) + ", " + literal(v.z) + "}";
}

// What a person would have typed into a colour picker, for the comment. The
// floats above are the truth; this is the caption.
std::string hexOf(Vec3 linear) {
    const Vec3 encoded = linearToSrgb(linear);
    auto channel = [](float c) {
        const float clamped = std::min(1.0f, std::max(0.0f, c));
        return int(std::lround(clamped * 255.0f));
    };

    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", channel(encoded.x), channel(encoded.y),
                  channel(encoded.z));
    return std::string(buffer);
}

int gridColumns(int slices, int requested) {
    if (requested > 0) return requested;
    // Squarish, so a sixteen-slice model is four by four rather than a strip
    // sixteen wide that no window shows at once.
    return std::max(1, int(std::ceil(std::sqrt(double(std::max(1, slices))))));
}

bool isIdentifier(const std::string& text) {
    if (text.empty()) return false;
    if (!(std::isalpha(uint8_t(text[0])) || text[0] == '_')) return false;

    for (char c : text) {
        if (!(std::isalnum(uint8_t(c)) || c == '_')) return false;
    }
    return true;
}

} // namespace

std::string identifierFor(const std::string& stem, const std::string& prefix) {
    std::string name = prefix;
    bool capitalise = true;

    for (char c : stem) {
        if (std::isalnum(uint8_t(c))) {
            name += capitalise ? char(std::toupper(uint8_t(c))) : c;
            capitalise = false;
        } else {
            // Anything else is a word break rather than a character: a space,
            // a dash and an underscore all mean the same thing in a file name.
            capitalise = true;
        }
    }

    // A stem of nothing but punctuation, or an empty one. The prefix alone is
    // a legal identifier and says what it is.
    if (name == prefix && prefix.empty()) return "buildModel";
    return name;
}

bool emitSource(const VoxelModel& model, std::string& out, const EmitOptions& options,
                std::string* error) {
    const IVec3 dims = model.dims();
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) {
        if (error) *error = "the model is empty";
        return false;
    }

    if (!isIdentifier(options.name)) {
        if (error) {
            *error = "\"" + options.name +
                     "\" is not a C++ identifier; pass one from identifierFor()";
        }
        return false;
    }

    // Which palette slots the voxels actually use. Walking the palette in
    // index order rather than in order of first appearance keeps the output
    // stable: the same model exports to the same bytes however it was drawn.
    const size_t materials = model.materialCount();
    std::vector<char> codeFor(materials + 1, '\0');
    std::vector<uint16_t> used;

    for (int y = 0; y < dims.y; ++y) {
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                const uint16_t cell = model.at({x, y, z});
                if (cell == VoxelModel::kEmpty || cell >= codeFor.size()) continue;
                if (codeFor[cell] != '\0') continue;
                codeFor[cell] = '?';   // marked; the real character is assigned below
                used.push_back(cell);
            }
        }
    }
    std::sort(used.begin(), used.end());

    if (used.size() > kCharsetSize) {
        if (error) {
            *error = "this model uses " + std::to_string(used.size()) +
                     " materials and the layer form has room for " + std::to_string(kCharsetSize) +
                     "; export it as a layer sheet instead";
        }
        return false;
    }

    for (size_t i = 0; i < used.size(); ++i) codeFor[used[i]] = kCharset[i];

    std::string text;
    text.reserve(size_t(dims.x + 8) * size_t(dims.y) * size_t(dims.z) + 1024);

    if (options.header) {
        text += "// Generated by " + options.tool + ". Do not edit by hand: the editor writes\n";
        text += "// this file whole, so the next export throws any change here away.\n";
        text += "//\n";
        text += "// " + std::to_string(dims.x) + " x " + std::to_string(dims.y) + " x " +
                std::to_string(dims.z) + " voxels, " + std::to_string(used.size()) +
                " material" + (used.size() == 1 ? "" : "s") + ".\n";
        text += "#pragma once\n\n";
        text += "#include \"engine/prop/voxelize.hpp\"\n\n";
    }

    text += "inline blocky::VoxelModel " + options.name + "() {\n";
    text += "    using blocky::Vec3;\n";
    text += "    using blocky::VoxelMaterial;\n\n";
    text += "    return blocky::voxelize::fromLayers(\n";
    text += "        {\n";

    for (int y = 0; y < dims.y; ++y) {
        text += "            // y = " + std::to_string(y) + "\n";
        text += "            {\n";
        for (int z = 0; z < dims.z; ++z) {
            std::string row;
            row.reserve(size_t(dims.x));
            for (int x = 0; x < dims.x; ++x) {
                const uint16_t cell = model.at({x, y, z});
                row += (cell == VoxelModel::kEmpty || cell >= codeFor.size()) ? '.' : codeFor[cell];
            }
            text += "                \"" + row + "\"";
            text += (z + 1 < dims.z) ? ",\n" : "\n";
        }
        text += "            }";
        text += (y + 1 < dims.y) ? ",\n" : "\n";
    }

    text += "        },\n";
    text += "        {\n";

    for (size_t i = 0; i < used.size(); ++i) {
        const uint16_t slot = used[i];
        const VoxelMaterial& material = model.material(slot);

        text += "            {'";
        text += codeFor[slot];
        text += "', VoxelMaterial{.albedo = " + vecLiteral(material.albedo);
        text += ", .emission = " + vecLiteral(material.emission);
        text += ",\n                            .roughness = " + literal(material.roughness);
        text += ", .metallic = " + literal(material.metallic) + "}}";
        text += (i + 1 < used.size()) ? "," : "";
        text += "  // " + hexOf(material.albedo) + "\n";
    }

    text += "        });\n";
    text += "}\n";

    out = std::move(text);
    return true;
}

bool emitCanvasSource(const ImageU8& image, std::string& out, const EmitOptions& options,
                      std::string* error) {
    if (image.width() <= 0 || image.height() <= 0) {
        if (error) *error = "the canvas is empty";
        return false;
    }
    if (!isIdentifier(options.name)) {
        if (error) {
            *error = "\"" + options.name +
                     "\" is not a C++ identifier; pass one from identifierFor()";
        }
        return false;
    }

    // Distinct opaque-ish colours, in the order they are first met. Unlike the
    // model, where the palette already fixes an order, an image has none --
    // so reading order is the order, and it is stable because the image is.
    std::vector<uint32_t> colours;
    std::vector<char> codeFor;

    auto pack = [](ImageU8::RGBA c) {
        return uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16) |
               (uint32_t(c.a) << 24);
    };

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const ImageU8::RGBA texel = image.get(x, y);
            if (texel.a == 0) continue;   // transparency is the notation, not a colour

            const uint32_t value = pack(texel);
            if (std::find(colours.begin(), colours.end(), value) == colours.end()) {
                colours.push_back(value);
            }
        }
    }

    if (colours.size() > kCharsetSize) {
        if (error) {
            *error = "this canvas has " + std::to_string(colours.size()) +
                     " colours and the row form has room for " + std::to_string(kCharsetSize) +
                     "; save it as a PNG instead";
        }
        return false;
    }

    codeFor.resize(colours.size());
    for (size_t i = 0; i < colours.size(); ++i) codeFor[i] = kCharset[i];

    std::string text;
    text.reserve(size_t(image.width() + 8) * size_t(image.height()) + 512);

    if (options.header) {
        text += "// Generated by " + options.tool + ". Do not edit by hand: the editor writes\n";
        text += "// this file whole, so the next export throws any change here away.\n";
        text += "//\n";
        text += "// " + std::to_string(image.width()) + " x " + std::to_string(image.height()) +
                " texels, " + std::to_string(colours.size()) + " colour" +
                (colours.size() == 1 ? "" : "s") + ".\n";
        text += "#pragma once\n\n";
        text += "#include \"engine/assets/pixel_art.hpp\"\n\n";
    }

    text += "inline blocky::ImageU8 " + options.name + "() {\n";
    text += "    using blocky::ImageU8;\n\n";
    text += "    return blocky::pixelart::fromRows(\n";
    text += "        {\n";

    for (int y = 0; y < image.height(); ++y) {
        std::string row;
        row.reserve(size_t(image.width()));
        for (int x = 0; x < image.width(); ++x) {
            const ImageU8::RGBA texel = image.get(x, y);
            if (texel.a == 0) {
                row += '.';
                continue;
            }
            const auto found = std::find(colours.begin(), colours.end(), pack(texel));
            row += codeFor[size_t(found - colours.begin())];
        }
        text += "            \"" + row + "\"";
        text += (y + 1 < image.height()) ? ",\n" : "\n";
    }

    text += "        },\n";
    text += "        {\n";

    for (size_t i = 0; i < colours.size(); ++i) {
        const uint32_t value = colours[i];
        const int r = int(value & 0xFFu), g = int((value >> 8) & 0xFFu);
        const int b = int((value >> 16) & 0xFFu), a = int((value >> 24) & 0xFFu);

        char line[128];
        std::snprintf(line, sizeof(line), "            {'%c', ImageU8::RGBA{%d, %d, %d, %d}}",
                      codeFor[i], r, g, b, a);
        text += line;
        text += (i + 1 < colours.size()) ? "," : "";

        // The hex a designer would recognise. Straight sRGB here, unlike the
        // model's linear floats: an ImageU8 *is* what goes on disk, so there
        // is nothing to convert and nothing to lose.
        std::snprintf(line, sizeof(line), "  // #%02x%02x%02x\n", r, g, b);
        text += line;
    }

    text += "        });\n";
    text += "}\n";

    out = std::move(text);
    return true;
}

bool emitSpriteSource(const std::vector<Sprite>& sprites, std::string& out,
                      const EmitOptions& options, std::string* error) {
    if (sprites.empty()) {
        if (error) *error = "there are no sprites";
        return false;
    }
    if (!isIdentifier(options.name)) {
        if (error) {
            *error = "\"" + options.name +
                     "\" is not a C++ identifier; pass one from identifierFor()";
        }
        return false;
    }

    std::string text;
    text.reserve(sprites.size() * 320 + 512);

    if (options.header) {
        text += "// Generated by " + options.tool + ". Do not edit by hand: the editor writes\n";
        text += "// this file whole, so the next export throws any change here away.\n";
        text += "//\n";
        text += "// " + std::to_string(sprites.size()) + " sprite" +
                (sprites.size() == 1 ? "" : "s") + ".\n";
        text += "#pragma once\n\n";
        text += "#include \"engine/sprite/sprite.hpp\"\n\n";
        text += "#include <vector>\n\n";
    }

    // The texture is a parameter rather than a field, because it points at
    // something the scene owns and a generated file cannot name it.
    text += "inline std::vector<blocky::Sprite> " + options.name +
            "(const blocky::Texture* texture = nullptr) {\n";
    text += "    using blocky::Sprite;\n";
    text += "    using blocky::Vec2;\n";
    text += "    using blocky::Vec3;\n\n";
    text += "    std::vector<Sprite> sprites;\n";
    text += "    sprites.reserve(" + std::to_string(sprites.size()) + ");\n";

    for (const Sprite& sprite : sprites) {
        text += "\n    {\n";
        text += "        Sprite sprite;\n";
        text += "        sprite.position = Vec3{" + literal(sprite.position.x) + ", " +
                literal(sprite.position.y) + ", " + literal(sprite.position.z) + "};\n";
        text += "        sprite.size = Vec2{" + literal(sprite.size.x) + ", " +
                literal(sprite.size.y) + "};\n";

        // Only what differs from a default `Sprite`, so a line that is there
        // is a decision somebody made rather than noise to read past.
        const Sprite plain;
        if (sprite.yawDegrees != plain.yawDegrees) {
            text += "        sprite.yawDegrees = " + literal(sprite.yawDegrees) + ";\n";
        }
        if (sprite.pitchDegrees != plain.pitchDegrees) {
            text += "        sprite.pitchDegrees = " + literal(sprite.pitchDegrees) + ";\n";
        }
        if (sprite.rollDegrees != plain.rollDegrees) {
            text += "        sprite.rollDegrees = " + literal(sprite.rollDegrees) + ";\n";
        }
        if (!(sprite.tint == plain.tint)) {
            text += "        sprite.tint = " + vecLiteral(sprite.tint) + ";  // " +
                    hexOf(sprite.tint) + "\n";
        }
        if (!(sprite.emission == plain.emission)) {
            text += "        sprite.emission = " + vecLiteral(sprite.emission) + ";\n";
        }
        if (sprite.roughness != plain.roughness) {
            text += "        sprite.roughness = " + literal(sprite.roughness) + ";\n";
        }
        if (sprite.alphaCutoff != plain.alphaCutoff) {
            text += "        sprite.alphaCutoff = " + literal(sprite.alphaCutoff) + ";\n";
        }
        if (sprite.doubleSided != plain.doubleSided) {
            text += std::string("        sprite.doubleSided = ") +
                    (sprite.doubleSided ? "true" : "false") + ";\n";
        }
        // Component by component: `Vec2` has no equality operator, and adding
        // one to the maths header to serve a printer would be the tail
        // wagging the dog.
        const bool sameUv = sprite.uvMin.x == plain.uvMin.x && sprite.uvMin.y == plain.uvMin.y &&
                            sprite.uvMax.x == plain.uvMax.x && sprite.uvMax.y == plain.uvMax.y;
        if (!sameUv) {
            text += "        sprite.uvMin = Vec2{" + literal(sprite.uvMin.x) + ", " +
                    literal(sprite.uvMin.y) + "};\n";
            text += "        sprite.uvMax = Vec2{" + literal(sprite.uvMax.x) + ", " +
                    literal(sprite.uvMax.y) + "};\n";
        }

        text += "        sprite.texture = texture;\n";
        text += "        sprites.push_back(sprite);\n";
        text += "    }\n";
    }

    text += "\n    return sprites;\n";
    text += "}\n";

    out = std::move(text);
    return true;
}

ImageU8 emitLayerSheet(const VoxelModel& model, int columns) {
    const IVec3 dims = model.dims();
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return ImageU8();

    const int cols = gridColumns(dims.y, columns);
    const int rows = (dims.y + cols - 1) / cols;

    ImageU8 sheet(cols * dims.x, rows * dims.z);

    for (int y = 0; y < dims.y; ++y) {
        const int originX = (y % cols) * dims.x;
        const int originY = (y / cols) * dims.z;

        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                const uint16_t cell = model.at({x, y, z});
                if (cell == VoxelModel::kEmpty) continue;   // left transparent

                const Vec3 encoded = linearToSrgb(model.material(cell).albedo);
                auto channel = [](float c) {
                    const float clamped = std::min(1.0f, std::max(0.0f, c));
                    return uint8_t(std::lround(clamped * 255.0f));
                };
                sheet.set(originX + x, originY + z,
                          {channel(encoded.x), channel(encoded.y), channel(encoded.z), 255});
            }
        }
    }
    return sheet;
}

bool readLayerSheet(const ImageU8& sheet, IVec3 dims, VoxelModel& out, int columns,
                    std::string* error) {
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) {
        if (error) *error = "the dimensions are empty";
        return false;
    }

    const int cols = gridColumns(dims.y, columns);
    const int rows = (dims.y + cols - 1) / cols;

    if (sheet.width() != cols * dims.x || sheet.height() != rows * dims.z) {
        if (error) {
            *error = "the sheet is " + std::to_string(sheet.width()) + "x" +
                     std::to_string(sheet.height()) + ", and " + std::to_string(dims.x) + "x" +
                     std::to_string(dims.y) + "x" + std::to_string(dims.z) + " needs " +
                     std::to_string(cols * dims.x) + "x" + std::to_string(rows * dims.z);
        }
        return false;
    }

    out.resize(dims);

    for (int y = 0; y < dims.y; ++y) {
        const int originX = (y % cols) * dims.x;
        const int originY = (y / cols) * dims.z;

        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                const ImageU8::RGBA pixel = sheet.get(originX + x, originY + z);
                if (pixel.a < 128) continue;

                VoxelMaterial material;
                material.albedo = srgbToLinear(Vec3{float(pixel.r) / 255.0f,
                                                    float(pixel.g) / 255.0f,
                                                    float(pixel.b) / 255.0f});
                // Folding does the deduplication: a sheet of four colours
                // comes back as four palette slots however many texels use
                // each one.
                out.set({x, y, z}, out.addMaterial(material));
            }
        }
    }
    return true;
}

std::string sheetFileName(const std::string& stem, IVec3 dims) {
    return stem + "_" + std::to_string(dims.x) + "x" + std::to_string(dims.y) + "x" +
           std::to_string(dims.z) + ".png";
}

bool parseSheetFileName(const std::string& path, IVec3& dims) {
    // Take the name off the path first: a directory called `models_2x2x2`
    // must not answer for the file inside it.
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);

    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);

    const size_t underscore = name.find_last_of('_');
    if (underscore == std::string::npos) return false;

    const std::string tail = name.substr(underscore + 1);

    int x = 0, y = 0, z = 0;
    char extra = '\0';
    // The trailing %c is the check that nothing follows: "8x8x8" parses, and
    // "8x8x8x8" is refused rather than quietly read as the first three.
    if (std::sscanf(tail.c_str(), "%dx%dx%d%c", &x, &y, &z, &extra) != 3) return false;
    if (x <= 0 || y <= 0 || z <= 0) return false;

    dims = {x, y, z};
    return true;
}

} // namespace edit
} // namespace blocky
