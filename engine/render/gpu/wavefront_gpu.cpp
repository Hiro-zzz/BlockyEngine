#include "engine/render/gpu/wavefront_gpu.hpp"

#include "engine/render/trace/lights.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace blocky {
namespace gpu {
namespace {

// Everything every stage needs: the buffers, the uniforms, and the world.
// Split across several literals only because MSVC will not take one longer
// than 16380 bytes.
const char* kDeclsA = R"GLSL(
#version 430 core
layout(local_size_x = 64) in;

struct Material {
    vec4 albedoRough;
    vec4 emissionMetal;
    vec4 absorbIor;
    vec4 transOpaque;
    vec4 topTintFlag;
};

struct Light {
    vec4 originPower;
    vec4 edgeU;
    vec4 edgeV;
    vec4 normal;
    vec4 radiance;
};

// One path in flight. Its index is its pixel's index, which is what lets the
// resolve at the end be a plain add instead of an atomic.
struct Path {
    vec4  throughput;   // rgb, w = distance travelled so far
    vec4  radiance;     // rgb accumulated
    vec4  origin;       // xyz
    vec4  dir;          // xyz
    uvec4 state;        // x rng, y unused, z medium, w flags
    vec4  firstAlbedo;  // rgb, w = distance to the first surface
    vec4  firstNormal;  // rgb
};

// The normal of a voxel face is axis aligned, so it is one of six values and
// fits in an index. That frees the two components the face uv needs, and the
// hit stays thirty-two bytes.
// The hit comes out of `extend` with its material already resolved -- the
// block texture sampled or the skin texel fetched -- which is the same seam
// intersectScene() has on the CPU: above it nobody asks what the geometry was.
// `extend` resolves the whole material, not just the geometry: the block
// texture sampled, the skin texel fetched, the voxel palette looked up. That
// is the seam intersectScene() has on the CPU -- above it nobody asks what
// kind of thing was hit, and `shade` needs the id only to track a medium.
//
//   id >= 0   a block, and the id is its BlockId
//   id == -1  an entity box     -2  a sprite quad     -3  a prop voxel
struct Hit {
    vec4 posT;         // xyz position, w t; negative t is a miss
    vec4 albedoId;     // rgb albedo, w id
    vec4 normalMetal;  // xyz world normal, w metallic
    vec4 emissRough;   // rgb emission, w roughness
};

// A BVH node, exactly as Bvh::Node holds it. The CPU traversal is already an
// explicit stack rather than recursion, which is what makes it portable:
// GLSL has no recursion at all.
struct BvhNode {
    vec4  lo;
    vec4  hi;
    uvec4 meta;   // start, count, right child, split axis
};

// A sprite quad, from SpriteSet::Flat plus the Sprite it clothes.
struct Quad {
    mat4 toWorld;      // rigid, so the ray parameter is unscaled either way
    mat4 toLocal;
    vec4 halfSided;    // xy half extent, z double sided, w alpha cutoff
    vec4 uvMinMax;     // xy uvMin, zw uvMax
    vec4 tintRough;    // rgb tint, w roughness
    vec4 emissionTex;  // rgb emission, w texture layer or -1
    vec4 texSize;      // xy the real texture size inside the padded layer
};

// A prop: a small dense voxel grid placed by a rigid transform with uniform
// scale. Uniform because a non-uniform one would stretch the local ray
// direction per axis and `t` would stop meaning the same thing either side.
struct Grid {
    mat4  toWorld;
    mat4  toLocal;
    ivec4 dimsVoxels;   // xyz dims, w offset into voxelWords
    ivec4 materialBase; // x offset into voxMaterials
    vec4  tint;
    vec4  emissionScale;
};

struct VoxMaterial {
    vec4 albedoRough;
    vec4 emissionMetal;
};

// A flattened entity box, exactly as EntitySet::FlatBox holds it.
struct Box {
    mat4  toLocal;
    mat4  toWorld;
    vec4  sizeCutout;    // xyz extent in model pixels, w non-zero to alpha test
    vec4  aabbMinLayer;  // xyz world aabb low corner, w skin layer
    vec4  aabbMax;
    ivec4 faces[6];      // x, y, width, height; width zero means no face
};

struct Shadow {
    vec4 originDist;    // xyz origin, w max distance
    vec4 dirPath;       // xyz direction, w path index
    vec4 contribution;  // rgb, already multiplied by throughput
};

// Layer id * 6 + face, tint and overlay already composited, linear float.
uniform sampler2DArray uBlockTex;
uniform sampler2DArray uSkinTex;
uniform sampler2DArray uSpriteTex;
uniform int uBoxCount;
uniform int uSpriteNodes;
uniform int uPropNodes;

// counters[0] next active, [1] sun rays, [2] area rays, [3] current active.
uniform ivec3 uGridMin;
uniform ivec3 uGridDim;
uniform ivec3 uWorldMin;
uniform ivec3 uWorldMax;

uniform ivec2 uSize;
uniform int   uPixelCount;
uniform int   uWave;
uniform int   uBounce;
uniform int   uMaxBounces;
uniform int   uRouletteStart;
uniform float uMaxDistance;
uniform float uClampIndirect;
uniform uint  uSeed;
uniform int   uShadowCounter;

uniform vec3  uCamPos;
uniform vec3  uCamRight;
uniform vec3  uCamUp;
uniform vec3  uCamForward;
uniform float uHalfWidth;
uniform float uHalfHeight;
uniform float uAperture;
uniform float uFocusDistance;
uniform int   uOrthographic;
uniform float uOrthoHalfW;
uniform float uOrthoHalfH;

uniform vec3  uSunDirection;
uniform vec3  uSunRadiance;
uniform float uSunCosMax;
uniform int   uSunOn;

uniform vec3  uSkyZenith;
uniform vec3  uSkyHorizon;
uniform vec3  uSkyGround;
uniform float uSkyIntensity;
uniform int   uOverrideBackground;
uniform vec3  uBackground;

uniform int   uLightCount;
uniform float uTotalPower;

const float INF = uintBitsToFloat(0x7F800000u);
const float kPi = 3.14159265358979323846;
const float kTwoPi = 6.28318530717958647692;
const float kInvPi = 0.31830988618379067154;
const float kSurfaceBias = 1e-3;
const int kMaxChunkSteps = 8192;
const int kMaxBlockSteps = 65536;

vec3 normalOfFace(int face) {
    vec3 n = vec3(0.0);
    n[face >> 1] = ((face & 1) != 0) ? 1.0 : -1.0;
    return n;
}

const uint kFlagCountEmission = 1u;
const uint kFlagCaptured = 2u;
)GLSL";


// ------------------------------------------------------- buffer blocks
//
// One block per group, because the limit is sixteen storage blocks per
// compute stage and the driver counts what is *declared*, not what is used.
// The probe that established that is gone, but the number it produced is
// why these are not one shared prelude: with every buffer in every stage,
// nothing past the world and the entities would fit.
const char* kBufWorld = R"GLSL(
layout(std430, binding = 0)  readonly buffer ChunkIndexBuf { int   chunkIndex[]; };
layout(std430, binding = 1)  readonly buffer BlocksBuf     { uint  blockWords[]; };
)GLSL";

const char* kBufMaterials = R"GLSL(
layout(std430, binding = 2)  readonly buffer MaterialBuf   { Material materials[]; };
)GLSL";

const char* kBufLights = R"GLSL(
layout(std430, binding = 3)  readonly buffer LightBuf      { Light lights[]; };
layout(std430, binding = 4)  readonly buffer CdfBuf        { float lightCdf[]; };
)GLSL";

const char* kBufFrame = R"GLSL(
layout(std430, binding = 5)           buffer AccumBuf      { vec4  accum[]; };
layout(std430, binding = 6)           buffer AlbedoBuf     { vec4  aovAlbedo[]; };
layout(std430, binding = 7)           buffer NormalBuf     { vec4  aovNormalDepth[]; };
)GLSL";

const char* kBufPaths = R"GLSL(
layout(std430, binding = 8)           buffer PathBuf       { Path  paths[]; };
)GLSL";

const char* kBufHits = R"GLSL(
layout(std430, binding = 9)           buffer HitBuf        { Hit   hits[]; };
)GLSL";

const char* kBufActiveIn = R"GLSL(
layout(std430, binding = 10)          buffer ActiveInBuf   { uint  activeIn[]; };
)GLSL";

const char* kBufActiveOut = R"GLSL(
layout(std430, binding = 11)          buffer ActiveOutBuf  { uint  activeOut[]; };
)GLSL";

const char* kBufCounters = R"GLSL(
layout(std430, binding = 12) coherent buffer CounterBuf    { uint  counters[]; };
)GLSL";

const char* kBufShadow = R"GLSL(
layout(std430, binding = 13)          buffer ShadowBuf     { Shadow shadowQ[]; };
)GLSL";

// Work group counts for the next indirect dispatch, written by the GPU for
// the GPU: [0] the active paths, [1] the sun rays, [2] the area rays.
const char* kBufArgs = R"GLSL(
layout(std430, binding = 14) coherent buffer ArgsBuf       { uvec4 dispatchArgs[]; };
)GLSL";

// Everything that lives off the voxel lattice: entity boxes, sprite quads and
// prop grids, each with the tree that finds them.
const char* kBufGeometry = R"GLSL(
layout(std430, binding = 15) readonly buffer BoxBuf        { Box     boxes[]; };
layout(std430, binding = 16) readonly buffer SpriteNodeBuf { BvhNode spriteNodes[]; };
layout(std430, binding = 17) readonly buffer SpriteBuf     { Quad    quads[]; };
layout(std430, binding = 18) readonly buffer PropNodeBuf   { BvhNode propNodes[]; };
layout(std430, binding = 19) readonly buffer PropBuf       { Grid    grids[]; };
layout(std430, binding = 20) readonly buffer VoxelBuf      { uint    voxelWords[]; };
layout(std430, binding = 21) readonly buffer VoxelMatBuf   { VoxMaterial voxMaterials[]; };
)GLSL";

