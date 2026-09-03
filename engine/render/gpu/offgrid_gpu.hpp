#pragma once
// Sprites and props for the GPU tracer: the two things that live off the
// voxel lattice.
//
// They are packed together because they need the same thing and nothing else
// does -- a BVH. The world does not need one, its chunk grid already is one;
// the entity boxes do not, sixty of them scan faster than a tree walks. These
// two hang in open air and there are thousands of them.
//
// As with the entity boxes, nothing is flattened twice. `SpriteSet::flats()`
// and `PropSet::flats()` hand over the placements the CPU walks, and
// `Bvh::nodes()` the tree it walks them with. The tree is not rebuilt here
// either: a second build could pick a different split and then the two
// renderers would be disagreeing about which primitives a node owns, which is
// not a thing a picture shows you.
//
// One liberty is taken. `Bvh` leaves hold indices into its `order` array, so
// the primitives are uploaded *permuted* by that array -- then a leaf's range
// indexes the buffer directly and the order array does not have to travel.
// That is not a shortcut; it is one fewer of the sixteen storage blocks a
// compute stage is allowed, and at this point every one is spoken for.
#include "engine/prop/prop_set.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/sprite/sprite_set.hpp"

#include <cstdint>

namespace blocky {
namespace gpu {

struct GpuBvhNode {
    float lo[4];
    float hi[4];
    uint32_t meta[4];   // start, count, right child, split axis
};

struct GpuQuad {
    float toWorld[16];
    float toLocal[16];
    float halfSided[4];    // half.xy, double sided, alpha cutoff
    float uvMinMax[4];
    float tintRough[4];
    float emissionTex[4];  // emission.rgb, texture layer or -1
    float texSize[4];      // the real size inside the padded layer
};

struct GpuGrid {
    float toWorld[16];
    float toLocal[16];
    int32_t dimsVoxels[4];    // dims.xyz, offset into the voxel word pool
    int32_t materialBase[4];  // x: offset into the material pool
    float tint[4];
    float emissionScale[4];
};

struct GpuVoxMaterial {
    float albedoRough[4];
    float emissionMetal[4];
};

class OffGridData {
public:
    ~OffGridData();
    OffGridData() = default;
    OffGridData(const OffGridData&) = delete;
    OffGridData& operator=(const OffGridData&) = delete;

    // Either may be null or empty; the shader is then told there is no tree
    // and skips the whole thing on the first line.
    bool upload(const SpriteSet* sprites, const PropSet* props);

    void destroy();
    void bind() const;              // storage blocks 16..21
    void bindSpriteTextures(int unit) const;

    int spriteNodeCount() const { return spriteNodes_; }
    int propNodeCount() const { return propNodes_; }
    size_t bytes() const { return bytes_; }

private:
    gl::GLuint spriteNodeBuf_ = 0, quadBuf_ = 0;
    gl::GLuint propNodeBuf_ = 0, gridBuf_ = 0, voxelBuf_ = 0, voxMatBuf_ = 0;
    gl::GLuint spriteTex_ = 0;

    int spriteNodes_ = 0;
    int propNodes_ = 0;
    size_t bytes_ = 0;
};

} // namespace gpu
} // namespace blocky
