#include "game/props.hpp"

#include "engine/core/image.hpp"

#include <cmath>

namespace game {
namespace {

using namespace blocky;

VoxelMaterial colour(int r, int g, int b, float roughness = 0.85f) {
    VoxelMaterial material;
    material.albedo = srgbToLinear(Vec3{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f});
    material.roughness = roughness;
    return material;
}

VoxelMaterial glowing(int r, int g, int b, float strength) {
    VoxelMaterial material = colour(r, g, b, 0.6f);
    material.emission = material.albedo * strength;
    return material;
}

// A crate. The bands are not decoration: a uniformly coloured cube tumbles
// invisibly, and half of what a sandbox is for is watching something turn.
VoxelModel buildCrate() {
    VoxelModel model;
    model.resize({8, 8, 8});
    uint16_t body = model.addMaterial(colour(184, 133, 77));
    uint16_t band = model.addMaterial(colour(87, 66, 51));
    uint16_t lid  = model.addMaterial(colour(219, 173, 107));

    for (int y = 0; y < 8; ++y)
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) {
                const bool post = (x == 0 || x == 7) && (z == 0 || z == 7);
                uint16_t material = body;
                if (post || y == 0 || y == 7) material = band;
                if (y == 7 && !post) material = lid;
                model.set({x, y, z}, material);
            }
    return model;
}

// A barrel: round in plan, with two hoops. Round matters -- it is the one
// shape in the catalogue that rolls, and a sandbox where nothing rolls is a
// sandbox where the floor might as well be sticky.
VoxelModel buildBarrel() {
    VoxelModel model;
    const int w = 10, h = 12;
    model.resize({w, h, w});
    uint16_t stave = model.addMaterial(colour(150, 96, 54));
    uint16_t hoop  = model.addMaterial(colour(96, 100, 108, 0.45f));
    uint16_t top   = model.addMaterial(colour(122, 80, 46));

    const float centre = float(w - 1) * 0.5f;
    for (int y = 0; y < h; ++y) {
        // The bulge: widest at the middle, tucked in at both ends, which is
        // what tells a barrel from a drum at a glance.
        const float t = (float(y) - float(h - 1) * 0.5f) / (float(h) * 0.5f);
        const float radius = centre * (0.86f + 0.14f * (1.0f - t * t));
        const bool hoopRow = y == 2 || y == 3 || y == h - 4 || y == h - 3;

        for (int z = 0; z < w; ++z)
            for (int x = 0; x < w; ++x) {
                const float dx = float(x) - centre, dz = float(z) - centre;
                if (dx * dx + dz * dz > radius * radius) continue;
                uint16_t material = hoopRow ? hoop : stave;
                if (y == 0 || y == h - 1) material = top;
                model.set({x, y, z}, material);
            }
    }
    return model;
}

// A plank. Long and thin, so it is the thing you weld two of and discover the
// solver was right about inertia.
VoxelModel buildPlank() {
    VoxelModel model;
    model.resize({20, 3, 6});
    uint16_t wood = model.addMaterial(colour(158, 115, 70));
    uint16_t seam = model.addMaterial(colour(110, 78, 47));
    for (int y = 0; y < 3; ++y)
        for (int z = 0; z < 6; ++z)
            for (int x = 0; x < 20; ++x) model.set({x, y, z}, (x % 7 == 0) ? seam : wood);
    return model;
}

// An L of steel. Its centre of mass is nowhere near the middle of its
// bounding box, which is the case a naive mass calculation gets wrong and
// this one does not.
VoxelModel buildGirder() {
    VoxelModel model;
    model.resize({12, 12, 5});
    uint16_t steel = model.addMaterial(colour(126, 132, 141, 0.4f));
    uint16_t rust  = model.addMaterial(colour(134, 84, 56, 0.7f));
    for (int y = 0; y < 12; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 12; ++x)
                if (x < 4 || y < 4)
                    model.set({x, y, z}, (y == 11 || x == 11) ? rust : steel);
    return model;
}

// A ball, and the only entry with any bounce. Everything else in a voxel
// world is a box hitting a box; this is the one that arrives somewhere its
// thrower did not aim.
VoxelModel buildBall() {
    VoxelModel model;
    const int d = 11;
    model.resize({d, d, d});
    uint16_t skin  = model.addMaterial(colour(206, 74, 62, 0.5f));
    uint16_t panel = model.addMaterial(colour(238, 232, 220, 0.5f));

    const float centre = float(d - 1) * 0.5f;
    const float radius = centre + 0.35f;
    for (int y = 0; y < d; ++y)
        for (int z = 0; z < d; ++z)
            for (int x = 0; x < d; ++x) {
                const float dx = float(x) - centre, dy = float(y) - centre, dz = float(z) - centre;
                if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                // A band round one axis, so a rolling ball reads as rolling.
                const bool stripe = std::fabs(dy) < 1.2f;
                model.set({x, y, z}, stripe ? panel : skin);
            }
    return model;
}

// A lamp. It glows and -- worth knowing before it disappoints -- it does not
// light anything: `LightSet` gathers emissive faces of the voxel *world*, so
// a prop shines in the picture and casts nothing. Put a glowstone block down
// for real light. It is here because a sandbox at dusk wants something to
// carry, and because the physgun beam looks right holding one.
VoxelModel buildLamp() {
    VoxelModel model;
    model.resize({6, 9, 6});
    uint16_t frame = model.addMaterial(colour(74, 68, 60, 0.35f));
    uint16_t core  = model.addMaterial(glowing(255, 214, 148, 5.0f));

    for (int y = 0; y < 9; ++y)
        for (int z = 0; z < 6; ++z)
            for (int x = 0; x < 6; ++x) {
                const bool corner = (x == 0 || x == 5) && (z == 0 || z == 5);
                const bool cap = y == 0 || y == 8;
                if (cap || corner) {
                    model.set({x, y, z}, frame);
                } else if (x > 0 && x < 5 && z > 0 && z < 5) {
                    // Hollow but for a core: the first lantern built here was
                    // sealed on all six sides, and an airtight glowing thing
                    // renders as a lump.
                    const bool inner = x > 1 && x < 4 && z > 1 && z < 4 && y > 2 && y < 6;
                    if (inner) model.set({x, y, z}, core);
                }
            }
    return model;
}

PropKind kindOf(const char* name, VoxelModel model, float voxelSize, float density,
                float friction, float restitution) {
    PropKind kind;
    kind.name = name;
    kind.model = std::move(model);
    kind.voxelSize = voxelSize;
    kind.density = density;
    kind.friction = friction;
    kind.restitution = restitution;
    kind.collider = buildCollider(kind.model, voxelSize);
    return kind;
}

} // namespace

std::vector<PropKind> buildPropCatalogue() {
    std::vector<PropKind> catalogue;
    catalogue.reserve(6);

    catalogue.push_back(kindOf("CRATE",  buildCrate(),  0.125f, 380.0f, 0.66f, 0.04f));
    catalogue.push_back(kindOf("BARREL", buildBarrel(), 0.110f, 460.0f, 0.55f, 0.08f));
    catalogue.push_back(kindOf("PLANK",  buildPlank(),  0.125f, 340.0f, 0.70f, 0.03f));
    catalogue.push_back(kindOf("GIRDER", buildGirder(), 0.110f, 1450.0f, 0.62f, 0.02f));
    catalogue.push_back(kindOf("BALL",   buildBall(),   0.100f, 260.0f, 0.42f, 0.55f));
    catalogue.push_back(kindOf("LAMP",   buildLamp(),   0.110f, 300.0f, 0.60f, 0.06f));

    return catalogue;
}

} // namespace game
