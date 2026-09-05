#include "engine/prop/voxel_boxes.hpp"

namespace blocky {

std::vector<VoxelBox> mergeVoxelBoxes(const VoxelModel& model, BoxMerge rule) {
    std::vector<VoxelBox> boxes;
    if (model.empty()) return boxes;

    const IVec3 dims = model.dims();
    std::vector<uint8_t> claimed(size_t(dims.x) * size_t(dims.y) * size_t(dims.z), 0);

    auto flat = [&](int x, int y, int z) {
        return (size_t(y) * size_t(dims.z) + size_t(z)) * size_t(dims.x) + size_t(x);
    };

    // Free to join the box being grown around `seed`. Under `Solid` the seed
    // is ignored, which is what makes that rule give the fewest boxes.
    auto free = [&](int x, int y, int z, uint16_t seed) {
        const uint16_t cell = model.at({x, y, z});
        if (cell == VoxelModel::kEmpty || claimed[flat(x, y, z)]) return false;
        return rule == BoxMerge::Solid || cell == seed;
    };

    // Grow along x, then z, then y. The order is arbitrary but must be fixed:
    // a different order gives a different partition, and the tests compare
    // against totals rather than against a specific set of boxes for exactly
    // that reason.
    for (int y = 0; y < dims.y; ++y) {
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                const uint16_t seed = model.at({x, y, z});
                if (!free(x, y, z, seed)) continue;

                int x1 = x;
                while (x1 + 1 < dims.x && free(x1 + 1, y, z, seed)) ++x1;

                int z1 = z;
                while (z1 + 1 < dims.z) {
                    bool wholeRow = true;
                    for (int ix = x; ix <= x1 && wholeRow; ++ix) wholeRow = free(ix, y, z1 + 1, seed);
                    if (!wholeRow) break;
                    ++z1;
                }

                int y1 = y;
                while (y1 + 1 < dims.y) {
                    bool wholeSlab = true;
                    for (int iz = z; iz <= z1 && wholeSlab; ++iz)
                        for (int ix = x; ix <= x1 && wholeSlab; ++ix)
                            wholeSlab = free(ix, y1 + 1, iz, seed);
                    if (!wholeSlab) break;
                    ++y1;
                }

                for (int iy = y; iy <= y1; ++iy)
                    for (int iz = z; iz <= z1; ++iz)
                        for (int ix = x; ix <= x1; ++ix) claimed[flat(ix, iy, iz)] = 1;

                VoxelBox box;
                box.min = {x, y, z};
                box.max = {x1 + 1, y1 + 1, z1 + 1};
                box.material = seed;
                boxes.push_back(box);
            }
        }
    }
    return boxes;
}

} // namespace blocky
