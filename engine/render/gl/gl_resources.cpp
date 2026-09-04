#include "engine/render/gl/gl_resources.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>

namespace blocky {
namespace {

// Face ordering matches BlockFace: -X, +X, -Y, +Y, -Z, +Z.
constexpr IVec3 kFaceNormals[FaceCount] = {
    {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
};

int axisOf(int face) { return face >> 1; }
bool facePositive(int face) { return (face & 1) != 0; }

// Texture coordinates for a corner of a face, using the same convention as
// the raycaster's faceUv -- so a block looks identical in the viewport and in
// a traced render. `local` components are 0 or 1.
void cornerUv(int face, float lx, float ly, float lz, uint8_t& u, uint8_t& v) {
    // stepSign is the direction the ray was travelling, i.e. the opposite of
    // the outward normal.
    bool positive = facePositive(face);
    float fu = 0.0f, fv = 0.0f;

    switch (axisOf(face)) {
        case 0:  // X faces
            fu = positive ? lz : 1.0f - lz;
            fv = 1.0f - ly;
            break;
        case 1:  // Y faces
            fu = lx;
            fv = positive ? lz : 1.0f - lz;
            break;
        default: // Z faces
            fu = positive ? 1.0f - lx : lx;
            fv = 1.0f - ly;
            break;
    }
    u = fu > 0.5f ? 1 : 0;
    v = fv > 0.5f ? 1 : 0;
}

// Minecraft smooth lighting, evaluated per vertex instead of per pixel.
uint8_t cornerAo(const World& world, IVec3 block, IVec3 normal, IVec3 du, IVec3 dv) {
    IVec3 front = block + normal;
    bool side1  = world.isOpaque(front + du);
    bool side2  = world.isOpaque(front + dv);
    bool corner = world.isOpaque(front + du + dv);

    int level = (side1 && side2) ? 0 : 3 - (int(side1) + int(side2) + int(corner));
    return uint8_t(level * 255 / 3);
}

void appendFace(const World& world, MeshData& mesh, IVec3 block, int face, uint16_t layer) {
    IVec3 normal = kFaceNormals[face];
    int axis = axisOf(face);
    bool positive = facePositive(face);

    // Tangent axes chosen so t1 x t2 = axis, which makes the corner order
    // below counter-clockwise seen from outside a positive-facing face.
    int t1 = (axis + 1) % 3;
    int t2 = (axis + 2) % 3;

    // (t1, t2) coordinates of the four corners, in winding order.
    const int corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    uint32_t base = uint32_t(mesh.vertices.size());

    for (int c = 0; c < 4; ++c) {
        // A negative-facing face is the same quad wound the other way.
        int index = positive ? c : 3 - c;

        float local[3] = {0.0f, 0.0f, 0.0f};
        local[axis] = positive ? 1.0f : 0.0f;
        local[t1] = float(corners[index][0]);
        local[t2] = float(corners[index][1]);

        IVec3 du{}, dv{};
        du[t1] = corners[index][0] == 0 ? -1 : 1;
        dv[t2] = corners[index][1] == 0 ? -1 : 1;

        BlockVertex vertex{};
        vertex.x = float(block.x) + local[0];
        vertex.y = float(block.y) + local[1];
        vertex.z = float(block.z) + local[2];
        vertex.layer = layer;
        vertex.normal = uint8_t(face);
        vertex.ao = cornerAo(world, block, normal, du, dv);
        cornerUv(face, local[0], local[1], local[2], vertex.u, vertex.v);

        mesh.vertices.push_back(vertex);
    }

    // Split along the shorter diagonal, so the AO gradient does not crease
    // the wrong way across the quad -- the classic voxel-AO artefact.
    const BlockVertex* v = mesh.vertices.data() + base;
    bool flip = int(v[0].ao) + int(v[2].ao) < int(v[1].ao) + int(v[3].ao);

    if (flip) {
        mesh.indices.insert(mesh.indices.end(),
                            {base + 1, base + 2, base + 3, base + 3, base + 0, base + 1});
    } else {
        mesh.indices.insert(mesh.indices.end(),
                            {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
    }
}

const char* shaderStageName(gl::GLenum stage) {
    return stage == gl::VERTEX_SHADER ? "vertex" : "fragment";
}

gl::GLuint compileStage(gl::GLenum stage, const char* source, std::string* error) {
    gl::GLuint shader = gl::CreateShader(stage);
    gl::ShaderSource(shader, 1, &source, nullptr);
    gl::CompileShader(shader);

    gl::GLint ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetShaderInfoLog(shader, length, nullptr, log.data());
        if (error) *error = std::string(shaderStageName(stage)) + " shader: " + log;
        gl::DeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// ============================================================== ShaderProgram
ShaderProgram::~ShaderProgram() { destroy(); }

bool ShaderProgram::build(const char* vertexSource, const char* fragmentSource, std::string* error) {
    destroy();

    gl::GLuint vertex = compileStage(gl::VERTEX_SHADER, vertexSource, error);
    if (!vertex) return false;

    gl::GLuint fragment = compileStage(gl::FRAGMENT_SHADER, fragmentSource, error);
    if (!fragment) { gl::DeleteShader(vertex); return false; }

    gl::GLuint program = gl::CreateProgram();
    gl::AttachShader(program, vertex);
    gl::AttachShader(program, fragment);
    gl::LinkProgram(program);

    gl::DeleteShader(vertex);
    gl::DeleteShader(fragment);

    gl::GLint ok = 0;
    gl::GetProgramiv(program, gl::LINK_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetProgramiv(program, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetProgramInfoLog(program, length, nullptr, log.data());
        if (error) *error = "link: " + log;
        gl::DeleteProgram(program);
        return false;
    }

    program_ = program;
    return true;
}

void ShaderProgram::destroy() {
    if (program_) gl::DeleteProgram(program_);
    program_ = 0;
}

void ShaderProgram::use() const { gl::UseProgram(program_); }

void ShaderProgram::setInt(const char* name, int value) const {
    gl::Uniform1i(gl::GetUniformLocation(program_, name), value);
}
void ShaderProgram::setFloat(const char* name, float value) const {
    gl::Uniform1f(gl::GetUniformLocation(program_, name), value);
}
void ShaderProgram::setVec3(const char* name, Vec3 value) const {
    gl::Uniform3fv(gl::GetUniformLocation(program_, name), 1, &value.x);
}
void ShaderProgram::setMat4(const char* name, const Mat4& value) const {
    gl::UniformMatrix4fv(gl::GetUniformLocation(program_, name), 1, 0, value.data());
}

// ==================================================================== GpuMesh
GpuMesh::~GpuMesh() { destroy(); }

void GpuMesh::upload(const MeshData& data) {
    destroy();
    if (data.empty()) return;

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);

    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(data.vertices.size() * sizeof(BlockVertex)),
                   data.vertices.data(), gl::STATIC_DRAW);

    gl::GenBuffers(1, &ebo_);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, ebo_);
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER, gl::GLsizeiptr(data.indices.size() * sizeof(uint32_t)),
                   data.indices.data(), gl::STATIC_DRAW);

    const gl::GLsizei stride = sizeof(BlockVertex);
    auto offset = [](size_t bytes) { return reinterpret_cast<const void*>(bytes); };

    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, gl::FLOAT, 0, stride, offset(offsetof(BlockVertex, x)));

    gl::EnableVertexAttribArray(1);
    gl::VertexAttribIPointer(1, 1, gl::UNSIGNED_SHORT, stride, offset(offsetof(BlockVertex, layer)));

    gl::EnableVertexAttribArray(2);
    gl::VertexAttribIPointer(2, 1, gl::UNSIGNED_BYTE, stride, offset(offsetof(BlockVertex, normal)));

    gl::EnableVertexAttribArray(3);
    gl::VertexAttribPointer(3, 1, gl::UNSIGNED_BYTE, 1, stride, offset(offsetof(BlockVertex, ao)));

    gl::EnableVertexAttribArray(4);
    gl::VertexAttribPointer(4, 2, gl::UNSIGNED_BYTE, 0, stride, offset(offsetof(BlockVertex, u)));

    gl::BindVertexArray(0);
    indexCount_ = int(data.indices.size());
}

void GpuMesh::destroy() {
    if (ebo_) gl::DeleteBuffers(1, &ebo_);
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = 0;
}

void GpuMesh::draw() const {
    if (!indexCount_) return;
    gl::BindVertexArray(vao_);
    gl::DrawElements(gl::TRIANGLES, indexCount_, gl::UNSIGNED_INT, nullptr);
    gl::BindVertexArray(0);
}

// ================================================================ world mesh
void appendChunkMesh(const World& world, const World::Chunk& chunk,
                     MeshData& opaque, MeshData& translucent) {
    const BlockRegistry& registry = world.registry();
    IVec3 base = chunk.coord * World::kChunkSize;

    for (int ly = 0; ly < World::kChunkSize; ++ly) {
        for (int lz = 0; lz < World::kChunkSize; ++lz) {
            for (int lx = 0; lx < World::kChunkSize; ++lx) {
                BlockId id = chunk.blocks[World::Chunk::index(lx, ly, lz)];
                if (id == block::Air) continue;

                const BlockDef& def = registry[id];
                IVec3 position = base + IVec3{lx, ly, lz};
                MeshData& target = def.transmissive() ? translucent : opaque;

                for (int face = 0; face < FaceCount; ++face) {
                    BlockId neighbour = world.get(position + kFaceNormals[face]);

                    // A face is drawn when the neighbour lets light past.
                    // Two touching water blocks hide the surface between
                    // them, exactly as the game does.
                    if (neighbour == id) continue;
                    if (registry[neighbour].opaque) continue;

                    appendFace(world, target, position, face,
                               uint16_t(GlBlockTextureArray::layerFor(id, face)));
                }
            }
        }
    }
}

void buildWorldMesh(const World& world, MeshData& opaque, MeshData& translucent) {
    opaque.clear();
    translucent.clear();

    world.forEachChunk([&](IVec3, const World::Chunk& chunk) {
        appendChunkMesh(world, chunk, opaque, translucent);
    });
}

// =============================================================== skin meshes
GpuSkinMesh::~GpuSkinMesh() { destroy(); }

void GpuSkinMesh::upload(const SkinMeshData& data) {
    destroy();
    if (data.empty()) return;

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);

    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(data.vertices.size() * sizeof(SkinVertex)),
                   data.vertices.data(), gl::STATIC_DRAW);

