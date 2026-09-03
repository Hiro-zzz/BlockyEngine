// Correctness tests for the entity system: box intersection, the transform
// chain, skin UV mapping and alpha cutout.
//
// Runs on a synthetic skin painted with a different colour on every face, so
// it needs no game files and a wrong face mapping shows up as a wrong colour
// rather than as a subtly odd render.
#include "engine/core/png.hpp"
#include "engine/entity/entity.hpp"

#include <cstdio>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

bool nearly(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

bool colorsMatch(Vec3 got, Vec3 expected) {
    return nearly(got.x, expected.x, 2e-3f) && nearly(got.y, expected.y, 2e-3f) &&
           nearly(got.z, expected.z, 2e-3f);
}

// A distinct sRGB colour per face, chosen so no two are close.
ImageU8::RGBA faceColor(SkinFace face) {
    switch (face) {
        case SkinFaceLeft:   return {220,  30,  30, 255};  // red
        case SkinFaceRight:  return { 30, 200,  60, 255};  // green
        case SkinFaceBottom: return { 40,  70, 230, 255};  // blue
        case SkinFaceTop:    return {240, 210,  40, 255};  // yellow
        case SkinFaceFront:  return {230, 120, 200, 255};  // pink
        default:             return { 60, 210, 220, 255};  // cyan
    }
}

Vec3 expectedLinear(SkinFace face) {
    ImageU8::RGBA c = faceColor(face);
    return srgbToLinear(Vec3{float(c.r) / 255.0f, float(c.g) / 255.0f, float(c.b) / 255.0f});
}

// Build a 64x64 skin where the head's six base rectangles are flat colours
// and every outer layer is fully transparent.
Skin makeTestSkin(bool opaqueHat) {
    ImageU8 image(64, 64);
    // Start fully transparent so untouched regions are obviously blank.
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) image.set(x, y, {0, 0, 0, 0});
    }

    Skin layout;  // used only for its rectangle table
    {
        ImageU8 blank(64, 64);
        std::vector<uint8_t> bytes = pngEncode(blank);
        layout.loadFromPng(bytes.data(), bytes.size(), nullptr);
    }

    for (int face = 0; face < SkinFaceCount; ++face) {
        SkinRect base = layout.faceRect(PartHead, LayerBase, SkinFace(face));
        for (int y = 0; y < base.height; ++y) {
            for (int x = 0; x < base.width; ++x) {
                image.set(base.x + x, base.y + y, faceColor(SkinFace(face)));
            }
        }
        if (opaqueHat) {
            SkinRect outer = layout.faceRect(PartHead, LayerOuter, SkinFace(face));
            for (int y = 0; y < outer.height; ++y) {
                for (int x = 0; x < outer.width; ++x) {
                    image.set(outer.x + x, outer.y + y, {10, 10, 10, 255});  // near-black hat
                }
            }
        }
    }

    // Give the body a flat opaque colour so the arm and leg tests have
    // something solid to sit against.
    for (int face = 0; face < SkinFaceCount; ++face) {
        for (SkinPart part : {PartBody, PartRightArm, PartLeftArm, PartRightLeg, PartLeftLeg}) {
            SkinRect r = layout.faceRect(part, LayerBase, SkinFace(face));
            for (int y = 0; y < r.height; ++y) {
                for (int x = 0; x < r.width; ++x) image.set(r.x + x, r.y + y, {128, 128, 128, 255});
            }
        }
    }

    std::vector<uint8_t> bytes = pngEncode(image);

    Skin skin;
    std::string error;
    if (!skin.loadFromPng(bytes.data(), bytes.size(), &error)) {
        std::printf("  FAIL  synthetic skin did not load: %s\n", error.c_str());
        ++gFailures;
    }
    return skin;
}

// A model consisting of the head box alone, so every face is reachable.
EntityModel headOnlyModel(const Skin& skin) {
    EntityModel full = buildPlayerModel(skin);
    EntityModel head;
    head.name = "head_only";
    head.heightBlocks = 2.0f;
    for (const ModelBox& box : full.boxes) {
        if (box.part == PartHead) head.boxes.push_back(box);
    }
    return head;
}

