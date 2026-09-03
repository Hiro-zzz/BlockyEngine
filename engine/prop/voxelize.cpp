#include "engine/prop/voxelize.hpp"

#include <algorithm>

namespace blocky {
namespace voxelize {
namespace {

// One colour for the whole voxel. With a texture library that is the mean of
// the six faces, which reads far closer to the real block than the flat
// palette entry does.
Vec3 albedoFor(const BlockDef& def, BlockId id, const BlockTextureLibrary* textures) {
    if (!textures) return def.tintTop ? lerp(def.albedo, def.topTint, 0.25f) : def.albedo;

    Vec3 total{0.0f};
    for (int face = 0; face < 6; ++face) total += textures->averageAlbedo(id, face, def.albedo);
    return total / 6.0f;
}

}  // namespace

VoxelModel fromWorld(const World& world, IVec3 min, IVec3 max, const CaptureOptions& options) {
    VoxelModel model;

    IVec3 lo = minv(min, max);
    IVec3 hi = maxv(min, max);
    IVec3 dims{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return model;

    model.resize(dims);

    const BlockRegistry& registry = world.registry();

    // One palette slot per distinct block, resolved once rather than per voxel.
    std::vector<uint16_t> slotFor(registry.size(), VoxelModel::kEmpty);
    std::vector<bool> resolved(registry.size(), false);

    for (int y = 0; y < dims.y; ++y) {
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                BlockId id = world.get({lo.x + x, lo.y + y, lo.z + z});
                if (id == block::Air) continue;
                if (id >= registry.size()) continue;

                const BlockDef& def = registry[id];
                if (options.skipTransmissive && def.transmissive()) continue;

                if (!resolved[id]) {
                    VoxelMaterial material;
                    material.albedo = albedoFor(def, id, options.textures);
                    material.emission = def.emission;
                    material.roughness = def.roughness;
                    material.metallic = def.metallic;
                    slotFor[id] = model.addMaterial(material);
                    resolved[id] = true;
                }

                model.set({x, y, z}, slotFor[id]);
            }
        }
    }

    if (options.trim) model.trim();
    return model;
}

VoxelModel fromLayers(const std::vector<std::vector<std::string>>& layers,
                      const std::vector<VoxelKey>& key) {
    VoxelModel model;
    if (layers.empty()) return model;

    // The grid is as big as the largest slice; short rows simply stop early.
    int width = 0, depth = 0;
    for (const std::vector<std::string>& slice : layers) {
        depth = std::max(depth, int(slice.size()));
        for (const std::string& row : slice) width = std::max(width, int(row.size()));
    }
    if (width == 0 || depth == 0) return model;

    model.resize({width, int(layers.size()), depth});

    // Resolve the key once; a model drawn as text shares very few materials.
    std::vector<uint16_t> slots(key.size(), VoxelModel::kEmpty);
    for (size_t i = 0; i < key.size(); ++i) slots[i] = model.addMaterial(key[i].material);

    for (int y = 0; y < int(layers.size()); ++y) {
        const std::vector<std::string>& slice = layers[size_t(y)];
        for (int z = 0; z < int(slice.size()); ++z) {
            const std::string& row = slice[size_t(z)];
            for (int x = 0; x < int(row.size()); ++x) {
                char code = row[size_t(x)];
                if (code == '.' || code == ' ') continue;

                for (size_t i = 0; i < key.size(); ++i) {
                    if (key[i].code != code) continue;
                    model.set({x, y, z}, slots[i]);
                    break;
                }
            }
        }
    }
    return model;
}

} // namespace voxelize
} // namespace blocky
