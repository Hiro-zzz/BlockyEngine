#include "engine/render/gpu/entities_gpu.hpp"

#include <cstring>
#include <unordered_map>

namespace blocky {
namespace gpu {
namespace {

constexpr int kSkinSize = 64;

void copyMatrix(float (&out)[16], const Mat4& m) {
    // Mat4 stores m[column][row] and GLSL's mat4 is column major too, so this
    // is a straight copy rather than a transpose. transformPoint() reading
    // m[0][0], m[1][0], m[2][0], m[3][0] for the x row is what pins that
    // down: m[3] is the translation column.
    std::memcpy(out, m.data(), sizeof(float) * 16);
}

} // namespace

EntityData::~EntityData() { destroy(); }

bool EntityData::upload(const EntitySet* entities) {
    destroy();
    if (!entities || entities->empty()) return true;

    const std::vector<EntitySet::FlatBox>& boxes = entities->boxes();

    // One layer per distinct skin. Several boxes share a skin -- a player is
    // twenty-four boxes and one image -- so the map is what keeps the array
    // from holding two dozen copies of the same 64x64.
    std::unordered_map<const Skin*, int> layerOf;
    std::vector<const Skin*> skins;
    for (const EntitySet::FlatBox& box : boxes) {
        if (!box.skin) continue;
        if (layerOf.find(box.skin) != layerOf.end()) continue;
        layerOf[box.skin] = int(skins.size());
        skins.push_back(box.skin);
    }

    std::vector<GpuBox> packed(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) {
        const EntitySet::FlatBox& src = boxes[i];
        GpuBox& dst = packed[i];

        copyMatrix(dst.toLocal, src.toLocal);
        copyMatrix(dst.toWorld, src.toWorld);

        dst.size[0] = src.size.x; dst.size[1] = src.size.y; dst.size[2] = src.size.z;
        dst.cutout = src.cutout ? 1.0f : 0.0f;

        dst.aabbMin[0] = src.aabbMin.x; dst.aabbMin[1] = src.aabbMin.y;
        dst.aabbMin[2] = src.aabbMin.z;
        const auto found = src.skin ? layerOf.find(src.skin) : layerOf.end();
        dst.skinLayer = float(found == layerOf.end() ? 0 : found->second);

        dst.aabbMax[0] = src.aabbMax.x; dst.aabbMax[1] = src.aabbMax.y;
        dst.aabbMax[2] = src.aabbMax.z;
        dst.pad = 0.0f;

        for (int f = 0; f < SkinFaceCount; ++f) {
            const SkinRect& r = src.faces[f];
            dst.faces[f][0] = r.x;
            dst.faces[f][1] = r.y;
            // An invalid rectangle is how a box says it has no such face, and
            // the shader has to see the same thing: width zero, not a
            // rectangle of zero area that still samples a texel.
            dst.faces[f][2] = r.valid() ? r.width : 0;
            dst.faces[f][3] = r.valid() ? r.height : 0;
        }
    }

    boxCount_ = int(packed.size());
    gl::GenBuffers(1, &boxBuf_);
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, boxBuf_);
    gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(packed.size() * sizeof(GpuBox)),
                   packed.data(), gl::STATIC_DRAW);

    // ---- the skins
    skinCount_ = int(skins.empty() ? 1 : skins.size());
    std::vector<float> texels(size_t(kSkinSize) * kSkinSize * 4 * size_t(skinCount_), 0.0f);

    for (size_t s = 0; s < skins.size(); ++s) {
        const Texture& texture = skins[s]->texture();
        const size_t layerOffset = s * size_t(kSkinSize) * kSkinSize * 4;
        for (int y = 0; y < kSkinSize; ++y) {
            for (int x = 0; x < kSkinSize; ++x) {
                const bool inside = x < texture.width() && y < texture.height();
                const Vec3 c = inside ? texture.texel(x, y) : Vec3{0.0f};
                const float a = inside ? texture.alphaAt(x, y) : 0.0f;
                const size_t i = layerOffset + (size_t(y) * kSkinSize + size_t(x)) * 4;
                texels[i + 0] = c.x;
                texels[i + 1] = c.y;
                texels[i + 2] = c.z;
                texels[i + 3] = a;
            }
        }
    }

    gl::GenTextures(1, &skinTex_);
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, skinTex_);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexImage3D(gl::TEXTURE_2D_ARRAY, 0, gl::RGBA32F, kSkinSize, kSkinSize, skinCount_, 0,
                   gl::RGBA, gl::FLOAT, texels.data());
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, 0);

    return gl::GetError() == gl::NO_ERROR_;
}

void EntityData::bindBoxes(gl::GLuint binding) const {
    if (boxBuf_) gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, binding, boxBuf_);
}

void EntityData::bindSkins(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, skinTex_);
}

void EntityData::destroy() {
    if (boxBuf_) { gl::DeleteBuffers(1, &boxBuf_); boxBuf_ = 0; }
    if (skinTex_) { gl::DeleteTextures(1, &skinTex_); skinTex_ = 0; }
    boxCount_ = 0;
    skinCount_ = 0;
}

} // namespace gpu
} // namespace blocky