// --------------------------------------------------------- face identity
void testHeadFaces() {
    std::printf("head box: every face reports its own colour and normal\n");

    Skin skin = makeTestSkin(/*opaqueHat=*/false);
    EntityModel model = headOnlyModel(skin);

    Entity entity;
    entity.model = &model;
    entity.skin = &skin;
    entity.position = {0.0f, 0.0f, 0.0f};

    EntitySet entities;
    entities.add(entity);

    // The head spans pixels y 24..32, x and z -4..4, so in blocks it is the
    // cube from (-0.25, 1.5, -0.25) to (0.25, 2.0, 0.25).
    const Vec3 center{0.0f, 1.75f, 0.0f};

    struct Case { SkinFace face; Vec3 normal; };
    const Case cases[] = {
        {SkinFaceLeft,   {-1.0f, 0.0f, 0.0f}},
        {SkinFaceRight,  { 1.0f, 0.0f, 0.0f}},
        {SkinFaceBottom, { 0.0f,-1.0f, 0.0f}},
        {SkinFaceTop,    { 0.0f, 1.0f, 0.0f}},
        {SkinFaceFront,  { 0.0f, 0.0f,-1.0f}},
        {SkinFaceBack,   { 0.0f, 0.0f, 1.0f}},
    };

    for (const Case& c : cases) {
        Vec3 origin = center + c.normal * 3.0f;
        Ray ray{origin, -c.normal};

        EntityHit hit;
        if (!entities.intersect(ray, 100.0f, hit)) {
            std::printf("  FAIL  %s face was not hit\n", Skin::faceName(c.face));
            ++gFailures;
            continue;
        }

        // The face plane is a quarter of a block from the centre.
        check(nearly(hit.t, 3.0f - 0.25f, 2e-3f), Skin::faceName(c.face));
        check(colorsMatch(hit.normal, c.normal), Skin::faceName(c.face));
        if (!colorsMatch(hit.albedo, expectedLinear(c.face))) {
            std::printf("  FAIL  %s face has the wrong colour: got (%.3f %.3f %.3f)\n",
                        Skin::faceName(c.face), hit.albedo.x, hit.albedo.y, hit.albedo.z);
            ++gFailures;
        }
    }
}

// ------------------------------------------------------------ yaw and pose
void testYawRotatesTheModel() {
    std::printf("yaw: turning the entity moves its front face\n");

    Skin skin = makeTestSkin(false);
    EntityModel model = headOnlyModel(skin);

    Entity entity;
    entity.model = &model;
    entity.skin = &skin;
    entity.yawDegrees = 90.0f;

    EntitySet entities;
    entities.add(entity);

    const Vec3 center{0.0f, 1.75f, 0.0f};

    // A yaw of +90 degrees about +Y turns -Z towards -X, so the front face
    // should now be the one facing -X.
    EntityHit hit;
    check(entities.intersect({center + Vec3{-3.0f, 0.0f, 0.0f}, {1, 0, 0}}, 100.0f, hit),
          "the turned head is hit from -X");
    if (hit.entityIndex >= 0) {
        check(colorsMatch(hit.albedo, expectedLinear(SkinFaceFront)),
              "after a 90 degree yaw the front face points at -X");
    }

    // And the original front direction now shows a side face, not the front.
    EntityHit other;
    check(entities.intersect({center + Vec3{0.0f, 0.0f, -3.0f}, {0, 0, 1}}, 100.0f, other),
          "the turned head is still hit from -Z");
    if (other.entityIndex >= 0) {
        check(!colorsMatch(other.albedo, expectedLinear(SkinFaceFront)),
              "the front face no longer faces -Z");
    }
}

void testPoseRotatesOnePart() {
    std::printf("pose: rotating the head does not move the body\n");

    Skin skin = makeTestSkin(false);
    EntityModel model = buildPlayerModel(skin);

    Entity turned;
    turned.model = &model;
    turned.skin = &skin;
    turned.pose[joint::Head].rotationDegrees = {0.0f, 90.0f, 0.0f};

    EntitySet entities;
    entities.add(turned);

    // Head front now faces -X.
    EntityHit hit;
    check(entities.intersect({{-3.0f, 1.75f, 0.0f}, {1, 0, 0}}, 100.0f, hit),
          "the head is hit from -X after being turned");
    if (hit.entityIndex >= 0) {
        check(hit.part == PartHead, "the -X ray at head height hits the head");
        check(colorsMatch(hit.albedo, expectedLinear(SkinFaceFront)),
              "the turned head shows its front face at -X");
    }

    // The torso is unaffected: at chest height the front is still at -Z.
    EntityHit chest;
    check(entities.intersect({{0.0f, 1.1f, -3.0f}, {0, 0, 1}}, 100.0f, chest),
          "the chest is hit from -Z");
    if (chest.entityIndex >= 0) check(chest.part == PartBody, "the chest ray hits the body");
}

