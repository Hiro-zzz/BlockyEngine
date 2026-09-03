#include "engine/render/gpu/block_textures_gpu.hpp"

#include <vector>

namespace blocky {
namespace gpu {

BlockTextureArray::~BlockTextureArray() { destroy(); }

bool BlockTextureArray::build(const BlockRegistry& registry, const BlockTextureLibrary* library) {
    destroy();

    constexpr int kSize = 16;   // every block texture in the game is 16x16
    layers_ = int(registry.size()) * FaceCount;

    std::vector<float> texels(size_t(kSize) * kSize * 4 * size_t(layers_), 0.0f);

    for (BlockId id = 0; id < BlockId(registry.size()); ++id) {
        const BlockDef& def = registry[id];

        for (int face = 0; face < FaceCount; ++face) {
            // The same fallback surfaceAlbedo() computes, and for the same
            // reason: a block with no texture is its palette colour, and the
            // top of a tinted block is a different colour from its sides.
            const Vec3 flat = (def.tintTop && face == FacePosY) ? def.topTint : def.albedo;

            const size_t layerOffset = size_t(layerFor(id, face)) * size_t(kSize) * kSize * 4;

            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    const Vec2 uv{(float(x) + 0.5f) / float(kSize),
                                  (float(y) + 0.5f) / float(kSize)};

                    // Transmissive blocks go through unchanged. The tracer
                    // refracts at them and never reads this value; baking the
                    // viewport's preview colour here would put a number in
                    // the array that the CPU would never produce.
                    const Vec3 linear = library ? library->sampleAlbedo(id, face, uv, flat) : flat;

                    const size_t i = layerOffset + (size_t(y) * kSize + size_t(x)) * 4;
                    texels[i + 0] = linear.x;
                    texels[i + 1] = linear.y;
                    texels[i + 2] = linear.z;
                    texels[i + 3] = 1.0f;
                }
            }
        }
    }

    gl::GenTextures(1, &texture_);
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, texture_);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexImage3D(gl::TEXTURE_2D_ARRAY, 0, gl::RGBA32F, kSize, kSize, layers_, 0, gl::RGBA,
                   gl::FLOAT, texels.data());

    // Nearest both ways and no mipmaps: the shader will ask for level zero
    // explicitly, because a compute shader has no derivatives to pick one.
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_R, gl::CLAMP_TO_EDGE);
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, 0);

    return gl::GetError() == gl::NO_ERROR_;
}

void BlockTextureArray::bind(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, texture_);
}

void BlockTextureArray::destroy() {
    if (texture_) {
        gl::DeleteTextures(1, &texture_);
        texture_ = 0;
    }
    layers_ = 0;
}

} // namespace gpu
} // namespace blocky
