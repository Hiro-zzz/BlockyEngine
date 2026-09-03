#include "engine/prop/voxel_model.hpp"

#include <algorithm>
#include <limits>

namespace blocky {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

bool nearlyEqual(const VoxelMaterial& a, const VoxelMaterial& b) {
    auto close = [](Vec3 u, Vec3 v) { return maxComponent(absv(u - v)) < 1e-5f; };
    return close(a.albedo, b.albedo) && close(a.emission, b.emission) &&
           std::fabs(a.roughness - b.roughness) < 1e-5f &&
           std::fabs(a.metallic - b.metallic) < 1e-5f;
}

// Slab test against the whole grid, reporting which plane the ray came in
// through so the first cell can be shaded with the right normal.
bool clipToBox(Vec3 origin, Vec3 direction, Vec3 lo, Vec3 hi, float tMin, float tMax, float& t0,
               float& t1, int& entryAxis) {
    t0 = tMin;
    t1 = tMax;
    entryAxis = -1;

    for (int a = 0; a < 3; ++a) {
        float inv = 1.0f / direction[a];  // infinity here is deliberate and correct
        float near = (lo[a] - origin[a]) * inv;
        float far = (hi[a] - origin[a]) * inv;
        if (inv < 0.0f) { float tmp = near; near = far; far = tmp; }

        if (near > t0) { t0 = near; entryAxis = a; }
        if (far < t1) t1 = far;
        if (t0 > t1) return false;
    }
    return true;
}

}  // namespace

void VoxelModel::clear() {
    dims_ = {0, 0, 0};
    voxels_.clear();
    palette_.assign(1, VoxelMaterial{});
    solid_ = 0;
    min_ = max_ = IVec3{0, 0, 0};
}

void VoxelModel::resize(IVec3 dims) {
    dims_ = {std::max(0, dims.x), std::max(0, dims.y), std::max(0, dims.z)};
    voxels_.assign(size_t(dims_.x) * size_t(dims_.y) * size_t(dims_.z), kEmpty);
    solid_ = 0;
    min_ = max_ = IVec3{0, 0, 0};
}

void VoxelModel::set(IVec3 v, uint16_t material) {
    if (!inside(v)) return;

    uint16_t& slot = voxels_[index(v)];
    if (slot == material) return;

    if (slot == kEmpty) {
        // First solid voxel starts the box; after that it only grows.
        if (solid_ == 0) {
            min_ = max_ = v;
        } else {
            min_ = minv(min_, v);
            max_ = maxv(max_, v);
        }
        ++solid_;
    } else if (material == kEmpty) {
        --solid_;
        // As with the world, the box is not narrowed on removal: recomputing
        // it every time would be quadratic, and a slightly loose box costs the
        // traversal a couple of empty steps.
    }
    slot = material;
}

uint16_t VoxelModel::addMaterial(const VoxelMaterial& material) {
    for (size_t i = 1; i < palette_.size(); ++i) {
        if (nearlyEqual(palette_[i], material)) return uint16_t(i);
    }
    palette_.push_back(material);
    return uint16_t(palette_.size() - 1);
}

const VoxelMaterial& VoxelModel::material(uint16_t index) const {
    return index < palette_.size() ? palette_[index] : palette_[0];
}

void VoxelModel::trim() {
    if (solid_ == 0) { clear(); return; }

    // The tracked box can be loose after removals, so measure it properly here.
    IVec3 lo{dims_.x, dims_.y, dims_.z};
    IVec3 hi{-1, -1, -1};
    for (int y = 0; y < dims_.y; ++y) {
        for (int z = 0; z < dims_.z; ++z) {
            for (int x = 0; x < dims_.x; ++x) {
                if (voxels_[index({x, y, z})] == kEmpty) continue;
                lo = minv(lo, IVec3{x, y, z});
                hi = maxv(hi, IVec3{x, y, z});
            }
        }
    }

    IVec3 size{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
    if (size == dims_) { min_ = lo; max_ = hi; return; }

    std::vector<uint16_t> moved(size_t(size.x) * size_t(size.y) * size_t(size.z), kEmpty);
    for (int y = 0; y < size.y; ++y) {
        for (int z = 0; z < size.z; ++z) {
            for (int x = 0; x < size.x; ++x) {
                size_t to = (size_t(y) * size_t(size.z) + size_t(z)) * size_t(size.x) + size_t(x);
                moved[to] = voxels_[index({x + lo.x, y + lo.y, z + lo.z})];
            }
        }
    }

    dims_ = size;
    voxels_.swap(moved);
    min_ = IVec3{0, 0, 0};
    max_ = IVec3{size.x - 1, size.y - 1, size.z - 1};
}

bool VoxelModel::trace(Vec3 origin, Vec3 direction, float tMin, float tMax, VoxelHit& hit) const {
    if (solid_ == 0) return false;

    const Vec3 lo{0.0f, 0.0f, 0.0f};
    const Vec3 hi = toVec3(dims_);

    float t0 = 0.0f, t1 = 0.0f;
    int entryAxis = -1;
    if (!clipToBox(origin, direction, lo, hi, tMin, tMax, t0, t1, entryAxis)) return false;

    // Amanatides and Woo, one level: the grid is small enough that there is no
    // emptiness worth a second level to skip over.
    Vec3 point = origin + direction * t0;
    IVec3 cell = floorToInt(point);
    cell = maxv(minv(cell, IVec3{dims_.x - 1, dims_.y - 1, dims_.z - 1}), IVec3{0, 0, 0});

    IVec3 step{0, 0, 0};
    Vec3 tMaxAxis{kInf, kInf, kInf};
    Vec3 tDelta{kInf, kInf, kInf};

    for (int a = 0; a < 3; ++a) {
        float d = direction[a];
        if (d > 0.0f) {
            step[a] = 1;
            tMaxAxis[a] = t0 + (float(cell[a] + 1) - point[a]) / d;
            tDelta[a] = 1.0f / d;
        } else if (d < 0.0f) {
            step[a] = -1;
            tMaxAxis[a] = t0 + (float(cell[a]) - point[a]) / d;
            tDelta[a] = 1.0f / -d;
        }
    }

    float t = t0;
    int axis = entryAxis;

    while (t <= t1) {
        if (!inside(cell)) break;

        uint16_t material = voxels_[index(cell)];
        if (material != kEmpty) {
            // A ray that started inside the grid has no entry plane to shade,
            // so give it something sane rather than an undefined normal.
            int shadeAxis = axis < 0 ? 1 : axis;
            int sign = axis < 0 ? 1 : step[shadeAxis];

            hit.t = t;
            hit.voxel = cell;
            hit.material = material;
            hit.position = origin + direction * t;
            hit.normal = Vec3{0.0f, 0.0f, 0.0f};
            hit.normal[shadeAxis] = float(-sign);
            return true;
        }

        // Step to whichever neighbour the ray reaches first.
        int a = 0;
        if (tMaxAxis.y < tMaxAxis[a]) a = 1;
        if (tMaxAxis.z < tMaxAxis[a]) a = 2;
        if (tMaxAxis[a] > t1) break;

        t = tMaxAxis[a];
        cell[a] += step[a];
        tMaxAxis[a] += tDelta[a];
        axis = a;
    }
    return false;
}

bool VoxelModel::occluded(Vec3 origin, Vec3 direction, float tMin, float tMax) const {
    VoxelHit hit;
    return trace(origin, direction, tMin, tMax, hit);
}

} // namespace blocky
