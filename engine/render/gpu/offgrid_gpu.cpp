#include "engine/render/gpu/offgrid_gpu.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace blocky {
namespace gpu {
namespace {

void copyMatrix(float (&out)[16], const Mat4& m) {
    std::memcpy(out, m.data(), sizeof(float) * 16);
}

void makeBuffer(gl::GLuint& buffer, const void* data, size_t bytes) {
    if (buffer == 0) gl::GenBuffers(1, &buffer);
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, buffer);
    gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(bytes), data, gl::STATIC_DRAW);
}

// A declared storage block with nothing bound to it is asking for trouble
// even when the shader never reads it, so every buffer gets at least one
// element.
template <class T>
void makeBufferOrStub(gl::GLuint& buffer, std::vector<T>& data, size_t& total) {
    if (data.empty()) data.resize(1, T{});
    makeBuffer(buffer, data.data(), data.size() * sizeof(T));
    total += data.size() * sizeof(T);
}

std::vector<GpuBvhNode> packNodes(const Bvh& bvh) {
    std::vector<GpuBvhNode> out(bvh.nodes().size());
    for (size_t i = 0; i < bvh.nodes().size(); ++i) {
        const Bvh::Node& n = bvh.nodes()[i];
        GpuBvhNode& g = out[i];
        g.lo[0] = n.lo.x; g.lo[1] = n.lo.y; g.lo[2] = n.lo.z; g.lo[3] = 0.0f;
        g.hi[0] = n.hi.x; g.hi[1] = n.hi.y; g.hi[2] = n.hi.z; g.hi[3] = 0.0f;
        g.meta[0] = n.start;
        g.meta[1] = n.count;
        g.meta[2] = n.right;
        g.meta[3] = n.axis;
    }
    return out;
}

} // namespace

OffGridData::~OffGridData() { destroy(); }

