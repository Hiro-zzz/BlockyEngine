#pragma once
// Surface scattering models.
//
// Voxel faces are axis-aligned and perfectly flat, so there is no distinction
// between geometric and shading normals here -- none of the usual shadow
// terminator hacks are needed. Everything below works in a local frame where
// the surface normal is +Z.
#include "engine/core/math.hpp"
#include "engine/core/random.hpp"

namespace blocky {
namespace bsdf {

// --------------------------------------------------------------- local frame
struct Frame {
    Vec3 t, b, n;

    explicit Frame(Vec3 normal) : n(normal) { orthonormalBasis(n, t, b); }

    Vec3 toLocal(Vec3 v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
    Vec3 toWorld(Vec3 v) const { return t * v.x + b * v.y + n * v.z; }
};

// ------------------------------------------------------------------ Fresnel
// Schlick approximation, used for conductors where F0 is the metal colour.
inline Vec3 fresnelSchlick(float cosTheta, Vec3 f0) {
    float m = saturate(1.0f - cosTheta);
    float m2 = m * m;
    float m5 = m2 * m2 * m;
    return f0 + (Vec3{1.0f} - f0) * m5;
}

// Exact dielectric Fresnel for unpolarised light. `eta` is the ratio of the
// incident to the transmitted index of refraction. Returns 1 under total
// internal reflection.
inline float fresnelDielectric(float cosThetaI, float eta) {
    cosThetaI = saturate(cosThetaI);
    float sin2ThetaI = 1.0f - cosThetaI * cosThetaI;
    float sin2ThetaT = eta * eta * sin2ThetaI;
    if (sin2ThetaT >= 1.0f) return 1.0f;  // total internal reflection

    float cosThetaT = std::sqrt(1.0f - sin2ThetaT);
    float rParallel = (cosThetaI - eta * cosThetaT) / (cosThetaI + eta * cosThetaT);
    float rPerpendicular = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
    return 0.5f * (rParallel * rParallel + rPerpendicular * rPerpendicular);
}

// Refract `incident` (pointing at the surface) about `normal` (facing the
// incident ray). Returns false under total internal reflection.
inline bool refractRay(Vec3 incident, Vec3 normal, float eta, Vec3& out) {
    float cosThetaI = -dot(incident, normal);
    float sin2ThetaT = eta * eta * (1.0f - cosThetaI * cosThetaI);
    if (sin2ThetaT >= 1.0f) return false;

    float cosThetaT = std::sqrt(1.0f - sin2ThetaT);
    out = normalize(incident * eta + normal * (eta * cosThetaI - cosThetaT));
    return true;
}

// ---------------------------------------------------------------- GGX / TR
inline float roughnessToAlpha(float roughness) {
    // Keep alpha away from zero so the distribution never degenerates.
    return std::max(1e-3f, roughness * roughness);
}

inline float ggxD(Vec3 h, float alpha) {
    float a2 = alpha * alpha;
    float cos2 = h.z * h.z;
    float d = cos2 * (a2 - 1.0f) + 1.0f;
    return a2 / std::max(kPi * d * d, 1e-9f);
}

// Smith masking-shadowing for one direction.
inline float smithG1(Vec3 v, float alpha) {
    float cos2 = v.z * v.z;
    if (cos2 <= 0.0f) return 0.0f;
    float tan2 = (1.0f - cos2) / cos2;
    return 2.0f / (1.0f + std::sqrt(1.0f + alpha * alpha * tan2));
}

// Sample the distribution of visible normals (Heitz 2018). Sampling visible
// normals rather than the raw NDF is what keeps grazing angles from
// exploding into fireflies.
inline Vec3 sampleGgxVndf(Vec3 wo, float alpha, float u1, float u2) {
    // Stretch the view direction so the ellipsoid becomes a hemisphere.
    Vec3 vh = normalize(Vec3{alpha * wo.x, alpha * wo.y, wo.z});

    float lenSq = vh.x * vh.x + vh.y * vh.y;
    Vec3 t1 = lenSq > 0.0f ? Vec3{-vh.y, vh.x, 0.0f} / std::sqrt(lenSq) : Vec3{1.0f, 0.0f, 0.0f};
    Vec3 t2 = cross(vh, t1);

    float r = std::sqrt(u1);
    float phi = kTwoPi * u2;
    float px = r * std::cos(phi);
    float py = r * std::sin(phi);
    float s = 0.5f * (1.0f + vh.z);
    py = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - px * px)) + s * py;

    Vec3 nh = t1 * px + t2 * py + vh * std::sqrt(std::max(0.0f, 1.0f - px * px - py * py));
    return normalize(Vec3{alpha * nh.x, alpha * nh.y, std::max(1e-6f, nh.z)});
}

// f * cos(theta_i) for a GGX conductor, in the local frame.
inline Vec3 ggxEvalTimesCos(Vec3 wo, Vec3 wi, Vec3 f0, float alpha) {
    if (wo.z <= 0.0f || wi.z <= 0.0f) return Vec3{0.0f};

    Vec3 h = normalize(wo + wi);
    float d = ggxD(h, alpha);
    float g = smithG1(wo, alpha) * smithG1(wi, alpha);
    Vec3 f = fresnelSchlick(saturate(dot(wi, h)), f0);

    // The wi.z of the BRDF cancels against the cosine factor.
    return f * (d * g / std::max(4.0f * wo.z, 1e-9f));
}

// Probability density of the VNDF sampling scheme, in solid angle.
inline float ggxPdf(Vec3 wo, Vec3 wi, float alpha) {
    if (wo.z <= 0.0f || wi.z <= 0.0f) return 0.0f;
    Vec3 h = normalize(wo + wi);
    float g1 = smithG1(wo, alpha);
    return g1 * ggxD(h, alpha) * std::max(0.0f, dot(wo, h)) / std::max(wo.z, 1e-9f) / 4.0f;
}

} // namespace bsdf
} // namespace blocky
