#include "game/spawnmenu.hpp"

#include "engine/platform/window.hpp"

#include <algorithm>
#include <cmath>

namespace game {

using namespace blocky;

namespace {

// Layout, in pixels. Fixed rather than proportional: a tile has to be big
// enough to read a silhouette in, and that is a size in pixels rather than a
// fraction of however wide somebody dragged the window.
constexpr float kTile = 116.0f;
constexpr float kGap = 12.0f;
constexpr int   kColumns = 4;
constexpr float kPad = 22.0f;
constexpr float kHeaderHeight = 46.0f;

struct Grid {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    int rows = 0;
};

Grid gridFor(int tiles, int width, int height) {
    Grid grid;
    grid.rows = std::max(1, (tiles + kColumns - 1) / kColumns);
    grid.width = kPad * 2.0f + float(kColumns) * kTile + float(kColumns - 1) * kGap;
    grid.height = kHeaderHeight + kPad * 2.0f + float(grid.rows) * kTile +
                  float(grid.rows - 1) * kGap;
    grid.x = std::floor((float(width) - grid.width) * 0.5f);
    grid.y = std::floor((float(height) - grid.height) * 0.5f);
    return grid;
}

void tileOrigin(const Grid& grid, int index, float& x, float& y) {
    const int column = index % kColumns;
    const int row = index / kColumns;
    x = grid.x + kPad + float(column) * (kTile + kGap);
    y = grid.y + kHeaderHeight + kPad + float(row) * (kTile + kGap);
}

Rgba fromLinear(Vec3 linear, float alpha = 1.0f) {
    const Vec3 encoded = linearToSrgb(minv(maxv(linear, Vec3{0.0f}), Vec3{1.0f}));
    return {encoded.x, encoded.y, encoded.z, alpha};
}

// A model drawn as an orthographic front view, one rect per column.
//
// Not a rendered thumbnail. The model is right here and the overlay draws
// rectangles, so the cheapest honest picture of a voxel model is its own
// voxels. Depth comes from one rule -- a voxel with nothing above it is a top
// face and gets brightened -- which is enough to tell a barrel from a crate at
// this size, and is the same trick the block textures use instead of a bevel.
void drawModelPreview(Overlay& overlay, const VoxelModel& model, float boxX, float boxY,
                      float boxSize, float dim) {
    const IVec3 dims = model.dims();
    if (dims.x <= 0 || dims.y <= 0 || dims.z <= 0) return;

    const int span = std::max(dims.x, dims.y);
    const float scale = std::max(1.0f, std::floor(boxSize / float(span)));
    const float drawnW = scale * float(dims.x);
    const float drawnH = scale * float(dims.y);
    const float originX = boxX + std::floor((boxSize - drawnW) * 0.5f);
    const float originY = boxY + std::floor((boxSize - drawnH) * 0.5f);

    for (int x = 0; x < dims.x; ++x) {
        for (int y = 0; y < dims.y; ++y) {
            // Front-most filled voxel in this column.
            int front = -1;
            for (int z = 0; z < dims.z; ++z) {
                if (model.at({x, y, z}) != VoxelModel::kEmpty) { front = z; break; }
            }
            if (front < 0) continue;

            const VoxelMaterial& material = model.material(model.at({x, y, front}));
            const bool lit = y + 1 >= dims.y || model.at({x, y + 1, front}) == VoxelModel::kEmpty;
            const Vec3 albedo = material.albedo * (lit ? 1.28f : 0.82f) * dim +
                                material.emission * 0.22f;

            // Screen y grows downward; the model's does not.
            const float px = originX + scale * float(x);
            const float py = originY + drawnH - scale * float(y + 1);
            overlay.rect(px, py, scale, scale, fromLinear(albedo));
        }
    }
}

// The ragdoll tile has no model to draw -- it is built from the player's skin
// at spawn time -- so it gets a figure: six rects in the shape of a person.
void drawRagdollPreview(Overlay& overlay, float boxX, float boxY, float boxSize, float dim) {
    const float unit = std::max(1.0f, std::floor(boxSize / 12.0f));
    const float cx = boxX + boxSize * 0.5f;
    const float top = boxY + unit * 1.5f;
    const Rgba skin = fromLinear(Vec3{0.62f, 0.44f, 0.32f} * dim);
    const Rgba shirt = fromLinear(Vec3{0.24f, 0.42f, 0.60f} * dim);
    const Rgba legs = fromLinear(Vec3{0.20f, 0.24f, 0.34f} * dim);

    overlay.rect(cx - unit, top, unit * 2.0f, unit * 2.0f, skin);                   // head
    overlay.rect(cx - unit, top + unit * 2.0f, unit * 2.0f, unit * 3.0f, shirt);    // torso
    overlay.rect(cx - unit * 2.0f, top + unit * 2.0f, unit, unit * 3.0f, skin);     // arms
    overlay.rect(cx + unit, top + unit * 2.0f, unit, unit * 3.0f, skin);
    overlay.rect(cx - unit, top + unit * 5.0f, unit * 0.8f, unit * 3.0f, legs);     // legs
    overlay.rect(cx + unit * 0.2f, top + unit * 5.0f, unit * 0.8f, unit * 3.0f, legs);
}

const char* tileName(const std::vector<PropKind>& catalogue, int tile) {
    if (tile >= 0 && tile < int(catalogue.size())) return catalogue[size_t(tile)].name.c_str();
    return "RAGDOLL";
}

} // namespace

SpawnRequest updateSpawnMenu(SpawnMenu& menu, const Window& window,
                             const std::vector<PropKind>& catalogue, int width, int height,
                             float dt) {
    SpawnRequest request;
    if (menu.flashSeconds > 0.0f) menu.flashSeconds = std::max(0.0f, menu.flashSeconds - dt);
    if (!menu.open) return request;

    const int tiles = int(catalogue.size()) + 1;   // the ragdoll is the last one
    const Grid grid = gridFor(tiles, width, height);
    const Vec2 pointer = window.mousePosition();

    menu.hovered = -1;
    for (int i = 0; i < tiles; ++i) {
        float x = 0.0f, y = 0.0f;
        tileOrigin(grid, i, x, y);
        if (pointer.x >= x && pointer.x < x + kTile && pointer.y >= y && pointer.y < y + kTile) {
            menu.hovered = i;
            break;
        }
    }

    if (window.mouseLeftPressed() && menu.hovered >= 0) {
        request.made = true;
        request.tile = menu.hovered;
        menu.flashTile = menu.hovered;
        menu.flashSeconds = 0.35f;
    }
    return request;
}

void drawSpawnMenu(const SpawnMenu& menu, Overlay& overlay,
                   const std::vector<PropKind>& catalogue) {
    if (!menu.open) return;

    const int tiles = int(catalogue.size()) + 1;
    const Grid grid = gridFor(tiles, overlay.width(), overlay.height());

    // The whole screen dims a little. Without it the grid floats over a world
    // that is still moving, and the eye keeps being pulled past it.
    overlay.rect(0.0f, 0.0f, float(overlay.width()), float(overlay.height()),
                 rgba(0.0f, 0.0f, 0.0f, 0.35f));

    overlay.verticalGradient(grid.x, grid.y, grid.width, grid.height,
                             rgb8(30, 33, 38, 0.96f), rgb8(20, 22, 26, 0.96f));
    overlay.frame(grid.x, grid.y, grid.width, grid.height, 2.0f, rgb8(255, 170, 40, 0.85f));

    overlay.textShadowed(grid.x + kPad, grid.y + 14.0f, "SPAWN", 2.0f, rgb8(255, 170, 40));
    const std::string close = "Q OR ESC TO CLOSE";
    overlay.text(grid.x + grid.width - kPad - overlay.measure(close, 1.0f), grid.y + 20.0f, close,
                 1.0f, rgb8(150, 156, 166));

    for (int i = 0; i < tiles; ++i) {
        float x = 0.0f, y = 0.0f;
        tileOrigin(grid, i, x, y);

        const bool hovered = i == menu.hovered;
        const bool flashing = i == menu.flashTile && menu.flashSeconds > 0.0f;

        overlay.rect(x, y, kTile, kTile,
                     hovered ? rgb8(52, 58, 68, 0.95f) : rgb8(38, 42, 49, 0.95f));
        if (flashing) {
            const float alpha = 0.55f * (menu.flashSeconds / 0.35f);
            overlay.rect(x, y, kTile, kTile, rgba(1.0f, 0.67f, 0.16f, alpha));
        }
        overlay.frame(x, y, kTile, kTile, 1.0f,
                      hovered ? rgb8(255, 170, 40, 0.9f) : rgb8(64, 70, 80, 0.9f));

        const float dim = hovered ? 1.0f : 0.86f;
        const float boxSize = kTile - 34.0f;
        if (i < int(catalogue.size())) {
            drawModelPreview(overlay, catalogue[size_t(i)].model, x + 6.0f, y + 6.0f, boxSize, dim);
        } else {
            drawRagdollPreview(overlay, x + 6.0f, y + 6.0f, boxSize, dim);
        }

        const std::string name = tileName(catalogue, i);
        overlay.text(x + std::floor((kTile - overlay.measure(name, 1.0f)) * 0.5f),
                     y + kTile - 18.0f, name, 1.0f,
                     hovered ? rgb8(255, 220, 170) : rgb8(178, 184, 194));
    }
}

} // namespace game