    gl::GenBuffers(1, &ebo_);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, ebo_);
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER, gl::GLsizeiptr(data.indices.size() * sizeof(uint32_t)),
                   data.indices.data(), gl::STATIC_DRAW);

    const gl::GLsizei stride = sizeof(SkinVertex);
    auto offset = [](size_t bytes) { return reinterpret_cast<const void*>(bytes); };

    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, gl::FLOAT, 0, stride, offset(offsetof(SkinVertex, x)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 3, gl::FLOAT, 0, stride, offset(offsetof(SkinVertex, nx)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 2, gl::FLOAT, 0, stride, offset(offsetof(SkinVertex, u)));

    gl::BindVertexArray(0);
    indexCount_ = int(data.indices.size());
}

void GpuSkinMesh::destroy() {
    if (ebo_) gl::DeleteBuffers(1, &ebo_);
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = 0;
}

void GpuSkinMesh::draw() const {
    if (!indexCount_) return;
    gl::BindVertexArray(vao_);
    gl::DrawElements(gl::TRIANGLES, indexCount_, gl::UNSIGNED_INT, nullptr);
    gl::BindVertexArray(0);
}

void buildEntityMesh(const Entity& entity, SkinMeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    if (!entity.model || !entity.skin) return;

    constexpr float kPixelsPerBlock = 16.0f;

    Mat4 placement = translate(entity.position) *
                     scale(Vec3{entity.scale / kPixelsPerBlock}) *
                     rotateAxis({0.0f, 1.0f, 0.0f}, radians(entity.yawDegrees));

    const Texture& skin = entity.skin->texture();
    const float skinWidth = float(skin.width() > 0 ? skin.width() : 64);
    const float skinHeight = float(skin.height() > 0 ? skin.height() : 64);

    // The same joint resolve the tracer does, so the preview and the final
    // frame agree about where a posed limb ended up.
    std::vector<Mat4> jointToModel;
    entity.model->skeleton.resolve(entity.pose, jointToModel);

    for (const ModelBox& box : entity.model->boxes) {
        Vec3 inflated = box.size + Vec3{2.0f * box.inflate};

        Mat4 jointMatrix = (box.joint >= 0 && size_t(box.joint) < jointToModel.size())
                               ? jointToModel[size_t(box.joint)]
                               : Mat4::identity();

        Mat4 toWorld = placement * jointMatrix * translate(box.origin - Vec3{box.inflate});

        for (int faceIndex = 0; faceIndex < SkinFaceCount; ++faceIndex) {
            SkinFace face = SkinFace(faceIndex);
            const SkinRect& rect = box.faces[faceIndex];
            if (!rect.valid()) continue;

            // An outer-layer face that is entirely cut away contributes
            // nothing but overdraw, so drop it at build time.
            if (box.cutout) {
                bool anySolid = false;
                for (int y = 0; y < rect.height && !anySolid; ++y) {
                    for (int x = 0; x < rect.width; ++x) {
                        if (skin.alphaAt(rect.x + x, rect.y + y) >= 0.5f) { anySolid = true; break; }
                    }
                }
                if (!anySolid) continue;
            }

            Vec3 normalLocal = skinFaceNormal(face);
            int axis = normalLocal.x != 0.0f ? 0 : (normalLocal.y != 0.0f ? 1 : 2);
            bool positive = normalLocal[axis] > 0.0f;

            int t1 = (axis + 1) % 3;
            int t2 = (axis + 2) % 3;
            const int corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

            uint32_t base = uint32_t(out.vertices.size());
            Vec3 worldNormal = normalize(transformDir(toWorld, normalLocal));

            for (int c = 0; c < 4; ++c) {
                int index = positive ? c : 3 - c;

                Vec3 local{};
                local[axis] = positive ? inflated[axis] : 0.0f;
                local[t1] = corners[index][0] ? inflated[t1] : 0.0f;
                local[t2] = corners[index][1] ? inflated[t2] : 0.0f;

                Vec2 uv = boxFaceUv(local, inflated, face);
                Vec3 world = transformPoint(toWorld, local);

                SkinVertex vertex{};
                vertex.x = world.x; vertex.y = world.y; vertex.z = world.z;
                vertex.nx = worldNormal.x; vertex.ny = worldNormal.y; vertex.nz = worldNormal.z;
                // Sample the middle of the texel row/column the corner sits on
                // so nearest filtering cannot land on the neighbouring face.
                vertex.u = (float(rect.x) + uv.x * float(rect.width)) / skinWidth;
                vertex.v = (float(rect.y) + uv.y * float(rect.height)) / skinHeight;
                out.vertices.push_back(vertex);
            }

            out.indices.insert(out.indices.end(),
                               {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
        }
    }
}

// ============================================================ block textures
GlBlockTextureArray::~GlBlockTextureArray() { destroy(); }

bool GlBlockTextureArray::build(const BlockRegistry& registry, const BlockTextureLibrary* library) {
    destroy();

    // The library decides how big a tile is: a generated set at 32 a face
    // would otherwise be sampled back down to the game's 16 on the way to the
    // GPU, which is the one place that loss would be invisible until someone
    // wondered why the viewport looked softer than the trace.
    int kSize = 16;
    if (library) kSize = std::max(16, std::min(128, library->tileSize()));
    layers_ = int(registry.size()) * FaceCount;

    // RGBA8 in sRGB, so the GPU linearises on sample exactly like the CPU
    // path does with srgbToLinear.
    std::vector<uint8_t> pixels(size_t(kSize) * kSize * 4 * size_t(layers_), 0);

    for (BlockId id = 0; id < BlockId(registry.size()); ++id) {
        const BlockDef& def = registry[id];

        for (int face = 0; face < FaceCount; ++face) {
            // Transmissive blocks have no surface texture; preview them with
            // the colour their medium would give after a few blocks of travel.
            Vec3 flat = def.albedo;
            if (def.tintTop && face == FacePosY) flat = def.topTint;
            if (def.transmissive()) {
                Vec3 a = def.absorption * 6.0f;
                flat = Vec3{std::exp(-a.x), std::exp(-a.y), std::exp(-a.z)};
            }

            size_t layerOffset = size_t(layerFor(id, face)) * size_t(kSize) * kSize * 4;

            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    Vec2 uv{(float(x) + 0.5f) / float(kSize), (float(y) + 0.5f) / float(kSize)};
                    Vec3 linear = flat;
                    if (library && !def.transmissive()) {
                        linear = library->sampleAlbedo(id, face, uv, flat);
                    }
                    Vec3 encoded = linearToSrgb(minv(maxv(linear, Vec3{0.0f}), Vec3{1.0f}));

                    size_t i = layerOffset + (size_t(y) * kSize + size_t(x)) * 4;
                    pixels[i + 0] = uint8_t(encoded.x * 255.0f + 0.5f);
                    pixels[i + 1] = uint8_t(encoded.y * 255.0f + 0.5f);
                    pixels[i + 2] = uint8_t(encoded.z * 255.0f + 0.5f);
                    pixels[i + 3] = 255;
                }
            }
        }
    }

    gl::GenTextures(1, &texture_);
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, texture_);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexImage3D(gl::TEXTURE_2D_ARRAY, 0, gl::SRGB8_ALPHA8, kSize, kSize, layers_, 0,
                   gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());

    // Nearest magnification keeps the pixel art crisp; mipmaps stop distant
    // blocks from shimmering.
    gl::GenerateMipmap(gl::TEXTURE_2D_ARRAY);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MIN_FILTER, gl::NEAREST_MIPMAP_LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D_ARRAY, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, 0);

    return gl::GetError() == gl::NO_ERROR_;
}

