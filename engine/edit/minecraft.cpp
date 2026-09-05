#include "engine/edit/minecraft.hpp"

#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/prop/voxel_boxes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace blocky {
namespace edit {

namespace {

// The palette texture: a 16x16 grid of 4x4 blocks.
constexpr int kPaletteCells = 16;            // per side, so 256 materials
constexpr int kCellTexels = 4;               // per side of one colour's block
constexpr int kPaletteSize = kPaletteCells * kCellTexels;

// A model must fit within -16..32, and this one starts at the origin.
constexpr int kMaxSide = 32;

bool isResourceName(const std::string& text) {
    if (text.empty()) return false;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.';
        if (!ok) return false;
    }
    return true;
}

// Trailing zeros off a number, so an integer coordinate reads as one. The
// files are read by people as often as by the game.
std::string number(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4f", double(value));

    std::string text(buffer);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text.empty() ? "0" : text;
}

// Which cell of the palette grid a material sits in, and the UV rectangle
// that reads it. Vanilla UVs are 0..16 across the whole texture whatever its
// resolution, so one grid cell is exactly one UV unit.
void paletteCell(int slot, int& cellX, int& cellY) {
    cellX = slot % kPaletteCells;
    cellY = slot / kPaletteCells;
}

// Is the whole face of this box buried? Only then may it be dropped: a face
// half covered still has to be drawn, and the boxes are a partition so
// "covered" means every cell just outside it is solid.
bool faceIsBuried(const VoxelModel& model, const VoxelBox& box, int axis, bool positive) {
    IVec3 lo = box.min, hi = box.max;
    int plane = positive ? hi[axis] : lo[axis] - 1;

    // Off the grid is open air, always visible.
    if (plane < 0 || plane >= model.dims()[axis]) return false;

    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    for (int ia = lo[a]; ia < hi[a]; ++ia) {
        for (int ib = lo[b]; ib < hi[b]; ++ib) {
            IVec3 cell{};
            cell[axis] = plane;
            cell[a] = ia;
            cell[b] = ib;
            if (model.at(cell) == VoxelModel::kEmpty) return false;
        }
    }
    return true;
}

const char* kFaceNames[3][2] = {
    {"west", "east"},     // -X, +X
    {"down", "up"},       // -Y, +Y
    {"north", "south"},   // -Z, +Z
};

// Vanilla `minecraft:block/block`. Right for a block-shaped item in a hand
// and in the inventory; a real block takes its display from the blockstate,
// which is why this is optional.
const char* kDisplayBlock = R"JSON(  "display": {
    "thirdperson_righthand": {"rotation": [75, 45, 0], "translation": [0, 2.5, 0], "scale": [0.375, 0.375, 0.375]},
    "thirdperson_lefthand": {"rotation": [75, 45, 0], "translation": [0, 2.5, 0], "scale": [0.375, 0.375, 0.375]},
    "firstperson_righthand": {"rotation": [0, 45, 0], "translation": [0, 0, 0], "scale": [0.4, 0.4, 0.4]},
    "firstperson_lefthand": {"rotation": [0, 225, 0], "translation": [0, 0, 0], "scale": [0.4, 0.4, 0.4]},
    "ground": {"rotation": [0, 0, 0], "translation": [0, 3, 0], "scale": [0.25, 0.25, 0.25]},
    "gui": {"rotation": [30, 225, 0], "translation": [0, 0, 0], "scale": [0.625, 0.625, 0.625]},
    "head": {"rotation": [0, 0, 0], "translation": [0, 0, 0], "scale": [1, 1, 1]},
    "fixed": {"rotation": [0, 0, 0], "translation": [0, 0, 0], "scale": [0.5, 0.5, 0.5]}
  },
)JSON";

