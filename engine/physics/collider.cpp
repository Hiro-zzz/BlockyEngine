#include "engine/physics/collider.hpp"

#include "engine/prop/voxel_boxes.hpp"

#include <algorithm>

namespace blocky {

Collider buildCollider(const VoxelModel& model, float voxelSize) {
    Collider collider;
    if (model.empty() || voxelSize <= 0.0f) return collider;

    // The walk lives in `prop/voxel_boxes.hpp` because an exported model needs
    // the same one with a different equivalence -- boxes of one material
    // rather than the fewest boxes. `Solid` is this caller's rule: physics has
    // no opinion about colour, and merging across it gives fewer boxes to
    // test.
    const std::vector<VoxelBox> boxes = mergeVoxelBoxes(model, BoxMerge::Solid);

    if (boxes.empty()) return collider;

    // Centre of mass in voxel coordinates, weighted by cell count. Uniform
    // density makes this the centroid of the solid set exactly.
    double totalCells = 0.0;
    Vec3 weighted{};
    for (const VoxelBox& box : boxes) {
        float cells = float(box.cells());
        Vec3 centre = (toVec3(box.min) + toVec3(box.max)) * 0.5f;
        weighted = weighted + centre * cells;
        totalCells += double(cells);
    }
    collider.centreOfMassVoxel = weighted / float(totalCells);

    collider.boxes.reserve(boxes.size());
    Vec3 lo{}, hi{};
    bool first = true;

    for (const VoxelBox& box : boxes) {
        Vec3 centre = (toVec3(box.min) + toVec3(box.max)) * 0.5f;
        Vec3 extent = toVec3(box.max) - toVec3(box.min);

        ColliderBox out;
        out.center = (centre - collider.centreOfMassVoxel) * voxelSize;
        out.halfExtents = extent * (0.5f * voxelSize);
        collider.boxes.push_back(out);

        Vec3 boxLo = out.center - out.halfExtents;
        Vec3 boxHi = out.center + out.halfExtents;
        lo = first ? boxLo : minv(lo, boxLo);
        hi = first ? boxHi : maxv(hi, boxHi);
        first = false;
    }

    collider.boundsMin = lo;
    collider.boundsMax = hi;
    collider.radius = std::max(length(lo), length(hi));
    collider.volume = float(totalCells) * voxelSize * voxelSize * voxelSize;
    return collider;
}

Mat3 inertiaTensor(const Collider& collider, float density, float* massOut) {
    Mat3 inertia{};
    float mass = 0.0f;

    for (const ColliderBox& box : collider.boxes) {
        Vec3 full = box.halfExtents * 2.0f;
        float m = density * full.x * full.y * full.z;
        mass += m;

        // Own inertia about the box centre. The boxes are axis-aligned in body
        // space, so this stays diagonal and no rotation is needed here.
        float ixx = m * (full.y * full.y + full.z * full.z) / 12.0f;
        float iyy = m * (full.x * full.x + full.z * full.z) / 12.0f;
        float izz = m * (full.x * full.x + full.y * full.y) / 12.0f;

        inertia.m[0][0] += ixx;
        inertia.m[1][1] += iyy;
        inertia.m[2][2] += izz;

        // Parallel axis: m * (|r|^2 * I - r (x) r). This is where the
        // off-diagonal terms come from, and why Mat3 needs a general inverse
        // rather than three reciprocals.
        Vec3 r = box.center;
        float r2 = dot(r, r);
        float e[3] = {r.x, r.y, r.z};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float delta = (i == j) ? r2 : 0.0f;
                inertia.m[j][i] += m * (delta - e[i] * e[j]);
            }
        }
    }

    if (massOut) *massOut = mass;
    return inertia;
}

} // namespace blocky
