#include "engine/entity/entity.hpp"

#include <limits>

namespace blocky {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kPixelsPerBlock = 16.0f;

// Alpha at or above this counts as solid. Matches the game's cutout rule.
constexpr float kAlphaCutoff = 0.5f;

SkinFace faceFromAxis(int axis, bool positive) {
    switch (axis) {
        case 0: return positive ? SkinFaceRight : SkinFaceLeft;
        case 1: return positive ? SkinFaceTop : SkinFaceBottom;
        default: return positive ? SkinFaceBack : SkinFaceFront;
    }
}

} // namespace

Mat4 entityToWorld(const Entity& entity) {
    // Translate to the entity, scale pixels into blocks, then yaw.
    return translate(entity.position) *
           scale(Vec3{entity.scale / kPixelsPerBlock}) *
           rotateAxis({0.0f, 1.0f, 0.0f}, radians(entity.yawDegrees));
}

void resolveEntityJoints(const Entity& entity, std::vector<Mat4>& jointToWorld) {
    jointToWorld.clear();
    if (!entity.model) return;

    entity.model->skeleton.resolve(entity.pose, jointToWorld);

    const Mat4 placement = entityToWorld(entity);
    for (Mat4& matrix : jointToWorld) matrix = placement * matrix;
}

bool entityJointToWorld(const Entity& entity, int joint, Mat4& out) {
    if (joint < 0) return false;

    std::vector<Mat4> joints;
    resolveEntityJoints(entity, joints);
    if (size_t(joint) >= joints.size()) return false;

    out = joints[size_t(joint)];
    return true;
}

void EntitySet::clear() {
    boxes_.clear();
    entities_.clear();
    entityCount_ = 0;
    hasBounds_ = false;
}

void EntitySet::add(const Entity& entity) {
    if (!entity.model || !entity.skin) return;

    int entityIndex = int(entityCount_++);
    entities_.push_back(entity);

    // Every joint in world space, with its parents and the entity's own
    // placement already folded in. Resolved once per entity rather than once
    // per box: a rigged model can have dozens of boxes sharing seven joints.
    //
    // This is the same call an attachment makes, deliberately: a lantern hung
    // off a wrist has to agree with the wrist.
    std::vector<Mat4> jointToWorld;
    resolveEntityJoints(entity, jointToWorld);
    const Mat4 placement = entityToWorld(entity);

    for (const ModelBox& box : entity.model->boxes) {
        Vec3 inflated = box.size + Vec3{2.0f * box.inflate};
        if (inflated.x <= 0.0f || inflated.y <= 0.0f || inflated.z <= 0.0f) continue;

        // A box with no joint sits exactly where it was authored.
        Mat4 jointMatrix = (box.joint >= 0 && size_t(box.joint) < jointToWorld.size())
                               ? jointToWorld[size_t(box.joint)]
                               : placement;

        // The joint places the part; the box's own corner offset then puts
        // local (0,0,0) at its min corner.
        FlatBox flat;
        flat.toWorld = jointMatrix * translate(box.origin - Vec3{box.inflate});
        flat.toLocal = inverseAffine(flat.toWorld);
        flat.size = inflated;
        flat.skin = entity.skin;
        flat.cutout = box.cutout;
        flat.entityIndex = entityIndex;
        flat.part = box.part;
        flat.layer = box.layer;
        for (int face = 0; face < SkinFaceCount; ++face) flat.faces[face] = box.faces[face];

        // World AABB over the eight corners, for a cheap reject.
        flat.aabbMin = Vec3{kInf, kInf, kInf};
        flat.aabbMax = Vec3{-kInf, -kInf, -kInf};
        for (int corner = 0; corner < 8; ++corner) {
            Vec3 point{(corner & 1) ? inflated.x : 0.0f,
                       (corner & 2) ? inflated.y : 0.0f,
                       (corner & 4) ? inflated.z : 0.0f};
            Vec3 world = transformPoint(flat.toWorld, point);
            flat.aabbMin = minv(flat.aabbMin, world);
            flat.aabbMax = maxv(flat.aabbMax, world);
        }

        if (!hasBounds_) {
            boundsLo_ = flat.aabbMin;
            boundsHi_ = flat.aabbMax;
            hasBounds_ = true;
        } else {
            boundsLo_ = minv(boundsLo_, flat.aabbMin);
            boundsHi_ = maxv(boundsHi_, flat.aabbMax);
        }

        boxes_.push_back(std::move(flat));
    }
}