// The materials the model actually uses, in palette order, and where each one
// lands in the texture. Slot zero of the model is emptiness and never appears.
std::vector<uint16_t> usedMaterials(const VoxelModel& model) {
    std::vector<uint16_t> used;
    std::vector<uint8_t> seen(model.materialCount() + 1, 0);

    const IVec3 dims = model.dims();
    for (int y = 0; y < dims.y; ++y)
        for (int z = 0; z < dims.z; ++z)
            for (int x = 0; x < dims.x; ++x) {
                const uint16_t cell = model.at({x, y, z});
                if (cell == VoxelModel::kEmpty || cell >= seen.size() || seen[cell]) continue;
                seen[cell] = 1;
                used.push_back(cell);
            }

    std::sort(used.begin(), used.end());
    return used;
}

} // namespace

std::string resourceName(const std::string& title) {
    std::string name;
    bool lastWasBreak = false;

    for (char c : title) {
        const char lower = char(std::tolower(uint8_t(c)));
        const bool legal = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
                           lower == '_' || lower == '-' || lower == '.';
        if (legal) {
            name += lower;
            lastWasBreak = false;
        } else if (!lastWasBreak && !name.empty()) {
            // Runs of illegal characters collapse: "iron  --  mug" is
            // `iron_mug`, not `iron______mug`.
            name += '_';
            lastWasBreak = true;
        }
    }
    while (!name.empty() && name.back() == '_') name.pop_back();
    return name.empty() ? "model" : name;
}

ImageU8 buildMinecraftPalette(const VoxelModel& model) {
    ImageU8 palette(kPaletteSize, kPaletteSize);

    const std::vector<uint16_t> used = usedMaterials(model);
    for (size_t i = 0; i < used.size() && i < size_t(kPaletteCells * kPaletteCells); ++i) {
        const Vec3 encoded = linearToSrgb(model.material(used[i]).albedo);
        auto channel = [](float c) {
            const float clamped = std::min(1.0f, std::max(0.0f, c));
            return uint8_t(std::lround(clamped * 255.0f));
        };
        const ImageU8::RGBA colour{channel(encoded.x), channel(encoded.y), channel(encoded.z), 255};

        int cellX = 0, cellY = 0;
        paletteCell(int(i), cellX, cellY);

        for (int ty = 0; ty < kCellTexels; ++ty)
            for (int tx = 0; tx < kCellTexels; ++tx)
                palette.set(cellX * kCellTexels + tx, cellY * kCellTexels + ty, colour);
    }
    return palette;
}

