#include "engine/render/gl/viewport.hpp"

#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"
#include "engine/platform/window.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/render/gl/chunk_cache.hpp"
#include "engine/render/gl/gl_loader.hpp"
#include "engine/render/gl/gl_resources.hpp"
#include "engine/render/gl/render_target.hpp"

#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace blocky {
namespace {

// Shared by both shaders: the same analytic sky the CPU renderers use, plus
// the same ACES curve and sRGB encode, so the viewport and a traced frame
// agree on tone.
const char* kCommonFragment = R"GLSL(
uniform vec3  uSunDirection;
uniform vec3  uSunRadiance;
uniform vec3  uSkyZenith;
uniform vec3  uSkyHorizon;
uniform vec3  uSkyGround;
uniform float uSkyIntensity;
uniform float uAmbient;
uniform float uSunScale;

// Zero is off. Anything above one is how many steps the illumination is
// allowed to take between dark and lit.
uniform float uCelBands;

vec3 skySample(vec3 d) {
    if (d.y >= 0.0) {
        return mix(uSkyHorizon, uSkyZenith, pow(clamp(d.y, 0.0, 1.0), 0.45)) * uSkyIntensity;
    }
    return mix(uSkyHorizon, uSkyGround, clamp(-d.y * 3.0, 0.0, 1.0)) * uSkyIntensity;
}

vec3 aces(vec3 c) {
    const float a = 2.51, b = 0.03, cc = 2.43, d = 0.59, e = 0.14;
    return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
    vec3 low  = c * 12.92;
    vec3 high = 1.055 * pow(max(c, vec3(1e-5)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, step(c, vec3(0.0031308)));
}

vec3 shade(vec3 albedo, vec3 normal, float occlusion) {
    float ndl = max(dot(normal, uSunDirection), 0.0);
    vec3 direct = uSunRadiance * ndl * 0.31830988 * uSunScale;
    vec3 ambient = skySample(normal) * uAmbient * occlusion;
    vec3 light = direct + ambient;

    // Cel: quantise the **illumination**, then put the albedo back.
    //
    // Banding the finished colour would band the texture along with the
    // light, and a stone block would come out in four shades of grey that
    // have nothing to do with where the sun is. Same argument the denoiser
    // makes for filtering illumination rather than colour, and the same one
    // `post/stylize.hpp` makes about where cel belongs -- the difference here
    // is that a rasterised frame has exactly one bounce, so this *is* the
    // final illumination rather than one contribution towards it.
    if (uCelBands > 0.5) {
        float level = dot(light, vec3(0.2126, 0.7152, 0.0722));
        float stepped = (floor(level * uCelBands) + 0.5) / uCelBands;
        light *= stepped / max(level, 1e-4);
    }
    return albedo * light;
}
)GLSL";

const char* kBlockVertex = R"GLSL(#version 460 core
layout(location = 0) in vec3  aPosition;
layout(location = 1) in uint  aLayer;
layout(location = 2) in uint  aNormal;
layout(location = 3) in float aOcclusion;
layout(location = 4) in vec2  aUv;

uniform mat4 uViewProjection;

flat out uint vLayer;
flat out vec3 vNormal;
out float vOcclusion;
out vec2  vUv;

const vec3 kNormals[6] = vec3[6](
    vec3(-1, 0, 0), vec3(1, 0, 0),
    vec3(0, -1, 0), vec3(0, 1, 0),
    vec3(0, 0, -1), vec3(0, 0, 1));

void main() {
    vLayer = aLayer;
    vNormal = kNormals[aNormal];
    vOcclusion = aOcclusion;
    vUv = aUv;
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)GLSL";

const char* kBlockFragmentBody = R"GLSL(
uniform sampler2DArray uBlocks;
uniform float uAlpha;

flat in uint vLayer;
flat in vec3 vNormal;
in float vOcclusion;
in vec2  vUv;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragNormal;

void main() {
    // The array is sRGB, so the sample arrives already linearised.
    vec3 albedo = texture(uBlocks, vec3(vUv, float(vLayer))).rgb;

    // Linear light, straight out. Exposure and the tone curve now live in the
    // one pass that can see the whole frame -- see render_target.hpp.
    fragColor = vec4(shade(albedo, vNormal, vOcclusion), uAlpha);
    fragNormal = vec4(vNormal * 0.5 + 0.5, 1.0);
}
)GLSL";

const char* kSkinVertex = R"GLSL(#version 460 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uViewProjection;

out vec3 vNormal;
out vec2 vUv;

void main() {
    vNormal = aNormal;
    vUv = aUv;
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)GLSL";