bool EntitySet::bounds(Vec3& lo, Vec3& hi) const {
    if (!hasBounds_) return false;
    lo = boundsLo_;
    hi = boundsHi_;
    return true;
}

bool EntitySet::hitBox(const FlatBox& box, const Ray& ray, float maxDistance, float& tOut,
                       Vec3& normal, Vec3& albedo, SkinFace& faceOut) const {
    // Cheap world-space reject before the matrix work.
    {
        float t0 = 0.0f, t1 = maxDistance;
        for (int a = 0; a < 3; ++a) {
            float inv = 1.0f / ray.direction[a];
            float near = (box.aabbMin[a] - ray.origin[a]) * inv;
            float far  = (box.aabbMax[a] - ray.origin[a]) * inv;
            if (inv < 0.0f) std::swap(near, far);
            t0 = std::max(t0, near);
            t1 = std::min(t1, far);
            if (t0 > t1) return false;
        }
    }

    // Box-local space, where the box is the axis-aligned [0, size] slab.
    Vec3 origin = transformPoint(box.toLocal, ray.origin);
    Vec3 direction = transformDir(box.toLocal, ray.direction);

    float t0 = 0.0f, t1 = kInf;
    int enterAxis = -1;

    for (int a = 0; a < 3; ++a) {
        float inv = 1.0f / direction[a];
        float near = (0.0f - origin[a]) * inv;
        float far  = (box.size[a] - origin[a]) * inv;
        if (inv < 0.0f) std::swap(near, far);
        if (near > t0) { t0 = near; enterAxis = a; }
        if (far < t1) t1 = far;
        if (t0 > t1) return false;
    }

    // A ray starting inside the box has no entry face to shade.
    if (enterAxis < 0) return false;

    // t is expressed in the local parameterisation, but the local direction
    // was scaled by the same uniform factor as the space, so the ray
    // parameter itself is unchanged.
    if (t0 <= 1e-4f || t0 >= maxDistance) return false;

    Vec3 localPoint = origin + direction * t0;
    bool positive = direction[enterAxis] < 0.0f;  // entered through the far face
    SkinFace face = faceFromAxis(enterAxis, positive);

    const SkinRect& rect = box.faces[face];
    if (!rect.valid()) return false;

    Vec2 uv = boxFaceUv(localPoint, box.size, face);

    int texX = rect.x + int(saturate(uv.x) * float(rect.width));
    int texY = rect.y + int(saturate(uv.y) * float(rect.height));
    texX = std::min(texX, rect.x + rect.width - 1);
    texY = std::min(texY, rect.y + rect.height - 1);

    if (box.cutout && box.skin->texture().alphaAt(texX, texY) < kAlphaCutoff) return false;

    tOut = t0;
    normal = normalize(transformDir(box.toWorld, skinFaceNormal(face)));
    albedo = box.skin->texture().texel(texX, texY);
    faceOut = face;
    return true;
}

bool EntitySet::intersect(const Ray& ray, float maxDistance, EntityHit& hit) const {
    bool found = false;
    float bestT = maxDistance;

    for (const FlatBox& box : boxes_) {
        float t = 0.0f;
        Vec3 normal, albedo;
        SkinFace face = SkinFaceFront;
        if (!hitBox(box, ray, bestT, t, normal, albedo, face)) continue;

        bestT = t;
        found = true;
        hit.t = t;
        hit.normal = normal;
        hit.albedo = albedo;
        hit.entityIndex = box.entityIndex;
        hit.part = box.part;
        hit.layer = box.layer;
    }

    if (found) hit.position = ray.origin + ray.direction * hit.t;
    return found;
}

bool EntitySet::occluded(const Ray& ray, float maxDistance) const {
    for (const FlatBox& box : boxes_) {
        float t = 0.0f;
        Vec3 normal, albedo;
        SkinFace face = SkinFaceFront;
        if (hitBox(box, ray, maxDistance, t, normal, albedo, face)) return true;
    }
    return false;
}

} // namespace blocky
