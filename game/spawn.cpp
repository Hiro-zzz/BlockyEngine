#include "game/spawn.hpp"

#include "engine/physics/body.hpp"

#include <algorithm>

namespace game {

using namespace blocky;

namespace {

// How many voxels a lifted block is cut into. One would do -- the collider
// and the mass would be identical -- but a single voxel carries a single
// colour, and a grass block that came up grey-brown all over reads as the
// wrong block rather than as a limitation. Eight lets each face keep its own
// texture's average, so grass is green on top and dirt down the sides.
constexpr int kBlockDetail = 8;

// Heavy. A block is a cubic metre of whatever it is, and the point of
// comparison is the crate: a lifted stone block should not skitter when a
// crate lands on it.
constexpr float kBlockDensity = 1100.0f;

// Which face of a cube a shell voxel belongs to, when it belongs to more than
// one. Top wins, then the sides, then the bottom -- looking down at a lifted
// block from above is the common case, and the top is the face that carries
// the difference on the blocks anybody notices (grass, snow, a log's rings).
int faceOfShellVoxel(int x, int y, int z, int n) {
    if (y == n - 1) return FacePosY;
    if (x == 0) return FaceNegX;
    if (x == n - 1) return FacePosX;
    if (z == 0) return FaceNegZ;
    if (z == n - 1) return FacePosZ;
    if (y == 0) return FaceNegY;
    return -1;
}

Vec2 faceUv(int face, int x, int y, int z, int n) {
    const float u = (float(x) + 0.5f) / float(n);
    const float v = (float(y) + 0.5f) / float(n);
    const float w = (float(z) + 0.5f) / float(n);
    switch (face) {
        case FacePosY: return {u, w};
        case FaceNegY: return {u, w};
        case FaceNegX:
        case FacePosX: return {w, 1.0f - v};
        default:       return {u, 1.0f - v};
    }
}

} // namespace

const PropYard::BlockProp& PropYard::blockPropFor(BlockId id, const BlockRegistry& registry,
                                                  const BlockTextureLibrary* textures) {
    auto found = blockPropByBlock_.find(uint32_t(id));
    if (found != blockPropByBlock_.end()) return blockProps_[found->second];

    const int n = kBlockDetail;
    const BlockDef& def = registry[id];

    BlockProp prop;
    prop.model.resize({n, n, n});

    // `addMaterial` folds identical entries itself -- it was written for
    // extruded item textures, which ask for one per texel and are mostly the
    // same colour -- so there is no table to keep here.
    auto materialFor = [&](Vec3 albedo) {
        VoxelMaterial material;
        material.albedo = albedo;
        material.emission = def.emission;
        material.roughness = def.roughness;
        material.metallic = def.metallic;
        return prop.model.addMaterial(material);
    };

    // The side colour stands in for everything that is never seen. Interior
    // voxels still have to exist: they are what the mass is made of.
    const Vec3 sideAlbedo =
        textures ? textures->sampleAlbedo(id, FaceNegZ, {0.5f, 0.5f}, def.albedo) : def.albedo;
    const uint16_t interior = materialFor(sideAlbedo);

    for (int y = 0; y < n; ++y)
        for (int z = 0; z < n; ++z)
            for (int x = 0; x < n; ++x) {
                const int face = faceOfShellVoxel(x, y, z, n);
                if (face < 0 || !textures) {
                    prop.model.set({x, y, z}, interior);
                    continue;
                }
                const Vec3 albedo =
                    textures->sampleAlbedo(id, face, faceUv(face, x, y, z, n), def.albedo);
                prop.model.set({x, y, z}, materialFor(albedo));
            }

    prop.collider = buildCollider(prop.model, 1.0f / float(n));

    blockProps_.push_back(std::move(prop));
    blockPropByBlock_.emplace(uint32_t(id), blockProps_.size() - 1);
    return blockProps_.back();
}

int PropYard::spawn(PhysicsWorld& physics, size_t kind, Vec3 at, Vec3 velocity, Vec3 spin) {
    if (!catalogue_ || kind >= catalogue_->size()) return -1;
    const PropKind& entry = (*catalogue_)[kind];

    RigidBody body;
    setMassFromCollider(body, entry.collider, entry.density);
    body.voxelSize = entry.voxelSize;
    body.position = at;
    body.linearVelocity = velocity;
    body.angularVelocity = spin;
    body.friction = entry.friction;
    body.restitution = entry.restitution;

    Spawned made;
    made.body = physics.add(body);
    made.model = &entry.model;
    spawned_.push_back(made);
    return made.body;
}

int PropYard::liftBlock(PhysicsWorld& physics, World& world, IVec3 cell,
                        const BlockTextureLibrary* textures) {
    const BlockId id = world.get(cell);
    if (!world.registry().isSolid(id)) return -1;

    const BlockProp& prop = blockPropFor(id, world.registry(), textures);

    // The block leaves the lattice before the body arrives, or the body spawns
    // inside the very cell it came from and the first step shoves it out.
    world.set(cell, BlockId(block::Air));

    RigidBody body;
    setMassFromCollider(body, prop.collider, kBlockDensity);
    body.voxelSize = 1.0f / float(kBlockDetail);
    // A cell spans [cell, cell + 1); a body sits at the centre of its own
    // collider, so it lands exactly where the block was.
    body.position = Vec3{float(cell.x) + 0.5f, float(cell.y) + 0.5f, float(cell.z) + 0.5f};
    body.friction = 0.72f;
    body.restitution = 0.02f;

    Spawned made;
    made.body = physics.add(body);
    made.model = &prop.model;
    spawned_.push_back(made);
    return made.body;
}

void PropYard::clear() {
    spawned_.clear();
    // The models stay: they are a cache keyed by block, and the next world is
    // built out of the same palette.
}

void PropYard::forgetDead(const PhysicsWorld& physics) {
    spawned_.erase(std::remove_if(spawned_.begin(), spawned_.end(),
                                  [&](const Spawned& s) { return !physics.alive(s.body); }),
                   spawned_.end());
}

int PropYard::trim(PhysicsWorld& physics, size_t budget, int keepBody) {
    int removed = 0;
    while (spawned_.size() > budget) {
        // Oldest first, but never the one being held: a tool whose grip
        // evaporated mid-drag is a bug the player reads as the game dropping
        // things at random.
        auto victim = spawned_.begin();
        if (victim->body == keepBody && spawned_.size() > 1) ++victim;
        if (victim->body == keepBody) break;

        if (physics.alive(victim->body)) physics.remove(victim->body);
        spawned_.erase(victim);
        ++removed;
    }
    return removed;
}

void PropYard::draw(PropSet& props, const PhysicsWorld& physics) const {
    for (const Spawned& made : spawned_) {
        if (!made.model || !physics.alive(made.body)) continue;
        props.addTransformed(made.model, bodyToWorld(physics.body(made.body)), made.tint);
    }
}

} // namespace game
