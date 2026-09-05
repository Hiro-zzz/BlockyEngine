#include "engine/edit/canvas.hpp"

#include "engine/assets/entity/skin.hpp"
#include "engine/core/png.hpp"

#include <algorithm>
#include <utility>

namespace blocky {
namespace edit {

void Canvas::create(int width, int height) {
    image_.resize(std::max(1, width), std::max(1, height));
    history_.clear();
}

void Canvas::create(CanvasPreset preset) {
    switch (preset) {
        case CanvasPreset::BlockTexture:
        case CanvasPreset::ItemTexture: create(16, 16); break;
        case CanvasPreset::Skin:        create(64, 64); break;
    }
}

bool Canvas::load(const std::string& path, std::string* error) {
    ImageU8 loaded;
    if (!pngLoad(path, loaded, error)) return false;
    image_ = std::move(loaded);
    history_.clear();   // the cells it names are gone
    return true;
}

bool Canvas::save(const std::string& path, std::string* error) const {
    return pngSave(path, image_, error);
}

ImageU8::RGBA Canvas::pixel(int x, int y) const {
    if (!inside(x, y)) return {0, 0, 0, 0};
    return image_.get(x, y);
}

uint32_t Canvas::pack(ImageU8::RGBA c) {
    return uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16) | (uint32_t(c.a) << 24);
}

ImageU8::RGBA Canvas::unpack(uint32_t value) {
    return {uint8_t(value & 0xFFu), uint8_t((value >> 8) & 0xFFu),
            uint8_t((value >> 16) & 0xFFu), uint8_t((value >> 24) & 0xFFu)};
}

void Canvas::putRaw(int x, int y, ImageU8::RGBA colour) {
    if (!inside(x, y)) return;

    const uint32_t before = pack(image_.get(x, y));
    const uint32_t after = pack(colour);
    if (before == after) return;

    history_.record(index(x, y), before, after);
    image_.set(x, y, colour);
}

void Canvas::put(int x, int y, ImageU8::RGBA colour) {
    putRaw(x, y, colour);
    if (!mirrorX) return;

    const int mirrored = image_.width() - 1 - x;
    if (mirrored != x) putRaw(mirrored, y, colour);
}

bool Canvas::openStroke(const char* name) {
    if (history_.recording()) return false;
    history_.begin(name);
    return true;
}

void Canvas::beginStroke(std::string name) { history_.begin(std::move(name)); }
void Canvas::endStroke() { history_.commit(); }

void Canvas::pencil(int x, int y, ImageU8::RGBA colour) {
    const bool mine = openStroke("pencil");
    put(x, y, colour);
    if (mine) history_.commit();
}

void Canvas::line(int x0, int y0, int x1, int y1, ImageU8::RGBA colour) {
    const bool mine = openStroke("line");

    // Bresenham, integer throughout. A line drawn in floats and rounded
    // lands on a different set of texels depending on which end it started
    // from, and at sixteen pixels across that is a visible difference.
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put(x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }

    if (mine) history_.commit();
}

void Canvas::rectFill(int x0, int y0, int x1, int y1, ImageU8::RGBA colour) {
    const bool mine = openStroke("rectangle");

    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) put(x, y, colour);

    if (mine) history_.commit();
}

void Canvas::rectOutline(int x0, int y0, int x1, int y1, ImageU8::RGBA colour) {
    const bool mine = openStroke("outline");

    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    for (int x = x0; x <= x1; ++x) { put(x, y0, colour); put(x, y1, colour); }
    for (int y = y0; y <= y1; ++y) { put(x0, y, colour); put(x1, y, colour); }

    if (mine) history_.commit();
}

void Canvas::fillAll(ImageU8::RGBA colour) {
    const bool mine = openStroke("fill canvas");

    for (int y = 0; y < image_.height(); ++y)
        for (int x = 0; x < image_.width(); ++x) putRaw(x, y, colour);

    if (mine) history_.commit();
}

