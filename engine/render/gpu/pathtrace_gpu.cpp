#include "engine/render/gpu/pathtrace_gpu.hpp"

#include "engine/render/trace/lights.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace blocky {
namespace gpu {
namespace {

// Transliterated from render/trace/pathtrace.cpp, bsdf.hpp and lights.cpp.
// Where this differs from those files it is either forced by GLSL or it is a
// bug -- which is a useful thing to be able to say while reading it.
const char* kSource1 = R"GLSL(
#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;

struct Material {
    vec4 albedoRough;      // rgb albedo, a roughness
    vec4 emissionMetal;    // rgb emission, a metallic
    vec4 absorbIor;        // rgb absorption per block, a ior
    vec4 transOpaque;      // x transmission, y opaque, zw unused
    vec4 topTintFlag;      // rgb top-face tint, a non-zero when it applies
};

struct Light {
    vec4 originPower;
    vec4 edgeU;
    vec4 edgeV;
    vec4 normal;
    vec4 radiance;
};

layout(std430, binding = 0) readonly buffer ChunkIndexBuf { int   chunkIndex[]; };
layout(std430, binding = 1) readonly buffer BlocksBuf     { uint  blockWords[]; };
layout(std430, binding = 2) readonly buffer MaterialBuf   { Material materials[]; };
layout(std430, binding = 3) readonly buffer LightBuf      { Light lights[]; };
layout(std430, binding = 4) readonly buffer CdfBuf        { float lightCdf[]; };
layout(std430, binding = 5)          buffer AccumBuf      { vec4  accum[]; };
layout(std430, binding = 6)          buffer AlbedoBuf     { vec4  aovAlbedo[]; };
layout(std430, binding = 7)          buffer NormalBuf     { vec4  aovNormalDepth[]; };

uniform ivec3 uGridMin;
uniform ivec3 uGridDim;
uniform ivec3 uWorldMin;
uniform ivec3 uWorldMax;

uniform ivec2 uSize;
uniform int   uSampleBase;      // first sample index of this batch
uniform int   uSampleCount;     // how many to do now
uniform int   uMaxBounces;
uniform int   uRouletteStart;
uniform float uMaxDistance;
uniform float uClampIndirect;
uniform uint  uSeed;

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
const float kCoatF0 = 0.04;
const int kMaxChunkSteps = 8192;
const int kMaxBlockSteps = 65536;

// ------------------------------------------------------------------- rng
// PCG-RXS-M-XS, 32 bit. The engine's Rng is the 64-bit PCG32; the streams
// therefore differ, which is why the CPU comparison is statistical and not
// per-pixel. Everything that matters -- the distributions being sampled --
// is the same.
uint rngState;

uint nextUint() {
    rngState = rngState * 747796405u + 2891336453u;
    uint word = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return (word >> 22u) ^ word;
}

float nextFloat() { return float(nextUint() >> 8) * (1.0 / 16777216.0); }

void seedRng(uint pixel, uint sample_) {
    rngState = pixel * 0x9E3779B9u + sample_ * 0x85EBCA6Bu + uSeed * 0xC2B2AE35u;
    nextUint();
    nextUint();
}

// ------------------------------------------------------------------ basis
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

)GLSL";

const char* kSource2 = R"GLSL(
// ------------------------------------------------------------------- bsdf
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

float fresnelDielectric(float cosThetaI, float eta) {
    cosThetaI = clamp(cosThetaI, 0.0, 1.0);
    float sin2I = 1.0 - cosThetaI * cosThetaI;
    float sin2T = eta * eta * sin2I;
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

const char* kSource3 = R"GLSL(
// ------------------------------------------------------------- traversal
struct Dda {
    ivec3 cell; ivec3 stp; vec3 tMax; vec3 tDelta; float t; int axis;
};

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

struct WorldHit {
    bool  found;
    float t;
    vec3  position;
    vec3  normal;
    int   id;
};

bool traceWorld(vec3 ro, vec3 rd, float maxDistance, int passThrough, out WorldHit h) {
    h.found = false; h.t = 0.0; h.position = vec3(0.0); h.normal = vec3(0.0); h.id = 0;

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
                    h.found = true;
                    h.t = d.t;
                    h.position = ro + rd * d.t;
                    h.normal = vec3(0.0);
                    h.normal[axis] = float(-stepSign);
                    h.id = id;
                    return true;
                }
                if (ddaExit(d.tMax) > segEnd) break;
                ddaAdvance(d);
            }
        } else if (mediumMode) {
            int axis = chunks.axis < 0 ? 1 : chunks.axis;
            int stepSign = rd[axis] > 0.0 ? 1 : -1;
            h.found = true; h.t = chunks.t; h.position = ro + rd * chunks.t;
            h.normal = vec3(0.0); h.normal[axis] = float(-stepSign); h.id = 0;
            return true;
        }
        if (ddaExit(chunks.tMax) > t1) break;
        ddaAdvance(chunks);
    }

    if (mediumMode && !clipped) {
        int axis = exitAxis < 0 ? 1 : exitAxis;
        int stepSign = rd[axis] > 0.0 ? 1 : -1;
        h.found = true; h.t = t1; h.position = ro + rd * t1;
        h.normal = vec3(0.0); h.normal[axis] = float(-stepSign); h.id = 0;
        return true;
    }
    return false;
}

