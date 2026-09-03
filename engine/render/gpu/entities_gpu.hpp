#pragma once
// Entity boxes for the GPU tracer.
//
// An entity is already flattened by the time the tracer sees it: `add()`
// resolves the pose through the skeleton and turns the model into a list of
// oriented boxes in world space, each carrying the six skin rectangles that
// clothe it. That flattening is not repeated here -- `EntitySet::boxes()`
// hands over the same boxes the CPU walks, and this only rewrites them into
// the layout a shader can read.
//
// Doing it the other way, rebuilding the boxes from `entities()`, would mean
// two implementations of the same joint chain. They would agree right up
// until one of them did not, and the symptom would be a character with an arm
// in a slightly different place in two renders of the same frame.
//
// The skins go into a texture array, one 64x64 layer each, RGBA32F and
// linear. Float rather than eight-bit sRGB for the same reason the block
// array is: the CPU reads `Texture::texel`, which is already linear, and the
// comparison should not have to forgive a round trip through sRGB. The
// lookups are `texelFetch`, because the CPU indexes texels directly and never
// filters -- a skin is pixel art and bilinear would smear it.
#include "engine/entity/entity.hpp"
#include "engine/render/gl/gl_loader.hpp"

#include <cstdint>
#include <vector>

namespace blocky {
namespace gpu {

// One flattened box, in the shader's std430 layout. 272 bytes; a player is
// twenty-four of them.
struct GpuBox {
    float toLocal[16];
    float toWorld[16];
    float size[3], cutout;
    float aabbMin[3], skinLayer;
    float aabbMax[3], pad;
    int32_t faces[6][4];   // x, y, width, height; width <= 0 means no face
};

class EntityData {
public:
    ~EntityData();
    EntityData() = default;
    EntityData(const EntityData&) = delete;
    EntityData& operator=(const EntityData&) = delete;

    // Packs the set's boxes and gathers every distinct skin into the array.
    // Passing null, or a set with nothing in it, leaves this empty and the
    // shader is told there are no boxes.
    bool upload(const EntitySet* entities);

    void destroy();

    void bindBoxes(gl::GLuint binding) const;
    void bindSkins(int unit) const;

    int boxCount() const { return boxCount_; }
    int skinCount() const { return skinCount_; }
    size_t bytes() const { return size_t(boxCount_) * sizeof(GpuBox); }

private:
    gl::GLuint boxBuf_ = 0;
    gl::GLuint skinTex_ = 0;
    int boxCount_ = 0;
    int skinCount_ = 0;
};

} // namespace gpu
} // namespace blocky