bool buildMinecraftModel(const VoxelModel& model, std::string& json,
                         const McExportOptions& options, McStats* stats, std::string* error) {
    if (model.empty()) {
        if (error) *error = "the model is empty";
        return false;
    }

    const IVec3 dims = model.dims();
    if (dims.x > kMaxSide || dims.y > kMaxSide || dims.z > kMaxSide) {
        if (error) {
            *error = "a Minecraft model must fit within -16..32, and this one is " +
                     std::to_string(dims.x) + "x" + std::to_string(dims.y) + "x" +
                     std::to_string(dims.z) + "; trim it or build it smaller";
        }
        return false;
    }

    if (!isResourceName(options.space) || !isResourceName(options.name)) {
        if (error) {
            *error = "\"" + options.space + ":" + options.name +
                     "\" is not a resource name; pass names from resourceName()";
        }
        return false;
    }

    const std::vector<uint16_t> used = usedMaterials(model);
    if (used.size() > size_t(kPaletteCells * kPaletteCells)) {
        if (error) {
            *error = "this model uses " + std::to_string(used.size()) +
                     " materials and the palette texture holds " +
                     std::to_string(kPaletteCells * kPaletteCells);
        }
        return false;
    }

    // Material slot to palette index, so a face can find its texel.
    std::vector<int> slotOf(model.materialCount() + 1, -1);
    for (size_t i = 0; i < used.size(); ++i) slotOf[used[i]] = int(i);

    const std::vector<VoxelBox> boxes = mergeVoxelBoxes(model, BoxMerge::ByMaterial);

    const std::string reference = options.space + ":" + options.folder + "/" + options.name;

    std::string text;
    text.reserve(boxes.size() * 320 + 1024);

    text += "{\n";
    text += "  \"credit\": \"" + options.description + "\",\n";
    text += "  \"textures\": {\n";
    text += "    \"0\": \"" + reference + "\",\n";
    text += "    \"particle\": \"" + reference + "\"\n";
    text += "  },\n";

    if (options.display) text += kDisplayBlock;

    text += "  \"elements\": [\n";

    int faceCount = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const VoxelBox& box = boxes[i];

        int cellX = 0, cellY = 0;
        const int slot = box.material < slotOf.size() ? slotOf[box.material] : -1;
        paletteCell(slot < 0 ? 0 : slot, cellX, cellY);

        const std::string uv = "[" + number(float(cellX)) + ", " + number(float(cellY)) + ", " +
                               number(float(cellX + 1)) + ", " + number(float(cellY + 1)) + "]";

        text += "    {\n";
        text += "      \"from\": [" + number(float(box.min.x)) + ", " + number(float(box.min.y)) +
                ", " + number(float(box.min.z)) + "],\n";
        text += "      \"to\": [" + number(float(box.max.x)) + ", " + number(float(box.max.y)) +
                ", " + number(float(box.max.z)) + "],\n";
        text += "      \"faces\": {\n";

        // Buried faces are dropped. The boxes are a partition, so a face is
        // buried exactly when every cell just outside it is solid -- and a
        // face that is only half covered still has to be drawn, which is why
        // this is a test over the whole rectangle rather than a corner.
        std::vector<std::string> faces;
        for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
                if (faceIsBuried(model, box, axis, side == 1)) continue;
                faces.push_back(std::string("        \"") + kFaceNames[axis][side] +
                                "\": {\"uv\": " + uv + ", \"texture\": \"#0\"}");
                ++faceCount;
            }
        }

        for (size_t f = 0; f < faces.size(); ++f) {
            text += faces[f];
            text += (f + 1 < faces.size()) ? ",\n" : "\n";
        }

        text += "      }\n";
        text += "    }";
        text += (i + 1 < boxes.size()) ? ",\n" : "\n";
    }

    text += "  ]\n";
    text += "}\n";

    if (stats) {
        stats->elements = int(boxes.size());
        stats->faces = faceCount;
        stats->materials = int(used.size());
    }

    json = std::move(text);
    return true;
}

bool writeMinecraftPack(const VoxelModel& model, const std::string& root,
                        const McExportOptions& options, McStats* stats, std::string* error) {
    std::string json;
    if (!buildMinecraftModel(model, json, options, stats, error)) return false;

    const std::string assets = root + "/assets/" + options.space;
    const std::string modelPath = assets + "/models/" + options.folder + "/" + options.name + ".json";
    const std::string texturePath =
        assets + "/textures/" + options.folder + "/" + options.name + ".png";

    if (!createParentDirectories(modelPath) || !createParentDirectories(texturePath)) {
        if (error) *error = "could not make the pack directories under " + root;
        return false;
    }

    if (!writeFileBytes(modelPath, reinterpret_cast<const uint8_t*>(json.data()), json.size(),
                        error)) {
        return false;
    }
    if (!pngSave(texturePath, buildMinecraftPalette(model), error)) return false;

    // Without this the folder is a pile of files; with it the game will load
    // it. The format number is the caller's, because it belongs to a game
    // version nothing here can know.
    const std::string meta = "{\n  \"pack\": {\n    \"pack_format\": " +
                             std::to_string(options.packFormat) + ",\n    \"description\": \"" +
                             options.description + "\"\n  }\n}\n";

    return writeFileBytes(root + "/pack.mcmeta", reinterpret_cast<const uint8_t*>(meta.data()),
                          meta.size(), error);
}

} // namespace edit
} // namespace blocky
