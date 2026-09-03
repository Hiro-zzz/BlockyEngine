#pragma once
// Block textures for the GPU tracer.
//
// The viewport already builds a texture array with the same layout -- layer
// `id * 6 + face`, tint and grass overlay composited in -- and it would be
// tidy to hand that one to the tracer as well. It cannot be: the two want
// different things, in three ways that all matter.
//
//   * The viewport substitutes a *preview* colour for water and glass, the
//     shade they would be after a few blocks of travel, because a rasterizer
//     cannot trace a medium. A path tracer can, and never wants that value.
//   * The viewport stores eight-bit sRGB. The CPU tracer samples a float
//     texture and multiplies the tint in linear light, so eight bits after
//     the tint is a quantisation the comparison would have to forgive.
//   * The viewport mipmaps, to stop distant blocks shimmering. A compute
//     shader has no derivatives, so its LOD would be undefined -- and the CPU
//     samples the full-resolution image regardless.
//
// So this builds its own array: RGBA32F, linear, no mipmaps. What it does
// share is the one thing that must not drift -- every texel comes out of the
// same `BlockTextureLibrary::sampleAlbedo` the CPU tracer calls, with the
// same flat fallback `surfaceAlbedo` would use. The two arrays cannot mean
// different things because neither decides anything.
//
// A null library is not a special case. Every layer then bakes the block's
// flat palette colour, with the top-face tint applied where the block asks
// for one, which is exactly what the CPU falls back to. So the shader has one
// path and no branch: it always samples.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/world/block.hpp"

namespace blocky {
namespace gpu {

class BlockTextureArray {
public:
    ~BlockTextureArray();
    BlockTextureArray() = default;
    BlockTextureArray(const BlockTextureArray&) = delete;
    BlockTextureArray& operator=(const BlockTextureArray&) = delete;

    // `library` may be null; see the note above.
    bool build(const BlockRegistry& registry, const BlockTextureLibrary* library);
    void destroy();
    void bind(int unit) const;

    bool valid() const { return texture_ != 0; }
    int  layerCount() const { return layers_; }

    static int layerFor(BlockId id, int face) { return int(id) * FaceCount + face; }

private:
    gl::GLuint texture_ = 0;
    int layers_ = 0;
};

} // namespace gpu
} // namespace blocky