// Beer-Lambert along a shadow ray, zero as soon as anything opaque is in it.
vec3 transmittance(vec3 ro, vec3 rd, float maxDistance) {
    vec3 result = vec3(1.0);
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

// ------------------------------------------------------------------- sky
vec3 skySample(vec3 dir) {
    float h = dir.y;
    if (h >= 0.0) {
        float t = pow(clamp(h, 0.0, 1.0), 0.45);
        return mix(uSkyHorizon, uSkyZenith, t) * uSkyIntensity;
    }
    float t = clamp(-h * 3.0, 0.0, 1.0);
    return mix(uSkyHorizon, uSkyGround, t) * uSkyIntensity;
}

// ---------------------------------------------------------------- shading
const int kLobeDiffuse = 0;
const int kLobeCoated = 1;
const int kLobeGlossy = 2;
const int kLobeMirror = 3;
const int kLobeDielectric = 4;

int lobeFor(Material m, out float alpha) {
    alpha = roughnessToAlpha(m.albedoRough.a);
    if (m.transOpaque.x > 0.0) return kLobeDielectric;
    if (m.emissionMetal.a > 0.5) return (m.albedoRough.a < 0.06) ? kLobeMirror : kLobeGlossy;
    return kLobeDiffuse;   // no MaterialStyle on this path, so never Coated
}

vec3 evalTimesCos(int lobe, vec3 albedo, float alpha, vec3 woLocal, vec3 wiLocal) {
    if (lobe == kLobeDiffuse) return albedo * (kInvPi * wiLocal.z);
    if (lobe == kLobeGlossy) return ggxEvalTimesCos(woLocal, wiLocal, albedo, alpha);
    return vec3(0.0);
}

vec3 clampFirefly(vec3 c, float limit) {
    if (limit <= 0.0) return c;
    float m = max(c.r, max(c.g, c.b));
    return m > limit ? c * (limit / m) : c;
}

// ----------------------------------------------------------- light sample
bool sampleAreaLight(vec3 point, out vec3 dir, out float dist, out vec3 radiance, out float pdf) {
    dir = vec3(0.0); dist = 0.0; radiance = vec3(0.0); pdf = 0.0;
    if (uLightCount <= 0 || uTotalPower <= 0.0) return false;

    float target = nextFloat() * uTotalPower;
    int lo = 0, hi = uLightCount - 1;
    while (lo < hi) {                      // lower_bound over the power cdf
        int mid = (lo + hi) >> 1;
        if (lightCdf[mid] < target) lo = mid + 1; else hi = mid;
    }
    Light L = lights[lo];

    vec3 onLight = L.originPower.xyz + L.edgeU.xyz * nextFloat() + L.edgeV.xyz * nextFloat();
    vec3 offset = onLight - point;
    float d2 = dot(offset, offset);
    if (d2 < 1e-8) return false;

    dist = sqrt(d2);
    dir = offset / dist;

    float cosLight = dot(L.normal.xyz, -dir);
    if (cosLight <= 1e-4) return false;

    pdf = (L.originPower.w / uTotalPower) * d2 / cosLight;
    radiance = L.radiance.rgb;
    return pdf > 0.0;
}

)GLSL";