const char* kCommonRng = R"GLSL(
uint rngState;

uint nextUint() {
    rngState = rngState * 747796405u + 2891336453u;
    uint word = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return (word >> 22u) ^ word;
}

float nextFloat() { return float(nextUint() >> 8) * (1.0 / 16777216.0); }

void orthonormalBasis(vec3 n, out vec3 t, out vec3 b) {
    float sgn = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sgn + n.z);
    float d = n.x * n.y * a;
    t = vec3(1.0 + sgn * n.x * n.x * a, sgn * d, -sgn * n.x);
    b = vec3(d, sgn + n.y * n.y * a, -n.y);
}

vec3 toLocal(vec3 v, vec3 t, vec3 b, vec3 n) { return vec3(dot(v, t), dot(v, b), dot(v, n)); }
vec3 toWorld(vec3 v, vec3 t, vec3 b, vec3 n) { return t * v.x + b * v.y + n * v.z; }

vec3 uniformCone(vec3 axis, float cosThetaMax) {
    float u1 = nextFloat(), u2 = nextFloat();
    float cosTheta = 1.0 - u1 * (1.0 - cosThetaMax);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = kTwoPi * u2;
    vec3 t, b;
    orthonormalBasis(axis, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + axis * cosTheta);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

float fresnelDielectric(float cosThetaI, float eta) {
    cosThetaI = clamp(cosThetaI, 0.0, 1.0);
    float sin2T = eta * eta * (1.0 - cosThetaI * cosThetaI);
    if (sin2T >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sin2T);
    float rPar = (cosThetaI - eta * cosT) / (cosThetaI + eta * cosT);
    float rPer = (eta * cosThetaI - cosT) / (eta * cosThetaI + cosT);
    return 0.5 * (rPar * rPar + rPer * rPer);
}

bool refractRay(vec3 incident, vec3 normal, float eta, out vec3 outDir) {
    float cosI = -dot(incident, normal);
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) { outDir = vec3(0.0); return false; }
    float cosT = sqrt(1.0 - sin2T);
    outDir = normalize(incident * eta + normal * (eta * cosI - cosT));
    return true;
}

float roughnessToAlpha(float r) { return max(1e-3, r * r); }

float ggxD(vec3 h, float alpha) {
    float a2 = alpha * alpha;
    float c2 = h.z * h.z;
    float d = c2 * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-9);
}

float smithG1(vec3 v, float alpha) {
    float c2 = v.z * v.z;
    if (c2 <= 0.0) return 0.0;
    float tan2 = (1.0 - c2) / c2;
    return 2.0 / (1.0 + sqrt(1.0 + alpha * alpha * tan2));
}

vec3 sampleGgxVndf(vec3 wo, float alpha, float u1, float u2) {
    vec3 vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
    float lenSq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lenSq > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lenSq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u1);
    float phi = kTwoPi * u2;
    float px = r * cos(phi);
    float py = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    py = (1.0 - s) * sqrt(max(0.0, 1.0 - px * px)) + s * py;
    vec3 nh = t1 * px + t2 * py + vh * sqrt(max(0.0, 1.0 - px * px - py * py));
    return normalize(vec3(alpha * nh.x, alpha * nh.y, max(1e-6, nh.z)));
}

vec3 ggxEvalTimesCos(vec3 wo, vec3 wi, vec3 f0, float alpha) {
    if (wo.z <= 0.0 || wi.z <= 0.0) return vec3(0.0);
    vec3 h = normalize(wo + wi);
    float d = ggxD(h, alpha);
    float g = smithG1(wo, alpha) * smithG1(wi, alpha);
    vec3 f = fresnelSchlick(clamp(dot(wi, h), 0.0, 1.0), f0);
    return f * (d * g / max(4.0 * wo.z, 1e-9));
}
)GLSL";

const char* kCommonWalk = R"GLSL(
struct Dda { ivec3 cell; ivec3 stp; vec3 tMax; vec3 tDelta; float t; int axis; };

Dda ddaBegin(vec3 ro, vec3 rd, float cellSize, float tStart, int entryAxis) {
    Dda d;
    d.t = tStart; d.axis = entryAxis;
    d.tMax = vec3(INF); d.tDelta = vec3(INF); d.stp = ivec3(0);
    vec3 p = ro + rd * tStart;
    d.cell = ivec3(floor(p / cellSize));
    for (int a = 0; a < 3; ++a) {
        float dir = rd[a];
        if (dir > 0.0) {
            d.stp[a] = 1;
            d.tMax[a] = tStart + (float(d.cell[a] + 1) * cellSize - p[a]) / dir;
            d.tDelta[a] = cellSize / dir;
        } else if (dir < 0.0) {
            d.stp[a] = -1;
            d.tMax[a] = tStart + (float(d.cell[a]) * cellSize - p[a]) / dir;
            d.tDelta[a] = cellSize / -dir;
        }
    }
    return d;
}

float ddaExit(vec3 tMax) { return min(min(tMax.x, tMax.y), tMax.z); }

void ddaAdvance(inout Dda d) {
    int a = 0;
    if (d.tMax.y < d.tMax[a]) a = 1;
    if (d.tMax.z < d.tMax[a]) a = 2;
    d.t = d.tMax[a];
    d.cell[a] += d.stp[a];
    d.tMax[a] += d.tDelta[a];
    d.axis = a;
}

bool intersectAabb(vec3 ro, vec3 rd, vec3 lo, vec3 hi,
                   out float t0, out float t1, out int entryAxis, out int exitAxis) {
    t0 = 0.0; t1 = INF; entryAxis = -1; exitAxis = -1;
    for (int a = 0; a < 3; ++a) {
        float inv = 1.0 / rd[a];
        float tN = (lo[a] - ro[a]) * inv;
        float tF = (hi[a] - ro[a]) * inv;
        if (inv < 0.0) { float tmp = tN; tN = tF; tF = tmp; }
        if (tN > t0) { t0 = tN; entryAxis = a; }
        if (tF < t1) { t1 = tF; exitAxis = a; }
        if (t0 > t1) return false;
    }
    return true;
}

int chunkSlot(ivec3 c) {
    ivec3 g = c - uGridMin;
    if (any(lessThan(g, ivec3(0))) || any(greaterThanEqual(g, uGridDim))) return -1;
    return chunkIndex[(g.y * uGridDim.z + g.z) * uGridDim.x + g.x];
}

uint blockAt(int slot, ivec3 cell) {
    int idx = ((cell.y & 15) << 8) | ((cell.z & 15) << 4) | (cell.x & 15);
    uint word = blockWords[uint(slot) * 2048u + uint(idx >> 1)];
    return ((idx & 1) == 0) ? (word & 0xFFFFu) : (word >> 16);
}

vec2 faceUv(vec3 local, int axis, int stepSign) {
    if (axis == 0) return vec2(stepSign > 0 ? 1.0 - local.z : local.z, 1.0 - local.y);
    if (axis == 1) return vec2(local.x, stepSign > 0 ? 1.0 - local.z : local.z);
    return vec2(stepSign > 0 ? local.x : 1.0 - local.x, 1.0 - local.y);
}

bool traceWorld(vec3 ro, vec3 rd, float maxDistance, int passThrough,
                out float outT, out vec3 outPos, out int outFace, out vec2 outUv, out int outId) {
    outT = 0.0; outPos = vec3(0.0); outFace = 3; outUv = vec2(0.0); outId = 0;

    bool mediumMode = (passThrough != 0);
    vec3 lo = vec3(uWorldMin);
    vec3 hi = vec3(uWorldMax) + vec3(1.0);

    float t0, t1; int entryAxis, exitAxis;
    if (!intersectAabb(ro, rd, lo, hi, t0, t1, entryAxis, exitAxis)) return false;

    bool clipped = maxDistance < t1;
    t1 = min(t1, maxDistance);
    if (t0 > t1) return false;

    Dda chunks = ddaBegin(ro, rd, 16.0, t0, entryAxis);

    for (int outer = 0; outer < kMaxChunkSteps; ++outer) {
        if (chunks.t > t1) break;
        int slot = chunkSlot(chunks.cell);
        if (slot >= 0) {
            float segEnd = min(ddaExit(chunks.tMax), t1);
            Dda d = ddaBegin(ro, rd, 1.0, chunks.t, chunks.axis);
            for (int inner = 0; inner < kMaxBlockSteps; ++inner) {
                if (d.t > segEnd) break;
                if ((d.cell >> 4) != chunks.cell) {
                    if (ddaExit(d.tMax) > segEnd) break;
                    ddaAdvance(d);
                    continue;
                }
                int id = int(blockAt(slot, d.cell));
                if (id != passThrough) {
                    int axis = d.axis;
                    int stepSign = (axis >= 0 && d.stp[axis] != 0) ? d.stp[axis] : 1;
                    if (axis < 0) { axis = 1; stepSign = 1; }
                    outT = d.t;
                    outPos = ro + rd * d.t;
                    outFace = axis * 2 + ((stepSign < 0) ? 1 : 0);
                    outUv = faceUv(clamp(outPos - vec3(d.cell), vec3(0.0), vec3(1.0)),
                                   axis, stepSign);
                    outId = id;
                    return true;
                }
                if (ddaExit(d.tMax) > segEnd) break;
                ddaAdvance(d);
            }
        } else if (mediumMode) {
            int axis = chunks.axis < 0 ? 1 : chunks.axis;
            int stepSign = rd[axis] > 0.0 ? 1 : -1;
            outT = chunks.t; outPos = ro + rd * chunks.t;
            outFace = axis * 2 + ((stepSign < 0) ? 1 : 0);
            outUv = faceUv(clamp(outPos - floor(outPos), vec3(0.0), vec3(1.0)), axis, stepSign);
            outId = 0;
            return true;
        }
        if (ddaExit(chunks.tMax) > t1) break;
        ddaAdvance(chunks);
    }

    if (mediumMode && !clipped) {
        int axis = exitAxis < 0 ? 1 : exitAxis;
        int stepSign = rd[axis] > 0.0 ? 1 : -1;
        outT = t1; outPos = ro + rd * t1;
        outFace = axis * 2 + ((stepSign < 0) ? 1 : 0);
        outUv = faceUv(clamp(outPos - floor(outPos), vec3(0.0), vec3(1.0)), axis, stepSign);
        outId = 0;
        return true;
    }
    return false;
}