void GlBlockTextureArray::destroy() {
    if (texture_) gl::DeleteTextures(1, &texture_);
    texture_ = 0;
    layers_ = 0;
}

void GlBlockTextureArray::bind(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D_ARRAY, texture_);
}

// ================================================================= 2D texture
GlTexture2D::~GlTexture2D() { destroy(); }

bool GlTexture2D::upload(const ImageU8& image) {
    destroy();
    if (image.empty()) return false;

    gl::GenTextures(1, &texture_);
    gl::BindTexture(gl::TEXTURE_2D, texture_);
    gl::PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::SRGB8_ALPHA8, image.width(), image.height(), 0,
                   gl::RGBA, gl::UNSIGNED_BYTE, image.data());

    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::BindTexture(gl::TEXTURE_2D, 0);

    return gl::GetError() == gl::NO_ERROR_;
}

void GlTexture2D::destroy() {
    if (texture_) gl::DeleteTextures(1, &texture_);
    texture_ = 0;
}

void GlTexture2D::bind(int unit) const {
    gl::ActiveTexture(gl::TEXTURE0 + gl::GLenum(unit));
    gl::BindTexture(gl::TEXTURE_2D, texture_);
}

// =============================================================== prop meshes
GpuPropMesh::~GpuPropMesh() { destroy(); }