const char* kSource4 = R"GLSL(
// ------------------------------------------------------------------ path
vec3 tracePath(vec3 ro, vec3 rd, out vec3 firstAlbedo, out vec3 firstNormal,
               out float firstDistance) {
    firstAlbedo = vec3(1.0);
    firstNormal = vec3(0.0);
    firstDistance = -1.0;
    bool captured = false;

    vec3 throughput = vec3(1.0);
    vec3 result = vec3(0.0);
    bool countEmission = true;
    int medium = 0;
    float travelled = 0.0;

    for (int bounce = 0; ; ++bounce) {
        WorldHit hit;
        if (!traceWorld(ro, rd, uMaxDistance, medium, hit)) {
            vec3 background = (bounce == 0 && uOverrideBackground != 0) ? uBackground
                                                                       : skySample(rd);
            result += throughput * background;
            break;
        }

        travelled += hit.t;

        if (medium != 0) throughput *= exp(-materials[medium].absorbIor.rgb * hit.t);

        Material m = materials[hit.id];
        vec3 emission = m.emissionMetal.rgb;
        // surfaceAlbedo(): the top face of a tinted block is a different
        // colour from its sides.
        vec3 blockAlbedo = (m.topTintFlag.a > 0.5 && hit.normal.y > 0.0)
                               ? m.topTintFlag.rgb : m.albedoRough.rgb;
        if (countEmission && max(emission.r, max(emission.g, emission.b)) > 0.0) {
            vec3 contribution = throughput * emission;
            result += (bounce == 0) ? contribution : clampFirefly(contribution, uClampIndirect);
        }

        if (bounce >= uMaxBounces) break;

        vec3 normal = hit.normal;
        vec3 wo = -rd;

        bool leaving  = (medium != 0) && (hit.id == 0);
        bool entering = (medium == 0) && (m.transOpaque.x > 0.0);

        if (leaving || entering) {
            float mediumIor = leaving ? materials[medium].absorbIor.a : m.absorbIor.a;
            float eta = leaving ? mediumIor : 1.0 / mediumIor;
            float reflectance = fresnelDielectric(clamp(dot(normal, wo), 0.0, 1.0), eta);

            vec3 next;
            if (nextFloat() < reflectance) {
                next = reflect(rd, normal);
            } else if (refractRay(rd, normal, eta, next)) {
                medium = leaving ? 0 : hit.id;
            } else {
                next = reflect(rd, normal);
            }
            ro = hit.position + next * kSurfaceBias;
            rd = next;
            countEmission = true;
            continue;
        }

        vec3 albedo = blockAlbedo;
        float alpha;
        int lobe = lobeFor(m, alpha);

        vec3 t, b;
        orthonormalBasis(normal, t, b);
        vec3 woLocal = toLocal(wo, t, b, normal);
        if (woLocal.z <= 0.0) break;

        vec3 shadingPoint = hit.position + normal * kSurfaceBias;

        if (!captured) {
            firstAlbedo = albedo;
            firstNormal = normal;
            firstDistance = travelled;
            captured = true;
        }

        // ---- next-event estimation
        if (lobe == kLobeDiffuse || lobe == kLobeGlossy) {
            vec3 nee = vec3(0.0);

            if (uSunOn != 0) {
                vec3 wi = uniformCone(uSunDirection, uSunCosMax);
                vec3 wiLocal = toLocal(wi, t, b, normal);
                if (wiLocal.z > 0.0) {
                    vec3 f = evalTimesCos(lobe, albedo, alpha, woLocal, wiLocal);
                    if (max(f.r, max(f.g, f.b)) > 0.0) {
                        vec3 tr = transmittance(shadingPoint, wi, uMaxDistance);
                        nee += f * uSunRadiance * tr;
                    }
                }
            }

            vec3 ldir, lrad; float ldist, lpdf;
            if (sampleAreaLight(shadingPoint, ldir, ldist, lrad, lpdf)) {
                vec3 wiLocal = toLocal(ldir, t, b, normal);
                if (wiLocal.z > 0.0 && lpdf > 0.0) {
                    vec3 f = evalTimesCos(lobe, albedo, alpha, woLocal, wiLocal);
                    if (max(f.r, max(f.g, f.b)) > 0.0) {
                        vec3 tr = transmittance(shadingPoint, ldir, ldist - 4.0 * kSurfaceBias);
                        nee += f * lrad * tr / lpdf;
                    }
                }
            }

            vec3 contribution = throughput * nee;
            result += (bounce == 0) ? contribution : clampFirefly(contribution, uClampIndirect);
        }

        // ---- sample the bsdf
        vec3 wiLocal;
        vec3 weight;

        if (lobe == kLobeDiffuse) {
            float u1 = nextFloat(), u2 = nextFloat();
            float r = sqrt(u1), phi = kTwoPi * u2;
            wiLocal = vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
            weight = albedo;
            countEmission = false;
        } else if (lobe == kLobeGlossy) {
            vec3 h = sampleGgxVndf(woLocal, alpha, nextFloat(), nextFloat());
            wiLocal = h * (2.0 * dot(woLocal, h)) - woLocal;
            if (wiLocal.z <= 0.0) break;
            weight = fresnelSchlick(clamp(dot(wiLocal, h), 0.0, 1.0), albedo) *
                     smithG1(wiLocal, alpha);
            countEmission = false;
        } else if (lobe == kLobeMirror) {
            wiLocal = vec3(-woLocal.x, -woLocal.y, woLocal.z);
            weight = fresnelSchlick(woLocal.z, albedo);
            countEmission = true;
        } else {
            break;
        }

        throughput *= weight;
        if (max(throughput.r, max(throughput.g, throughput.b)) <= 0.0) break;

        if (bounce >= uRouletteStart) {
            float survival = max(0.05, min(1.0, max(throughput.r,
                                                    max(throughput.g, throughput.b))));
            if (nextFloat() > survival) break;
            throughput /= survival;
        }

        ro = shadingPoint;
        rd = toWorld(wiLocal, t, b, normal);
    }

    return result;
}

