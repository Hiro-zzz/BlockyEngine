#include "engine/physics/collider.hpp"

#include <algorithm>

namespace blocky {
namespace {

// Voxel-space box, half-open: [min, max).
struct VoxelBox {
    IVec3 min{};
    IVec3 max{};

    int cells() const {
        return (max.x - min.x) * (max.y - min.y) * (max.z - min.z);
    }
};

}  // namespace

Collider buildCollider(const VoxelModel& model, float voxelSize) {
    Collider collider;
    if (model.empty() || voxelSize <= 0.0f) return collider;

    IVec3 dims = model.dims();
    std::vector<uint8_t> claimed(size_t(dims.x) * size_t(dims.y) * size_t(dims.z), 0);

    auto flat = [&](int x, int y, int z) {
        return (size_t(y) * size_t(dims.z) + size_t(z)) * size_t(dims.x) + size_t(x);
    };
    auto free = [&](int x, int y, int z) {
        return model.at({x, y, z}) != VoxelModel::kEmpty && !claimed[flat(x, y, z)];
    };

    std::vector<VoxelBox> boxes;

    // Grow along x, then z, then y. The order is arbitrary but must be fixed:
    // a different order gives a different partition, and the tests compare
    // against totals rather than against a specific set of boxes for exactly
    // that reason.
    for (int y = 0; y < dims.y; ++y) {
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                if (!free(x, y, z)) continue;

                int x1 = x;
                while (x1 + 1 < dims.x && free(x1 + 1, y, z)) ++x1;

                int z1 = z;
                while (z1 + 1 < dims.z) {
                    bool wholeRow = true;
                    for (int ix = x; ix <= x1 && wholeRow; ++ix) wholeRow = free(ix, y, z1 + 1);
                    if (!wholeRow) break;
                    ++z1;
                }

                int y1 = y;
                while (y1 + 1 < dims.y) {
                    bool wholeSlab = true;
                    for (int iz = z; iz <= z1 && wholeSlab; ++iz)
                        for (int ix = x; ix <= x1 && wholeSlab; ++ix)
                            wholeSlab = free(ix, y1 + 1, iz);
                    if (!wholeSlab) break;
                    ++y1;
                }

                for (int iy = y; iy <= y1; ++iy)
                    for (int iz = z; iz <= z1; ++iz)
                        for (int ix = x; ix <= x1; ++ix) claimed[flat(ix, iy, iz)] = 1;

                boxes.push_back({{x, y, z}, {x1 + 1, y1 + 1, z1 + 1}});
            }
        }
    }

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
