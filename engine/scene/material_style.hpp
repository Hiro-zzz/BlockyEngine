#pragma once
// A whole-scene override of what surfaces are made of.
//
// This is the first of the two seams a render style can use, and it is the
// one that happens *before* light transport: it changes the material, and the
// renderer then lights that material honestly. A plastic scene really does
// have a specular highlight travelling through the same path tracer as
// everything else, which is why it lands in reflections and in the denoiser's
// albedo buffer without any special case.
//
// The other seam is post -- see render/post/stylize.hpp. That one changes how
// finished light is displayed, and cannot do this: no amount of filtering an
// image gives a surface a highlight it never had.
//
// Applied in intersectScene, the single place a material is resolved, so the
// path tracer and the direct renderer can never disagree about it.
#include "engine/core/math.hpp"

namespace blocky {

// What the style hands back. Deliberately a plain value rather than a mutated
// SceneHit: SceneHit lives above this header, in the render layer.
struct StyledMaterial {
    Vec3  albedo{};
    float roughness = 1.0f;
    float metallic = 0.0f;

    // Strength of the dielectric coat, 0 for none.
    float coat = 0.0f;
    float coatRoughness = 0.14f;
};

struct MaterialStyle {
    // Negative keeps whatever the material already had.
    float roughness = -1.0f;
    float metallic  = -1.0f;

    // A *white* highlight laid over the diffuse base and weighted by Fresnel.
    // This is exactly what separates plastic from metal: a conductor tints
    // its highlight with its own colour and has no diffuse lobe at all, while
    // a dielectric keeps its diffuse colour and reflects the light source's
    // own colour on top. Painting a block metallic to fake gloss gives the
    // first when what a toy wants is the second.
    float coat = 0.0f;
    float coatRoughness = 0.14f;

    // Albedo grading, applied *before* lighting rather than after, so bounce
    // light carries the change too -- a saturated red wall bleeds a saturated
    // red onto its neighbour. Grading the finished image cannot do that.
    float saturation = 1.0f;
    float gain = 1.0f;

    // Nothing to do: intersectScene skips the whole call.
    bool identity() const {
        return roughness < 0.0f && metallic < 0.0f && coat <= 0.0f && saturation == 1.0f &&
               gain == 1.0f;
    }

    StyledMaterial apply(Vec3 albedo, float baseRoughness, float baseMetallic) const {
        StyledMaterial out;
        out.roughness = roughness >= 0.0f ? roughness : baseRoughness;
        out.metallic  = metallic  >= 0.0f ? metallic  : baseMetallic;
        out.coat = coat;
        out.coatRoughness = coatRoughness;

        float grey = dot(albedo, Vec3{0.2126f, 0.7152f, 0.0722f});
        out.albedo = (Vec3{grey} + (albedo - Vec3{grey}) * saturation) * gain;
        out.albedo = {std::max(0.0f, out.albedo.x), std::max(0.0f, out.albedo.y),
                      std::max(0.0f, out.albedo.z)};
        return out;
    }

    // ------------------------------------------------------------- presets
    // Leave every material exactly as the scene declared it.
    static MaterialStyle realistic() { return {}; }

    // Moulded toy: every surface a dielectric, gold included, under one tight
    // white highlight. Metals lose their tinted reflection on purpose -- that
    // tint is the single strongest cue that a thing is metal.
    static MaterialStyle plastic() {
        MaterialStyle s;
        s.metallic = 0.0f;
        s.coat = 1.0f;
        s.coatRoughness = 0.10f;
        s.saturation = 1.20f;
        return s;
    }

    // Unfired clay: fully rough, no highlight anywhere, colour pulled back.
    // Useful on its own and as the base for cel shading, which wants nothing
    // in the frame that its bands cannot quantise.
    static MaterialStyle matte() {
        MaterialStyle s;
        s.roughness = 1.0f;
        s.metallic = 0.0f;
        s.coat = 0.0f;
        s.saturation = 0.94f;
        return s;
    }
};

} // namespace blocky