void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= uSize.x || xy.y >= uSize.y) return;

    uint pixel = uint(xy.y) * uint(uSize.x) + uint(xy.x);

    vec3 sum = vec3(0.0);
    vec3 albedoSum = vec3(0.0);
    vec3 normalSum = vec3(0.0);
    float depthSum = 0.0;
    int captured = 0;

    for (int s = 0; s < uSampleCount; ++s) {
        seedRng(pixel, uint(uSampleBase + s));

        float jx = nextFloat();
        float jy = nextFloat();
        float u = (float(xy.x) + jx) / float(uSize.x);
        float v = (float(xy.y) + jy) / float(uSize.y);

        float sx = 2.0 * u - 1.0;
        float sy = 1.0 - 2.0 * v;

        vec3 ro, rd;
        if (uOrthographic != 0) {
            ro = uCamPos + uCamRight * (sx * uOrthoHalfW) + uCamUp * (sy * uOrthoHalfH);
            rd = uCamForward;
        } else {
            ro = uCamPos;
            rd = normalize(uCamForward + uCamRight * (sx * uHalfWidth) +
                           uCamUp * (sy * uHalfHeight));
            if (uAperture > 0.0) {
                float focalScale = uFocusDistance / max(dot(rd, uCamForward), 1e-4);
                vec3 focal = ro + rd * focalScale;
                // A point in the unit disc, by rejection-free polar mapping.
                float a = nextFloat() * kTwoPi;
                float rr = sqrt(nextFloat());
                vec3 offset = uCamRight * (cos(a) * rr * uAperture) +
                              uCamUp * (sin(a) * rr * uAperture);
                ro = uCamPos + offset;
                rd = normalize(focal - ro);
            }
        }

        vec3 fa, fn; float fd;
        sum += tracePath(ro, rd, fa, fn, fd);
        if (fd >= 0.0) {
            albedoSum += fa;
            normalSum += fn;
            depthSum += fd;
            ++captured;
        }
    }

    accum[pixel] += vec4(sum, float(uSampleCount));
    // w carries the number of samples that found a surface, which is what
    // the albedo, the normal and the depth all have to be divided by.
    aovAlbedo[pixel] += vec4(albedoSum, float(captured));
    aovNormalDepth[pixel] += vec4(normalSum, depthSum);
}
)GLSL";

