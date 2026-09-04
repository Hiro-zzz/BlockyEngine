#include "engine/assets/entity/skin_gen.hpp"

#include "engine/core/png.hpp"

#include <algorithm>
#include <vector>

namespace blocky::skingen {

bool begin(Skin& skin, ImageU8& sheet, std::string* error) {
    // A blank opaque sheet is a valid skin of the right size, which is all
    // that is needed to ask it where every rectangle lives.
    ImageU8 blank(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) blank.set(x, y, {128, 128, 128, 255});

    // In through `loadFromPng` because that is the only door into a Skin, so a
    // drawn character takes exactly the path a real file takes -- including the
    // classic/slim detection. A second entry point would be a second thing to
    // keep in step.
    std::vector<uint8_t> png = pngEncode(blank);
    if (!skin.loadFromPng(png.data(), png.size(), error)) return false;

    // Painting starts from nothing rather than from the blank: an untouched
    // texel must end up transparent, which is how an outer layer that nobody
    // drew on gets cut away entirely instead of hanging around as a grey shell.
    sheet = ImageU8(64, 64);
    return true;
}

bool finish(Skin& skin, const ImageU8& sheet) { return skin.replaceImage(sheet); }

SkinRect topRows(const SkinRect& rect, int rows) {
    if (!rect.valid() || rows <= 0) return {};
    return {rect.x, rect.y, rect.width, std::min(rows, rect.height)};
}

SkinRect bottomRows(const SkinRect& rect, int rows) {
    if (!rect.valid() || rows <= 0) return {};
    rows = std::min(rows, rect.height);
    return {rect.x, rect.y + rect.height - rows, rect.width, rows};
}

void fill(ImageU8& sheet, const SkinRect& rect, RGBA colour) {
    if (!rect.valid()) return;
    for (int y = 0; y < rect.height; ++y)
        for (int x = 0; x < rect.width; ++x) sheet.set(rect.x + x, rect.y + y, colour);
}

void scatter(ImageU8& sheet, const SkinRect& rect, int amount) {
    if (!rect.valid() || amount <= 0) return;
    for (int y = 0; y < rect.height; ++y) {
        for (int x = 0; x < rect.width; ++x) {
            int px = rect.x + x, py = rect.y + y;
            uint32_t h = uint32_t(px) * 73856093u ^ uint32_t(py) * 19349663u;
            h = h * 2654435761u;
            int delta = int((h >> 16) % uint32_t(2 * amount + 1)) - amount;

            RGBA c = sheet.get(px, py);
            if (c.a == 0) continue;
            auto bump = [delta](uint8_t v) { return uint8_t(std::clamp(int(v) + delta, 0, 255)); };
            sheet.set(px, py, {bump(c.r), bump(c.g), bump(c.b), c.a});
        }
    }
}

void part(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int noise) {
    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinRect rect = layout.faceRect(which, LayerBase, SkinFace(face));
        fill(sheet, rect, colour);
        scatter(sheet, rect, noise);
    }
}

void limbEnd(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int rows) {
    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinFace f = SkinFace(face);
        SkinRect rect = layout.faceRect(which, LayerBase, f);
        if (!rect.valid()) continue;
        if (f == SkinFaceTop) continue;
        fill(sheet, f == SkinFaceBottom ? rect : bottomRows(rect, rows), colour);
    }
}

void cap(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int rows,
         bool includeTop) {
    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinFace f = SkinFace(face);
        SkinRect rect = layout.faceRect(which, LayerBase, f);
        if (!rect.valid()) continue;
        if (f == SkinFaceBottom) continue;
        if (f == SkinFaceTop) {
            if (includeTop) fill(sheet, rect, colour);
            continue;
        }
        fill(sheet, topRows(rect, rows), colour);
    }
}

void band(ImageU8& sheet, const Skin& layout, SkinPart which, RGBA colour, int fromTop, int rows) {
    if (rows <= 0) return;
    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinFace f = SkinFace(face);
        if (f == SkinFaceTop || f == SkinFaceBottom) continue;

        SkinRect rect = layout.faceRect(which, LayerBase, f);
        if (!rect.valid() || fromTop >= rect.height) continue;

        fill(sheet, {rect.x, rect.y + fromTop, rect.width,
                     std::min(rows, rect.height - fromTop)}, colour);
    }
}

void faceFeatures(ImageU8& sheet, const SkinRect& front, RGBA white, RGBA iris, RGBA mouth) {
    if (front.width < 8 || front.height < 8) return;

    auto put = [&](int x, int y, RGBA c) { sheet.set(front.x + x, front.y + y, c); };

    // Two rows, and **mirrored**, because both are what `face::scanFace` looks
    // for: a symmetric pair of texels standing out from what sits directly
    // above and below them.
    //
    // The version this replaced painted each eye left to right -- white then
    // iris, twice -- so the white of one eye sat opposite the iris of the
    // other and the pair was not a mirror at all. The scan rejected every skin
    // drawn this way, which is why the game's own fallback player has been
    // reporting "no candidate looked enough like an eye" since it was written,
    // and why two scenes whose whole subject is the eye rig could not use a
    // drawn character. A real skin puts the white on the outside of both eyes,
    // which is a mirror; so does this now.
    for (int row = 3; row <= 4; ++row) {
        put(1, row, white);   // the entity's own right eye, in the left half
        put(2, row, iris);    // of the rectangle -- a skin stores each face
        put(5, row, iris);    // as a viewer sees it
        put(6, row, white);
    }
    for (int x = 2; x < 6; ++x) put(x, 6, mouth);
}

} // namespace blocky::skingen