// Defined below, next to the box maths it belongs with. GLSL wants the
// declaration first, and a shadow ray has to ask about entities before it
// walks a single chunk.
bool intersectEntities(vec3 ro, vec3 rd, float maxDistance, bool anyHit,
                       out float outT, out vec3 outNormal, out vec3 outAlbedo);
bool intersectSprites(vec3 ro, vec3 rd, float maxDistance, bool anyHit,
                      out float outT, out vec3 outNormal, out vec3 outAlbedo,
                      out vec3 outEmission, out float outRoughness);
bool intersectProps(vec3 ro, vec3 rd, float maxDistance, bool anyHit,
                    out float outT, out vec3 outNormal, out vec3 outAlbedo,
                    out vec3 outEmission, out float outRoughness, out float outMetallic);

vec3 transmittance(vec3 ro, vec3 rd, float maxDistance) {
    vec3 result = vec3(1.0);

    // Entities, sprites and props are all opaque where they are not cut away,
    // so one in the way ends the shadow ray outright -- which is the order
    // sceneTransmittance() asks in, before it walks a single chunk.
    {
        float t; vec3 n; vec3 a; vec3 e; float r; float m;
        if (intersectEntities(ro, rd, maxDistance, true, t, n, a)) return vec3(0.0);
        if (intersectSprites(ro, rd, maxDistance, true, t, n, a, e, r)) return vec3(0.0);
        if (intersectProps(ro, rd, maxDistance, true, t, n, a, e, r, m)) return vec3(0.0);
    }
    vec3 lo = vec3(uWorldMin);
    vec3 hi = vec3(uWorldMax) + vec3(1.0);

    float t0, t1; int entryAxis, exitAxis;
    if (!intersectAabb(ro, rd, lo, hi, t0, t1, entryAxis, exitAxis)) return result;
    t1 = min(t1, maxDistance);
    if (t0 > t1) return result;

    Dda chunks = ddaBegin(ro, rd, 16.0, t0, entryAxis);
    for (int outer = 0; outer < kMaxChunkSteps; ++outer) {
        if (chunks.t > t1) break;
        int slot = chunkSlot(chunks.cell);
        if (slot >= 0) {
            float segEnd = min(ddaExit(chunks.tMax), t1);
            Dda d = ddaBegin(ro, rd, 1.0, chunks.t, chunks.axis);
            for (int inner = 0; inner < kMaxBlockSteps; ++inner) {
                if (d.t > segEnd) break;
                if ((d.cell >> 4) != chunks.cell) {
                    if (ddaExit(d.tMax) > segEnd) break;
                    ddaAdvance(d);
                    continue;
                }
                int id = int(blockAt(slot, d.cell));
                if (id != 0) {
                    Material m = materials[id];
                    if (m.transOpaque.y > 0.5) return vec3(0.0);
                    if (m.transOpaque.x > 0.0) {
                        float len = max(0.0, min(ddaExit(d.tMax), segEnd) - d.t);
                        result *= exp(-m.absorbIor.rgb * len);
                        if (max(result.r, max(result.g, result.b)) < 1e-4) return vec3(0.0);
                    }
                }
                if (ddaExit(d.tMax) > segEnd) break;
                ddaAdvance(d);
            }
        }
        if (ddaExit(chunks.tMax) > t1) break;
        ddaAdvance(chunks);
    }
    return result;
}

)GLSL";

// Split from the block above only because MSVC will not take a string
// literal longer than 16380 bytes. The two are always compiled together.
const char* kCommonOffGrid = R"GLSL(
// boxFaceUv() from model.cpp. u is chosen so each face reads correctly from
// outside the box, and v always runs downwards in texture space.
vec2 boxFaceUv(vec3 local, vec3 size, int face) {
    vec3 t = vec3(size.x > 0.0 ? local.x / size.x : 0.0,
                  size.y > 0.0 ? local.y / size.y : 0.0,
                  size.z > 0.0 ? local.z / size.z : 0.0);
    if (face == 4) return vec2(1.0 - t.x, 1.0 - t.y);   // front
    if (face == 5) return vec2(t.x,       1.0 - t.y);   // back
    if (face == 1) return vec2(1.0 - t.z, 1.0 - t.y);   // right
    if (face == 0) return vec2(t.z,       1.0 - t.y);   // left
    if (face == 3) return vec2(1.0 - t.x, t.z);         // top
    return vec2(1.0 - t.x, 1.0 - t.z);                  // bottom
}

// Nearest entity box along the ray, with the alpha cutout applied. A linear
// scan, like the CPU: sixty boxes for a couple of characters, and a tree over
// that many costs more to walk than it saves.
bool intersectEntities(vec3 ro, vec3 rd, float maxDistance, bool anyHit,
                       out float outT, out vec3 outNormal, out vec3 outAlbedo) {
    outT = 0.0; outNormal = vec3(0.0); outAlbedo = vec3(0.0);
    bool found = false;
    float best = maxDistance;

    for (int i = 0; i < uBoxCount; ++i) {
        // Cheap world-space reject before touching the matrices.
        float w0 = 0.0, w1 = best;
        bool ok = true;
        for (int a = 0; a < 3; ++a) {
            float inv = 1.0 / rd[a];
            float n = (boxes[i].aabbMinLayer[a] - ro[a]) * inv;
            float f = (boxes[i].aabbMax[a] - ro[a]) * inv;
            if (inv < 0.0) { float tmp = n; n = f; f = tmp; }
            w0 = max(w0, n);
            w1 = min(w1, f);
            if (w0 > w1) { ok = false; break; }
        }
        if (!ok) continue;

        // Box-local space, where the box is the axis-aligned [0, size] slab.
        vec3 lo = (boxes[i].toLocal * vec4(ro, 1.0)).xyz;
        vec3 ld = (boxes[i].toLocal * vec4(rd, 0.0)).xyz;

        float s0 = 0.0, s1 = INF;
        int enterAxis = -1;
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            float inv = 1.0 / ld[a];
            float n = (0.0 - lo[a]) * inv;
            float f = (boxes[i].sizeCutout[a] - lo[a]) * inv;
            if (inv < 0.0) { float tmp = n; n = f; f = tmp; }
            if (n > s0) { s0 = n; enterAxis = a; }
            if (f < s1) s1 = f;
            if (s0 > s1) { inside = false; break; }
        }
        if (!inside) continue;

        // A ray starting inside the box has no entry face to shade.
        if (enterAxis < 0) continue;
        if (s0 <= 1e-4 || s0 >= best) continue;

        vec3 localPoint = lo + ld * s0;
        int face = enterAxis * 2 + ((ld[enterAxis] < 0.0) ? 1 : 0);

        ivec4 rect = boxes[i].faces[face];
        if (rect.z <= 0 || rect.w <= 0) continue;

        vec2 uv = boxFaceUv(localPoint, boxes[i].sizeCutout.xyz, face);
        int texX = min(rect.x + int(clamp(uv.x, 0.0, 1.0) * float(rect.z)), rect.x + rect.z - 1);
        int texY = min(rect.y + int(clamp(uv.y, 0.0, 1.0) * float(rect.w)), rect.y + rect.w - 1);

        vec4 texel = texelFetch(uSkinTex, ivec3(texX, texY, int(boxes[i].aabbMinLayer.w)), 0);
        if (boxes[i].sizeCutout.w > 0.5 && texel.a < 0.5) continue;

        best = s0;
        found = true;
        outT = s0;
        outAlbedo = texel.rgb;
        outNormal = normalize((boxes[i].toWorld * vec4(normalOfFace(face), 0.0)).xyz);
        if (anyHit) return true;
    }
    return found;
}

// ------------------------------------------------------ off the lattice
//
// Sprites and props both hang in open air rather than on the voxel grid, so
// both are found through a tree. What follows is Bvh::traverse with the
// lambda inlined, twice: GLSL has neither templates nor function pointers,
// and two copies of twenty lines beat an indirection the compiler would have
// to undo anyway.

bool slabHit(vec3 lo, vec3 hi, vec3 origin, vec3 invDir, float tMax) {
    float t0 = 0.0, t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        float n = (lo[a] - origin[a]) * invDir[a];
        float f = (hi[a] - origin[a]) * invDir[a];
        if (invDir[a] < 0.0) { float tmp = n; n = f; f = tmp; }
        if (n > t0) t0 = n;
        if (f < t1) t1 = f;
        if (t0 > t1) return false;
    }
    return true;
}