// ---------------------------------------------------------------- cutout
void testOuterLayerCutout() {
    std::printf("cutout: a transparent hat is seen through, an opaque one is not\n");

    {
        Skin skin = makeTestSkin(/*opaqueHat=*/false);
        EntityModel model = headOnlyModel(skin);
        Entity entity;
        entity.model = &model;
        entity.skin = &skin;

        EntitySet entities;
        entities.add(entity);

        EntityHit hit;
        check(entities.intersect({{0.0f, 1.75f, -3.0f}, {0, 0, 1}}, 100.0f, hit),
              "transparent hat: the head is still hit");
        check(hit.layer == LayerBase, "transparent hat: the base layer is what gets hit");
        check(colorsMatch(hit.albedo, expectedLinear(SkinFaceFront)),
              "transparent hat: the base colour comes through");
    }
    {
        Skin skin = makeTestSkin(/*opaqueHat=*/true);
        EntityModel model = headOnlyModel(skin);
        Entity entity;
        entity.model = &model;
        entity.skin = &skin;

        EntitySet entities;
        entities.add(entity);

        EntityHit hit;
        check(entities.intersect({{0.0f, 1.75f, -3.0f}, {0, 0, 1}}, 100.0f, hit),
              "opaque hat: something is hit");
        check(hit.layer == LayerOuter, "opaque hat: the outer layer wins");
        // The hat stands half a pixel proud of the head.
        check(hit.t < 3.0f - 0.25f, "opaque hat: the hit is in front of the base box");
    }
}

// ------------------------------------------------------------ model layout
void testPlayerModelLayout() {
    std::printf("player model: proportions and handedness\n");

    Skin skin = makeTestSkin(false);
    EntityModel model = buildPlayerModel(skin);

    check(model.boxes.size() == 12, "twelve boxes: six parts, two layers each");

    Entity entity;
    entity.model = &model;
    entity.skin = &skin;
    entity.position = {0.0f, 0.0f, 0.0f};

    EntitySet entities;
    entities.add(entity);

    Vec3 lo, hi;
    check(entities.bounds(lo, hi), "the set has bounds");
    // Two blocks tall, feet on the ground. The hat adds half a pixel.
    check(nearly(lo.y, 0.0f, 0.02f), "the feet rest on y = 0");
    check(nearly(hi.y, 2.0f, 0.05f), "the model is two blocks tall");

    // A ray from +X at arm height must reach the right arm, since the
    // entity's right hand is at +X.
    EntityHit hit;
    check(entities.intersect({{4.0f, 1.2f, 0.0f}, {-1, 0, 0}}, 100.0f, hit),
          "an arm is hit from +X");
    check(hit.part == PartRightArm, "the arm at +X is the entity's right arm");

    EntityHit left;
    check(entities.intersect({{-4.0f, 1.2f, 0.0f}, {1, 0, 0}}, 100.0f, left),
          "an arm is hit from -X");
    check(left.part == PartLeftArm, "the arm at -X is the entity's left arm");

    // Legs below the torso. Each leg is four pixels wide, so the right one
    // spans x from 0 to 0.25 blocks -- aim at the middle of that.
    EntityHit leg;
    check(entities.intersect({{0.12f, 0.4f, -3.0f}, {0, 0, 1}}, 100.0f, leg),
          "a leg is hit at knee height");
    check(leg.part == PartRightLeg, "the leg at +X is the entity's right leg");
}

void testSlimArms() {
    std::printf("slim model: three-pixel arms\n");

    Skin skin = makeTestSkin(false);
    skin.setModel(SkinModel::Slim);
    EntityModel model = buildPlayerModel(skin);

    // Find the right arm base box and confirm its width and placement.
    bool found = false;
    for (const ModelBox& box : model.boxes) {
        if (box.part != PartRightArm || box.layer != LayerBase) continue;
        found = true;
        check(nearly(box.size.x, 3.0f), "slim arm is three pixels wide");
        check(nearly(box.origin.x, 4.0f), "slim arm still meets the torso at x = 4");
    }
    check(found, "the slim model has a right arm");
}