const char* kSkinFragmentBody = R"GLSL(
uniform sampler2D uSkin;

in vec3 vNormal;
in vec2 vUv;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragNormal;

void main() {
    vec4 texel = texture(uSkin, vUv);
    // Cutout, matching the tracer's alpha threshold.
    if (texel.a < 0.5) discard;

    vec3 normal = normalize(vNormal);
    fragColor = vec4(shade(texel.rgb, normal, 1.0), 1.0);
    fragNormal = vec4(normal * 0.5 + 0.5, 1.0);
}
)GLSL";

// Props carry their colour in the vertex rather than in a texture, and are
// drawn in their model's own voxel space with the placement matrix as a
// uniform -- the same matrix the tracer walks and the physics writes.
const char* kPropVertex = R"GLSL(#version 460 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColour;
layout(location = 3) in vec3 aEmission;

uniform mat4 uViewProjection;
uniform mat4 uModel;
uniform vec3 uTint;

out vec3 vNormal;
out vec3 vColour;
out vec3 vEmission;

void main() {
    // The placement is a rotation and a *uniform* scale, so the rotation part
    // transforms a normal correctly once renormalised. A non-uniform scale
    // would need the inverse transpose -- and PropSet refuses to accept one.
    vNormal = normalize(mat3(uModel) * aNormal);
    vColour = aColour * uTint;
    vEmission = aEmission;
    gl_Position = uViewProjection * (uModel * vec4(aPosition, 1.0));
}
)GLSL";

const char* kPropFragmentBody = R"GLSL(
in vec3 vNormal;
in vec3 vColour;
in vec3 vEmission;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragNormal;

void main() {
    vec3 normal = normalize(vNormal);
    fragColor = vec4(shade(vColour, normal, 1.0) + vEmission, 1.0);
    fragNormal = vec4(normal * 0.5 + 0.5, 1.0);
}
)GLSL";

const char* kSkyVertex = R"GLSL(#version 460 core
out vec2 vNdc;

void main() {
    // One oversized triangle covers the viewport without a vertex buffer.
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)GLSL";

const char* kSkyFragmentBody = R"GLSL(
uniform vec3  uCameraForward;
uniform vec3  uCameraRight;
uniform vec3  uCameraUp;
uniform float uTanHalfFov;
uniform float uAspect;

in vec2 vNdc;
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragNormal;

void main() {
    vec3 direction = normalize(uCameraForward
        + uCameraRight * (vNdc.x * uTanHalfFov * uAspect)
        + uCameraUp * (vNdc.y * uTanHalfFov));
    fragColor = vec4(skySample(direction), 1.0);

    // Alpha zero marks "no surface here". The outline pass needs to know the
    // difference between two faces meeting and a face meeting the sky, and
    // depth alone cannot say it: the sky is at the far plane, which is a
    // discontinuity of exactly the kind an edge test is looking for.
    fragNormal = vec4(0.5, 0.5, 0.5, 0.0);
}
)GLSL";

// ------------------------------------------------------------- the post pass
//
// Everything that has to see the finished frame rather than one fragment of
// it. The order is the CPU renderer's order, and deliberately: bloom before
// the tone curve because light adds in linear space, grading before it for the
// same reason, and the curve exactly once at the far end.

const char* kFullscreenVertex = R"GLSL(#version 460 core
out vec2 vUv;
void main() {
    // The same oversized triangle the sky uses. Two triangles would put a
    // seam down the middle where the interpolators meet.
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 1.0, 1.0);
}
)GLSL";

// Bright pass and blur, at half size. Bloom is the one effect where working at
// a lower resolution is not a compromise: the blur wants to be wide, and width
// is free when the pixels are big.
const char* kBrightFragment = R"GLSL(#version 460 core
uniform sampler2D uSource;
uniform float uThreshold;
uniform float uSoftness;

in vec2 vUv;
out vec4 fragColor;

void main() {
    vec3 c = texture(uSource, vUv).rgb;
    float level = dot(c, vec3(0.2126, 0.7152, 0.0722));

    // A soft knee rather than a cliff. A hard threshold makes a surface that
    // crosses it flicker between blooming and not as the camera moves.
    float weight = smoothstep(uThreshold, uThreshold + uSoftness, level);
    fragColor = vec4(c * weight, 1.0);
}
)GLSL";

const char* kBlurFragment = R"GLSL(#version 460 core
uniform sampler2D uSource;
uniform float uTexelX;
uniform float uTexelY;

in vec2 vUv;
out vec4 fragColor;