void GpuPropMesh::upload(const PropMeshData& data) {
    destroy();
    if (data.empty()) return;

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);

    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(gl::ARRAY_BUFFER, vbo_);
    gl::BufferData(gl::ARRAY_BUFFER, gl::GLsizeiptr(data.vertices.size() * sizeof(PropVertex)),
                   data.vertices.data(), gl::STATIC_DRAW);

    gl::GenBuffers(1, &ebo_);
    gl::BindBuffer(gl::ELEMENT_ARRAY_BUFFER, ebo_);
    gl::BufferData(gl::ELEMENT_ARRAY_BUFFER, gl::GLsizeiptr(data.indices.size() * sizeof(uint32_t)),
                   data.indices.data(), gl::STATIC_DRAW);

    const gl::GLsizei stride = sizeof(PropVertex);
    auto offset = [](size_t bytes) { return reinterpret_cast<const void*>(bytes); };

    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 3, gl::FLOAT, 0, stride, offset(offsetof(PropVertex, x)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 3, gl::FLOAT, 0, stride, offset(offsetof(PropVertex, nx)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 3, gl::FLOAT, 0, stride, offset(offsetof(PropVertex, r)));
    gl::EnableVertexAttribArray(3);
    gl::VertexAttribPointer(3, 3, gl::FLOAT, 0, stride, offset(offsetof(PropVertex, er)));

    gl::BindVertexArray(0);
    indexCount_ = int(data.indices.size());
}