// One quad in its own space. hitQuad() in sprite_set.cpp.
bool hitQuad(int index, vec3 ro, vec3 rd, float tLimit,
             out float outT, out vec3 outNormal, out vec3 outAlbedo) {
    outT = 0.0; outNormal = vec3(0.0); outAlbedo = vec3(0.0);

    vec3 lo = (quads[index].toLocal * vec4(ro, 1.0)).xyz;
    vec3 ld = (quads[index].toLocal * vec4(rd, 0.0)).xyz;

    if (abs(ld.z) < 1e-9) return false;              // parallel to the plane

    bool fromBehind = ld.z > 0.0;
    if (fromBehind && quads[index].halfSided.z < 0.5) return false;

    float t = -lo.z / ld.z;
    if (t <= 1e-4 || t >= tLimit) return false;

    vec2 halfExtent = quads[index].halfSided.xy;
    float x = lo.x + ld.x * t;
    float y = lo.y + ld.y * t;
    if (abs(x) > halfExtent.x || abs(y) > halfExtent.y) return false;

    float u = halfExtent.x > 0.0 ? (x + halfExtent.x) / (2.0 * halfExtent.x) : 0.0;
    float v = halfExtent.y > 0.0 ? 1.0 - (y + halfExtent.y) / (2.0 * halfExtent.y) : 0.0;
    vec2 uv = mix(quads[index].uvMinMax.xy, quads[index].uvMinMax.zw,
                  clamp(vec2(u, v), 0.0, 1.0));

    vec3 albedo = quads[index].tintRough.rgb;
    int layer = int(quads[index].emissionTex.w);
    if (layer >= 0) {
        // Texture::sample indexes texels directly, so this does too: the same
        // int(uv * size), clamped the same way, and no filtering between.
        vec2 texSize = quads[index].texSize.xy;
        ivec2 texel = clamp(ivec2(uv * texSize), ivec2(0), ivec2(texSize) - ivec2(1));
        vec4 c = texelFetch(uSpriteTex, ivec3(texel, layer), 0);
        // The cutout is what makes a glyph a glyph rather than the rectangle
        // it is drawn on: a rejected texel lets the ray carry on untouched.
        if (c.a < quads[index].halfSided.w) return false;
        albedo *= c.rgb;
    }

    vec3 n = normalize((quads[index].toWorld * vec4(0.0, 0.0, 1.0, 0.0)).xyz);
    outT = t;
    outNormal = fromBehind ? -n : n;
    outAlbedo = albedo;
    return true;
}

bool intersectSprites(vec3 ro, vec3 rd, float maxDistance, bool anyHit,
                      out float outT, out vec3 outNormal, out vec3 outAlbedo,
                      out vec3 outEmission, out float outRoughness) {
    outT = 0.0; outNormal = vec3(0.0); outAlbedo = vec3(0.0);
    outEmission = vec3(0.0); outRoughness = 1.0;
    if (uSpriteNodes <= 0) return false;

    vec3 invDir = vec3(1.0) / rd;
    uint stack[64];
    int top = 0;
    stack[top++] = 0u;

    float tMax = maxDistance;
    bool found = false;

    while (top > 0) {
        uint ni = stack[--top];
        if (!slabHit(spriteNodes[ni].lo.xyz, spriteNodes[ni].hi.xyz, ro, invDir, tMax)) continue;

        uint count = spriteNodes[ni].meta.y;
        if (count > 0u) {
            uint start = spriteNodes[ni].meta.x;
            for (uint i = start; i < start + count; ++i) {
                float t; vec3 n; vec3 a;
                if (!hitQuad(int(i), ro, rd, tMax, t, n, a)) continue;
                tMax = t;
                found = true;
                outT = t; outNormal = n; outAlbedo = a;
                outEmission = quads[i].emissionTex.rgb;
                outRoughness = quads[i].tintRough.w;
                if (anyHit) return true;
            }
            continue;
        }

        // Far child pushed first, so the near one is popped and tested first
        // and its hits tighten tMax before the far side is walked.
        uint left = ni + 1u;
        uint right = spriteNodes[ni].meta.z;
        if (invDir[spriteNodes[ni].meta.w] < 0.0) {
            stack[top++] = left;  stack[top++] = right;
        } else {
            stack[top++] = right; stack[top++] = left;
        }
    }
    return found;
}

// One prop grid. Amanatides and Woo again, one level, because the grid is
// small enough that there is no emptiness worth a second level to skip.
bool hitGrid(int index, vec3 ro, vec3 rd, float tLimit,
             out float outT, out vec3 outNormal, out int outMaterial) {
    outT = 0.0; outNormal = vec3(0.0); outMaterial = 0;

    vec3 o = (grids[index].toLocal * vec4(ro, 1.0)).xyz;
    vec3 d = (grids[index].toLocal * vec4(rd, 0.0)).xyz;

    ivec3 dims = grids[index].dimsVoxels.xyz;

    float t0 = 1e-4, t1 = tLimit;
    int entryAxis = -1;
    for (int a = 0; a < 3; ++a) {
        float inv = 1.0 / d[a];
        float n = (0.0 - o[a]) * inv;
        float f = (float(dims[a]) - o[a]) * inv;
        if (inv < 0.0) { float tmp = n; n = f; f = tmp; }
        if (n > t0) { t0 = n; entryAxis = a; }
        if (f < t1) t1 = f;
        if (t0 > t1) return false;
    }

    vec3 point = o + d * t0;
    ivec3 cell = clamp(ivec3(floor(point)), ivec3(0), dims - ivec3(1));

    ivec3 stp = ivec3(0);
    vec3 tMaxAxis = vec3(INF);
    vec3 tDelta = vec3(INF);
    for (int a = 0; a < 3; ++a) {
        if (d[a] > 0.0) {
            stp[a] = 1;
            tMaxAxis[a] = t0 + (float(cell[a] + 1) - point[a]) / d[a];
            tDelta[a] = 1.0 / d[a];
        } else if (d[a] < 0.0) {
            stp[a] = -1;
            tMaxAxis[a] = t0 + (float(cell[a]) - point[a]) / d[a];
            tDelta[a] = 1.0 / -d[a];
        }
    }

    float t = t0;
    int axis = entryAxis;
    int base = grids[index].dimsVoxels.w;

    for (int step = 0; step < 4096; ++step) {
        if (t > t1) break;
        if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, dims))) break;

        int vi = (cell.y * dims.z + cell.z) * dims.x + cell.x;
        uint word = voxelWords[uint(base + (vi >> 1))];
        uint material = ((vi & 1) == 0) ? (word & 0xFFFFu) : (word >> 16);

        if (material != 0u) {
            // A ray that started inside the grid has no entry plane to shade,
            // so give it something sane rather than an undefined normal.
            int shadeAxis = axis < 0 ? 1 : axis;
            int shadeSign = axis < 0 ? 1 : stp[shadeAxis];
            outT = t;
            outNormal = vec3(0.0);
            outNormal[shadeAxis] = float(-shadeSign);
            outMaterial = grids[index].materialBase.x + int(material);
            return true;
        }

        int a = 0;
        if (tMaxAxis.y < tMaxAxis[a]) a = 1;
        if (tMaxAxis.z < tMaxAxis[a]) a = 2;
        if (tMaxAxis[a] > t1) break;
        t = tMaxAxis[a];
        cell[a] += stp[a];
        tMaxAxis[a] += tDelta[a];
        axis = a;
    }
    return false;
}

bool intersectProps(vec3 ro, vec3 rd, float maxDistance, bool anyHit,
                    out float outT, out vec3 outNormal, out vec3 outAlbedo,
                    out vec3 outEmission, out float outRoughness, out float outMetallic) {
    outT = 0.0; outNormal = vec3(0.0); outAlbedo = vec3(0.0);
    outEmission = vec3(0.0); outRoughness = 1.0; outMetallic = 0.0;
    if (uPropNodes <= 0) return false;

    vec3 invDir = vec3(1.0) / rd;
    uint stack[64];
    int top = 0;
    stack[top++] = 0u;

    float tMax = maxDistance;
    bool found = false;

    while (top > 0) {
        uint ni = stack[--top];
        if (!slabHit(propNodes[ni].lo.xyz, propNodes[ni].hi.xyz, ro, invDir, tMax)) continue;

        uint count = propNodes[ni].meta.y;
        if (count > 0u) {
            uint start = propNodes[ni].meta.x;
            for (uint i = start; i < start + count; ++i) {
                float t; vec3 n; int mat;
                if (!hitGrid(int(i), ro, rd, tMax, t, n, mat)) continue;
                tMax = t;
                found = true;
                outT = t;
                outNormal = normalize((grids[i].toWorld * vec4(n, 0.0)).xyz);
                outAlbedo = voxMaterials[mat].albedoRough.rgb * grids[i].tint.rgb;
                outEmission = voxMaterials[mat].emissionMetal.rgb * grids[i].emissionScale.rgb;
                outRoughness = voxMaterials[mat].albedoRough.a;
                outMetallic = voxMaterials[mat].emissionMetal.a;
                if (anyHit) return true;
            }
            continue;
        }

        uint left = ni + 1u;
        uint right = propNodes[ni].meta.z;
        if (invDir[propNodes[ni].meta.w] < 0.0) {
            stack[top++] = left;  stack[top++] = right;
        } else {
            stack[top++] = right; stack[top++] = left;
        }
    }
    return found;
}

)GLSL";

// Touches no buffer, so `shade` can have these without dragging the whole
// traversal -- and the six storage blocks the traversal would bring with it.
const char* kShadeHelpers = R"GLSL(
vec3 skySample(vec3 dir) {
    float h = dir.y;
    if (h >= 0.0) return mix(uSkyHorizon, uSkyZenith, pow(clamp(h, 0.0, 1.0), 0.45)) * uSkyIntensity;
    return mix(uSkyHorizon, uSkyGround, clamp(-h * 3.0, 0.0, 1.0)) * uSkyIntensity;
}

vec3 clampFirefly(vec3 c, float limit) {
    if (limit <= 0.0) return c;
    float m = max(c.r, max(c.g, c.b));
    return m > limit ? c * (limit / m) : c;
}
)GLSL";

// ------------------------------------------------------------------- stages

