#pragma once
// Entity instances and the ray queries against them.
//
// An entity is a model plus a skin plus a placement plus a pose. Placing one
// flattens its boxes into world space once; after that the scene is a flat
// list of oriented boxes, which is all the renderer ever sees. There is no
// animation -- a pose is a fixed arrangement, chosen by the scene author.
//
// The pose is a Pose over the model's skeleton; see engine/rig/rig.hpp. Boxes
// no longer carry their own pivot, so a joint's rotation carries everything
// hanging below it.
#include "engine/core/math.hpp"
#include "engine/entity/model.hpp"
#include "engine/rig/rig.hpp"
#include "engine/world/raycast.hpp"

#include <vector>

namespace blocky {

struct Entity {
    const EntityModel* model = nullptr;
    const Skin*        skin  = nullptr;

    // Feet rest at `position`; x and z are the centre of the model.
    Vec3  position{};
    float yawDegrees = 0.0f;   // rotation about +Y; 0 means looking towards -Z
    float scale = 1.0f;        // 1 = a two-block-tall player

    Pose pose;
};

// ------------------------------------------------------- where an entity is
//
// `add()` needs these to flatten the boxes, and so does anything that wants to
// hang something off a limb -- an item in a fist, a lantern on a belt. They
// are public so that there is one answer rather than two: a scene rebuilding
// this placement by hand agrees with the renderer right up until the day one
// of them changes.

// The entity's own space -- model pixels, +Y up, looking towards -Z -- into
// world space. Rigid plus a uniform scale, which is what props require.
Mat4 entityToWorld(const Entity& entity);

// Every joint of the model's skeleton in world space, pose already resolved.
// Left empty when the entity has no model.
void resolveEntityJoints(const Entity& entity, std::vector<Mat4>& jointToWorld);

// One joint of the same. False when there is no such joint, leaving `out`
// untouched. Resolves the whole skeleton to answer, so a caller placing
// several things should keep the vector instead.
bool entityJointToWorld(const Entity& entity, int joint, Mat4& out);

struct EntityHit {
    float t = 0.0f;
    Vec3  position{};
    Vec3  normal{};    // world space, unit, pointing back along the ray
    Vec3  albedo{};    // linear light, straight from the skin
    int   entityIndex = -1;
    SkinPart part = PartHead;
    SkinLayer layer = LayerBase;
};

class EntitySet {
public:
    void clear();

    // Flattens the entity into world-space boxes immediately. The model and
    // skin must outlive the set.
    void add(const Entity& entity);

    bool   empty() const { return boxes_.empty(); }
    size_t boxCount() const { return boxes_.size(); }
    size_t entityCount() const { return entityCount_; }

    // Nearest box surface along the ray, with alpha-tested outer layers.
    bool intersect(const Ray& ray, float maxDistance, EntityHit& hit) const;

    // Cheaper query for shadow rays.
    bool occluded(const Ray& ray, float maxDistance) const;

    // World-space bounds of everything placed so far.
    bool bounds(Vec3& lo, Vec3& hi) const;

    // The entities as they were added. The tracer only ever needs the
    // flattened boxes, but the viewport meshes each entity separately so that
    // every one keeps its own skin texture.
    const std::vector<Entity>& entities() const { return entities_; }

    // One box in world space, with its skin rectangles resolved. This is what
    // `add()` flattens an entity into and what the tracer actually walks.
    struct FlatBox {
        Mat4 toWorld;
        Mat4 toLocal;
        Vec3 size{};             // inflated extent, in model pixels
        Vec3 aabbMin{}, aabbMax{};
        const Skin* skin = nullptr;
        SkinRect faces[SkinFaceCount];
        bool cutout = false;
        int  entityIndex = -1;
        SkinPart part = PartHead;
        SkinLayer layer = LayerBase;
    };

    // The flattened boxes, so a shader can be handed the same ones the CPU
    // walks rather than flattening the entities a second time. Two flattenings
    // would be two chances to disagree about a pivot or a skin rectangle, and
    // the disagreement would look like a character with a limb in the wrong
    // place rather than like a bug.
    const std::vector<FlatBox>& boxes() const { return boxes_; }

private:

    // Returns true and fills the surface data when the ray enters this box
    // through a texel that is not cut away.
    bool hitBox(const FlatBox& box, const Ray& ray, float maxDistance, float& t,
                Vec3& normal, Vec3& albedo, SkinFace& face) const;

    std::vector<FlatBox> boxes_;
    std::vector<Entity> entities_;
    size_t entityCount_ = 0;
    Vec3 boundsLo_{}, boundsHi_{};
    bool hasBounds_ = false;
};

} // namespace blocky
