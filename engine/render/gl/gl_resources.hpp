#pragma once
// GPU-side resources for the viewport: shader programs, meshes, and the
// block texture array.
//
// A texture *array* rather than an atlas. Every block texture is 16x16, so
// they stack into layers of equal size -- which means no packing, no UV
// remapping, and none of the bleeding an atlas suffers at mip boundaries.
#include "engine/assets/blocks/block_textures.hpp"
#include "engine/assets/texture.hpp"
#include "engine/core/math.hpp"
#include "engine/entity/entity.hpp"
#include "engine/prop/voxel_model.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/world/world.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace blocky {

// ---------------------------------------------------------------- shaders
class ShaderProgram {
public:
    ~ShaderProgram();
    ShaderProgram() = default;
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    bool build(const char* vertexSource, const char* fragmentSource, std::string* error = nullptr);
    void destroy();

    void use() const;
    bool valid() const { return program_ != 0; }

    void setInt(const char* name, int value) const;
    void setFloat(const char* name, float value) const;
    void setVec3(const char* name, Vec3 value) const;
    void setMat4(const char* name, const Mat4& value) const;

private:
    gl::GLuint program_ = 0;
};

// ----------------------------------------------------------------- meshes
// Twenty bytes a vertex. Positions are world-space floats; everything else is
// packed, because a chunky island produces a lot of these.
struct BlockVertex {
    float    x, y, z;
    uint16_t layer;    // index into the block texture array
    uint8_t  normal;   // 0..5, expanded in the shader
    uint8_t  ao;       // 0..255 corner occlusion
    uint8_t  u, v;     // face corner, 0 or 1
    uint8_t  pad[2];
};

struct MeshData {
    std::vector<BlockVertex> vertices;
    std::vector<uint32_t> indices;

    bool empty() const { return indices.empty(); }
    void clear() { vertices.clear(); indices.clear(); }
};

class GpuMesh {
public:
    ~GpuMesh();
    GpuMesh() = default;
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    // Movable but never copyable, for the same reason as GpuSkinMesh: two
    // objects owning one GPU handle would delete it twice. A per-chunk cache
    // keeps these in a container, which is what made it necessary here.
    GpuMesh(GpuMesh&& other) noexcept { *this = std::move(other); }
    GpuMesh& operator=(GpuMesh&& other) noexcept {
        if (this != &other) {
            destroy();
            vao_ = other.vao_; vbo_ = other.vbo_; ebo_ = other.ebo_;
            indexCount_ = other.indexCount_;
            other.vao_ = other.vbo_ = other.ebo_ = 0;
            other.indexCount_ = 0;
        }
        return *this;
    }

    void upload(const MeshData& data);
    void destroy();
    void draw() const;

    int indexCount() const { return indexCount_; }
    bool empty() const { return indexCount_ == 0; }

private:
    gl::GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
};

// --------------------------------------------------------------- meshing
// Walks the world and emits a quad for every face whose neighbour lets light
// through -- the same visibility rule the game uses.
//
// `opaque` and `translucent` come back separately so the translucent pass can
// be drawn after, with blending on and depth writes off.
void buildWorldMesh(const World& world, MeshData& opaque, MeshData& translucent);

// One chunk's worth of faces, **appended** to the two meshes -- the caller
// clears, because both callers want different things: the whole-world builder
// accumulates every chunk into one pair, and a per-chunk cache reuses one
// scratch pair over and over.
//
// `buildWorldMesh` is a loop over this, so a chunk meshed on its own and the
// same chunk meshed as part of the world cannot come out differently.
//
// Face culling reads the six neighbouring blocks and vertex AO reads the eight
// around each corner, so this reaches into adjacent chunks through `world`. A
// chunk is therefore not self-contained, which is exactly why a change near a
// boundary restamps its neighbours -- see `World::chunkStamp`.
void appendChunkMesh(const World& world, const World::Chunk& chunk,
                     MeshData& opaque, MeshData& translucent);

// One entity's boxes, in world space. Texture coordinates address the skin
// directly, so this mesh is drawn with the skin bound as a plain 2D texture.
struct SkinVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct SkinMeshData {
    std::vector<SkinVertex> vertices;
    std::vector<uint32_t> indices;
    bool empty() const { return indices.empty(); }
};

class GpuSkinMesh {
public:
    ~GpuSkinMesh();
    GpuSkinMesh() = default;
    GpuSkinMesh(const GpuSkinMesh&) = delete;
    GpuSkinMesh& operator=(const GpuSkinMesh&) = delete;

    // Movable but never copyable: two objects owning one GPU handle would
    // delete it twice.
    GpuSkinMesh(GpuSkinMesh&& other) noexcept { *this = std::move(other); }
    GpuSkinMesh& operator=(GpuSkinMesh&& other) noexcept {
        if (this != &other) {
            destroy();
            vao_ = other.vao_; vbo_ = other.vbo_; ebo_ = other.ebo_;
            indexCount_ = other.indexCount_;
            other.vao_ = other.vbo_ = other.ebo_ = 0;
            other.indexCount_ = 0;
        }
        return *this;
    }