void Canvas::bucket(int x, int y, ImageU8::RGBA colour) {
    if (!inside(x, y)) return;

    const uint32_t seed = pack(image_.get(x, y));
    const uint32_t wanted = pack(colour);

    // Without this the walk never ends: the test for "still the seed colour"
    // would stay true after painting, and every cell would queue its
    // neighbours again for ever.
    if (seed == wanted) return;

    const bool mine = openStroke("bucket");

    // An explicit stack rather than recursion. A canvas of one colour is
    // four thousand cells deep at 64x64, which is a stack overflow on a
    // thread that has a window to service.
    std::vector<std::pair<int, int>> stack;
    stack.push_back(std::make_pair(x, y));

    while (!stack.empty()) {
        const std::pair<int, int> cell = stack.back();
        stack.pop_back();

        const int px = cell.first, py = cell.second;
        if (!inside(px, py)) continue;
        if (pack(image_.get(px, py)) != seed) continue;

        // Straight to putRaw. The flood already reaches every cell it
        // should, and mirroring each one would paint the reflection of a
        // region that was never flooded.
        putRaw(px, py, colour);

        stack.push_back(std::make_pair(px + 1, py));
        stack.push_back(std::make_pair(px - 1, py));
        stack.push_back(std::make_pair(px, py + 1));
        stack.push_back(std::make_pair(px, py - 1));
    }

    if (mine) history_.commit();
}

bool Canvas::undo() {
    return history_.undo([this](uint32_t at, uint32_t value) {
        const int x = int(at % uint32_t(image_.width()));
        const int y = int(at / uint32_t(image_.width()));
        image_.set(x, y, unpack(value));
    });
}

bool Canvas::redo() {
    return history_.redo([this](uint32_t at, uint32_t value) {
        const int x = int(at % uint32_t(image_.width()));
        const int y = int(at / uint32_t(image_.width()));
        image_.set(x, y, unpack(value));
    });
}

std::vector<Guide> Canvas::guides(bool slimArms) const {
    std::vector<Guide> guides;
    if (image_.width() != 64 || image_.height() != 64) return guides;

    // Ask the real layout rather than restating it. `Skin` owns the table
    // that says where a part's faces are, and a second copy of it here would
    // be a second chance to swap left and right -- the exact mistake
    // docs/conventions.md records as having cost a real bug.
    const ImageU8 blank(64, 64);
    const std::vector<uint8_t> encoded = pngEncode(blank);

    Skin layout;
    if (!layout.loadFromPng(encoded.data(), encoded.size(), nullptr)) return guides;

    // Every column of a blank sheet is transparent, so the automatic
    // slim/classic test has nothing to read. Say which one this is.
    layout.setModel(slimArms ? SkinModel::Slim : SkinModel::Classic);

    for (int part = 0; part < PartCount; ++part) {
        for (int layer = 0; layer < LayerCount; ++layer) {
            const SkinPart which = SkinPart(part);
            const SkinLayer band = SkinLayer(layer);
            if (!layout.hasLayer(which, band)) continue;

            int minX = 64, minY = 64, maxX = 0, maxY = 0;
            bool any = false;
            for (int face = 0; face < SkinFaceCount; ++face) {
                const SkinRect rect = layout.faceRect(which, band, SkinFace(face));
                if (!rect.valid()) continue;
                any = true;
                minX = std::min(minX, rect.x);
                minY = std::min(minY, rect.y);
                maxX = std::max(maxX, rect.x + rect.width);
                maxY = std::max(maxY, rect.y + rect.height);
            }
            if (!any) continue;

            Guide guide;
            guide.x = minX;
            guide.y = minY;
            guide.width = maxX - minX;
            guide.height = maxY - minY;
            guide.label = Skin::partName(which);
            if (band == LayerOuter) guide.label += " (outer)";
            guides.push_back(guide);
        }
    }
    return guides;
}

} // namespace edit
} // namespace blocky
