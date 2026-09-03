#include "engine/prop/prop_set.hpp"

namespace blocky {
namespace {

constexpr float kMinDistance = 1e-4f;

Vec3 anchorVoxel(const Prop& prop, IVec3 dims) {
    Vec3 size = toVec3(dims);
    switch (prop.anchor) {
        case Prop::Anchor::Centre:       return size * 0.5f;
        case Prop::Anchor::Min:          return Vec3{0.0f, 0.0f, 0.0f};
        default:                         return {size.x * 0.5f, 0.0f, size.z * 0.5f};
    }
}

}  // namespace

void PropSet::clear() {
    flats_.clear();
    props_.clear();
    boxes_.clear();
    bvh_.clear();
    voxels_ = 0;
    hasBounds_ = false;
}

void PropSet::add(const Prop& prop) {
    if (!prop.model || prop.model->empty() || prop.voxelSize <= 0.0f) return;

    const IVec3 dims = prop.model->dims();

    Mat4 rotation = rotateAxis({0.0f, 1.0f, 0.0f}, radians(prop.yawDegrees)) *
                    rotateAxis({1.0f, 0.0f, 0.0f}, radians(prop.pitchDegrees)) *
                    rotateAxis({0.0f, 0.0f, 1.0f}, radians(prop.rollDegrees));

    // World = position + R * ((voxel - anchor) * voxelSize). The scale is
    // uniform on purpose: a non-uniform one would stretch the local ray
    // direction unevenly, and the ray parameter would stop meaning the same
    // thing on the two sides of the transform.
    Mat4 toWorld = translate(prop.position) * rotation * scale(Vec3{prop.voxelSize}) *
                   translate(-anchorVoxel(prop, dims));

    place(prop.model, toWorld, prop.tint, prop.emissionScale, &prop);
}

void PropSet::addTransformed(const VoxelModel* model, const Mat4& toWorld, Vec3 tint,
                             Vec3 emissionScale) {
    if (!model || model->empty()) return;
    place(model, toWorld, tint, emissionScale, nullptr);
}

void PropSet::place(const VoxelModel* model, const Mat4& toWorld, Vec3 tint, Vec3 emissionScale,
                    const Prop* source) {
    const IVec3 dims = model->dims();

    Flat flat;
    flat.toWorld = toWorld;
    flat.toLocal = inverseAffine(toWorld);
    flat.model = model;
    flat.tint = tint;
    flat.emissionScale = emissionScale;

    Aabb box;
    for (int corner = 0; corner < 8; ++corner) {
        Vec3 v{(corner & 1) ? float(dims.x) : 0.0f, (corner & 2) ? float(dims.y) : 0.0f,
               (corner & 4) ? float(dims.z) : 0.0f};
        box.expand(transformPoint(toWorld, v));
    }

    flats_.push_back(flat);
    if (source) {
        props_.push_back(*source);
    } else {
        Prop recorded;
        recorded.model = model;
        recorded.tint = tint;
        recorded.emissionScale = emissionScale;
        props_.push_back(recorded);
    }
    boxes_.push_back(box);
    voxels_ += uint64_t(model->solidCount());

    if (!hasBounds_) {
        boundsLo_ = box.lo;
        boundsHi_ = box.hi;
        hasBounds_ = true;
    } else {
        boundsLo_ = minv(boundsLo_, box.lo);
        boundsHi_ = maxv(boundsHi_, box.hi);
    }

    bvh_.clear();
}

void PropSet::add(const std::vector<Prop>& props) {
    for (const Prop& prop : props) add(prop);
}

void PropSet::build() {
    bvh_.build(boxes_);
}

bool PropSet::intersect(const Ray& ray, float maxDistance, PropHit& hit) const {
    if (bvh_.empty()) return false;

    Vec3 invDirection = inverseDirection(ray.direction);

    bool found = false;
    bvh_.traverse(ray.origin, invDirection, maxDistance, [&](uint32_t index, float& tMax) {
        const Flat& flat = flats_[index];

        // Into voxel units. The direction is scaled by the same factor as the
        // space, so `t` still measures world distance on the way back out.
        Vec3 origin = transformPoint(flat.toLocal, ray.origin);
        Vec3 direction = transformDir(flat.toLocal, ray.direction);

        VoxelHit local;
        if (!flat.model->trace(origin, direction, kMinDistance, tMax, local)) return false;

        const VoxelMaterial& material = flat.model->material(local.material);

        tMax = local.t;
        found = true;
        hit.t = local.t;
        hit.position = ray.origin + ray.direction * local.t;
        hit.normal = normalize(transformDir(flat.toWorld, local.normal));
        hit.albedo = material.albedo * flat.tint;
        hit.emission = material.emission * flat.emissionScale;
        hit.roughness = material.roughness;
        hit.metallic = material.metallic;
        hit.propIndex = int(index);
        hit.voxel = local.voxel;
        return true;
    });

    return found;
}

bool PropSet::occluded(const Ray& ray, float maxDistance) const {
    if (bvh_.empty()) return false;

    Vec3 invDirection = inverseDirection(ray.direction);

    return bvh_.traverse<true>(ray.origin, invDirection, maxDistance,
                               [&](uint32_t index, float& tMax) {
                                   const Flat& flat = flats_[index];
                                   Vec3 origin = transformPoint(flat.toLocal, ray.origin);
                                   Vec3 direction = transformDir(flat.toLocal, ray.direction);
                                   return flat.model->occluded(origin, direction, kMinDistance,
                                                               tMax);
                               });
}

bool PropSet::bounds(Vec3& lo, Vec3& hi) const {
    if (!hasBounds_) return false;
    lo = boundsLo_;
    hi = boundsHi_;
    return true;
}

} // namespace blocky