// One shader in four pieces, because MSVC will not take a string literal
// longer than 16380 bytes. glShaderSource has always taken an array, so the
// only cost is remembering that the pieces must stay in order.
const char* kSources[] = {kSource1, kSource2, kSource3, kSource4};

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

gl::GLuint compile(std::string* error) {
    const gl::GLuint shader = gl::CreateShader(gl::COMPUTE_SHADER);
    gl::ShaderSource(shader, 4, kSources, nullptr);
    gl::CompileShader(shader);

    gl::GLint ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetShaderInfoLog(shader, length, nullptr, log.data());
        setError(error, "pathtrace compute shader: " + log);
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
        setError(error, "pathtrace link: " + log);
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

void setVec3(gl::GLuint p, const char* name, Vec3 v) {
    const float value[3] = {v.x, v.y, v.z};
    gl::Uniform3fv(gl::GetUniformLocation(p, name), 1, value);
}

void setIVec3(gl::GLuint p, const char* name, IVec3 v) {
    gl::Uniform3i(gl::GetUniformLocation(p, name), v.x, v.y, v.z);
}

void setInt(gl::GLuint p, const char* name, int v) {
    gl::Uniform1i(gl::GetUniformLocation(p, name), v);
}

void setFloat(gl::GLuint p, const char* name, float v) {
    gl::Uniform1f(gl::GetUniformLocation(p, name), v);
}

} // namespace

PathTracer::~PathTracer() { destroy(); }

bool PathTracer::build(std::string* error) {
    destroy();
    program_ = compile(error);
    return program_ != 0;
}

bool PathTracer::upload(const Scene& scene, std::string* error) {
    const PackedWorld packed = packWorld(scene.world);
    if (packed.chunkIndex.empty()) {
        setError(error, "gpu path tracer: the world is empty");
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
    meta_.chunkCount = packed.chunkCount;

    // Materials, straight out of the registry -- no block textures on this
    // path yet, so what the shader sees is the flat palette the CPU falls
    // back to when a scene has no assets. Both sides must be told the same.
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
        m.topTint[0] = def.topTint.x; m.topTint[1] = def.topTint.y;
        m.topTint[2] = def.topTint.z;
        m.tintTop = def.tintTop ? 1.0f : 0.0f;
    }
    makeBuffer(materialBuf_, materials.data(), materials.size() * sizeof(GpuMaterial),
               gl::STATIC_DRAW);

    // The same light list the CPU builds, handed over rather than gathered
    // again: two gatherings are two chances to disagree.
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
    // An empty buffer is not a legal binding, so a scene with no emissive
    // faces still gets one entry that the shader never selects.
    if (lights.empty()) lights.resize(1, GpuLight{});
    makeBuffer(lightBuf_, lights.data(), lights.size() * sizeof(GpuLight), gl::STATIC_DRAW);

    std::vector<float> cdf = set.cdf();
    if (cdf.empty()) cdf.push_back(0.0f);
    makeBuffer(cdfBuf_, cdf.data(), cdf.size() * sizeof(float), gl::STATIC_DRAW);

    return gl::GetError() == gl::NO_ERROR_;
}