bool OffGridData::upload(const SpriteSet* sprites, const PropSet* props) {
    destroy();
    bytes_ = 0;

    // ------------------------------------------------------------- sprites
    std::vector<GpuBvhNode> spriteNodes;
    std::vector<GpuQuad> quads;
    std::vector<const Texture*> atlases;
    std::unordered_map<const Texture*, int> layerOf;
    int atlasW = 1, atlasH = 1;

    if (sprites && !sprites->empty() && !sprites->bvh().empty()) {
        spriteNodes = packNodes(sprites->bvh());

        for (const Sprite& s : sprites->sprites()) {
            if (!s.texture || s.texture->empty()) continue;
            if (layerOf.find(s.texture) != layerOf.end()) continue;
            layerOf[s.texture] = int(atlases.size());
            atlases.push_back(s.texture);
            atlasW = std::max(atlasW, s.texture->width());
            atlasH = std::max(atlasH, s.texture->height());
        }

        // Permuted by the tree's order, so a leaf indexes this array directly.
        const std::vector<uint32_t>& order = sprites->bvh().order();
        quads.resize(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            const uint32_t src = order[i];
            const SpriteSet::Flat& flat = sprites->flats()[src];
            const Sprite& s = sprites->sprites()[src];
            GpuQuad& q = quads[i];

            copyMatrix(q.toWorld, flat.toWorld);
            copyMatrix(q.toLocal, flat.toLocal);

            q.halfSided[0] = flat.half.x;
            q.halfSided[1] = flat.half.y;
            q.halfSided[2] = s.doubleSided ? 1.0f : 0.0f;
            q.halfSided[3] = s.alphaCutoff;

            q.uvMinMax[0] = s.uvMin.x; q.uvMinMax[1] = s.uvMin.y;
            q.uvMinMax[2] = s.uvMax.x; q.uvMinMax[3] = s.uvMax.y;

            q.tintRough[0] = s.tint.x; q.tintRough[1] = s.tint.y; q.tintRough[2] = s.tint.z;
            q.tintRough[3] = s.roughness;

            q.emissionTex[0] = s.emission.x;
            q.emissionTex[1] = s.emission.y;
            q.emissionTex[2] = s.emission.z;

            const auto found = s.texture ? layerOf.find(s.texture) : layerOf.end();
            q.emissionTex[3] = found == layerOf.end() ? -1.0f : float(found->second);

            q.texSize[0] = s.texture ? float(s.texture->width()) : 0.0f;
            q.texSize[1] = s.texture ? float(s.texture->height()) : 0.0f;
            q.texSize[2] = q.texSize[3] = 0.0f;
        }
    }
    spriteNodes_ = int(spriteNodes.size());

    // The atlases differ in size -- the game font is 128x128, a particle
    // sheet is 16x16 -- and a texture array will not have that. So every
    // layer is the largest of them and each sprite carries its own real
    // size, which the shader indexes by rather than normalising through.
    {
        const int layers = int(atlases.empty() ? 1 : atlases.size());
        std::vector<float> texels(size_t(atlasW) * size_t(atlasH) * 4 * size_t(layers), 0.0f);
        for (size_t l = 0; l < atlases.size(); ++l) {
            const Texture& t = *atlases[l];
            const size_t base = l * size_t(atlasW) * size_t(atlasH) * 4;
            for (int y = 0; y < t.height(); ++y) {
                for (int x = 0; x < t.width(); ++x) {
                    const Vec3 c = t.texel(x, y);
                    const size_t i = base + (size_t(y) * size_t(atlasW) + size_t(x)) * 4;
                    texels[i + 0] = c.x;
                    texels[i + 1] = c.y;
                    texels[i + 2] = c.z;
                    texels[i + 3] = t.alphaAt(x, y);
                }
            }
        }

        gl::GenTextures(1, &spriteTex_);
        gl::BindTexture(gl::TEXTURE_2D_ARRAY, spriteTex_);
        gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        gl::TexImage3D(gl::TEXTURE_2D_ARRAY, 0, gl::RGBA32F, atlasW, atlasH, layers, 0, gl::RGBA,
                       gl::FLOAT, texels.data());
        gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
        gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
        gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
        gl::BindTexture(gl::TEXTURE_2D_ARRAY, 0);
        bytes_ += texels.size() * sizeof(float);
    }

    // --------------------------------------------------------------- props
    std::vector<GpuBvhNode> propNodes;
    std::vector<GpuGrid> grids;
    std::vector<uint32_t> voxelWords;
    std::vector<GpuVoxMaterial> voxMaterials;

    if (props && !props->empty() && !props->bvh().empty()) {
        propNodes = packNodes(props->bvh());

        // One copy of each distinct model, however many props place it.
        struct ModelSlot { int voxelOffset; int materialOffset; };
        std::unordered_map<const VoxelModel*, ModelSlot> slotOf;

        const std::vector<uint32_t>& order = props->bvh().order();
        grids.resize(order.size());

        for (size_t i = 0; i < order.size(); ++i) {
            const PropSet::Flat& flat = props->flats()[order[i]];
            GpuGrid& g = grids[i];

            copyMatrix(g.toWorld, flat.toWorld);
            copyMatrix(g.toLocal, flat.toLocal);

            ModelSlot slot{0, 0};
            if (flat.model && !flat.model->empty()) {
                const auto found = slotOf.find(flat.model);
                if (found != slotOf.end()) {
                    slot = found->second;
                } else {
                    slot.voxelOffset = int(voxelWords.size());
                    slot.materialOffset = int(voxMaterials.size());

                    const IVec3 dims = flat.model->dims();
                    const size_t count = size_t(dims.x) * size_t(dims.y) * size_t(dims.z);
                    // Two material indices to a word, exactly as the block
                    // pool packs BlockIds.
                    for (size_t v = 0; v < count; v += 2) {
                        const IVec3 a{int(v % size_t(dims.x)),
                                      int(v / (size_t(dims.x) * size_t(dims.z))),
                                      int((v / size_t(dims.x)) % size_t(dims.z))};
                        const size_t w = v + 1;
                        const IVec3 b{int(w % size_t(dims.x)),
                                      int(w / (size_t(dims.x) * size_t(dims.z))),
                                      int((w / size_t(dims.x)) % size_t(dims.z))};
                        const uint32_t lo = flat.model->at(a);
                        const uint32_t hi = w < count ? flat.model->at(b) : 0u;
                        voxelWords.push_back(lo | (hi << 16));
                    }

                    // Index zero is the empty material and is never read, but
                    // it has to be there so a stored index means the same
                    // number on both sides.
                    for (size_t mi = 0; mi <= flat.model->materialCount(); ++mi) {
                        const VoxelMaterial& m = flat.model->material(uint16_t(mi));
                        GpuVoxMaterial gm{};
                        gm.albedoRough[0] = m.albedo.x;
                        gm.albedoRough[1] = m.albedo.y;
                        gm.albedoRough[2] = m.albedo.z;
                        gm.albedoRough[3] = m.roughness;
                        gm.emissionMetal[0] = m.emission.x;
                        gm.emissionMetal[1] = m.emission.y;
                        gm.emissionMetal[2] = m.emission.z;
                        gm.emissionMetal[3] = m.metallic;
                        voxMaterials.push_back(gm);
                    }
                    slotOf[flat.model] = slot;
                }

                const IVec3 dims = flat.model->dims();
                g.dimsVoxels[0] = dims.x;
                g.dimsVoxels[1] = dims.y;
                g.dimsVoxels[2] = dims.z;
            }
            g.dimsVoxels[3] = slot.voxelOffset;
            g.materialBase[0] = slot.materialOffset;
            g.materialBase[1] = g.materialBase[2] = g.materialBase[3] = 0;

            g.tint[0] = flat.tint.x; g.tint[1] = flat.tint.y; g.tint[2] = flat.tint.z;
            g.tint[3] = 0.0f;
            g.emissionScale[0] = flat.emissionScale.x;
            g.emissionScale[1] = flat.emissionScale.y;
            g.emissionScale[2] = flat.emissionScale.z;
            g.emissionScale[3] = 0.0f;
        }
    }
    propNodes_ = int(propNodes.size());

    makeBufferOrStub(spriteNodeBuf_, spriteNodes, bytes_);
    makeBufferOrStub(quadBuf_, quads, bytes_);
    makeBufferOrStub(propNodeBuf_, propNodes, bytes_);
    makeBufferOrStub(gridBuf_, grids, bytes_);
    makeBufferOrStub(voxelBuf_, voxelWords, bytes_);
    makeBufferOrStub(voxMatBuf_, voxMaterials, bytes_);

    return gl::GetError() == gl::NO_ERROR_;
}

void OffGridData::bind() const {
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 16, spriteNodeBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 17, quadBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 18, propNodeBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 19, gridBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 20, voxelBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 21, voxMatBuf_);
}

void OffGridData::bindSpriteTextures(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, spriteTex_);
}

void OffGridData::destroy() {
    const gl::GLuint buffers[] = {spriteNodeBuf_, quadBuf_, propNodeBuf_,
                                  gridBuf_,       voxelBuf_, voxMatBuf_};
    for (gl::GLuint b : buffers) {
        if (b) gl::DeleteBuffers(1, &b);
    }
    spriteNodeBuf_ = quadBuf_ = propNodeBuf_ = gridBuf_ = voxelBuf_ = voxMatBuf_ = 0;
    if (spriteTex_) { gl::DeleteTextures(1, &spriteTex_); spriteTex_ = 0; }
    spriteNodes_ = propNodes_ = 0;
    bytes_ = 0;
}

} // namespace gpu
} // namespace blocky