const char* kGenerate = R"GLSL(
void main() {
    uint p = gl_GlobalInvocationID.x;
    if (p >= uint(uPixelCount)) return;

    ivec2 xy = ivec2(int(p) % uSize.x, int(p) / uSize.x);

    rngState = p * 0x9E3779B9u + uint(uWave) * 0x85EBCA6Bu + uSeed * 0xC2B2AE35u;
    nextUint();
    nextUint();

    float u = (float(xy.x) + nextFloat()) / float(uSize.x);
    float v = (float(xy.y) + nextFloat()) / float(uSize.y);
    float sx = 2.0 * u - 1.0;
    float sy = 1.0 - 2.0 * v;

    vec3 ro, rd;
    if (uOrthographic != 0) {
        ro = uCamPos + uCamRight * (sx * uOrthoHalfW) + uCamUp * (sy * uOrthoHalfH);
        rd = uCamForward;
    } else {
        ro = uCamPos;
        rd = normalize(uCamForward + uCamRight * (sx * uHalfWidth) + uCamUp * (sy * uHalfHeight));
        if (uAperture > 0.0) {
            float focalScale = uFocusDistance / max(dot(rd, uCamForward), 1e-4);
            vec3 focal = ro + rd * focalScale;
            float a = nextFloat() * kTwoPi;
            float rr = sqrt(nextFloat());
            vec3 offset = uCamRight * (cos(a) * rr * uAperture) +
                          uCamUp * (sin(a) * rr * uAperture);
            ro = uCamPos + offset;
            rd = normalize(focal - ro);
        }
    }

    paths[p].throughput = vec4(1.0, 1.0, 1.0, 0.0);
    paths[p].radiance = vec4(0.0);
    paths[p].origin = vec4(ro, 0.0);
    paths[p].dir = vec4(rd, 0.0);
    paths[p].state = uvec4(rngState, 0u, 0u, kFlagCountEmission);
    paths[p].firstAlbedo = vec4(1.0, 1.0, 1.0, -1.0);
    paths[p].firstNormal = vec4(0.0);

    activeIn[p] = p;
}
)GLSL";

const char* kExtend = R"GLSL(
void main() {
    uint k = gl_GlobalInvocationID.x;
    if (k >= counters[3]) return;
    uint p = activeIn[k];

    vec3 ro = paths[p].origin.xyz;
    vec3 rd = paths[p].dir.xyz;

    float t; vec3 pos; int face; vec2 uv; int id;
    bool found = traceWorld(ro, rd, uMaxDistance, int(paths[p].state.z), t, pos, face, uv, id);

    float nearest = found ? t : uMaxDistance;
    vec3 albedo = vec3(1.0);
    vec3 normal = vec3(0.0, 1.0, 0.0);
    vec3 emission = vec3(0.0);
    float roughness = 1.0;
    float metallic = 0.0;
    int outId = 0;

    if (found) {
        albedo = textureLod(uBlockTex, vec3(uv, float(id * 6 + face)), 0.0).rgb;
        normal = normalOfFace(face);
        emission = materials[id].emissionMetal.rgb;
        roughness = materials[id].albedoRough.a;
        metallic = materials[id].emissionMetal.a;
        outId = id;
    }

    // Nothing here is skipped for the ray being inside a medium: a character
    // standing in the shallows is still standing there, and so is a bubble
    // hanging in the water above them. The order is intersectScene's.
    float ht; vec3 hn; vec3 ha; vec3 he; float hr; float hm;

    // Entity: skin, cloth and leather. Matte, never metallic, never a medium.
    if (intersectEntities(ro, rd, nearest, false, ht, hn, ha)) {
        nearest = ht; albedo = ha; normal = hn;
        emission = vec3(0.0); roughness = 1.0; metallic = 0.0;
        outId = -1;
        found = true;
    }

    // Sprite: paper, ash and glowing dust. Emission comes through, so an
    // ember reads as an ember -- though it lights only itself.
    if (intersectSprites(ro, rd, nearest, false, ht, hn, ha, he, hr)) {
        nearest = ht; albedo = ha; normal = hn;
        emission = he; roughness = hr; metallic = 0.0;
        outId = -2;
        found = true;
    }

    // Prop: a full voxel material, metallic included, so a gold ingot is an
    // actual conductor rather than a yellow brick. Never a medium.
    if (intersectProps(ro, rd, nearest, false, ht, hn, ha, he, hr, hm)) {
        nearest = ht; albedo = ha; normal = hn;
        emission = he; roughness = hr; metallic = hm;
        outId = -3;
        found = true;
    }

    hits[p].posT = vec4(ro + rd * nearest, found ? nearest : -1.0);
    hits[p].albedoId = vec4(albedo, float(outId));
    hits[p].normalMetal = vec4(normal, metallic);
    hits[p].emissRough = vec4(emission, roughness);
}
)GLSL";

const char* kShade = R"GLSL(
void main() {
    uint k = gl_GlobalInvocationID.x;
    if (k >= counters[3]) return;
    uint p = activeIn[k];

    rngState = paths[p].state.x;

    vec3 throughput = paths[p].throughput.rgb;
    vec3 radiance = paths[p].radiance.rgb;
    float travelled = paths[p].throughput.w;
    int medium = int(paths[p].state.z);
    uint flags = paths[p].state.w;

    vec3 ro = paths[p].origin.xyz;
    vec3 rd = paths[p].dir.xyz;

    float t = hits[p].posT.w;
    if (t < 0.0) {
        vec3 background = (uBounce == 0 && uOverrideBackground != 0) ? uBackground : skySample(rd);
        paths[p].radiance = vec4(radiance + throughput * background, 0.0);
        return;                                    // the path ends here
    }

    vec3 position = hits[p].posT.xyz;
    vec3 normal = hits[p].normalMetal.xyz;
    vec3 albedo = hits[p].albedoId.rgb;
    int id = int(hits[p].albedoId.w);
    float metallic = hits[p].normalMetal.w;
    float roughness = hits[p].emissRough.w;
    vec3 emission = hits[p].emissRough.rgb;

    // Only a block can be a medium: refraction is tracked by the BlockId the
    // ray is inside, and nothing off the lattice is a block.
    bool isBlock = (id >= 0);

    travelled += t;
    if (medium != 0) throughput *= exp(-materials[medium].absorbIor.rgb * t);
    if ((flags & kFlagCountEmission) != 0u && max(emission.r, max(emission.g, emission.b)) > 0.0) {
        vec3 contribution = throughput * emission;
        radiance += (uBounce == 0) ? contribution : clampFirefly(contribution, uClampIndirect);
    }

    if (uBounce >= uMaxBounces) {
        paths[p].radiance = vec4(radiance, 0.0);
        return;
    }

    vec3 wo = -rd;
    bool leaving  = (medium != 0) && isBlock && (id == 0);
    bool entering = (medium == 0) && isBlock && (materials[id].transOpaque.x > 0.0);

    if (leaving || entering) {
        float mediumIor = leaving ? materials[medium].absorbIor.a : materials[id].absorbIor.a;
        float eta = leaving ? mediumIor : 1.0 / mediumIor;
        float reflectance = fresnelDielectric(clamp(dot(normal, wo), 0.0, 1.0), eta);

        vec3 next;
        if (nextFloat() < reflectance) {
            next = reflect(rd, normal);
        } else if (refractRay(rd, normal, eta, next)) {
            medium = leaving ? 0 : id;
        } else {
            next = reflect(rd, normal);
        }

        paths[p].throughput = vec4(throughput, travelled);
        paths[p].radiance = vec4(radiance, 0.0);
        paths[p].origin = vec4(position + next * kSurfaceBias, 0.0);
        paths[p].dir = vec4(next, 0.0);
        paths[p].state = uvec4(rngState, 0u, uint(medium), flags | kFlagCountEmission);
        activeOut[atomicAdd(counters[0], 1u)] = p;
        return;
    }

    // The material came resolved out of `extend`, so the lobe follows from
    // two numbers rather than from what kind of thing was hit.
    float alpha = roughnessToAlpha(roughness);
    int lobe = 0;                                   // 0 diffuse, 2 glossy, 3 mirror
    if (metallic > 0.5) lobe = (roughness < 0.06) ? 3 : 2;

    vec3 tt, bb;
    orthonormalBasis(normal, tt, bb);
    vec3 woLocal = toLocal(wo, tt, bb, normal);
    if (woLocal.z <= 0.0) {
        paths[p].radiance = vec4(radiance, 0.0);
        return;
    }

    vec3 shadingPoint = position + normal * kSurfaceBias;

    if ((flags & kFlagCaptured) == 0u) {
        paths[p].firstAlbedo = vec4(albedo, travelled);
        paths[p].firstNormal = vec4(normal, 0.0);
        flags |= kFlagCaptured;
    }

    // ---- next event estimation, pushed as shadow rays for a later stage
    if (lobe == 0 || lobe == 2) {
        if (uSunOn != 0) {
            vec3 wi = uniformCone(uSunDirection, uSunCosMax);
            vec3 wiLocal = toLocal(wi, tt, bb, normal);
            if (wiLocal.z > 0.0) {
                vec3 f = (lobe == 0) ? albedo * (kInvPi * wiLocal.z)
                                     : ggxEvalTimesCos(woLocal, wiLocal, albedo, alpha);
                if (max(f.r, max(f.g, f.b)) > 0.0) {
                    vec3 c = throughput * f * uSunRadiance;
                    if (uBounce > 0) c = clampFirefly(c, uClampIndirect);
                    uint slot = atomicAdd(counters[1], 1u);
                    shadowQ[slot].originDist = vec4(shadingPoint, uMaxDistance);
                    shadowQ[slot].dirPath = vec4(wi, float(p));
                    shadowQ[slot].contribution = vec4(c, 0.0);
                }
            }
        }

        if (uLightCount > 0 && uTotalPower > 0.0) {
            float target = nextFloat() * uTotalPower;
            int lo = 0, hi = uLightCount - 1;
            while (lo < hi) {
                int mid = (lo + hi) >> 1;
                if (lightCdf[mid] < target) lo = mid + 1; else hi = mid;
            }
            Light L = lights[lo];
            vec3 onLight = L.originPower.xyz + L.edgeU.xyz * nextFloat() + L.edgeV.xyz * nextFloat();
            vec3 offset = onLight - shadingPoint;
            float d2 = dot(offset, offset);
            if (d2 >= 1e-8) {
                float dist = sqrt(d2);
                vec3 dir = offset / dist;
                float cosLight = dot(L.normal.xyz, -dir);
                if (cosLight > 1e-4) {
                    float pdf = (L.originPower.w / uTotalPower) * d2 / cosLight;
                    vec3 wiLocal = toLocal(dir, tt, bb, normal);
                    if (pdf > 0.0 && wiLocal.z > 0.0) {
                        vec3 f = (lobe == 0) ? albedo * (kInvPi * wiLocal.z)
                                             : ggxEvalTimesCos(woLocal, wiLocal, albedo, alpha);
                        if (max(f.r, max(f.g, f.b)) > 0.0) {
                            vec3 c = throughput * f * L.radiance.rgb / pdf;
                            if (uBounce > 0) c = clampFirefly(c, uClampIndirect);
                            uint slot = atomicAdd(counters[2], 1u);
                            shadowQ[uint(uPixelCount) + slot].originDist =
                                vec4(shadingPoint, dist - 4.0 * kSurfaceBias);
                            shadowQ[uint(uPixelCount) + slot].dirPath = vec4(dir, float(p));
                            shadowQ[uint(uPixelCount) + slot].contribution = vec4(c, 0.0);
                        }
                    }
                }
            }
        }
    }

    // ---- sample the bsdf
    vec3 wiLocal;
    vec3 weight;
    if (lobe == 0) {
        float u1 = nextFloat(), u2 = nextFloat();
        float r = sqrt(u1), phi = kTwoPi * u2;
        wiLocal = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
        weight = albedo;
        flags &= ~kFlagCountEmission;
    } else if (lobe == 2) {
        vec3 h = sampleGgxVndf(woLocal, alpha, nextFloat(), nextFloat());
        wiLocal = h * (2.0 * dot(woLocal, h)) - woLocal;
        if (wiLocal.z <= 0.0) { paths[p].radiance = vec4(radiance, 0.0); return; }
        weight = fresnelSchlick(clamp(dot(wiLocal, h), 0.0, 1.0), albedo) * smithG1(wiLocal, alpha);
        flags &= ~kFlagCountEmission;
    } else {
        wiLocal = vec3(-woLocal.x, -woLocal.y, woLocal.z);
        weight = fresnelSchlick(woLocal.z, albedo);
        flags |= kFlagCountEmission;
    }

    throughput *= weight;
    if (max(throughput.r, max(throughput.g, throughput.b)) <= 0.0) {
        paths[p].radiance = vec4(radiance, 0.0);
        return;
    }

    if (uBounce >= uRouletteStart) {
        float survival = max(0.05, min(1.0, max(throughput.r, max(throughput.g, throughput.b))));
        if (nextFloat() > survival) {
            paths[p].radiance = vec4(radiance, 0.0);
            return;
        }
        throughput /= survival;
    }

    paths[p].throughput = vec4(throughput, travelled);
    paths[p].radiance = vec4(radiance, 0.0);
    paths[p].origin = vec4(shadingPoint, 0.0);
    paths[p].dir = vec4(toWorld(wiLocal, tt, bb, normal), 0.0);
    paths[p].state = uvec4(rngState, 0u, uint(medium), flags);
    activeOut[atomicAdd(counters[0], 1u)] = p;
}
)GLSL";

