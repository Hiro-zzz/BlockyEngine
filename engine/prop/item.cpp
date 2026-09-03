#include "engine/prop/item.hpp"

#include <algorithm>
#include <vector>

namespace blocky {
namespace item {

void buildModel(const Texture& texture, VoxelModel& out, const ItemOptions& options) {
    out.clear();
    if (texture.empty()) return;

    const int width = texture.width();
    const int height = texture.height();
    const int depth = std::max(1, options.depthTexels);

    out.resize({width, height, depth});

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (texture.alphaAt(x, y) < options.alphaCutoff) continue;

            VoxelMaterial material;
            material.albedo = texture.texel(x, y);  // already linear
            material.emission = options.emission;
            material.roughness = options.roughness;
            material.metallic = options.metallic;
            uint16_t slot = out.addMaterial(material);

            // Image v runs down, the model's +Y runs up.
            const int modelY = height - 1 - y;
            for (int z = 0; z < depth; ++z) out.set({x, modelY, z}, slot);
        }
    }

    if (options.trim) out.trim();
}

bool loadModel(const AssetSource& source, const std::string& path, VoxelModel& out,
               const ItemOptions& options, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!source.read(path, bytes, error)) return false;

    Texture texture;
    if (!texture.loadFromPng(bytes.data(), bytes.size(), error)) return false;

    // Some items are stored as a vertical strip of animation frames; the first
    // one is what a still render wants.
    texture.cropToFirstSquareFrame();

    buildModel(texture, out, options);
    if (out.empty()) {
        if (error) *error = "item: texture has no opaque texels: " + path;
        return false;
    }
    return true;
}

bool loadByName(const AssetSource& source, const std::string& name, VoxelModel& out,
                const ItemOptions& options, std::string* error) {
    return loadModel(source, texturePath(name), out, options, error);
}

} // namespace item
} // namespace blocky