void GpuPropMesh::destroy() {
    if (ebo_) gl::DeleteBuffers(1, &ebo_);
    if (vbo_) gl::DeleteBuffers(1, &vbo_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    vao_ = vbo_ = ebo_ = 0;
    indexCount_ = 0;
}

void GpuPropMesh::draw() const {
    if (!indexCount_) return;
    gl::BindVertexArray(vao_);
    gl::DrawElements(gl::TRIANGLES, indexCount_, gl::UNSIGNED_INT, nullptr);
    gl::BindVertexArray(0);
}

void buildPropMesh(const VoxelModel& model, PropMeshData& out) {
    out.clear();
    if (model.empty()) return;

    IVec3 dims = model.dims();

    for (int y = 0; y < dims.y; ++y) {
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                IVec3 cell{x, y, z};
                uint16_t index = model.at(cell);
                if (index == VoxelModel::kEmpty) continue;

                const VoxelMaterial& material = model.material(index);

                for (int face = 0; face < FaceCount; ++face) {
                    if (model.at(cell + kFaceNormals[face]) != VoxelModel::kEmpty) continue;

                    // Same tangent choice as the block mesher, so the winding
                    // rule is the one thing these two do not have to agree on
                    // twice: t1 x t2 = axis makes the corners below run
                    // counter-clockwise seen from outside a positive face.
                    int axis = axisOf(face);
                    bool positive = facePositive(face);
                    int t1 = (axis + 1) % 3;
                    int t2 = (axis + 2) % 3;

                    const int corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                    Vec3 normal = toVec3(kFaceNormals[face]);
                    uint32_t base = uint32_t(out.vertices.size());

                    for (int c = 0; c < 4; ++c) {
                        int pick = positive ? c : 3 - c;

                        float local[3] = {0.0f, 0.0f, 0.0f};
                        local[axis] = positive ? 1.0f : 0.0f;
                        local[t1] = float(corners[pick][0]);
                        local[t2] = float(corners[pick][1]);

                        PropVertex vertex{};
                        vertex.x = float(x) + local[0];
                        vertex.y = float(y) + local[1];
                        vertex.z = float(z) + local[2];
                        vertex.nx = normal.x;
                        vertex.ny = normal.y;
                        vertex.nz = normal.z;
                        vertex.r = material.albedo.x;
                        vertex.g = material.albedo.y;
                        vertex.b = material.albedo.z;
                        vertex.er = material.emission.x;
                        vertex.eg = material.emission.y;
                        vertex.eb = material.emission.z;
                        out.vertices.push_back(vertex);
                    }

                    out.indices.insert(out.indices.end(),
                                       {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
                }
            }
        }
    }
}

} // namespace blocky