const char* kShadow = R"GLSL(
void main() {
    uint k = gl_GlobalInvocationID.x;
    if (k >= counters[uShadowCounter]) return;

    // The sun queue starts at zero and the area queue one frame further on,
    // so the two never share a slot and each path appears at most once in
    // either -- which is what lets the add below be a plain add.
    uint base = (uShadowCounter == 1) ? 0u : uint(uPixelCount);
    Shadow s = shadowQ[base + k];

    vec3 tr = transmittance(s.originDist.xyz, s.dirPath.xyz, s.originDist.w);
    if (max(tr.r, max(tr.g, tr.b)) <= 0.0) return;

    uint p = uint(s.dirPath.w);
    paths[p].radiance.rgb += s.contribution.rgb * tr;
}
)GLSL";

const char* kAdvance = R"GLSL(
void main() {
    if (gl_GlobalInvocationID.x != 0u) return;
    counters[3] = counters[0];
    counters[0] = 0u;
    counters[1] = 0u;
    counters[2] = 0u;
}
)GLSL";

// Turns the three counts into three work group counts. One thread, and the
// whole point of the exercise: without it every stage dispatches over the
// full frame even at bounce six, where one path in a hundred is still going.
const char* kArgs = R"GLSL(
void main() {
    if (gl_GlobalInvocationID.x != 0u) return;
    dispatchArgs[0] = uvec4((counters[3] + 63u) / 64u, 1u, 1u, 0u);
    dispatchArgs[1] = uvec4((counters[1] + 63u) / 64u, 1u, 1u, 0u);
    dispatchArgs[2] = uvec4((counters[2] + 63u) / 64u, 1u, 1u, 0u);
}
)GLSL";

const char* kResolve = R"GLSL(
void main() {
    uint p = gl_GlobalInvocationID.x;
    if (p >= uint(uPixelCount)) return;

    accum[p] += vec4(paths[p].radiance.rgb, 1.0);

    float d = paths[p].firstAlbedo.w;
    if (d >= 0.0) {
        aovAlbedo[p] += vec4(paths[p].firstAlbedo.rgb, 1.0);
        aovNormalDepth[p] += vec4(paths[p].firstNormal.rgb, d);
    }
}
)GLSL";

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

gl::GLuint compileStage(const std::vector<const char*>& pieces, const char* name,
                        std::string* error) {
    const gl::GLuint shader = gl::CreateShader(gl::COMPUTE_SHADER);
    gl::ShaderSource(shader, gl::GLsizei(pieces.size()), pieces.data(), nullptr);
    gl::CompileShader(shader);

    gl::GLint ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetShaderInfoLog(shader, length, nullptr, log.data());
        setError(error, std::string("wavefront ") + name + ": " + log);
        gl::DeleteShader(shader);
        return 0;
    }

    const gl::GLuint program = gl::CreateProgram();
    gl::AttachShader(program, shader);
    gl::LinkProgram(program);
    gl::DeleteShader(shader);

    gl::GetProgramiv(program, gl::LINK_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetProgramiv(program, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetProgramInfoLog(program, length, nullptr, log.data());
        setError(error, std::string("wavefront link ") + name + ": " + log);
        gl::DeleteProgram(program);
        return 0;
    }
    return program;
}

void makeBuffer(gl::GLuint& buffer, const void* data, size_t bytes, gl::GLenum usage) {
    if (buffer == 0) gl::GenBuffers(1, &buffer);
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, buffer);
    gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(bytes), data, usage);
}

} // namespace

WavefrontTracer::~WavefrontTracer() { destroy(); }

bool WavefrontTracer::build(std::string* error) {
    destroy();

    // Each stage is composed from the types, the maths, the buffers it
    // actually touches, and its own main. The buffer blocks are not shared
    // because the driver counts *declared* storage blocks against a limit of
    // sixteen per stage -- with all of them everywhere, nothing past the
    // entities would link.
    //
    // Worst case below is `extend` and `shadow` at fourteen.
    generate_ = compileStage({kDeclsA, kCommonRng, kBufPaths, kBufActiveIn, kGenerate},
                             "generate", error);
    if (!generate_) return false;

    extend_ = compileStage({kDeclsA, kCommonRng, kBufWorld, kBufMaterials, kBufPaths, kBufHits,
                            kBufActiveIn, kBufCounters, kBufGeometry, kCommonWalk, kCommonOffGrid, kExtend},
                           "extend", error);
    if (!extend_) return false;

    shade_ = compileStage({kDeclsA, kCommonRng, kBufMaterials, kBufLights, kBufPaths, kBufHits,
                           kBufActiveIn, kBufActiveOut, kBufCounters, kBufShadow, kShadeHelpers,
                           kShade},
                          "shade", error);
    if (!shade_) return false;

    shadow_ = compileStage({kDeclsA, kCommonRng, kBufWorld, kBufMaterials, kBufPaths,
                            kBufCounters, kBufShadow, kBufGeometry, kCommonWalk, kCommonOffGrid, kShadow},
                           "shadow", error);
    if (!shadow_) return false;

    resolve_ = compileStage({kDeclsA, kBufPaths, kBufFrame, kResolve}, "resolve", error);
    if (!resolve_) return false;

    // `advance` is the one-thread kernel that swaps the queue counters over,
    // so the bounce loop never has to read a count back to the host.
    advance_ = compileStage({kDeclsA, kBufCounters, kBufArgs, kAdvance}, "advance", error);
    if (!advance_) return false;

    args_ = compileStage({kDeclsA, kBufCounters, kBufArgs, kArgs}, "args", error);
    return args_ != 0;
}