// A mirrored face is invisible on a symmetric skin and ruins an asymmetric
// one, so check the direction of u and v explicitly rather than by eye.
void testFaceOrientation() {
    std::printf("face orientation: u and v run the right way\n");

    Skin layout;
    {
        ImageU8 blank(64, 64);
        std::vector<uint8_t> bytes = pngEncode(blank);
        layout.loadFromPng(bytes.data(), bytes.size(), nullptr);
    }
    SkinRect front = layout.faceRect(PartHead, LayerBase, SkinFaceFront);

    // Paint the four quadrants of the head's front rectangle.
    const ImageU8::RGBA kTopLeft{255, 0, 0, 255};
    const ImageU8::RGBA kTopRight{0, 255, 0, 255};
    const ImageU8::RGBA kBottomLeft{0, 0, 255, 255};
    const ImageU8::RGBA kBottomRight{255, 255, 0, 255};

    // Transparent everywhere else, so the hat layer is cut away and the ray
    // reaches the face we are actually testing.
    ImageU8 image(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) image.set(x, y, {0, 0, 0, 0});
    }
    for (int y = 0; y < front.height; ++y) {
        for (int x = 0; x < front.width; ++x) {
            bool left = x < front.width / 2;
            bool top = y < front.height / 2;
            image.set(front.x + x, front.y + y,
                      top ? (left ? kTopLeft : kTopRight) : (left ? kBottomLeft : kBottomRight));
        }
    }

    std::vector<uint8_t> bytes = pngEncode(image);
    Skin skin;
    check(skin.loadFromPng(bytes.data(), bytes.size(), nullptr), "quadrant skin loads");

    EntityModel model = headOnlyModel(skin);
    Entity entity;
    entity.model = &model;
    entity.skin = &skin;

    EntitySet entities;
    entities.add(entity);

    auto sampleAt = [&](float x, float y) {
        EntityHit hit;
        if (!entities.intersect({{x, y, -3.0f}, {0, 0, 1}}, 100.0f, hit)) return Vec3{-1.0f};
        return hit.albedo;
    };

    auto asLinear = [](ImageU8::RGBA c) {
        return srgbToLinear(Vec3{float(c.r) / 255.0f, float(c.g) / 255.0f, float(c.b) / 255.0f});
    };

    // The head spans x and y from -0.25..0.25 and 1.5..2.0 blocks.
    // Looking at someone, their right side (+X here) appears on our left, and
    // the texture stores the face as we see it -- so +X must map to low u,
    // which is the left half of the rectangle.
    check(colorsMatch(sampleAt(0.15f, 1.9f), asLinear(kTopLeft)),
          "upper +X corner samples the top-left of the texture");
    check(colorsMatch(sampleAt(-0.15f, 1.9f), asLinear(kTopRight)),
          "upper -X corner samples the top-right of the texture");
    check(colorsMatch(sampleAt(0.15f, 1.6f), asLinear(kBottomLeft)),
          "lower +X corner samples the bottom-left of the texture");
    check(colorsMatch(sampleAt(-0.15f, 1.6f), asLinear(kBottomRight)),
          "lower -X corner samples the bottom-right of the texture");
}

// --------------------------------------------------- the unwrap is a roll
//
// The face-colour test at the top of this file paints its synthetic skin
// *through* faceRect() and then reads it back through faceRect(), so it
// agrees with any rectangle table at all -- the same round trip that let two
// rescaling bugs sit in the video codec. Something outside the table has to
// pin the table.
//
// That something is the shape of the unwrap. The four side rectangles lie in
// the file in the order their faces meet going around the box, so two points
// a hair either side of a model edge must land in neighbouring texel columns
// of the skin. Three of the four seams are neighbouring rectangles; the
// fourth is where the strip wraps, and there the two samples must be the
// outer ends of the strip.
//
// Left and right were the wrong way round in that table until 2026-09-01.
// On a symmetric skin it is invisible -- swapping the two sides and reversing
// each one front-to-back is exactly the mirroring that builds a legacy skin's
// left limbs -- and on an asymmetric one it hangs the character's fringe over
// the wrong ear.
struct Texel { int x, y; };

Texel texelOn(const Skin& skin, SkinPart part, SkinLayer layer, SkinFace face,
              Vec3 local, Vec3 size) {
    SkinRect rect = skin.faceRect(part, layer, face);
    Vec2 uv = boxFaceUv(local, size, face);

    int tx = int(uv.x * float(rect.width));
    int ty = int(uv.y * float(rect.height));
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx > rect.width - 1) tx = rect.width - 1;
    if (ty > rect.height - 1) ty = rect.height - 1;
    return {rect.x + tx, rect.y + ty};
}

