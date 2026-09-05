// Tests for the editor core: undo, the pixel canvas, the voxel document, and
// export.
//
// ------------------------------------------------- what the export test is for
//
// Round-tripping the emitted source through `fromLayers` would prove less
// than it looks, and this project has been burned twice by exactly that --
// two quantisation multipliers in the codec, and a skin face table that swapped
// left for right. The rule is in `docs/architecture.md`: two halves that agree
// with each other prove only that they agree.
//
// Here one half is not ours-today. `voxelize::fromLayers` predates the editor,
// is what hand-written models already use, and is checked by `test_prop`
// against its own march. So it is a fair reader for the text -- an emitter
// that transposed z and x, or numbered its layers downwards, produces a model
// that comes back transposed and the comparison fails.
//
// What that still does not check is whether the text is **valid C++**. An
// unbalanced brace or a `1f` where `1.0f` belongs is a whole class of bug the
// string comparison cannot see. So the emitted file is committed as
// `scenes/common/emitted_probe.hpp` and included right here: MSVC compiles it
// every build, and the compiler is the foreign decoder. Regenerate it with
//
//     build.cmd release run test_edit   (then)   scene_test_edit regenerate
//
// and the byte comparison below is what tells you it is out of date.
//
// Needs no game files: every document here is built in code.
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/edit/canvas.hpp"
#include "engine/edit/emit.hpp"
#include "engine/edit/minecraft.hpp"
#include "engine/edit/sculpt.hpp"
#include "engine/edit/sprite_doc.hpp"
#include "engine/prop/voxel_boxes.hpp"
#include "engine/prop/voxelize.hpp"

#include "scenes/common/emitted_probe.hpp"
#include "scenes/common/emitted_motes.hpp"
#include "scenes/common/emitted_swatch.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

const char* kProbePath = "scenes/common/emitted_probe.hpp";
const char* kSwatchPath = "scenes/common/emitted_swatch.hpp";
const char* kMotesPath = "scenes/common/emitted_motes.hpp";

ImageU8::RGBA rgba(int r, int g, int b, int a = 255) {
    return {uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a)};
}

bool samePixel(ImageU8::RGBA a, ImageU8::RGBA b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool sameImage(const ImageU8& a, const ImageU8& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    return a.storage() == b.storage();
}

// ---------------------------------------------------------------- the probe
//
// Asymmetric on every axis and in every direction, which is the only reason
// it can catch a transposed export. A shape that is a mirror of itself in x
// and z would come back looking right from a writer that swapped them.
VoxelModel buildProbe() {
    VoxelModel model;
    model.resize({5, 4, 3});

    VoxelMaterial stone;
    stone.albedo = srgbToLinear(Vec3{0.42f, 0.44f, 0.47f});

    VoxelMaterial gold;
    gold.albedo = srgbToLinear(Vec3{0.98f, 0.79f, 0.20f});
    gold.roughness = 0.28f;
    gold.metallic = 1.0f;

    // Emissive, because a layer sheet cannot carry this and the source can.
    VoxelMaterial ember;
    ember.albedo = srgbToLinear(Vec3{0.85f, 0.24f, 0.09f});
    ember.emission = Vec3{4.0f, 0.9f, 0.2f};

    const uint16_t s = model.addMaterial(stone);
    const uint16_t g = model.addMaterial(gold);
    const uint16_t e = model.addMaterial(ember);

    for (int x = 0; x < 5; ++x) model.set({x, 0, 0}, s);   // a bar along +X at z = 0
    model.set({0, 1, 0}, s);
    model.set({0, 2, 0}, g);
    model.set({0, 3, 0}, g);                                // a post at the -X end
    model.set({4, 1, 2}, e);                                // one ember in the far corner
    model.set({2, 2, 1}, g);
    return model;
}

// `1f` is not a float literal and `1.0f` is, and the difference is a build
// error in a file this test also compiles -- so the compiler already catches
// it. This catches it *here*, where the message says which literal, instead of
// in a wall of C2059 pointing at generated code.
//
// Comments are skipped, and they have to be: the hex colour the emitter puts
// at the end of each material line is exactly the digit-then-f this looks for.
bool everyFloatLiteralIsWellFormed(const std::string& source) {
    bool ok = true;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            i = source.find('\n', i);
            if (i == std::string::npos) break;
            continue;
        }
        if (source[i] != 'f' || i == 0) continue;
        if (source[i - 1] < '0' || source[i - 1] > '9') continue;

        size_t start = i;
        while (start > 0) {
            const char c = source[start - 1];
            const bool part = (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                              c == '+' || c == '-';
            if (!part) break;
            --start;
        }
        const std::string token = source.substr(start, i - start);
        if (token.find('.') == std::string::npos && token.find('e') == std::string::npos &&
            token.find('E') == std::string::npos) {
            std::printf("        bad literal: %sf\n", token.c_str());
            ok = false;
        }
    }
    return ok;
}