bool WavefrontTracer::uploadStatic(const Scene& scene, std::string* error) {
    const PackedWorld packed = packWorld(scene.world);
    if (packed.chunkIndex.empty()) {
        setError(error, "wavefront: the world is empty");
        return false;
    }

    makeBuffer(chunkIndexBuf_, packed.chunkIndex.data(),
               packed.chunkIndex.size() * sizeof(int32_t), gl::STATIC_DRAW);
    makeBuffer(blocksBuf_, packed.blocks.data(), packed.blocks.size() * sizeof(uint32_t),
               gl::STATIC_DRAW);

    meta_ = PackedWorld{};
    meta_.gridMin = packed.gridMin;
    meta_.gridDim = packed.gridDim;
    meta_.worldMin = packed.worldMin;
    meta_.worldMax = packed.worldMax;

    const BlockRegistry& registry = scene.world.registry();
    std::vector<GpuMaterial> materials(registry.size());
    for (size_t i = 0; i < registry.size(); ++i) {
        const BlockDef& def = registry[BlockId(i)];
        GpuMaterial& m = materials[i];
        m.albedo[0] = def.albedo.x; m.albedo[1] = def.albedo.y; m.albedo[2] = def.albedo.z;
        m.roughness = def.roughness;
        m.emission[0] = def.emission.x; m.emission[1] = def.emission.y;
        m.emission[2] = def.emission.z;
        m.metallic = def.metallic;
        m.absorption[0] = def.absorption.x; m.absorption[1] = def.absorption.y;
        m.absorption[2] = def.absorption.z;
        m.ior = def.ior;
        m.transmission = def.transmission;
        m.opaque = def.opaque ? 1.0f : 0.0f;
        m.pad0 = m.pad1 = 0.0f;
        m.topTint[0] = def.topTint.x; m.topTint[1] = def.topTint.y; m.topTint[2] = def.topTint.z;
        m.tintTop = def.tintTop ? 1.0f : 0.0f;
    }
    makeBuffer(materialBuf_, materials.data(), materials.size() * sizeof(GpuMaterial),
               gl::STATIC_DRAW);

    LightSet set;
    set.build(scene.world);
    lightCount_ = set.lights().size();
    totalPower_ = set.totalPower();

    std::vector<GpuLight> lights(lightCount_);
    for (size_t i = 0; i < lightCount_; ++i) {
        const AreaLight& a = set.lights()[i];
        GpuLight& g = lights[i];
        g.origin[0] = a.origin.x; g.origin[1] = a.origin.y; g.origin[2] = a.origin.z;
        g.power = a.power;
        g.edgeU[0] = a.edgeU.x; g.edgeU[1] = a.edgeU.y; g.edgeU[2] = a.edgeU.z;
        g.edgeV[0] = a.edgeV.x; g.edgeV[1] = a.edgeV.y; g.edgeV[2] = a.edgeV.z;
        g.normal[0] = a.normal.x; g.normal[1] = a.normal.y; g.normal[2] = a.normal.z;
        g.radiance[0] = a.radiance.x; g.radiance[1] = a.radiance.y; g.radiance[2] = a.radiance.z;
        g.pad0 = g.pad1 = g.pad2 = g.pad3 = 0.0f;
    }
    if (lights.empty()) lights.resize(1, GpuLight{});
    makeBuffer(lightBuf_, lights.data(), lights.size() * sizeof(GpuLight), gl::STATIC_DRAW);

    std::vector<float> cdf = set.cdf();
    if (cdf.empty()) cdf.push_back(0.0f);
    makeBuffer(cdfBuf_, cdf.data(), cdf.size() * sizeof(float), gl::STATIC_DRAW);

    // Built from the scene's own library, or from nothing -- either way every
    // layer ends up holding what surfaceAlbedo() would return for that face.
    if (!textures_.build(scene.world.registry(), scene.blockTextures)) {
        setError(error, "wavefront: could not build the block texture array");
        return false;
    }

    return gl::GetError() == gl::NO_ERROR_;
}

bool WavefrontTracer::uploadDynamic(const Scene& scene, std::string* error) {
    // The same boxes EntitySet already flattened, rewritten for std430.
    if (!entities_.upload(scene.entities)) {
        setError(error, "wavefront: could not upload the entity boxes");
        return false;
    }

    // The same trees SpriteSet and PropSet already built.
    if (!offgrid_.upload(scene.sprites, scene.props)) {
        setError(error, "wavefront: could not upload the sprites and props");
        return false;
    }

    return gl::GetError() == gl::NO_ERROR_;
}

bool WavefrontTracer::resizeTargets(int width, int height) {
    const size_t pixels = size_t(width) * size_t(height);

    const std::vector<float> zeros4(pixels * 4, 0.0f);
    makeBuffer(accumBuf_, zeros4.data(), zeros4.size() * sizeof(float), gl::DYNAMIC_READ);
    makeBuffer(albedoBuf_, zeros4.data(), zeros4.size() * sizeof(float), gl::DYNAMIC_READ);
    makeBuffer(normalBuf_, zeros4.data(), zeros4.size() * sizeof(float), gl::DYNAMIC_READ);

    makeBuffer(pathBuf_, nullptr, pixels * 7 * 4 * sizeof(float), gl::DYNAMIC_DRAW);
    // Four vec4 a hit: position and t, albedo and id, normal and
    // metallic, emission and roughness. The material is resolved in
    // `extend`, so all of it has to fit here.
    makeBuffer(hitBuf_, nullptr, pixels * 4 * 4 * sizeof(float), gl::DYNAMIC_DRAW);
    makeBuffer(activeInBuf_, nullptr, pixels * sizeof(uint32_t), gl::DYNAMIC_DRAW);
    makeBuffer(activeOutBuf_, nullptr, pixels * sizeof(uint32_t), gl::DYNAMIC_DRAW);

    // Two queues in one buffer: the sun's at zero, the area lights' one
    // frame further along.
    makeBuffer(sunQueueBuf_, nullptr, pixels * 2 * 3 * 4 * sizeof(float), gl::DYNAMIC_DRAW);

    const uint32_t counters[4] = {0, 0, 0, 0};
    makeBuffer(counterBuf_, counters, sizeof(counters), gl::DYNAMIC_DRAW);

    // Three uvec4 slots. The indirect read takes the first three components
    // of a slot and ignores the fourth, so the padding costs nothing and the
    // offsets stay a clean sixteen bytes apart.
    const uint32_t args[12] = {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0};
    makeBuffer(argsBuf_, args, sizeof(args), gl::DYNAMIC_DRAW);

    bytes_ = pixels * (7 * 16 + 4 * 16 + 4 + 4 + 2 * 3 * 16 + 3 * 16);
    width_ = width;
    height_ = height;
    return true;
}

void WavefrontTracer::bindCommon() const {
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 0, chunkIndexBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 1, blocksBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 2, materialBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 3, lightBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 4, cdfBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 5, accumBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 6, albedoBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 7, normalBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 8, pathBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 9, hitBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 12, counterBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 13, sunQueueBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 14, argsBuf_);
}

namespace {

// Every stage is its own program, so every stage needs its own copy of the
// uniforms. Cheap, and much cheaper than the shared block it replaces would
// be to get wrong.
void setSceneUniforms(gl::GLuint p, const Scene& scene, const PathSettings& settings,
                      const PackedWorld& meta, const Camera& camera, Vec3 right, Vec3 up,
                      Vec3 forward, float halfWidth, float halfHeight, int pixelCount,
                      size_t lightCount, float totalPower) {
    auto loc = [&](const char* n) { return gl::GetUniformLocation(p, n); };
    auto vec3At = [&](const char* n, Vec3 v) {
        const float f[3] = {v.x, v.y, v.z};
        gl::Uniform3fv(loc(n), 1, f);
    };

    gl::UseProgram(p);
    gl::Uniform3i(loc("uGridMin"), meta.gridMin.x, meta.gridMin.y, meta.gridMin.z);
    gl::Uniform3i(loc("uGridDim"), meta.gridDim.x, meta.gridDim.y, meta.gridDim.z);
    gl::Uniform3i(loc("uWorldMin"), meta.worldMin.x, meta.worldMin.y, meta.worldMin.z);
    gl::Uniform3i(loc("uWorldMax"), meta.worldMax.x, meta.worldMax.y, meta.worldMax.z);

    gl::Uniform2i(loc("uSize"), settings.width, settings.height);
    gl::Uniform1i(loc("uPixelCount"), pixelCount);
    gl::Uniform1i(loc("uMaxBounces"), settings.maxBounces);
    gl::Uniform1i(loc("uRouletteStart"), settings.rouletteStartBounce);
    gl::Uniform1f(loc("uMaxDistance"), settings.maxDistance);
    gl::Uniform1f(loc("uClampIndirect"), settings.clampIndirect);
    gl::Uniform1ui(loc("uSeed"), gl::GLuint(settings.seed));

    vec3At("uCamPos", camera.position);
    vec3At("uCamRight", right);
    vec3At("uCamUp", up);
    vec3At("uCamForward", forward);
    gl::Uniform1f(loc("uHalfWidth"), halfWidth);
    gl::Uniform1f(loc("uHalfHeight"), halfHeight);
    gl::Uniform1f(loc("uAperture"), camera.aperture);
    gl::Uniform1f(loc("uFocusDistance"), camera.focusDistance);
    gl::Uniform1i(loc("uOrthographic"), camera.projection == Camera::Projection::Orthographic);
    gl::Uniform1f(loc("uOrthoHalfW"), camera.orthoHeight * 0.5f * camera.aspect);
    gl::Uniform1f(loc("uOrthoHalfH"), camera.orthoHeight * 0.5f);

    vec3At("uSunDirection", scene.sun.direction);
    vec3At("uSunRadiance", scene.sun.radiance());
    gl::Uniform1f(loc("uSunCosMax"),
                  std::cos(radians(std::max(scene.sun.angularRadiusDegrees, 0.01f))));
    gl::Uniform1i(loc("uSunOn"), scene.sun.intensity > 0.0f ? 1 : 0);

    vec3At("uSkyZenith", scene.sky.zenith);
    vec3At("uSkyHorizon", scene.sky.horizon);
    vec3At("uSkyGround", scene.sky.ground);
    gl::Uniform1f(loc("uSkyIntensity"), scene.sky.intensity);
    gl::Uniform1i(loc("uOverrideBackground"), scene.overrideBackground ? 1 : 0);
    vec3At("uBackground", scene.background);

    gl::Uniform1i(loc("uLightCount"), int(lightCount));
    gl::Uniform1f(loc("uTotalPower"), totalPower);
}

} // namespace