void main() {
    // Nine taps on a separable Gaussian, run once per axis by the caller.
    // The offsets exploit linear filtering: sampling between two texels
    // averages them for free, so five fetches cover nine texels.
    const float kOffset[3] = float[3](0.0, 1.3846153846, 3.2307692308);
    const float kWeight[3] = float[3](0.2270270270, 0.3162162162, 0.0702702703);

    vec2 step = vec2(uTexelX, uTexelY);
    vec3 sum = texture(uSource, vUv).rgb * kWeight[0];
    for (int i = 1; i < 3; ++i) {
        sum += texture(uSource, vUv + step * kOffset[i]).rgb * kWeight[i];
        sum += texture(uSource, vUv - step * kOffset[i]).rgb * kWeight[i];
    }
    fragColor = vec4(sum, 1.0);
}
)GLSL";

const char* kCompositeFragment = R"GLSL(#version 460 core
uniform sampler2D uColour;
uniform sampler2D uNormal;
uniform sampler2D uDepth;
uniform sampler2D uBloom;

uniform float uExposure;
uniform float uBloomIntensity;

uniform float uOutline;        // 0 = off, otherwise how dark the line goes
uniform float uOutlineDepth;   // depth difference that counts as an edge
uniform float uOutlineNormal;  // 1 - dot(n0, n1) that counts as an edge

uniform float uVignette;
uniform float uSaturation;
uniform float uContrast;

uniform vec3  uTint;
uniform float uFogDensity;
uniform vec3  uFogColour;

uniform float uTexelX;
uniform float uTexelY;

in vec2 vUv;
out vec4 fragColor;

vec3 aces(vec3 c) {
    const float a = 2.51, b = 0.03, cc = 2.43, d = 0.59, e = 0.14;
    return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
    vec3 low  = c * 12.92;
    vec3 high = 1.055 * pow(max(c, vec3(1e-5)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, step(c, vec3(0.0031308)));
}

// Linear eye depth from the hardware's non-linear one. Without this the whole
// depth range lives in the first few metres and every distant edge is missed.
float linearDepth(float z) {
    const float near = 0.05, far = 4096.0;
    float ndc = z * 2.0 - 1.0;
    return (2.0 * near * far) / (far + near - ndc * (far - near));
}

// The outline, from geometry rather than from colour.
//
// A filter over the picture would draw a line round every texture edge as
// well, which on a voxel world means a line round every block face -- the
// look of a colouring book rather than of cel shading. Depth and normal know
// where the *shapes* are.
float edgeAt(vec2 uv, vec2 texel) {
    vec4 n0 = texture(uNormal, uv);
    float d0 = linearDepth(texture(uDepth, uv).r);

    float depthEdge = 0.0;
    float normalEdge = 0.0;

    // A cross, not a full ring: four taps find every axis-aligned edge, and
    // on a world made of cubes there are no others worth the four extra.
    vec2 offsets[4] = vec2[4](vec2(texel.x, 0.0), vec2(-texel.x, 0.0),
                              vec2(0.0, texel.y), vec2(0.0, -texel.y));
    for (int i = 0; i < 4; ++i) {
        vec4 n1 = texture(uNormal, uv + offsets[i]);
        float d1 = linearDepth(texture(uDepth, uv + offsets[i]).r);

        // Scaled by distance: a fixed tolerance draws every far surface as
        // one solid line, because a step of one pixel spans more world the
        // further away it is.
        depthEdge = max(depthEdge, abs(d1 - d0) / max(d0 * uOutlineDepth, 1e-3));

        // Only between two real surfaces. Against the sky the depth test has
        // already said everything there is to say, and the sky's stored
        // normal is a placeholder.
        if (n0.a > 0.5 && n1.a > 0.5) {
            vec3 a = n0.rgb * 2.0 - 1.0;
            vec3 b = n1.rgb * 2.0 - 1.0;
            normalEdge = max(normalEdge, (1.0 - dot(a, b)) / uOutlineNormal);
        }
    }
    return clamp(max(depthEdge, normalEdge), 0.0, 1.0);
}

void main() {
    vec2 texel = vec2(uTexelX, uTexelY);
    vec3 colour = texture(uColour, vUv).rgb;

    // Light adds in linear space, so bloom goes on before the curve. Adding
    // it afterwards would brighten the dark half of the frame and clip the
    // bright half, which is the look people mean when they say bloom is
    // washed out.
    if (uBloomIntensity > 0.0) colour += texture(uBloom, vUv).rgb * uBloomIntensity;

    colour *= uExposure;

    if (uOutline > 0.0) {
        float edge = edgeAt(vUv, texel);
        colour *= mix(1.0, 1.0 - uOutline, edge);
    }

    // The medium, before the grade and before the curve: absorption is a
    // thing that happens to light on its way here, so it has to happen while
    // the numbers still are light. The sky's depth is the far plane, which is
    // exactly right -- underwater there is no sky, only more water.
    if (uFogDensity > 0.0) {
        float distance = linearDepth(texture(uDepth, vUv).r);
        colour = mix(colour, uFogColour, clamp(1.0 - exp(-distance * uFogDensity), 0.0, 1.0));
    }
    colour *= uTint;

    // Grading, still linear. Saturation about the frame's own luminance so a
    // grey stays grey.
    float luma = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    colour = mix(vec3(luma), colour, uSaturation);
    colour = max((colour - 0.5) * uContrast + 0.5, vec3(0.0));

    if (uVignette > 0.0) {
        vec2 d = (vUv - 0.5) * 2.0;
        float falloff = 1.0 - uVignette * dot(d, d) * 0.5;
        colour *= clamp(falloff, 0.0, 1.0);
    }

    fragColor = vec4(linearToSrgb(aces(colour)), 1.0);
}
)GLSL";