// The canvas probe. Asymmetric in both directions for the same reason the
// model one is asymmetric in three: an emitter that walked the rows the other
// way round would still round-trip a picture that reads the same upside down.
// Two texels are half transparent, because an alpha that is neither nothing
// nor everything is the value a careless writer drops.
ImageU8 buildSwatchProbe() {
    ImageU8 image(6, 4);

    const ImageU8::RGBA amber = rgba(255, 170, 40);
    const ImageU8::RGBA ink = rgba(26, 29, 34);
    const ImageU8::RGBA veil = rgba(120, 200, 255, 128);

    for (int x = 0; x < 6; ++x) image.set(x, 0, amber);   // a bar along the top
    image.set(0, 1, ink);
    image.set(0, 2, ink);                                  // a post down the left
    image.set(2, 2, veil);
    image.set(5, 3, veil);                                 // one corner, far from both
    return image;
}

// Three sprites, no two alike: different positions, sizes, angles, a tint, a
// glow, and one turned single-sided. Every field the emitter can choose to
// leave out has one entry that forces it in.
std::vector<Sprite> buildMoteProbe() {
    std::vector<Sprite> sprites;

    Sprite dust;
    dust.position = {0.25f, 1.5f, -0.75f};
    dust.size = {0.08f, 0.08f};
    dust.tint = srgbToLinear(Vec3{0.85f, 0.82f, 0.70f});
    sprites.push_back(dust);

    Sprite spark;
    spark.position = {-1.25f, 0.5f, 2.0f};
    spark.size = {0.14f, 0.2f};
    spark.yawDegrees = 33.0f;
    spark.pitchDegrees = -12.5f;
    spark.rollDegrees = 7.0f;
    spark.tint = srgbToLinear(Vec3{0.95f, 0.45f, 0.10f});
    spark.emission = Vec3{4.0f, 1.2f, 0.3f};
    sprites.push_back(spark);

    Sprite sign;
    sign.position = {3.0f, 2.25f, 0.0f};
    sign.size = {1.0f, 0.5f};
    sign.doubleSided = false;
    sign.roughness = 0.4f;
    sign.alphaCutoff = 0.25f;
    sign.uvMin = {0.25f, 0.0f};
    sign.uvMax = {0.75f, 1.0f};
    sprites.push_back(sign);

    return sprites;
}

bool sameSprite(const Sprite& a, const Sprite& b) {
    return a.position == b.position && a.size.x == b.size.x && a.size.y == b.size.y &&
           a.yawDegrees == b.yawDegrees && a.pitchDegrees == b.pitchDegrees &&
           a.rollDegrees == b.rollDegrees && a.tint == b.tint && a.emission == b.emission &&
           a.roughness == b.roughness && a.alphaCutoff == b.alphaCutoff &&
           a.doubleSided == b.doubleSided && a.uvMin.x == b.uvMin.x && a.uvMin.y == b.uvMin.y &&
           a.uvMax.x == b.uvMax.x && a.uvMax.y == b.uvMax.y;
}

bool sameMaterial(const VoxelMaterial& a, const VoxelMaterial& b) {
    return a.albedo == b.albedo && a.emission == b.emission && a.roughness == b.roughness &&
           a.metallic == b.metallic;
}