bool WavefrontTracer::render(const Scene& scene, const PathSettings& settings, Image& out,
                             RenderTargets* aovs, WavefrontStats* stats, std::string* error) {
    if (generate_ == 0) { setError(error, "wavefront: not built"); return false; }
    if (settings.width <= 0 || settings.height <= 0) return false;

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();

    const int pixelCount = settings.width * settings.height;
    const size_t pixels = size_t(pixelCount);
    resizeTargets(settings.width, settings.height);

    Camera camera = scene.camera;
    camera.aspect = float(settings.width) / float(settings.height);
    const Vec3 forward = normalize(camera.target - camera.position);
    const Vec3 right = normalize(cross(forward, camera.up));
    const Vec3 trueUp = cross(right, forward);
    const float halfHeight = std::tan(camera.fovY * 0.5f);
    const float halfWidth = halfHeight * camera.aspect;

    const gl::GLuint programs[] = {generate_, extend_, shade_,  shadow_,
                                   advance_,  args_,   resolve_};
    for (gl::GLuint p : programs) {
        setSceneUniforms(p, scene, settings, meta_, camera, right, trueUp, forward, halfWidth,
                         halfHeight, pixelCount, lightCount_, totalPower_);
    }

    bindCommon();
    textures_.bind(0);
    entities_.bindSkins(1);
    entities_.bindBoxes(15);
    offgrid_.bindSpriteTextures(2);
    offgrid_.bind();

    // Samplers and the box count go on every stage: the shared prelude is
    // compiled into all of them, and a stage that never calls the function
    // simply never reads the uniform.
    for (gl::GLuint p : programs) {
        gl::UseProgram(p);
        gl::Uniform1i(gl::GetUniformLocation(p, "uBlockTex"), 0);
        gl::Uniform1i(gl::GetUniformLocation(p, "uSkinTex"), 1);
        gl::Uniform1i(gl::GetUniformLocation(p, "uBoxCount"), entities_.boxCount());
        gl::Uniform1i(gl::GetUniformLocation(p, "uSpriteTex"), 2);
        gl::Uniform1i(gl::GetUniformLocation(p, "uSpriteNodes"), offgrid_.spriteNodeCount());
        gl::Uniform1i(gl::GetUniformLocation(p, "uPropNodes"), offgrid_.propNodeCount());
    }

    gl::BindBuffer(gl::DISPATCH_INDIRECT_BUFFER, argsBuf_);

    // Only `generate` and `resolve` still run over the whole frame; they have
    // to, because every pixel needs a ray and every pixel needs folding in.
    const gl::GLuint groups = gl::GLuint((pixelCount + 63) / 64);
    const gl::GLbitfield barrier = gl::SHADER_STORAGE_BARRIER_BIT;

    gl::Finish();
    const auto uploaded = Clock::now();

    int dispatches = 0;
    std::vector<uint64_t> alive(size_t(settings.maxBounces) + 1, 0);

    gl::GLuint activeIn = activeInBuf_, activeOut = activeOutBuf_;

    for (int wave = 0; wave < settings.samplesPerPixel; ++wave) {
        // Every wave is one sample for every pixel, so the path index is the
        // pixel index and nothing has to be atomic at the end.
        const uint32_t start[4] = {0, 0, 0, uint32_t(pixelCount)};
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, counterBuf_);
        gl::BufferSubData(gl::SHADER_STORAGE_BUFFER, 0, sizeof(start), start);

        activeIn = activeInBuf_;
        activeOut = activeOutBuf_;
        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 10, activeIn);
        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 11, activeOut);

        gl::UseProgram(generate_);
        gl::Uniform1i(gl::GetUniformLocation(generate_, "uWave"), wave);
        gl::DispatchCompute(groups, 1, 1);
        gl::MemoryBarrier(barrier);
        ++dispatches;

        for (int bounce = 0; bounce <= settings.maxBounces; ++bounce) {
            if (countAlive) {
                uint32_t c[4];
                gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, counterBuf_);
                gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0, sizeof(c), c);
                alive[size_t(bounce)] += c[3];
                if (c[3] == 0) break;
            }

            // Size the next two stages from the count the GPU itself holds.
            // COMMAND_BARRIER_BIT is the one that matters: without it the
            // indirect read is allowed to see the previous bounce's numbers.
            gl::UseProgram(args_);
            gl::DispatchCompute(1, 1, 1);
            gl::MemoryBarrier(barrier | gl::COMMAND_BARRIER_BIT);

            gl::UseProgram(extend_);
            gl::Uniform1i(gl::GetUniformLocation(extend_, "uBounce"), bounce);
            gl::DispatchComputeIndirect(0);
            gl::MemoryBarrier(barrier);

            gl::UseProgram(shade_);
            gl::Uniform1i(gl::GetUniformLocation(shade_, "uBounce"), bounce);
            gl::DispatchComputeIndirect(0);
            gl::MemoryBarrier(barrier);

            // Shade has just filled the two shadow counts, so the args have
            // to be recomputed before the queues can be dispatched over.
            gl::UseProgram(args_);
            gl::DispatchCompute(1, 1, 1);
            gl::MemoryBarrier(barrier | gl::COMMAND_BARRIER_BIT);

            gl::UseProgram(shadow_);
            gl::Uniform1i(gl::GetUniformLocation(shadow_, "uBounce"), bounce);
            gl::Uniform1i(gl::GetUniformLocation(shadow_, "uShadowCounter"), 1);
            gl::DispatchComputeIndirect(16);
            gl::MemoryBarrier(barrier);

            gl::Uniform1i(gl::GetUniformLocation(shadow_, "uShadowCounter"), 2);
            gl::DispatchComputeIndirect(32);
            gl::MemoryBarrier(barrier);

            gl::UseProgram(advance_);
            gl::DispatchCompute(1, 1, 1);
            gl::MemoryBarrier(barrier);

            dispatches += 7;

            std::swap(activeIn, activeOut);
            gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 10, activeIn);
            gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 11, activeOut);
        }

        gl::UseProgram(resolve_);
        gl::DispatchCompute(groups, 1, 1);
        gl::MemoryBarrier(barrier);
        ++dispatches;

        if (settings.progress && (wave % 8 == 0 || wave + 1 == settings.samplesPerPixel)) {
            std::printf("\r  wavefront %3d%%", 100 * (wave + 1) / settings.samplesPerPixel);
            std::fflush(stdout);
        }
    }
    gl::Finish();
    if (settings.progress) std::printf("\r                \r");

    const auto traced = Clock::now();

    std::vector<float> accum(pixels * 4);
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, accumBuf_);
    gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                         gl::GLsizeiptr(accum.size() * sizeof(float)), accum.data());

    out = Image(settings.width, settings.height);
    for (size_t i = 0; i < pixels; ++i) {
        const float n = std::max(1.0f, accum[i * 4 + 3]);
        out.data()[i] = Vec3{accum[i * 4] / n, accum[i * 4 + 1] / n, accum[i * 4 + 2] / n};
    }

    if (aovs) {
        std::vector<float> albedo(pixels * 4), normalDepth(pixels * 4);
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, albedoBuf_);
        gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                             gl::GLsizeiptr(albedo.size() * sizeof(float)), albedo.data());
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, normalBuf_);
        gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                             gl::GLsizeiptr(normalDepth.size() * sizeof(float)),
                             normalDepth.data());

        aovs->width = settings.width;
        aovs->height = settings.height;
        aovs->albedo = Image(settings.width, settings.height, Vec3{1.0f});
        aovs->normal = Image(settings.width, settings.height);
        aovs->depth.assign(pixels, -1.0f);

        for (size_t i = 0; i < pixels; ++i) {
            const float n = albedo[i * 4 + 3];
            if (n <= 0.0f) continue;
            aovs->albedo.data()[i] =
                Vec3{albedo[i * 4] / n, albedo[i * 4 + 1] / n, albedo[i * 4 + 2] / n};
            const Vec3 sum{normalDepth[i * 4], normalDepth[i * 4 + 1], normalDepth[i * 4 + 2]};
            aovs->normal.data()[i] = lengthSq(sum) > 1e-12f ? normalize(sum) : Vec3{0.0f};
            aovs->depth[i] = normalDepth[i * 4 + 3] / n;
        }
        aovs->color = out;
    }

    const auto finished = Clock::now();
    if (stats) {
        stats->uploadSeconds = std::chrono::duration<double>(uploaded - started).count();
        stats->traceSeconds = std::chrono::duration<double>(traced - uploaded).count();
        stats->readbackSeconds = std::chrono::duration<double>(finished - traced).count();
        stats->waves = settings.samplesPerPixel;
        stats->dispatches = dispatches;
        stats->lights = lightCount_;
        stats->bytesOnGpu = bytes_;
        stats->aliveAtBounce = alive;
    }

    const gl::GLenum err = gl::GetError();
    if (err != gl::NO_ERROR_) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "wavefront: GL error 0x%04X", unsigned(err));
        setError(error, buf);
        return false;
    }
    return true;
}

void WavefrontTracer::destroy() {
    const gl::GLuint programs[] = {generate_, extend_, shade_, shadow_, resolve_, advance_,
                                   args_};
    for (gl::GLuint p : programs) {
        if (p) gl::DeleteProgram(p);
    }
    generate_ = extend_ = shade_ = shadow_ = resolve_ = advance_ = args_ = 0;

    const gl::GLuint buffers[] = {chunkIndexBuf_, blocksBuf_,    materialBuf_,  lightBuf_,
                                  cdfBuf_,        accumBuf_,     albedoBuf_,    normalBuf_,
                                  pathBuf_,       hitBuf_,       activeInBuf_,  activeOutBuf_,
                                  counterBuf_,    sunQueueBuf_,  areaQueueBuf_,
                                  argsBuf_};
    for (gl::GLuint b : buffers) {
        if (b) gl::DeleteBuffers(1, &b);
    }
    chunkIndexBuf_ = blocksBuf_ = materialBuf_ = lightBuf_ = cdfBuf_ = 0;
    accumBuf_ = albedoBuf_ = normalBuf_ = pathBuf_ = hitBuf_ = 0;
    activeInBuf_ = activeOutBuf_ = counterBuf_ = sunQueueBuf_ = areaQueueBuf_ = 0;
    argsBuf_ = 0;
    width_ = height_ = 0;
}

} // namespace gpu
} // namespace blocky
