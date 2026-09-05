#include "engine/edit/sculpt.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace blocky {
namespace edit {

void Sculpt::create(IVec3 dims) {
    model_.resize({std::max(1, dims.x), std::max(1, dims.y), std::max(1, dims.z)});

    // A fresh document with no material selected would accept every tool and
    // place nothing, which reads as a broken editor rather than as an empty
    // palette.
    material_ = model_.addMaterial(VoxelMaterial{});
    history_.clear();
}

void Sculpt::adopt(VoxelModel model) {
    model_ = std::move(model);
    material_ = model_.materialCount() > 0 ? uint16_t(1) : model_.addMaterial(VoxelMaterial{});
    history_.clear();
}

uint32_t Sculpt::index(IVec3 v) const {
    const IVec3 size = model_.dims();
    return (uint32_t(v.y) * uint32_t(size.z) + uint32_t(v.z)) * uint32_t(size.x) + uint32_t(v.x);
}

IVec3 Sculpt::fromIndex(uint32_t at) const {
    const IVec3 size = model_.dims();
    const uint32_t x = at % uint32_t(size.x);
    const uint32_t rest = at / uint32_t(size.x);
    const uint32_t z = rest % uint32_t(size.z);
    const uint32_t y = rest / uint32_t(size.z);
    return {int32_t(x), int32_t(y), int32_t(z)};
}

void Sculpt::putRaw(IVec3 v, uint16_t material) {
    if (!model_.inside(v)) return;

    const uint16_t before = model_.at(v);
    if (before == material) return;

    history_.record(index(v), before, material);
    model_.set(v, material);
}

void Sculpt::put(IVec3 v, uint16_t material) {
    putRaw(v, material);
    if (!mirrorX) return;

    const int mirrored = model_.dims().x - 1 - v.x;
    if (mirrored != v.x) putRaw({mirrored, v.y, v.z}, material);
}

bool Sculpt::openStroke(const char* name) {
    if (history_.recording()) return false;
    history_.begin(name);
    return true;
}

void Sculpt::beginStroke(std::string name) { history_.begin(std::move(name)); }
void Sculpt::endStroke() { history_.commit(); }

void Sculpt::place(IVec3 v, uint16_t material) {
    const bool mine = openStroke("place");
    put(v, material);
    if (mine) history_.commit();
}

void Sculpt::erase(IVec3 v) {
    const bool mine = openStroke("erase");
    put(v, VoxelModel::kEmpty);
    if (mine) history_.commit();
}

void Sculpt::box(IVec3 a, IVec3 b, uint16_t material) {
    const bool mine = openStroke(material == VoxelModel::kEmpty ? "erase box" : "box");

    if (a.x > b.x) std::swap(a.x, b.x);
    if (a.y > b.y) std::swap(a.y, b.y);
    if (a.z > b.z) std::swap(a.z, b.z);

    for (int y = a.y; y <= b.y; ++y)
        for (int z = a.z; z <= b.z; ++z)
            for (int x = a.x; x <= b.x; ++x) put({x, y, z}, material);

    if (mine) history_.commit();
}

void Sculpt::clearAll() {
    const bool mine = openStroke("clear");

    const IVec3 size = model_.dims();
    for (int y = 0; y < size.y; ++y)
        for (int z = 0; z < size.z; ++z)
            for (int x = 0; x < size.x; ++x) putRaw({x, y, z}, VoxelModel::kEmpty);

    if (mine) history_.commit();
}

Pick Sculpt::pick(Vec3 origin, Vec3 direction, float tMax) const {
    Pick result;

    VoxelHit hit;
    if (!model_.trace(origin, direction, 0.0f, tMax, hit)) return result;

    result.hit = true;
    result.voxel = hit.voxel;
    result.normal = hit.normal;
    result.t = hit.t;

    // The face the ray came in through, so the cell in front of it is where
    // a placed voxel goes. The normal is unit and axis aligned, which is
    // what makes this a step of exactly one cell rather than a rounding.
    result.adjacent = {hit.voxel.x + int32_t(std::lround(hit.normal.x)),
                       hit.voxel.y + int32_t(std::lround(hit.normal.y)),
                       hit.voxel.z + int32_t(std::lround(hit.normal.z))};
    return result;
}

void Sculpt::resizeKeeping(IVec3 dims, IVec3 offset) {
    dims = {std::max(1, dims.x), std::max(1, dims.y), std::max(1, dims.z)};

    VoxelModel grown;
    grown.resize(dims);

    // Carry the palette across first, so a voxel keeps the material it had
    // rather than one that happens to land on the same index. Folding makes
    // this exact: identical materials come back as the same slot.
    const size_t materials = model_.materialCount();
    std::vector<uint16_t> remap(materials + 1, VoxelModel::kEmpty);
    for (size_t i = 1; i <= materials; ++i) {
        remap[i] = grown.addMaterial(model_.material(uint16_t(i)));
    }

    const IVec3 size = model_.dims();
    for (int y = 0; y < size.y; ++y) {
        for (int z = 0; z < size.z; ++z) {
            for (int x = 0; x < size.x; ++x) {
                const uint16_t cell = model_.at({x, y, z});
                if (cell == VoxelModel::kEmpty) continue;

                const IVec3 to{x + offset.x, y + offset.y, z + offset.z};
                if (!grown.inside(to)) continue;   // shrinking is allowed to drop what falls out
                grown.set(to, cell < remap.size() ? remap[cell] : VoxelModel::kEmpty);
            }
        }
    }

    // Slot zero is the empty sentinel, not a colour: an eraser stays an
    // eraser across a resize instead of becoming whatever grey slot zero
    // would fold into.
    const bool erasing = material_ == VoxelModel::kEmpty;
    const VoxelMaterial current = model_.material(material_);

    model_ = std::move(grown);
    material_ = erasing ? VoxelModel::kEmpty : model_.addMaterial(current);
    history_.clear();
}

void Sculpt::trimToContents() {
    const bool erasing = material_ == VoxelModel::kEmpty;
    const VoxelMaterial current = model_.material(material_);

    model_.trim();
    material_ = erasing ? VoxelModel::kEmpty : model_.addMaterial(current);
    history_.clear();
}

bool Sculpt::undo() {
    return history_.undo([this](uint32_t at, uint32_t value) {
        model_.set(fromIndex(at), uint16_t(value));
    });
}

bool Sculpt::redo() {
    return history_.redo([this](uint32_t at, uint32_t value) {
        model_.set(fromIndex(at), uint16_t(value));
    });
}

} // namespace edit
} // namespace blocky
