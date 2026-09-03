#pragma once
// Everything a renderer needs to produce a frame: the voxel world, a camera,
// and the lighting environment. Entities will join this struct at stage 4.
#include "engine/core/math.hpp"
#include "engine/scene/camera.hpp"
#include "engine/scene/material_style.hpp"
#include "engine/world/world.hpp"

namespace blocky {

// Game assets are optional: a scene renders with flat palette colours when no
// texture library is attached. Forward-declared so scene.hpp stays free of
// the asset and archive headers.
class BlockTextureLibrary;

// Entities live outside the voxel grid: oriented boxes at arbitrary
// positions. Also optional -- a scene with none renders exactly as before.
class EntitySet;

// Particles and floating text, both of them flat alpha-cut quads. Optional in
// the same way, and behind the same kind of pointer.
class SpriteSet;

// Items and props: small voxel grids placed by a transform, off the lattice.
class PropSet;

// Analytic three-colour sky. Cheap, controllable, and good enough to light a
// scene by; a physical Hosek-Wilkie model can replace it without touching
// anything that calls sample().
struct Sky {
    Vec3 zenith {0.16f, 0.30f, 0.62f};
    Vec3 horizon{0.62f, 0.74f, 0.92f};
    Vec3 ground {0.22f, 0.21f, 0.20f};
    float intensity = 1.0f;

    Vec3 sample(Vec3 direction) const {
        float h = direction.y;
        if (h >= 0.0f) {
            // Bias the blend towards the horizon so the gradient is not linear.
            float t = std::pow(saturate(h), 0.45f);
            return lerp(horizon, zenith, t) * intensity;
        }
        float t = saturate(-h * 3.0f);
        return lerp(horizon, ground, t) * intensity;
    }
};

struct SunLight {
    // Direction *towards* the sun, normalised.
    Vec3  direction = normalize(Vec3{0.45f, 0.78f, 0.35f});
    Vec3  color{1.0f, 0.96f, 0.86f};
    float intensity = 4.2f;
    // The real sun subtends about 0.53 degrees. Widening this softens shadows.
    float angularRadiusDegrees = 0.6f;

    Vec3 radiance() const { return color * intensity; }
};

struct Scene {
    // A scene is built against a palette, for the same reason a world is: the
    // engine has no blocks of its own to default to. This is the only reason
    // `Scene` has a constructor at all.
    explicit Scene(BlockRegistry& registry) : world(registry) {}

    World     world;
    Camera    camera;
    SunLight  sun;
    Sky       sky;

    // Block textures, or null to render with flat palette colours.
    const BlockTextureLibrary* blockTextures = nullptr;

    // Characters and mobs placed in the scene, or null for none.
    const EntitySet* entities = nullptr;

    // Particle quads and text labels, or null for none. Must have had
    // build() called on it, or the tracer will not see anything in it.
    const SpriteSet* sprites = nullptr;

    // Voxel items and props, or null for none. Same rule: build() first.
    const PropSet* props = nullptr;

    // Scales the sky contribution used as ambient light. Lower it for a
    // moodier, more contrasty render.
    float ambientStrength = 1.0f;

    // Overrides what every surface is made of, before any light touches it.
    // Default is identity, and intersectScene skips it entirely then.
    MaterialStyle materialStyle;

    // Colour returned by a camera ray that hits nothing. An orthographic
    // camera fires parallel rays, so sampling the sky with the ray direction
    // would paint one flat colour -- such scenes should set this explicitly.
    bool overrideBackground = false;
    Vec3 background{0.0f};

    Vec3 missColor(Vec3 direction) const {
        return overrideBackground ? background : sky.sample(direction);
    }

    // Frame the camera on everything currently in the world.
    void frameAll(float heightMargin = 1.15f, float yawDegrees = 45.0f);
};

} // namespace blocky