bool PathTracer::resizeTargets(int width, int height) {
    if (width == width_ && height == height_) return true;
    const size_t pixels = size_t(width) * size_t(height);
    const std::vector<float> zeros(pixels * 4, 0.0f);
    makeBuffer(accumBuf_, zeros.data(), zeros.size() * sizeof(float), gl::DYNAMIC_READ);
    makeBuffer(albedoBuf_, zeros.data(), zeros.size() * sizeof(float), gl::DYNAMIC_READ);
    makeBuffer(normalBuf_, zeros.data(), zeros.size() * sizeof(float), gl::DYNAMIC_READ);
    width_ = width;
    height_ = height;
    return true;
}

bool PathTracer::render(const Scene& scene, const PathSettings& settings, Image& out,
                        RenderTargets* aovs, TraceStats* stats, std::string* error) {
    if (program_ == 0) { setError(error, "gpu path tracer: not built"); return false; }
    if (settings.width <= 0 || settings.height <= 0) return false;

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();

    const size_t pixels = size_t(settings.width) * size_t(settings.height);
    width_ = height_ = 0;               // force a clear for every render
    resizeTargets(settings.width, settings.height);

    // Camera basis, resolved once on the host exactly as Camera does.
    Camera camera = scene.camera;
    camera.aspect = float(settings.width) / float(settings.height);
    const Vec3 forward = normalize(camera.target - camera.position);
    const Vec3 right = normalize(cross(forward, camera.up));
    const Vec3 trueUp = cross(right, forward);
    const float halfHeight = std::tan(camera.fovY * 0.5f);
    const float halfWidth = halfHeight * camera.aspect;

    gl::UseProgram(program_);

    setIVec3(program_, "uGridMin", meta_.gridMin);
    setIVec3(program_, "uGridDim", meta_.gridDim);
    setIVec3(program_, "uWorldMin", meta_.worldMin);
    setIVec3(program_, "uWorldMax", meta_.worldMax);

    gl::Uniform2i(gl::GetUniformLocation(program_, "uSize"), settings.width, settings.height);

    setInt(program_, "uMaxBounces", settings.maxBounces);
    setInt(program_, "uRouletteStart", settings.rouletteStartBounce);
    setFloat(program_, "uMaxDistance", settings.maxDistance);
    setFloat(program_, "uClampIndirect", settings.clampIndirect);
    gl::Uniform1ui(gl::GetUniformLocation(program_, "uSeed"), gl::GLuint(settings.seed));

    setVec3(program_, "uCamPos", camera.position);
    setVec3(program_, "uCamRight", right);
    setVec3(program_, "uCamUp", trueUp);
    setVec3(program_, "uCamForward", forward);
    setFloat(program_, "uHalfWidth", halfWidth);
    setFloat(program_, "uHalfHeight", halfHeight);
    setFloat(program_, "uAperture", camera.aperture);
    setFloat(program_, "uFocusDistance", camera.focusDistance);
    setInt(program_, "uOrthographic", camera.projection == Camera::Projection::Orthographic);
    setFloat(program_, "uOrthoHalfW", camera.orthoHeight * 0.5f * camera.aspect);
    setFloat(program_, "uOrthoHalfH", camera.orthoHeight * 0.5f);

    setVec3(program_, "uSunDirection", scene.sun.direction);
    setVec3(program_, "uSunRadiance", scene.sun.radiance());
    setFloat(program_, "uSunCosMax",
             std::cos(radians(std::max(scene.sun.angularRadiusDegrees, 0.01f))));
    setInt(program_, "uSunOn", scene.sun.intensity > 0.0f ? 1 : 0);

    setVec3(program_, "uSkyZenith", scene.sky.zenith);
    setVec3(program_, "uSkyHorizon", scene.sky.horizon);
    setVec3(program_, "uSkyGround", scene.sky.ground);
    setFloat(program_, "uSkyIntensity", scene.sky.intensity);
    setInt(program_, "uOverrideBackground", scene.overrideBackground ? 1 : 0);
    setVec3(program_, "uBackground", scene.background);

    setInt(program_, "uLightCount", int(lightCount_));
    setFloat(program_, "uTotalPower", totalPower_);

    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 0, chunkIndexBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 1, blocksBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 2, materialBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 3, lightBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 4, cdfBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 5, accumBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 6, albedoBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 7, normalBuf_);

    gl::Finish();
    const auto uploaded = Clock::now();

    const int groupsX = (settings.width + 7) / 8;
    const int groupsY = (settings.height + 7) / 8;
    // About eight million pixel-samples a dispatch: comfortably inside the
    // driver's patience on this machine, and few enough launches that the
    // synchronising between them does not show up in the total.
    int batch = samplesPerBatch;
    if (batch < 1) {
        const size_t budget = 8u * 1024u * 1024u;
        batch = int(std::max<size_t>(1, budget / std::max<size_t>(1, pixels)));
        batch = std::min(batch, settings.samplesPerPixel);
    }

    int done = 0;
    int batches = 0;
    while (done < settings.samplesPerPixel) {
        const int count = std::min(batch, settings.samplesPerPixel - done);
        setInt(program_, "uSampleBase", done);
        setInt(program_, "uSampleCount", count);
        gl::DispatchCompute(gl::GLuint(groupsX), gl::GLuint(groupsY), 1);
        gl::MemoryBarrier(gl::SHADER_STORAGE_BARRIER_BIT);
        gl::Finish();
        done += count;
        ++batches;

        if (settings.progress) {
            std::printf("\r  gpu %3d%%", 100 * done / settings.samplesPerPixel);
            std::fflush(stdout);
        }
    }
    if (settings.progress) std::printf("\r        \r");

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
            const float n = std::max(1.0f, albedo[i * 4 + 3]);
            aovs->albedo.data()[i] =
                Vec3{albedo[i * 4] / n, albedo[i * 4 + 1] / n, albedo[i * 4 + 2] / n};
            const Vec3 sum{normalDepth[i * 4], normalDepth[i * 4 + 1], normalDepth[i * 4 + 2]};
            aovs->normal.data()[i] = lengthSq(sum) > 1e-12f ? normalize(sum) : Vec3{0.0f};
            const float depth = normalDepth[i * 4 + 3];
            aovs->depth[i] = depth > 0.0f ? depth / n : -1.0f;
        }
        aovs->color = out;
    }

    const auto done_ = Clock::now();
    if (stats) {
        stats->uploadSeconds = std::chrono::duration<double>(uploaded - started).count();
        stats->traceSeconds = std::chrono::duration<double>(traced - uploaded).count();
        stats->readbackSeconds = std::chrono::duration<double>(done_ - traced).count();
        stats->batches = batches;
        stats->lights = lightCount_;
    }

    const gl::GLenum err = gl::GetError();
    if (err != gl::NO_ERROR_) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "gpu path tracer: GL error 0x%04X", unsigned(err));
        setError(error, buf);
        return false;
    }
    return true;
}

void PathTracer::destroy() {
    if (program_) { gl::DeleteProgram(program_); program_ = 0; }
    const gl::GLuint buffers[] = {chunkIndexBuf_, blocksBuf_, materialBuf_, lightBuf_,
                                  cdfBuf_,        accumBuf_,  albedoBuf_,   normalBuf_};
    for (gl::GLuint b : buffers) {
        if (b) gl::DeleteBuffers(1, &b);
    }
    chunkIndexBuf_ = blocksBuf_ = materialBuf_ = lightBuf_ = cdfBuf_ = 0;
    accumBuf_ = albedoBuf_ = normalBuf_ = 0;
    width_ = height_ = 0;
}

} // namespace gpu
} // namespace blocky