    void upload(const SkinMeshData& data);
    void destroy();
    void draw() const;
    bool empty() const { return indexCount_ == 0; }

private:
    gl::GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
};

// Builds the geometry for a single entity. Outer layers are skipped when
// their texels are transparent, which is the mesh-time equivalent of the
// tracer's alpha cutout.
void buildEntityMesh(const Entity& entity, SkinMeshData& out);

// ------------------------------------------------------------ prop meshes
// A voxel model's exposed faces, in the model's **own voxel coordinates**.
//
// Left in local space on purpose. `PropSet::Flat::toWorld` already maps voxel
// coordinates to the world -- it is the matrix the tracer walks and the one
// the physics writes -- so a mesh built once per model is drawn by handing
// that same matrix to the shader. Baking world space instead would mean
// remeshing every prop every time it moved, which for a sandbox is every prop
// every frame.
//
// Colour comes from the model's palette rather than a texture, so props need
// their own vertex format and their own shader; they share nothing with the
// block mesh but the lighting.
struct PropVertex {
    float x, y, z;
    float nx, ny, nz;
    float r, g, b;      // linear albedo
    float er, eg, eb;   // linear emission
};

struct PropMeshData {
    std::vector<PropVertex> vertices;
    std::vector<uint32_t> indices;

    bool empty() const { return indices.empty(); }
    void clear() { vertices.clear(); indices.clear(); }
};

class GpuPropMesh {
public:
    ~GpuPropMesh();
    GpuPropMesh() = default;
    GpuPropMesh(const GpuPropMesh&) = delete;
    GpuPropMesh& operator=(const GpuPropMesh&) = delete;

    GpuPropMesh(GpuPropMesh&& other) noexcept { *this = std::move(other); }
    GpuPropMesh& operator=(GpuPropMesh&& other) noexcept {
        if (this != &other) {
            destroy();
            vao_ = other.vao_; vbo_ = other.vbo_; ebo_ = other.ebo_;
            indexCount_ = other.indexCount_;
            other.vao_ = other.vbo_ = other.ebo_ = 0;
            other.indexCount_ = 0;
        }
        return *this;
    }

    void upload(const PropMeshData& data);
    void destroy();
    void draw() const;
    bool empty() const { return indexCount_ == 0; }

private:
    gl::GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    int indexCount_ = 0;
};

// One quad per voxel face whose neighbour is empty -- the same visibility
// rule the world mesher uses, and for the same reason: an interior face can
// never be seen and costs two triangles to prove it.
void buildPropMesh(const VoxelModel& model, PropMeshData& out);

// Sprite quads, into the same vertex format and so onto the same shader.
//
// Until this existed, particles and floating text were visible only in a
// trace -- which was fine while the viewport's job was finding a camera, and
// stopped being fine when something wanted to *place* a sprite. You cannot
// arrange what you cannot see.
//
// The corners come from `SpriteSet::flats()`, not from the angles: the set
// has already turned yaw, pitch and roll into a matrix, and turning them into
// a second one here is the same mistake as re-posing a skeleton for the GPU.
//
// **Untextured only.** A sprite's tint is its colour here, and a sprite that
// carries a texture is drawn as a flat quad of that tint rather than as its
// picture. Doing it properly needs alpha-cut sampling and a second shader,
// and what this is for -- seeing where a quad is and how big -- does not.
void buildSpriteMesh(const SpriteSet& sprites, PropMeshData& out);

// --------------------------------------------------------------- textures
// One layer per (block, face), with the tint and any overlay already
// composited in -- so the shader is a single array lookup and matches what
// the path tracer computes on the CPU.
class GlBlockTextureArray {
public:
    ~GlBlockTextureArray();
    GlBlockTextureArray() = default;
    GlBlockTextureArray(const GlBlockTextureArray&) = delete;
    GlBlockTextureArray& operator=(const GlBlockTextureArray&) = delete;

    // `library` may be null: every block then bakes to its flat palette colour.
    bool build(const BlockRegistry& registry, const BlockTextureLibrary* library);
    void destroy();
    void bind(int unit) const;

    static int layerFor(BlockId id, int face) { return int(id) * FaceCount + face; }
    int layerCount() const { return layers_; }

private:
    gl::GLuint texture_ = 0;
    int layers_ = 0;
};

// A skin uploaded as a plain 2D texture, nearest-filtered.
class GlTexture2D {
public:
    ~GlTexture2D();
    GlTexture2D() = default;
    GlTexture2D(const GlTexture2D&) = delete;
    GlTexture2D& operator=(const GlTexture2D&) = delete;

    GlTexture2D(GlTexture2D&& other) noexcept { *this = std::move(other); }
    GlTexture2D& operator=(GlTexture2D&& other) noexcept {
        if (this != &other) { destroy(); texture_ = other.texture_; other.texture_ = 0; }
        return *this;
    }

    bool upload(const ImageU8& image);
    void destroy();
    void bind(int unit) const;

    // Whether anything has been uploaded yet. A cache keyed by skin needs to
    // ask, so that a crowd wearing one skin uploads one texture.
    bool empty() const { return texture_ == 0; }

private:
    gl::GLuint texture_ = 0;
};

} // namespace blocky