void testUnwrapIsContinuous() {
    std::printf("unwrap: the four side faces roll around the box in file order\n");

    Skin layout;
    {
        ImageU8 blank(64, 64);
        std::vector<uint8_t> bytes = pngEncode(blank);
        layout.loadFromPng(bytes.data(), bytes.size(), nullptr);
    }

    // A quarter of a texel inside the face, so the sample lands squarely in
    // the first or last column rather than on the boundary between two.
    const float eps = 0.25f;

    const SkinPart parts[] = {PartHead, PartBody, PartRightArm, PartRightLeg};

    for (SkinPart part : parts) {
        const IVec3 iSize = layout.partSize(part);
        const Vec3 size{float(iSize.x), float(iSize.y), float(iSize.z)};
        const float midY = size.y * 0.5f;

        for (int layerIndex = 0; layerIndex < LayerCount; ++layerIndex) {
            const SkinLayer layer = SkinLayer(layerIndex);

            // Going around the box: +X, front (-Z), -X, back (+Z), and back
            // to +X. Each entry is the edge two of those faces share.
            struct Seam {
                SkinFace a, b;
                Vec3 onA, onB;      // a point just inside each face, at the edge
                bool wraps;         // the strip's two ends rather than neighbours
            };
            const Seam seams[] = {
                // The +X/front edge: x = max, z = min.
                {SkinFaceRight, SkinFaceFront,
                 {size.x, midY, eps}, {size.x - eps, midY, 0.0f}, false},
                // The front/-X edge: x = min, z = min.
                {SkinFaceFront, SkinFaceLeft,
                 {eps, midY, 0.0f}, {0.0f, midY, eps}, false},
                // The -X/back edge: x = min, z = max.
                {SkinFaceLeft, SkinFaceBack,
                 {0.0f, midY, size.z - eps}, {eps, midY, size.z}, false},
                // The back/+X edge: x = max, z = max. Here the strip wraps.
                {SkinFaceBack, SkinFaceRight,
                 {size.x - eps, midY, size.z}, {size.x, midY, size.z - eps}, true},
            };

            for (const Seam& seam : seams) {
                const Texel ta = texelOn(layout, part, layer, seam.a, seam.onA, size);
                const Texel tb = texelOn(layout, part, layer, seam.b, seam.onB, size);

                char what[160];
                std::snprintf(what, sizeof(what), "%s %s: %s|%s seam",
                              Skin::partName(part), layer == LayerBase ? "base" : "outer",
                              Skin::faceName(seam.a), Skin::faceName(seam.b));

                if (ta.y != tb.y) {
                    std::printf("  FAIL  %s: samples at one height landed on rows %d and %d\n",
                                what, ta.y, tb.y);
                    ++gFailures;
                }

                if (!seam.wraps) {
                    if (tb.x - ta.x != 1) {
                        std::printf("  FAIL  %s: columns %d and %d are not neighbours\n",
                                    what, ta.x, tb.x);
                        ++gFailures;
                    }
                } else {
                    // The far ends of the strip: the back rectangle's last
                    // column and the +X rectangle's first.
                    const SkinRect back  = layout.faceRect(part, layer, SkinFaceBack);
                    const SkinRect right = layout.faceRect(part, layer, SkinFaceRight);
                    if (ta.x != back.x + back.width - 1 || tb.x != right.x) {
                        std::printf("  FAIL  %s: the strip does not close (%d, %d)\n",
                                    what, ta.x, tb.x);
                        ++gFailures;
                    }
                }
            }

            // And the seam that names the sides: a viewer facing the entity
            // sees its right cheek on their left, so the +X rectangle is the
            // one before the front rectangle in the file, not after it.
            const SkinRect right = layout.faceRect(part, layer, SkinFaceRight);
            const SkinRect front = layout.faceRect(part, layer, SkinFaceFront);
            const SkinRect left  = layout.faceRect(part, layer, SkinFaceLeft);
            check(right.x < front.x && front.x < left.x,
                  "the entity's own right side is the first rectangle of the strip");
        }
    }
}

void testEmptySetIsHarmless() {
    std::printf("an empty set answers cleanly\n");

    EntitySet entities;
    EntityHit hit;
    check(entities.empty(), "a fresh set is empty");
    check(!entities.intersect({{0, 0, 0}, {0, 0, 1}}, 100.0f, hit), "nothing is hit");
    check(!entities.occluded({{0, 0, 0}, {0, 0, 1}}, 100.0f), "nothing occludes");

    Vec3 lo, hi;
    check(!entities.bounds(lo, hi), "an empty set has no bounds");
}

} // namespace

int main() {
    testEmptySetIsHarmless();
    testHeadFaces();
    testYawRotatesTheModel();
    testPoseRotatesOnePart();
    testFaceOrientation();
    testUnwrapIsContinuous();
    testOuterLayerCutout();
    testPlayerModelLayout();
    testSlimArms();

    if (gFailures == 0) {
        std::printf("\nall entity tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