std::string makeFragment(const char* body) {
    return std::string("#version 460 core\n") + kCommonFragment + body;
}

// --------------------------------------------------------------- fly camera
struct FlyCamera {
    Vec3  position{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;    // degrees, 0 looks towards -Z
    float pitch = 0.0f;  // degrees, positive looks up
    float speed = 8.0f;

    Vec3 forward() const {
        float y = radians(yaw), p = radians(pitch);
        return {-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
    }
    Vec3 right() const { return normalize(cross(forward(), Vec3{0.0f, 1.0f, 0.0f})); }
    Vec3 target() const { return position + forward(); }

    // Recover yaw and pitch from a camera that was aimed with lookAt.
    void adoptFrom(const Camera& camera) {
        Vec3 direction = normalize(camera.target - camera.position);
        position = camera.position;
        pitch = degrees(std::asin(std::max(-1.0f, std::min(1.0f, direction.y))));
        yaw = degrees(std::atan2(-direction.x, -direction.z));
    }
};

void printCamera(const FlyCamera& fly, const Camera& camera) {
    Vec3 t = fly.target();
    std::printf("\n// camera, ready to paste into a scene\n");
    std::printf("scene.camera.projection = Camera::Projection::Perspective;\n");
    std::printf("scene.camera.fovY = radians(%.1ff);\n", degrees(camera.fovY));
    std::printf("scene.camera.lookAt({%.3ff, %.3ff, %.3ff}, {%.3ff, %.3ff, %.3ff});\n\n",
                fly.position.x, fly.position.y, fly.position.z, t.x, t.y, t.z);
    std::fflush(stdout);
}

bool savePixels(const std::string& path, int width, int height) {
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
    gl::PixelStorei(gl::PACK_ALIGNMENT, 1);
    gl::Finish();
    gl::ReadPixels(0, 0, width, height, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());

    // GL hands back rows bottom-up; images are stored top-down.
    ImageU8 image(width, height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = pixels.data() + size_t(height - 1 - y) * size_t(width) * 4;
        for (int x = 0; x < width; ++x) {
            image.set(x, y, {row[x * 4], row[x * 4 + 1], row[x * 4 + 2], 255});
        }
    }

    std::string error;
    if (!pngSave(path, image, &error)) {
        std::printf("snapshot failed: %s\n", error.c_str());
        return false;
    }
    std::printf("wrote %s\n", path.c_str());
    return true;
}

} // namespace

int runViewport(Scene& scene, const ViewportSettings& settings) {
    const bool snapshot = !settings.snapshotPath.empty();

    Window window;
    // Before the window exists, which `setIcon` allows: the alternative is a
    // frame that appears with the default icon and swaps it a moment later.
    if (!settings.icon.empty()) window.setIcon(settings.icon);

    std::string error;
    if (!window.create(settings.title, settings.width, settings.height, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }

    gl::ContextSettings contextSettings;
    contextSettings.samples = settings.msaa;
    void* context = nullptr;
    if (!gl::createContext(window.deviceContext(), contextSettings, &context, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("gl: %s | %s\n", gl::rendererString().c_str(), gl::versionString().c_str());

    // ---- resources
    ShaderProgram blockShader, skinShader;
    if (!blockShader.build(kBlockVertex, makeFragment(kBlockFragmentBody).c_str(), &error)) {
        std::printf("block shader: %s\n", error.c_str());
        return 1;
    }
    if (!skinShader.build(kSkinVertex, makeFragment(kSkinFragmentBody).c_str(), &error)) {
        std::printf("skin shader: %s\n", error.c_str());
        return 1;
    }

    ShaderProgram propShader;
    if (!propShader.build(kPropVertex, makeFragment(kPropFragmentBody).c_str(), &error)) {
        std::printf("prop shader: %s\n", error.c_str());
        return 1;
    }

    ShaderProgram skyShader;
    if (!skyShader.build(kSkyVertex, makeFragment(kSkyFragmentBody).c_str(), &error)) {
        std::printf("sky shader: %s\n", error.c_str());
        return 1;
    }
    // Core profile refuses to draw without a bound vertex array, even when the
    // shader invents its own positions.
    gl::GLuint skyVao = 0;
    gl::GenVertexArrays(1, &skyVao);

    // Interface. Failing to build one is not fatal: the world still draws, and
    // a viewport with no menu is what every scene here already is.
    Overlay overlay;
    if (settings.onOverlay && !overlay.create(nullptr, &error))
        std::printf("overlay: %s\n", error.c_str());

    // ---- the post chain
    //
    // Not optional. The geometry shaders now write linear light and nothing
    // else, so without the pass that tone maps it there is no picture -- only
    // unbounded values clamped by an eight-bit window. A half-working
    // fallback here would be a second lighting path to keep in step with the
    // first, which is the arrangement this engine refuses everywhere else.
    ShaderProgram brightShader, blurShader, compositeShader;
    if (!brightShader.build(kFullscreenVertex, kBrightFragment, &error) ||
        !blurShader.build(kFullscreenVertex, kBlurFragment, &error) ||
        !compositeShader.build(kFullscreenVertex, kCompositeFragment, &error)) {
        std::printf("post shader: %s\n", error.c_str());
        return 1;
    }

    RenderTarget sceneTarget;
    ColourTarget bloomA, bloomB;

    // The default effects, used when the host asked for none. All zero, which
    // means the composite pass is a tone map and nothing else -- exactly what
    // the four shaders used to do for themselves.
    const ViewportEffects kNoEffects;

    GlBlockTextureArray blockTextures;
    blockTextures.build(scene.world.registry(), scene.blockTextures);

    // Per-chunk meshes rather than one mesh for the world, because the world
    // is allowed to change now: `onFrame` may edit blocks, and the cache
    // rebuilds only the chunks whose stamp moved.
    ChunkMeshCache chunks;
    auto meshStart = std::chrono::steady_clock::now();
    ChunkMeshCache::Stats meshStats = chunks.update(scene.world);
    double meshSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - meshStart).count();

    std::printf("mesh: %zu triangles across %d chunks in %.2f s\n", chunks.triangleCount(),
                meshStats.cached, meshSeconds);

    // One mesh per distinct VoxelModel, built the first time it is seen and
    // then drawn once per placement. A hundred crates sharing a model cost
    // one model's geometry -- the same trade PropSet makes on the CPU side.
    std::unordered_map<const VoxelModel*, GpuPropMesh> propMeshes;

    // Entities are meshed one at a time, and re-meshed every frame.
    //
    // Unlike a chunk there is no cheap way to ask whether one changed: a pose
    // is seven joint rotations that the host may have touched, and comparing
    // them costs about what rebuilding costs. A player is roughly a hundred
    // and seventy quads, so the whole thing is a rounding error next to the
    // world -- and the alternative, meshing once, is what made a script
    // unable to spawn a character at all.
    //
    // The skin is the part that would be expensive to redo, so textures are
    // cached by the Skin they came from: a crowd wearing one skin uploads one
    // texture.
    std::vector<GpuSkinMesh> entityMeshes;
    std::unordered_map<const Skin*, GlTexture2D> skinTextures;
    SkinMeshData skinScratch;

    auto refreshEntities = [&]() {
        if (!scene.entities) { entityMeshes.clear(); return; }

        const std::vector<Entity>& list = scene.entities->entities();
        if (entityMeshes.size() != list.size()) entityMeshes = std::vector<GpuSkinMesh>(list.size());

        for (size_t i = 0; i < list.size(); ++i) {
            skinScratch.vertices.clear();
            skinScratch.indices.clear();
            buildEntityMesh(list[i], skinScratch);
            entityMeshes[i].upload(skinScratch);

            if (!list[i].skin) continue;
            GlTexture2D& texture = skinTextures[list[i].skin];
            if (texture.empty()) texture.upload(list[i].skin->image());
        }
    };

    refreshEntities();
    if (scene.entities) std::printf("entities: %zu\n", scene.entities->entityCount());

    // ---- camera
    FlyCamera fly;
    fly.speed = settings.moveSpeed;
    fly.adoptFrom(scene.camera);

    Camera camera = scene.camera;
    camera.projection = Camera::Projection::Perspective;

    gl::Enable(gl::DEPTH_TEST);
    gl::DepthFunc(gl::LEQUAL);
    gl::Enable(gl::CULL_FACE);
    gl::CullFace(gl::BACK);
    gl::FrontFace(gl::CCW);
    if (settings.msaa > 0) gl::Enable(gl::MULTISAMPLE);
    gl::BlendFunc(gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);

    gl::setSwapInterval(settings.vsync ? 1 : 0);

    if (!snapshot) {
        window.show();
        std::printf(
            "\ncontrols\n"
            "  right mouse   look around\n"
            "  W A S D       move, Space / Shift up and down\n"
            "  wheel         movement speed\n"
            "  F             print the camera as pasteable code\n"
            "  P             save out/viewport.png\n"
            "  Esc           quit\n\n");
    }

    auto lastTime = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    int frames = 0;
    double titleTimer = 0.0;
    int exitCode = 0;

    for (;;) {
        if (!window.pumpEvents()) break;

        auto now = std::chrono::steady_clock::now();
        float dt = float(std::chrono::duration<double>(now - lastTime).count());
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;  // a stall must not teleport the camera

        // The host gets the frame before anything is drawn, so whatever it
        // changes is what this frame shows rather than what the last one did.
        if (settings.onFrame) {
            ViewportFrame frame;
            frame.dt = dt;
            frame.time = elapsed;
            frame.index = frames;
            frame.input = snapshot ? nullptr : &window;
            if (!settings.onFrame(frame)) break;
        }
        elapsed += double(dt);

        // Only the chunks the host actually touched are rebuilt. On a still
        // world this finds nothing to do and costs one walk of the chunk map.
        chunks.update(scene.world);
        refreshEntities();

        // ---- input
        // A host that drives the camera also owns the keys: WASD means walk
        // there, not fly. Escape stays with the viewport, because a window
        // that cannot be closed is not a kindness.
        if (!snapshot && settings.hostCamera) {
            if (!settings.hostEscape && window.keyPressed(key::Escape)) break;
            // Held only while this window has focus, so alt-tabbing gives the
            // pointer back instead of fighting for it -- and never while the
            // host says it wants the pointer, which is what a menu says.
            const bool wanted = !(settings.freeCursor && *settings.freeCursor);
            window.setCursorCaptured(wanted && window.focused());
        }

        if (!snapshot && !settings.hostCamera) {
            if (window.keyPressed(key::Escape)) break;

            window.setCursorCaptured(window.mouseRightDown());
            if (window.mouseRightDown()) {
                Vec2 delta = window.mouseDelta();
                fly.yaw -= delta.x * settings.mouseSensitivity;
                fly.pitch -= delta.y * settings.mouseSensitivity;
                fly.pitch = std::max(-89.5f, std::min(89.5f, fly.pitch));
            }

            float wheel = window.wheelDelta();
            if (wheel != 0.0f) fly.speed = std::max(0.5f, fly.speed * std::pow(1.2f, wheel));

            Vec3 move{0.0f};
            Vec3 forward = fly.forward();
            Vec3 right = fly.right();
            if (window.keyDown(key::W)) move += forward;
            if (window.keyDown(key::S)) move -= forward;
            if (window.keyDown(key::D)) move += right;
            if (window.keyDown(key::A)) move -= right;
            if (window.keyDown(key::Space)) move.y += 1.0f;
            if (window.keyDown(key::Shift)) move.y -= 1.0f;

            if (lengthSq(move) > 0.0f) fly.position += normalize(move) * (fly.speed * dt);

            if (window.keyPressed(key::F)) printCamera(fly, camera);
        }

        // ---- draw
        int width = std::max(1, window.width());
        int height = std::max(1, window.height());

        if (settings.hostCamera) {
            // The host's camera, adopted back into the fly state so the sky
            // basis, the title and F keep working off one place.
            camera = scene.camera;
            fly.adoptFrom(camera);
        } else {
            camera.position = fly.position;
            camera.target = fly.target();
        }
        camera.aspect = float(width) / float(height);

        Mat4 viewProjection = camera.projectionMatrix(0.05f, 4096.0f) * camera.viewMatrix();

        const ViewportEffects& fx = settings.effects ? *settings.effects : kNoEffects;

        // The world goes into the offscreen target in linear light, and one
        // pass at the end turns it into a picture.
        if (!sceneTarget.resize(width, height)) {
            std::printf("render target: incomplete at %dx%d\n", width, height);
            exitCode = 1;
            break;
        }
        sceneTarget.bindForDraw();
        gl::Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);

        auto applyEnvironment = [&](const ShaderProgram& program) {
            program.use();
            program.setMat4("uViewProjection", viewProjection);
            program.setVec3("uSunDirection", scene.sun.direction);
            program.setVec3("uSunRadiance", scene.sun.radiance());
            program.setVec3("uSkyZenith", scene.sky.zenith);
            program.setVec3("uSkyHorizon", scene.sky.horizon);
            program.setVec3("uSkyGround", scene.sky.ground);
            program.setFloat("uSkyIntensity", scene.sky.intensity);
            program.setFloat("uAmbient", scene.ambientStrength);
            program.setFloat("uSunScale", 1.0f);
            program.setFloat("uCelBands", fx.celBands);
        };

        // Sky first, with depth testing off. Disabling the test also disables
        // depth writes, so the background never occludes the world.
        {
            Vec3 forward = fly.forward();
            Vec3 right = fly.right();
            Vec3 up = cross(right, forward);

            applyEnvironment(skyShader);
            skyShader.setVec3("uCameraForward", forward);
            skyShader.setVec3("uCameraRight", right);
            skyShader.setVec3("uCameraUp", up);
            skyShader.setFloat("uTanHalfFov", std::tan(camera.fovY * 0.5f));
            skyShader.setFloat("uAspect", camera.aspect);

            gl::Disable(gl::DEPTH_TEST);
            gl::BindVertexArray(skyVao);
            gl::DrawArrays(gl::TRIANGLES, 0, 3);
            gl::BindVertexArray(0);
            gl::Enable(gl::DEPTH_TEST);
        }

        applyEnvironment(blockShader);
        blockTextures.bind(0);
        blockShader.setInt("uBlocks", 0);
        blockShader.setFloat("uAlpha", 1.0f);
        chunks.drawOpaque();

        if (scene.props && !scene.props->empty()) {
            applyEnvironment(propShader);
            for (const PropSet::Flat& placement : scene.props->flats()) {
                if (!placement.model) continue;

                GpuPropMesh& mesh = propMeshes[placement.model];
                if (mesh.empty()) {
                    PropMeshData data;
                    buildPropMesh(*placement.model, data);
                    mesh.upload(data);
                    if (mesh.empty()) continue;   // an empty model, meshed once and skipped after
                }

                propShader.setMat4("uModel", placement.toWorld);
                propShader.setVec3("uTint", placement.tint);
                mesh.draw();
            }
        }

        if (!entityMeshes.empty() && scene.entities) {
            applyEnvironment(skinShader);
            skinShader.setInt("uSkin", 0);

            const std::vector<Entity>& list = scene.entities->entities();
            for (size_t i = 0; i < entityMeshes.size() && i < list.size(); ++i) {
                auto texture = skinTextures.find(list[i].skin);
                if (texture == skinTextures.end()) continue;
                texture->second.bind(0);
                entityMeshes[i].draw();
            }
        }

        {
            applyEnvironment(blockShader);
            blockTextures.bind(0);
            blockShader.setInt("uBlocks", 0);
            blockShader.setFloat("uAlpha", 0.66f);
            blockShader.setFloat("uSunScale", 0.12f);
            gl::Enable(gl::BLEND);
            gl::Disable(gl::CULL_FACE);
            chunks.drawTranslucent();
            gl::Enable(gl::CULL_FACE);
            gl::Disable(gl::BLEND);
        }

        // ---- post
        // Bloom first, into its own half-sized pair, ping-ponged between the
        // horizontal and the vertical half of the blur. Two targets rather
        // than one because a shader cannot read the texture it is writing.
        const bool wantBloom = fx.bloomIntensity > 0.0f;
        if (wantBloom) {
            const int bw = std::max(1, width / 2), bh = std::max(1, height / 2);
            if (bloomA.resize(bw, bh) && bloomB.resize(bw, bh)) {
                gl::Disable(gl::DEPTH_TEST);
                gl::Disable(gl::CULL_FACE);
                gl::BindVertexArray(skyVao);

                bloomA.bindForDraw();
                brightShader.use();
                sceneTarget.bindColour(0);
                brightShader.setInt("uSource", 0);
                brightShader.setFloat("uThreshold", fx.bloomThreshold);
                brightShader.setFloat("uSoftness", std::max(1e-3f, fx.bloomSoftness));
                gl::DrawArrays(gl::TRIANGLES, 0, 3);

                // Separable: one pass across, one down. Two passes of five
                // fetches beat one pass of twenty-five and give the same
                // Gaussian, which is the whole reason to separate it.
                for (int pass = 0; pass < 2; ++pass) {
                    const bool horizontal = pass == 0;
                    const ColourTarget& from = horizontal ? bloomA : bloomB;
                    const ColourTarget& to = horizontal ? bloomB : bloomA;

                    to.bindForDraw();
                    blurShader.use();
                    from.bindColour(0);
                    blurShader.setInt("uSource", 0);
                    blurShader.setFloat("uTexelX", horizontal ? 1.0f / float(bw) : 0.0f);
                    blurShader.setFloat("uTexelY", horizontal ? 0.0f : 1.0f / float(bh));
                    gl::DrawArrays(gl::TRIANGLES, 0, 3);
                }
                gl::Enable(gl::CULL_FACE);
                gl::Enable(gl::DEPTH_TEST);
            }
        }

        // Composite to the window.
        RenderTarget::bindDefault();
        gl::Viewport(0, 0, width, height);
        gl::Disable(gl::DEPTH_TEST);
        gl::Disable(gl::CULL_FACE);

        compositeShader.use();
        sceneTarget.bindColour(0);
        sceneTarget.bindNormal(1);
        sceneTarget.bindDepth(2);
        if (wantBloom && bloomA.valid()) bloomA.bindColour(3);

        compositeShader.setInt("uColour", 0);
        compositeShader.setInt("uNormal", 1);
        compositeShader.setInt("uDepth", 2);
        compositeShader.setInt("uBloom", 3);
        compositeShader.setFloat("uExposure", settings.exposure);
        compositeShader.setFloat("uBloomIntensity",
                                 wantBloom && bloomA.valid() ? fx.bloomIntensity : 0.0f);
        compositeShader.setFloat("uOutline", fx.outline);
        compositeShader.setFloat("uOutlineDepth", std::max(1e-3f, fx.outlineDepthTolerance));
        compositeShader.setFloat("uOutlineNormal", std::max(1e-3f, fx.outlineNormalTolerance));
        compositeShader.setFloat("uVignette", fx.vignette);
        compositeShader.setFloat("uSaturation", fx.saturation);
        compositeShader.setFloat("uContrast", fx.contrast);
        compositeShader.setVec3("uTint", fx.tint);
        compositeShader.setFloat("uFogDensity", std::max(0.0f, fx.fogDensity));
        compositeShader.setVec3("uFogColour", fx.fogColour);
        compositeShader.setFloat("uTexelX", 1.0f / float(width));
        compositeShader.setFloat("uTexelY", 1.0f / float(height));

        gl::BindVertexArray(skyVao);
        gl::DrawArrays(gl::TRIANGLES, 0, 3);
        gl::Enable(gl::CULL_FACE);
        gl::Enable(gl::DEPTH_TEST);

        // Interface last, over a finished frame. It is drawn before the
        // snapshot is read back so that a photograph of a menu is possible at
        // all -- which is the only way an interface gets checked from a build
        // script, the same argument the snapshot mode itself rests on.
        if (settings.onOverlay && overlay.ready() && (!snapshot || settings.snapshotOverlay)) {
            overlay.begin(width, height);
            settings.onOverlay(overlay);
            overlay.end();
        }

        if (snapshot && frames + 1 >= std::max(1, settings.snapshotFrames)) {
            exitCode = savePixels(settings.snapshotPath, width, height) ? 0 : 1;
            break;
        }
        if (snapshot) {
            // Still warming up. The buffers are never presented in snapshot
            // mode, so there is nothing to swap -- just count the frame and
            // go round again.
            ++frames;
            continue;
        }

        if (window.keyPressed(key::P)) savePixels("out/viewport.png", width, height);

        gl::swapBuffers(window.deviceContext());

        // ---- frame rate in the title bar
        ++frames;
        titleTimer += double(dt);
        if (titleTimer >= 0.5) {
            char title[320];
            std::snprintf(title, sizeof(title), "%s  |  %.0f fps  |  %.1f blocks/s%s%s",
                          settings.title.c_str(), frames / titleTimer, double(fly.speed),
                          settings.statusText && !settings.statusText->empty() ? "  |  " : "",
                          settings.statusText ? settings.statusText->c_str() : "");
            window.setTitle(title);
            frames = 0;
            titleTimer = 0.0;
        }
    }

    if (!snapshot) printCamera(fly, camera);

    chunks.clear();
    propMeshes.clear();
    for (auto& mesh : entityMeshes) mesh.destroy();
    for (auto& entry : skinTextures) entry.second.destroy();
    blockTextures.destroy();
    blockShader.destroy();
    skinShader.destroy();
    skyShader.destroy();
    if (skyVao) gl::DeleteVertexArrays(1, &skyVao);

    gl::destroyContext(window.deviceContext(), context);
    window.destroy();
    return exitCode;
}

} // namespace blocky