// Compares what the voxels *are*, not which palette slot holds them. Two
// models may number their materials differently and still be the same model.
bool sameModel(const VoxelModel& a, const VoxelModel& b, std::string* where = nullptr) {
    if (!(a.dims() == b.dims())) {
        if (where) *where = "dimensions differ";
        return false;
    }
    const IVec3 dims = a.dims();
    for (int y = 0; y < dims.y; ++y) {
        for (int z = 0; z < dims.z; ++z) {
            for (int x = 0; x < dims.x; ++x) {
                const uint16_t ca = a.at({x, y, z}), cb = b.at({x, y, z});
                const bool emptyA = ca == VoxelModel::kEmpty, emptyB = cb == VoxelModel::kEmpty;
                if (emptyA != emptyB) {
                    if (where) {
                        *where = "occupancy differs at " + std::to_string(x) + "," +
                                 std::to_string(y) + "," + std::to_string(z);
                    }
                    return false;
                }
                if (emptyA) continue;
                if (!sameMaterial(a.material(ca), b.material(cb))) {
                    if (where) {
                        *where = "material differs at " + std::to_string(x) + "," +
                                 std::to_string(y) + "," + std::to_string(z);
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

// ------------------------------------------------------------------- history
void testHistory() {
    std::printf("history\n");

    edit::History history(3, 1000);
    check(!history.canUndo() && !history.canRedo(), "a fresh history has nothing to step to");

    history.begin("one");
    history.record(0, 10, 11);
    check(history.commit(), "a stroke that changed a cell commits");

    history.begin("nothing");
    check(!history.commit(), "a stroke that changed nothing does not");
    check(history.depth() == 1, "and does not become an undo step");

    history.begin("no-op");
    history.record(0, 7, 7);
    check(!history.commit(), "recording a cell as its own value changes nothing");

    // The repeated-cell rule. A brush dragged back over its own track names
    // the same cell twice, and undo has to reach the value from *before* the
    // stroke, not the one from the middle of it.
    uint32_t cell = 11;
    history.begin("drag");
    history.record(0, 11, 20);
    history.record(0, 20, 30);
    history.record(0, 30, 40);
    check(history.commit(), "the drag commits");

    history.undo([&](uint32_t, uint32_t value) { cell = value; });
    check(cell == 11, "undo of a repeated cell restores what it was before the stroke");

    history.redo([&](uint32_t, uint32_t value) { cell = value; });
    check(cell == 40, "and redo puts back what the stroke left");

    check(history.undoName() == "drag", "the step is named");

    // The cap drops the oldest, never the newest.
    edit::History capped(3, 1000);
    for (int i = 0; i < 6; ++i) {
        capped.begin("stroke " + std::to_string(i));
        capped.record(uint32_t(i), 0, uint32_t(i) + 1);
        capped.commit();
    }
    check(capped.depth() == 3, "the stroke cap holds");
    check(capped.undoName() == "stroke 5", "and it is the newest that survives");

    // A new action makes the redo branch unreachable.
    capped.undo([](uint32_t, uint32_t) {});
    check(capped.canRedo(), "an undone stroke can be redone");
    capped.begin("divergence");
    capped.record(99, 0, 1);
    capped.commit();
    check(!capped.canRedo(), "until a new action takes its place");
}

// -------------------------------------------------------------------- canvas
void testCanvas() {
    std::printf("canvas\n");

    edit::Canvas canvas;
    canvas.create(edit::CanvasPreset::BlockTexture);
    check(canvas.width() == 16 && canvas.height() == 16, "a block texture is sixteen across");
    check(canvas.pixel(0, 0).a == 0, "and starts transparent");

    const ImageU8 blank = canvas.image();

    const ImageU8::RGBA red = rgba(200, 40, 30);
    canvas.pencil(3, 4, red);
    check(samePixel(canvas.pixel(3, 4), red), "the pencil paints");
    check(canvas.undo(), "and is one undo step");
    check(sameImage(canvas.image(), blank), "undo restores the canvas exactly");
    check(canvas.redo() && samePixel(canvas.pixel(3, 4), red), "redo puts it back");

    // A drag is one step, however many calls it took.
    canvas.beginStroke("drag");
    for (int i = 0; i < 8; ++i) canvas.pencil(i, 8, red);
    canvas.pencil(4, 8, red);   // back over its own track
    canvas.endStroke();

    const size_t depthAfterDrag = canvas.history().depth();
    canvas.undo();
    check(canvas.history().depth() == depthAfterDrag - 1, "a drag is one step");
    check(canvas.pixel(0, 8).a == 0 && canvas.pixel(7, 8).a == 0, "and undoes whole");
    canvas.redo();

    // Clipping is normal, not an error.
    canvas.pencil(-5, 200, red);
    canvas.line(-4, 2, 20, 2, red);
    check(samePixel(canvas.pixel(0, 2), red) && samePixel(canvas.pixel(15, 2), red),
          "a line that runs off both edges still draws the part that fits");

    // Mirror is about the canvas centre.
    edit::Canvas mirrored;
    mirrored.create(16, 16);
    mirrored.mirrorX = true;
    mirrored.pencil(2, 5, red);
    check(samePixel(mirrored.pixel(2, 5), red) && samePixel(mirrored.pixel(13, 5), red),
          "mirrorX paints the reflected column");
    mirrored.undo();
    check(mirrored.pixel(2, 5).a == 0 && mirrored.pixel(13, 5).a == 0,
          "and both halves undo together");

    // Bucket.
    edit::Canvas flood;
    flood.create(8, 8);
    const ImageU8::RGBA wall = rgba(10, 10, 10);
    for (int y = 0; y < 8; ++y) flood.pencil(4, y, wall);

    const ImageU8::RGBA blue = rgba(40, 90, 200);
    flood.bucket(0, 0, blue);
    check(samePixel(flood.pixel(0, 0), blue) && samePixel(flood.pixel(3, 7), blue),
          "the bucket fills its side");
    check(flood.pixel(5, 0).a == 0, "and does not cross the wall");
    check(samePixel(flood.pixel(4, 0), wall), "nor paint it");

    const size_t before = flood.history().depth();
    flood.bucket(0, 0, blue);
    check(flood.history().depth() == before, "filling with the colour already there does nothing");

    check(flood.undo(), "the fill is one step");
    check(flood.pixel(0, 0).a == 0 && flood.pixel(3, 7).a == 0, "and lifts whole");

    // Through PNG and back, which is the only trip that matters: the file is
    // the deliverable.
    edit::Canvas art;
    art.create(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            art.pencil(x, y, rgba(x * 16, y * 16, (x ^ y) * 8, x == y ? 128 : 255));

    ImageU8 decoded;
    std::string error;
    const std::vector<uint8_t> encoded = pngEncode(art.image());
    check(pngDecode(encoded.data(), encoded.size(), decoded, &error), "the canvas encodes as PNG");
    check(sameImage(decoded, art.image()), "and comes back byte for byte, alpha included");

    // Skin guides come from the layout, not from a copy of it.
    edit::Canvas skin;
    skin.create(edit::CanvasPreset::Skin);
    const std::vector<edit::Guide> guides = skin.guides(false);
    check(!guides.empty(), "a 64x64 canvas has skin guides");

    bool headFound = false;
    for (const edit::Guide& guide : guides) {
        if (guide.label != "head") continue;
        headFound = true;
        // The head's six faces occupy the top-left 32x16 of the sheet.
        check(guide.x == 0 && guide.y == 0 && guide.width == 32 && guide.height == 16,
              "the head guide is where the skin layout puts it");
    }
    check(headFound, "the head is among the guides");
    check(skin.guides(true).size() == guides.size(), "slim has the same parts as classic");

    edit::Canvas texture;
    texture.create(16, 16);
    check(texture.guides().empty(), "a block texture has no layout to guide");
}

// -------------------------------------------------------------------- sculpt
void testSculpt() {
    std::printf("sculpt\n");

    edit::Sculpt sculpt;
    sculpt.create(8);
    check(sculpt.dims() == IVec3(8, 8, 8), "a fresh document is eight a side");
    check(sculpt.material() != VoxelModel::kEmpty, "and has a material selected");

    const uint16_t red = sculpt.addMaterial([] {
        VoxelMaterial m;
        m.albedo = Vec3{0.6f, 0.05f, 0.05f};
        return m;
    }());
    sculpt.setMaterial(red);

    sculpt.place({1, 1, 1});
    check(sculpt.at({1, 1, 1}) == red, "place puts the selected material down");
    check(sculpt.undo() && sculpt.at({1, 1, 1}) == VoxelModel::kEmpty, "and undoes");
    sculpt.redo();

    // Out of bounds is clamped rather than grown, because growing would move
    // every index the undo stack is holding.
    const size_t solid = sculpt.model().solidCount();
    sculpt.place({-1, 0, 0});
    sculpt.place({8, 0, 0});
    check(sculpt.model().solidCount() == solid, "placing outside the grid changes nothing");

    sculpt.box({2, 0, 2}, {4, 0, 4}, red);
    check(sculpt.model().solidCount() == solid + 9, "a box fills its inclusive extent");
    check(sculpt.undo(), "and is one step");
    check(sculpt.model().solidCount() == solid, "that lifts whole");
    sculpt.redo();

    sculpt.box({2, 0, 2}, {4, 0, 4}, VoxelModel::kEmpty);
    check(sculpt.model().solidCount() == solid, "the empty material erases a box");
    sculpt.undo();

    // Picking. A ray down the +X axis into a slab must name the cell it hit
    // and the empty one in front of it.
    edit::Sculpt slab;
    slab.create({8, 8, 8});
    const uint16_t grey = slab.material();
    slab.box({4, 0, 0}, {4, 7, 7}, grey);

    const edit::Pick hit = slab.pick(Vec3{0.5f, 3.5f, 3.5f}, Vec3{1.0f, 0.0f, 0.0f});
    check(hit.hit, "the ray finds the slab");
    check(hit.voxel == IVec3(4, 3, 3), "and names the struck cell");
    check(hit.adjacent == IVec3(3, 3, 3), "the adjacent cell is the one in front of the face");
    check(hit.normal.x < -0.5f, "and the normal points back down the ray");

    const edit::Pick miss = slab.pick(Vec3{0.5f, 3.5f, 3.5f}, Vec3{-1.0f, 0.0f, 0.0f});
    check(!miss.hit, "a ray pointing away hits nothing");

    // The adjacency rule holds for every face, not just the one that is easy
    // to reason about.
    const Vec3 origins[6] = {{-4.0f, 3.5f, 3.5f}, {12.0f, 3.5f, 3.5f}, {4.5f, -4.0f, 3.5f},
                             {4.5f, 12.0f, 3.5f}, {4.5f, 3.5f, -4.0f}, {4.5f, 3.5f, 12.0f}};
    const Vec3 aims[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int face = 0; face < 6; ++face) {
        const edit::Pick p = slab.pick(origins[face], aims[face]);
        if (!p.hit) {
            check(false, "every face of the slab is reachable");
            continue;
        }
        const IVec3 step = p.adjacent - p.voxel;
        const int taxicab = std::abs(step.x) + std::abs(step.y) + std::abs(step.z);
        check(taxicab == 1, "the placement cell is exactly one step off the struck one");
        check(slab.at(p.adjacent) == VoxelModel::kEmpty || !slab.inside(p.adjacent),
              "and the step lands somewhere a voxel can go");
    }

    // Mirror.
    edit::Sculpt symmetric;
    symmetric.create({8, 4, 4});
    symmetric.mirrorX = true;
    symmetric.place({1, 0, 0});
    check(symmetric.at({1, 0, 0}) != VoxelModel::kEmpty &&
              symmetric.at({6, 0, 0}) != VoxelModel::kEmpty,
          "mirrorX places the reflected voxel");
    symmetric.undo();
    check(symmetric.model().solidCount() == 0, "and both undo together");

    // Resize keeps the contents and the colours.
    edit::Sculpt grown;
    grown.create({4, 4, 4});
    VoxelMaterial blue;
    blue.albedo = Vec3{0.1f, 0.2f, 0.9f};
    grown.setMaterial(grown.addMaterial(blue));
    grown.place({0, 0, 0});
    grown.place({3, 3, 3});

    grown.resizeKeeping({8, 8, 8}, {2, 2, 2});
    check(grown.dims() == IVec3(8, 8, 8), "resize changes the size");
    check(grown.model().solidCount() == 2, "and keeps what was there");
    check(grown.at({2, 2, 2}) != VoxelModel::kEmpty && grown.at({5, 5, 5}) != VoxelModel::kEmpty,
          "at the offset asked for");
    check(sameMaterial(grown.model().material(grown.at({2, 2, 2})), blue),
          "carrying the material, not just the slot number");
    check(!grown.history().canUndo(), "and clears the history, whose indices it moved");
}

// -------------------------------------------------------------------- export
void testEmit() {
    std::printf("export\n");

    const VoxelModel probe = buildProbe();

    std::string source, error;
    check(edit::emitSource(probe, source, {"buildEmittedProbe"}, &error),
          "the probe emits as source");

    // The reader is not ours-today: `fromLayers` predates the editor and is
    // checked by test_prop. An emitter that transposed an axis fails here.
    {
        std::string where;
        const VoxelModel parsed = buildEmittedProbe();
        check(sameModel(parsed, probe, &where),
              where.empty() ? "the committed header rebuilds the probe" : where.c_str());
    }

    // And the compiled header must be what the emitter writes today.
    std::vector<uint8_t> committed;
    if (readFileBytes(kProbePath, committed, nullptr)) {
        const std::string onDisk(committed.begin(), committed.end());
        check(onDisk == source,
              "the committed header is byte-identical to what the emitter writes "
              "(run: scene_test_edit regenerate)");
    } else {
        check(false, "the committed header is readable from the repository root");
    }

    // Emission and roughness ride along in the source and cannot in a sheet.
    check(source.find("4.0f") != std::string::npos, "the emitted source carries emission");
    check(source.find(".roughness = 0.28") != std::string::npos,
          "and roughness, to nine digits");
    check(everyFloatLiteralIsWellFormed(source),
          "and never writes a literal C++ would reject");

    // A file name is not an identifier, and the two ways that can go wrong
    // are worth keeping apart: the maker fixes a name, and the emitter
    // refuses one it was handed.
    check(edit::identifierFor("mug") == "buildMug", "a plain name is capitalised");
    check(edit::identifierFor("iron mug") == "buildIronMug", "a space is a word break");
    check(edit::identifierFor("iron-mug") == edit::identifierFor("iron_mug"),
          "and so are a dash and an underscore");
    check(edit::identifierFor("2x4") == "build2x4",
          "a stem starting with a digit is legal behind the prefix");
    check(edit::identifierFor("mug", "make") == "makeMug", "the prefix is the caller's");

    {
        std::string text, why;
        check(!edit::emitSource(probe, text, {"my model"}, &why),
              "a name with a space is refused rather than repaired");
        check(why.find("my model") != std::string::npos, "and the message quotes it");
        check(edit::emitSource(probe, text, {edit::identifierFor("my model")}, &why),
              "and the repaired one is accepted");
    }

    // The charset limit is a real refusal, not a truncation.
    {
        VoxelModel crowded;
        crowded.resize({100, 1, 1});
        for (int x = 0; x < 100; ++x) {
            VoxelMaterial shade;
            shade.albedo = Vec3{float(x) / 100.0f, 0.0f, 0.0f};
            crowded.set({x, 0, 0}, crowded.addMaterial(shade));
        }
        std::string text, why;
        check(!edit::emitSource(crowded, text, {}, &why), "a hundred materials will not fit");
        check(!why.empty(), "and it says so");
    }

    // ------------------------------------------------------ the canvas half
    //
    // Same shape of argument, and the reader is again not ours-today in the
    // sense that matters: `pixelart::fromRows` is written to be usable by
    // hand, and the compiled header below is checked by MSVC either way.
    {
        const ImageU8 swatch = buildSwatchProbe();

        std::string canvasSource, why;
        check(edit::emitCanvasSource(swatch, canvasSource, {"buildEmittedSwatch"}, &why),
              "the canvas emits as source");

        check(sameImage(buildEmittedSwatch(), swatch),
              "the committed header rebuilds the canvas, alpha included");

        std::vector<uint8_t> committedSwatch;
        if (readFileBytes(kSwatchPath, committedSwatch, nullptr)) {
            const std::string onDisk(committedSwatch.begin(), committedSwatch.end());
            check(onDisk == canvasSource,
                  "the committed swatch is byte-identical to what the emitter writes "
                  "(run: scene_test_edit regenerate)");
        } else {
            check(false, "the committed swatch is readable from the repository root");
        }

        check(canvasSource.find("128}}") != std::string::npos,
              "a half-transparent texel keeps its alpha");
        check(canvasSource.find('.') != std::string::npos, "and an empty one is a dot");

        // A photograph is not pixel art, and the refusal says so.
        ImageU8 crowded(16, 16);
        for (int i = 0; i < 256; ++i) {
            crowded.set(i % 16, i / 16, rgba(i, 255 - i, (i * 7) % 256, 255));
        }
        std::string text;
        check(!edit::emitCanvasSource(crowded, text, {"buildCrowded"}, &why),
              "a canvas of 256 colours will not fit the row form");
        check(why.find("PNG") != std::string::npos, "and points at the format that will");

        check(!edit::emitCanvasSource(swatch, text, {"not an identifier"}, &why),
              "and a bad name is refused here too");
    }

    // ------------------------------------------------------ the sprite half
    //
    // No reader to disagree with here: the emitted code assigns the engine's
    // own struct field by field. So the compiler is not a *second* check on
    // top of a round trip, it is the only one there is -- and the comparison
    // has to be field by field rather than "looks right", because a dropped
    // `rollDegrees` is invisible in a picture and fatal in a scene.
    {
        const std::vector<Sprite> motes = buildMoteProbe();

        std::string spriteSource, why;
        check(edit::emitSpriteSource(motes, spriteSource, {"placeEmittedMotes"}, &why),
              "the sprites emit as source");

        const std::vector<Sprite> rebuilt = placeEmittedMotes();
        check(rebuilt.size() == motes.size(), "the committed header rebuilds every sprite");

        bool allSame = rebuilt.size() == motes.size();
        for (size_t i = 0; i < rebuilt.size() && i < motes.size(); ++i) {
            if (!sameSprite(rebuilt[i], motes[i])) {
                std::printf("        sprite %zu differs\n", i);
                allSame = false;
            }
        }
        check(allSame, "field for field, angles and flags included");
        check(rebuilt.empty() || rebuilt[0].texture == nullptr,
              "and the texture is left to the caller");

        std::vector<uint8_t> committedMotes;
        if (readFileBytes(kMotesPath, committedMotes, nullptr)) {
            const std::string onDisk(committedMotes.begin(), committedMotes.end());
            check(onDisk == spriteSource,
                  "the committed motes are byte-identical to what the emitter writes "
                  "(run: scene_test_edit regenerate)");
        } else {
            check(false, "the committed motes are readable from the repository root");
        }

        check(everyFloatLiteralIsWellFormed(spriteSource),
              "and its literals are ones C++ would accept");

        // A field equal to the default is left out, so a line that is there is
        // a decision somebody made rather than noise to read past.
        check(spriteSource.find("alphaCutoff") != std::string::npos,
              "a field that differs from the default is written");
        check(spriteSource.find("roughness = 1.0f") == std::string::npos,
              "and one that does not is not");

        std::string text;
        check(!edit::emitSpriteSource({}, text, {"placeNothing"}, &why),
              "an empty list is refused rather than emitting a function returning nothing");
    }

    // The sheet, and its stated cost.
    const ImageU8 sheet = edit::emitLayerSheet(probe);
    check(sheet.width() == 2 * 5 && sheet.height() == 2 * 3,
          "four slices lay out two by two");

    VoxelModel fromSheet;
    check(edit::readLayerSheet(sheet, probe.dims(), fromSheet, 0, &error),
          "the sheet reads back");
    check(fromSheet.dims() == probe.dims(), "at the same size");

    size_t occupancyMismatch = 0;
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 3; ++z)
            for (int x = 0; x < 5; ++x) {
                const bool a = probe.at({x, y, z}) != VoxelModel::kEmpty;
                const bool b = fromSheet.at({x, y, z}) != VoxelModel::kEmpty;
                if (a != b) ++occupancyMismatch;
            }
    check(occupancyMismatch == 0, "with every voxel where it was");

    // Colour survives to eight bits; the rest does not, and the test says so
    // rather than leaving it to be discovered.
    const uint16_t emberSlot = fromSheet.at({4, 1, 2});
    check(emberSlot != VoxelModel::kEmpty, "the ember voxel is in the sheet");
    check(fromSheet.material(emberSlot).emission == Vec3(0.0f, 0.0f, 0.0f),
          "and a picture cannot carry its glow");

    // A sheet of the wrong size is refused with the size it wanted.
    VoxelModel wrong;
    std::string why;
    check(!edit::readLayerSheet(sheet, {8, 8, 8}, wrong, 0, &why), "a mismatched sheet is refused");
    check(why.find("8x8x8") != std::string::npos, "and says what it was asked for");

    // The name carries the size, and only when it really does.
    check(edit::sheetFileName("mug", {8, 12, 8}) == "mug_8x12x8.png", "the name states the size");

    IVec3 parsed{};
    check(edit::parseSheetFileName("out/mug_8x12x8.png", parsed) && parsed == IVec3(8, 12, 8),
          "and reads back out of a path");
    check(edit::parseSheetFileName("a_b_c_2x3x4.png", parsed) && parsed == IVec3(2, 3, 4),
          "past any number of underscores");
    check(!edit::parseSheetFileName("models_2x2x2/mug.png", parsed),
          "a directory that looks like a size does not answer for the file");
    check(!edit::parseSheetFileName("mug.png", parsed), "a name without a size is refused");
    check(!edit::parseSheetFileName("mug_8x8x8x8.png", parsed),
          "and so is one with too many numbers");
}

bool writeGenerated(const char* path, const std::string& source) {
    std::string error;
    if (!writeFileBytes(path, reinterpret_cast<const uint8_t*>(source.data()), source.size(),
                        &error)) {
        std::printf("could not write %s: %s\n", path, error.c_str());
        return false;
    }
    std::printf("wrote %s (%zu bytes)\n", path, source.size());
    return true;
}

// ------------------------------------------------------------ sprite list
void testSpriteDoc() {
    std::printf("sprites\n");

    edit::SpriteDoc doc;
    check(doc.empty() && !doc.canUndo(), "a fresh list is empty with nothing to undo");

    for (const Sprite& sprite : buildMoteProbe()) doc.add(sprite);
    check(doc.size() == 3, "three sprites go in");
    check(doc.canUndo(), "and each is a step");

    // Snapshots, so undo restores the whole list rather than one field.
    doc.undo();
    check(doc.size() == 2, "undo takes the last one back");
    doc.redo();
    check(doc.size() == 3, "and redo returns it");
    check(sameSprite(doc.sprites()[2], buildMoteProbe()[2]), "unchanged");

    // A new action after an undo makes the redo branch unreachable, same rule
    // as the cell history.
    doc.undo();
    check(doc.canRedo(), "an undone step can be redone");
    doc.add(buildMoteProbe()[0]);
    check(!doc.canRedo(), "until something else takes its place");

    // Picking: a ray straight at a sprite finds it, one past its edge does not.
    edit::SpriteDoc aim;
    Sprite target;
    target.position = {0.0f, 0.0f, 0.0f};
    target.size = {0.5f, 0.5f};
    aim.add(target);

    check(aim.pick({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}) == 0, "a ray down the axis hits it");
    check(aim.pick({0.0f, 1.0f, 5.0f}, {0.0f, 0.0f, -1.0f}) < 0,
          "and one a metre off misses, because the tolerance is its own size");
    check(aim.pick({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}) < 0, "as does one pointing away");

    // Two in a line hand back the near one.
    Sprite behind;
    behind.position = {0.0f, 0.0f, -3.0f};
    behind.size = {0.5f, 0.5f};
    aim.add(behind);
    check(aim.pick({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}) == 0, "the nearer of two wins");

    check(aim.removeAt(0) && aim.size() == 1, "removing takes one out");
    check(!aim.removeAt(9), "and an index past the end is refused rather than crashing");
    aim.undo();
    check(aim.size() == 2, "the removal undoes");
}

// ---------------------------------------------------------------- Minecraft
//
// A confession first, because it decides what these checks are worth: nothing
// here can prove the game accepts the file. The C++ path has MSVC standing in
// as the foreign decoder; the JSON path has no such thing short of Minecraft
// itself. So this proves what it can -- that the text is well formed, that the
// geometry is the model's geometry, and that the UVs point where the palette
// put the colours -- and the rest is stated as untested rather than implied.
bool bracketsBalance(const std::string& json) {
    int curly = 0, square = 0;
    bool inString = false;

    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') ++curly;
        if (c == '}') --curly;
        if (c == '[') ++square;
        if (c == ']') --square;
        if (curly < 0 || square < 0) return false;
    }
    return curly == 0 && square == 0 && !inString;
}

void testMinecraft() {
    std::printf("minecraft\n");

    check(edit::resourceName("Iron Mug") == "iron_mug", "a title becomes a resource name");
    check(edit::resourceName("iron  @@  mug") == "iron_mug", "and runs of rubbish collapse");
    check(edit::resourceName("iron-mug") == "iron-mug", "while a dash is legal and survives");
    check(edit::resourceName("!!!") == "model", "a name of nothing usable falls back");

    // Two layers of different materials, so the shared face is buried on both
    // sides and both must drop it.
    VoxelModel stack;
    stack.resize({1, 2, 1});
    VoxelMaterial red, blue;
    red.albedo = srgbToLinear(Vec3{0.8f, 0.1f, 0.1f});
    blue.albedo = srgbToLinear(Vec3{0.1f, 0.2f, 0.8f});
    stack.set({0, 0, 0}, stack.addMaterial(red));
    stack.set({0, 1, 0}, stack.addMaterial(blue));

    std::string json, why;
    edit::McStats stats;
    check(edit::buildMinecraftModel(stack, json, {}, &stats, &why), "a two-colour stack exports");
    check(bracketsBalance(json), "and the text is balanced");
    check(stats.elements == 2, "one element per material, because a box may not span two");
    check(stats.materials == 2, "and both colours are in the palette");
    check(stats.faces == 10, "the buried face between them is dropped from both sides");

    // A solid cube of one colour is one element with all six faces showing.
    VoxelModel cube;
    cube.resize({4, 4, 4});
    const uint16_t grey = cube.addMaterial(VoxelMaterial{});
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 4; ++z)
            for (int x = 0; x < 4; ++x) cube.set({x, y, z}, grey);

    edit::McStats cubeStats;
    check(edit::buildMinecraftModel(cube, json, {}, &cubeStats, &why), "a solid cube exports");
    check(cubeStats.elements == 1, "and merges to one element");
    check(cubeStats.faces == 6, "with nothing buried");
    check(json.find("\"from\": [0, 0, 0]") != std::string::npos, "placed at the origin");
    check(json.find("\"to\": [4, 4, 4]") != std::string::npos, "one unit per voxel");
    check(json.find("\"uv\": [0, 0, 1, 1]") != std::string::npos,
          "and its one colour reads the first cell of the palette");

    // The elements are a partition of the solid cells, materials included --
    // the property the whole export rests on, checked against the model rather
    // than against the merge that produced them.
    {
        const VoxelModel probe = buildProbe();
        const std::vector<VoxelBox> boxes = mergeVoxelBoxes(probe, BoxMerge::ByMaterial);

        size_t covered = 0;
        bool oneMaterial = true;
        std::vector<uint8_t> hit(size_t(probe.dims().x) * size_t(probe.dims().y) *
                                     size_t(probe.dims().z),
                                 0);
        bool overlapped = false;

        for (const VoxelBox& box : boxes) {
            for (int y = box.min.y; y < box.max.y; ++y)
                for (int z = box.min.z; z < box.max.z; ++z)
                    for (int x = box.min.x; x < box.max.x; ++x) {
                        if (probe.at({x, y, z}) != box.material) oneMaterial = false;
                        const size_t at = (size_t(y) * size_t(probe.dims().z) + size_t(z)) *
                                              size_t(probe.dims().x) + size_t(x);
                        if (hit[at]) overlapped = true;
                        hit[at] = 1;
                        ++covered;
                    }
        }
        check(oneMaterial, "every box holds one material and only that one");
        check(!overlapped, "and no two boxes overlap");
        check(covered == probe.solidCount(), "and together they cover every solid voxel");
    }

    // The palette: one cell per material, and the cell is uniform so the first
    // mip levels stay pure.
    {
        const ImageU8 palette = edit::buildMinecraftPalette(stack);
        check(palette.width() == 64 && palette.height() == 64, "the palette is 64 square");

        const ImageU8::RGBA first = palette.get(0, 0);
        bool uniform = true;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                if (!samePixel(palette.get(x, y), first)) uniform = false;
        check(uniform, "and each colour fills its whole four-texel cell");
        check(!samePixel(palette.get(4, 0), first), "the next colour is the next cell");
    }

    // The refusals.
    VoxelModel huge;
    huge.resize({33, 1, 1});
    huge.set({0, 0, 0}, huge.addMaterial(VoxelMaterial{}));
    check(!edit::buildMinecraftModel(huge, json, {}, nullptr, &why),
          "a grid past 32 a side is refused");
    check(why.find("-16..32") != std::string::npos, "and says what the limit is");

    edit::McExportOptions bad;
    bad.name = "Iron Mug";
    check(!edit::buildMinecraftModel(cube, json, bad, nullptr, &why),
          "a name that is not a resource name is refused");

    // And the whole pack on disk.
    edit::McExportOptions options;
    options.name = "probe";
    const std::string root = "out/forge/pack";
    check(edit::writeMinecraftPack(buildProbe(), root, options, nullptr, &why),
          "the pack writes");
    check(fileExists(root + "/pack.mcmeta"), "with a pack.mcmeta");
    check(fileExists(root + "/assets/forge/models/item/probe.json"), "the model where it belongs");
    check(fileExists(root + "/assets/forge/textures/item/probe.png"), "and the texture beside it");
}

int regenerate() {
    std::string source, error;

    if (!edit::emitSource(buildProbe(), source, {"buildEmittedProbe"}, &error)) {
        std::printf("emit failed: %s\n", error.c_str());
        return 1;
    }
    if (!writeGenerated(kProbePath, source)) return 1;

    if (!edit::emitCanvasSource(buildSwatchProbe(), source, {"buildEmittedSwatch"}, &error)) {
        std::printf("canvas emit failed: %s\n", error.c_str());
        return 1;
    }
    if (!writeGenerated(kSwatchPath, source)) return 1;

    if (!edit::emitSpriteSource(buildMoteProbe(), source, {"placeEmittedMotes"}, &error)) {
        std::printf("sprite emit failed: %s\n", error.c_str());
        return 1;
    }
    if (!writeGenerated(kMotesPath, source)) return 1;

    std::printf("rebuild to compile them\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "regenerate") == 0) return regenerate();

    testHistory();
    testCanvas();
    testSculpt();
    testSpriteDoc();
    testEmit();
    testMinecraft();

    if (gFailures == 0) {
        std::printf("\nall editor tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
