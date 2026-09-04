// Tests for the sprite system: the BVH under it, the quad intersection, the
// font, text layout and the scatter helpers.
//
// The BVH check is the one that earns its keep. A tree can be wrong in ways
// that still produce a picture -- a child index off by one drops a subtree,
// and the result is a few particles quietly missing rather than a crash. So
// it is checked against a brute-force scan over the same boxes: same rays,
// same answer, every time, or the tree is wrong.
//
// Needs no game files. The built-in font stands in for Minecraft's, which is
// exactly the case a scene without assets hits.
#include "engine/core/bvh.hpp"
#include "engine/core/random.hpp"
#include "engine/render/trace/intersect.hpp"
#include "engine/sprite/font.hpp"
#include "engine/sprite/scatter.hpp"
#include "engine/sprite/sprite_set.hpp"
#include "engine/sprite/text.hpp"
#include "scenes/common/palette.hpp"

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

bool nearly(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// A one-texel texture, so a sprite can carry colour without any game assets.
Texture solidTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ImageU8 image(1, 1);
    image.set(0, 0, {r, g, b, a});
    Texture texture;
    texture.fromImage(image);
    return texture;
}

// --------------------------------------------------------------------- BVH
void testBvh() {
    std::printf("bvh\n");

    {   // An empty tree answers nothing rather than misbehaving.
        Bvh bvh;
        bvh.build({});
        check(bvh.empty(), "an empty build leaves an empty tree");

        bool visited = false;
        bvh.traverse({0, 0, 0}, {1, 1, 1}, 100.0f, [&](uint32_t, float&) {
            visited = true;
            return false;
        });
        check(!visited, "an empty tree visits nothing");
    }

    // Boxes strewn unevenly, so the SAH has something real to chew on: a
    // dense clump, a sparse haze, and a line.
    std::vector<Aabb> boxes;
    Rng rng(20260826u);
    for (int i = 0; i < 400; ++i) {
        Vec3 centre{rng.nextFloat() * 4.0f, rng.nextFloat() * 4.0f, rng.nextFloat() * 4.0f};
        Vec3 half{0.05f + rng.nextFloat() * 0.1f};
        boxes.push_back(Aabb{centre - half, centre + half});
    }
    for (int i = 0; i < 400; ++i) {
        Vec3 centre{rng.nextFloat() * 120.0f - 60.0f, rng.nextFloat() * 60.0f,
                    rng.nextFloat() * 120.0f - 60.0f};
        Vec3 half{0.2f + rng.nextFloat() * 0.6f};
        boxes.push_back(Aabb{centre - half, centre + half});
    }
    for (int i = 0; i < 200; ++i) {
        Vec3 centre{float(i) * 0.7f - 70.0f, 30.0f, 12.0f};
        Vec3 half{0.3f, 0.3f, 0.3f};
        boxes.push_back(Aabb{centre - half, centre + half});
    }

    Bvh bvh;
    bvh.build(boxes);

    check(!bvh.empty(), "the tree is built");
    check(bvh.primitiveCount() == boxes.size(), "every primitive is indexed exactly once");
    check(bvh.maxDepth() < 48, "the tree does not run to the depth limit");
    // A degenerate tree is one leaf per primitive all the way down; a healthy
    // one is far shallower than the count.
    check(bvh.maxDepth() < 32, "the tree is not degenerate");

    // Every index appears once and only once across all leaves. Traversing
    // with an infinite reach visits everything the slab test lets through,
    // so instead this walks the order list the tree exposes indirectly: a
    // ray that hits every box would be hard to build, so check by counting
    // the primitives the tree reports instead.
    {
        std::vector<int> seen(boxes.size(), 0);
        // A ray aimed down the line of boxes at y=30 z=12 crosses two hundred
        // of them, which is enough to prove the leaf runs are walked.
        Ray ray{{-80.0f, 30.0f, 12.0f}, {1.0f, 0.0f, 0.0f}};
        Vec3 inv = inverseDirection(ray.direction);
        int visits = 0;
        bvh.traverse(ray.origin, inv, 400.0f, [&](uint32_t index, float&) {
            ++seen[index];
            ++visits;
            return false;
        });
        check(visits > 0, "a ray along the row visits candidates");
        bool duplicated = false;
        for (int count : seen) if (count > 1) duplicated = true;
        check(!duplicated, "no primitive is visited twice on one ray");
    }

    // The heart of it: the tree must agree with a linear scan, ray for ray.
    auto slab = [](const Aabb& box, const Ray& ray, float tMax, float& tHit) {
        float t0 = 0.0f, t1 = tMax;
        for (int a = 0; a < 3; ++a) {
            float inv = 1.0f / ray.direction[a];
            float near = (box.lo[a] - ray.origin[a]) * inv;
            float far = (box.hi[a] - ray.origin[a]) * inv;
            if (inv < 0.0f) { float tmp = near; near = far; far = tmp; }
            if (near > t0) t0 = near;
            if (far < t1) t1 = far;
            if (t0 > t1) return false;
        }
        if (t0 <= 1e-4f) return false;
        tHit = t0;
        return true;
    };

    int mismatches = 0;
    const int kRays = 20000;
    for (int i = 0; i < kRays; ++i) {
        Vec3 origin{rng.nextFloat() * 200.0f - 100.0f, rng.nextFloat() * 80.0f - 10.0f,
                    rng.nextFloat() * 200.0f - 100.0f};
        Vec3 direction = normalize(Vec3{rng.nextFloat() * 2.0f - 1.0f, rng.nextFloat() * 2.0f - 1.0f,
                                        rng.nextFloat() * 2.0f - 1.0f});
        if (lengthSq(direction) < 0.5f) continue;
        Ray ray{origin, direction};

        float bruteT = 1e30f;
        int bruteIndex = -1;
        for (size_t b = 0; b < boxes.size(); ++b) {
            float t = 0.0f;
            if (!slab(boxes[b], ray, 500.0f, t)) continue;
            if (t < bruteT) { bruteT = t; bruteIndex = int(b); }
        }

        Vec3 inv = inverseDirection(direction);
        float treeT = 1e30f;
        int treeIndex = -1;
        bvh.traverse(origin, inv, 500.0f, [&](uint32_t index, float& tMax) {
            float t = 0.0f;
            if (!slab(boxes[index], ray, tMax, t)) return false;
            tMax = t;
            treeT = t;
            treeIndex = int(index);
            return true;
        });

        if (bruteIndex != treeIndex || !nearly(bruteT == 1e30f ? 0.0f : bruteT,
                                               treeT == 1e30f ? 0.0f : treeT, 1e-4f)) {
            ++mismatches;
        }
    }
    check(mismatches == 0, "the tree finds the same nearest box as a linear scan, 20k rays");
    if (mismatches != 0) std::printf("        %d of %d rays disagreed\n", mismatches, kRays);

    // Any-hit stops early but must never claim a miss where a hit exists.
    int anyHitWrong = 0;
    for (int i = 0; i < 4000; ++i) {
        Vec3 origin{rng.nextFloat() * 200.0f - 100.0f, rng.nextFloat() * 80.0f - 10.0f,
                    rng.nextFloat() * 200.0f - 100.0f};
        Vec3 direction = normalize(Vec3{rng.nextFloat() * 2.0f - 1.0f, rng.nextFloat() * 2.0f - 1.0f,
                                        rng.nextFloat() * 2.0f - 1.0f});
        Ray ray{origin, direction};

        bool brute = false;
        for (const Aabb& box : boxes) {
            float t = 0.0f;
            if (slab(box, ray, 500.0f, t)) { brute = true; break; }
        }

        Vec3 inv = inverseDirection(direction);
        bool tree = bvh.traverse<true>(origin, inv, 500.0f, [&](uint32_t index, float& tMax) {
            float t = 0.0f;
            return slab(boxes[index], ray, tMax, t);
        });

        if (brute != tree) ++anyHitWrong;
    }
    check(anyHitWrong == 0, "any-hit traversal agrees with a linear scan, 4k rays");
}

// ------------------------------------------------------------------ sprites
void testQuad() {
    std::printf("sprite quad\n");

    Texture white = solidTexture(255, 255, 255, 255);

    {   // A unit quad at the origin, facing +Z by default.
        SpriteSet set;
        Sprite sprite;
        sprite.position = {0.0f, 0.0f, 0.0f};
        sprite.size = {2.0f, 1.0f};
        sprite.texture = &white;
        set.add(sprite);
        set.build();

        SpriteHit hit;
        check(set.intersect({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "a ray down the axis hits the quad");
        check(nearly(hit.t, 5.0f, 1e-3f), "at the right distance");
        check(nearly(hit.normal.z, 1.0f, 1e-3f), "with the normal facing the ray");

        // Just outside the two-block width.
        check(!set.intersect({{1.2f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "a ray past the edge misses");
        // Just outside the one-block height.
        check(!set.intersect({{0.0f, 0.6f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "a ray over the top misses");
        check(set.intersect({{0.9f, 0.4f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "a ray inside the extent hits");

        // From behind, since the default is double sided.
        check(set.intersect({{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}}, 100.0f, hit),
              "a double-sided quad is visible from behind");
        check(nearly(hit.normal.z, -1.0f, 1e-3f), "and its normal flips to meet the ray");

        // A ray running along the plane finds no crossing.
        check(!set.intersect({{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, 100.0f, hit),
              "a ray in the plane of the quad misses");
    }

    {   // Single-sided rejects the back.
        SpriteSet set;
        Sprite sprite;
        sprite.size = {2.0f, 2.0f};
        sprite.doubleSided = false;
        sprite.texture = &white;
        set.add(sprite);
        set.build();

        SpriteHit hit;
        check(set.intersect({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "single-sided is visible from the front");
        check(!set.intersect({{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}}, 100.0f, hit),
              "single-sided is invisible from behind");
    }

    {   // The cutout is what makes a glyph a glyph: transparent texels let
        // the ray straight through to whatever is behind.
        ImageU8 image(2, 1);
        image.set(0, 0, {255, 255, 255, 255});  // left half opaque
        image.set(1, 0, {255, 255, 255, 0});    // right half cut away
        Texture split;
        split.fromImage(image);

        SpriteSet set;
        Sprite sprite;
        sprite.size = {2.0f, 2.0f};
        sprite.texture = &split;
        set.add(sprite);
        set.build();

        SpriteHit hit;
        check(set.intersect({{-0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "the opaque half of the texture stops the ray");
        check(!set.intersect({{0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "the cut-away half lets it through");
        check(!set.occluded({{0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f),
              "and casts no shadow there either");
        check(set.occluded({{-0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f),
              "while the opaque half does");
    }

    {   // Tint multiplies the texture, and both are linear.
        Texture grey = solidTexture(255, 255, 255, 255);
        SpriteSet set;
        Sprite sprite;
        sprite.size = {2.0f, 2.0f};
        sprite.texture = &grey;
        sprite.tint = {0.25f, 0.5f, 1.0f};
        sprite.emission = {3.0f, 0.0f, 0.0f};
        set.add(sprite);
        set.build();

        SpriteHit hit;
        check(set.intersect({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "the tinted quad is hit");
        check(nearly(hit.albedo.x, 0.25f, 1e-3f) && nearly(hit.albedo.z, 1.0f, 1e-3f),
              "tint multiplies through to the albedo");
        check(nearly(hit.emission.x, 3.0f, 1e-3f), "emission survives above one");
    }

    {   // Nearest wins, whichever order they were added in.
        SpriteSet set;
        for (float z : {-3.0f, 4.0f, 1.0f, -8.0f}) {
            Sprite sprite;
            sprite.position = {0.0f, 0.0f, z};
            sprite.size = {4.0f, 4.0f};
            sprite.texture = &white;
            set.add(sprite);
        }
        set.build();

        SpriteHit hit;
        check(set.intersect({{0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "a stack of quads is hit");
        check(nearly(hit.t, 6.0f, 1e-3f), "and the nearest one wins");
    }
}

void testOrientation() {
    std::printf("orientation\n");

    Texture white = solidTexture(255, 255, 255, 255);

    // Turning a quad to face a point and then firing from that point must
    // land square on the normal.
    const Vec3 targets[] = {{0.0f, 0.0f, 10.0f},  {10.0f, 0.0f, 0.0f}, {-10.0f, 0.0f, 0.0f},
                            {0.0f, 0.0f, -10.0f}, {6.0f, 8.0f, -3.0f}, {-4.0f, -7.0f, 5.0f}};

    for (Vec3 target : targets) {
        Sprite sprite;
        sprite.position = {0.0f, 0.0f, 0.0f};
        sprite.size = {3.0f, 3.0f};
        sprite.texture = &white;
        aimAt(sprite, target);

        SpriteSet set;
        set.add(sprite);
        set.build();

        Vec3 toOrigin = normalize(Vec3{0.0f, 0.0f, 0.0f} - target);
        SpriteHit hit;
        bool got = set.intersect({target, toOrigin}, 100.0f, hit);
        check(got, "a quad aimed at a point is hit from that point");
        if (!got) continue;

        // The normal should point straight back at where the ray came from.
        float alignment = dot(hit.normal, -toOrigin);
        check(nearly(alignment, 1.0f, 1e-3f), "and its normal looks straight at it");
    }
}

// --------------------------------------------------------------------- font
void dumpGlyph(const Font& font, char code) {
    Font::Glyph glyph = font.glyph(static_cast<unsigned char>(code));
    std::printf("    '%c'  width %.0f  advance %.0f\n", code, glyph.width, glyph.advance);

    const Texture& atlas = font.texture();
    int originX = (int(static_cast<unsigned char>(code)) % 16) * font.cellPixels();
    int originY = (int(static_cast<unsigned char>(code)) / 16) * font.cellPixels();

    for (int y = 0; y < font.cellPixels(); ++y) {
        std::printf("        ");
        for (int x = 0; x < font.cellPixels(); ++x) {
            std::printf("%c", atlas.alphaAt(originX + x, originY + y) >= 0.5f ? '#' : '.');
        }
        std::printf("\n");
    }
}

void testFont() {
    std::printf("font\n");

    Font font;
    check(!font.loaded(), "a fresh font holds nothing");

    font.useBuiltin();
    check(font.loaded(), "the built-in font loads");
    check(font.isBuiltin(), "and knows it is the built-in one");
    check(font.cellPixels() == 8, "on an eight-pixel cell");

    check(!font.glyph('A').blank, "A has ink");
    check(!font.glyph('0').blank, "0 has ink");
    check(font.glyph(' ').blank, "space has none");
    check(nearly(font.glyph(' ').advance, 4.0f, 1e-3f), "and still advances the pen");

    // Widths must actually differ, or the whole width-scanning exercise was
    // pointless and every label is monospaced.
    check(font.glyph('I').width < font.glyph('M').width, "I is narrower than M");
    check(font.glyph('.').width < font.glyph('W').width, "a full stop is narrower than a W");

    // Lowercase falls back to uppercase rather than vanishing.
    check(!font.glyph('a').blank, "lowercase falls back to its uppercase form");
    check(nearly(font.glyph('a').width, font.glyph('A').width, 1e-3f), "with the same width");

    // Measuring must add up, minus the trailing gap.
    float widthA = font.glyph('A').advance;
    float widthB = font.glyph('B').advance;
    check(nearly(font.measure("AB"), widthA + widthB - 1.0f, 1e-3f),
          "measure sums advances and trims the trailing gap");
    check(font.measure("") == 0.0f, "an empty string measures zero");
    check(font.measure("MMM") > font.measure("III"), "a wide string measures wider");

    // Eyeballing the table is the only real check on a hand-drawn font, so
    // print a few and let a human disagree.
    std::printf("  built-in glyphs, for the eye:\n");
    dumpGlyph(font, 'A');
    dumpGlyph(font, 'R');
    dumpGlyph(font, '5');
}

// --------------------------------------------------------------------- text
void testText() {
    std::printf("text layout\n");

    Font font;
    font.useBuiltin();

    TextStyle style;
    style.height = 1.0f;

    {   // One sprite per inked glyph, and none for the space.
        std::vector<Sprite> label = text::build(font, "AB C", {0.0f, 0.0f, 0.0f}, style);
        check(label.size() == 3, "a space produces no geometry");
    }

    {   // Centred by default, so the label straddles its anchor.
        std::vector<Sprite> label = text::build(font, "MMMM", {0.0f, 10.0f, 0.0f}, style);
        check(!label.empty(), "the label has glyphs");

        float lo = 1e30f, hi = -1e30f;
        for (const Sprite& sprite : label) {
            lo = std::min(lo, sprite.position.x);
            hi = std::max(hi, sprite.position.x);
        }
        check(nearly((lo + hi) * 0.5f, 0.0f, 0.2f), "centred alignment straddles the anchor");

        for (const Sprite& sprite : label) {
            check(nearly(sprite.position.y, 10.0f, 1e-3f), "a single line sits at the anchor");
        }
    }

    {   // Left alignment starts at the anchor and runs right.
        TextStyle left = style;
        left.align = TextStyle::Align::Left;
        std::vector<Sprite> label = text::build(font, "MMMM", {0.0f, 0.0f, 0.0f}, left);
        float lo = 1e30f;
        for (const Sprite& sprite : label) lo = std::min(lo, sprite.position.x);
        check(lo > -0.01f, "left alignment starts at the anchor");
    }

    {   // Two lines straddle the anchor, the first one above the second.
        std::vector<Sprite> label = text::build(font, "A\nB", {0.0f, 0.0f, 0.0f}, style);
        check(label.size() == 2, "two lines give two glyphs");
        check(label[0].position.y > label[1].position.y, "the first line sits above the second");
        check(nearly((label[0].position.y + label[1].position.y) * 0.5f, 0.0f, 1e-3f),
              "and the pair straddles the anchor");
    }

    {   // measure agrees with where the glyphs actually landed.
        Vec2 size = text::measure(font, "HELLO", style);
        std::vector<Sprite> label = text::build(font, "HELLO", {0.0f, 0.0f, 0.0f}, style);

        float lo = 1e30f, hi = -1e30f;
        for (const Sprite& sprite : label) {
            lo = std::min(lo, sprite.position.x - sprite.size.x * 0.5f);
            hi = std::max(hi, sprite.position.x + sprite.size.x * 0.5f);
        }
        check(nearly(hi - lo, size.x, 0.05f), "measure matches the laid-out width");
        check(nearly(size.y, style.height, 1e-3f), "a single line measures one line high");
    }

    {   // The whole label turns as one body: every glyph shares the
        // orientation, and the row still runs straight.
        Vec3 anchor{0.0f, 5.0f, 0.0f};
        Vec3 viewer{40.0f, 5.0f, 40.0f};
        std::vector<Sprite> label = text::facing(font, "WIDE", anchor, viewer, style);
        check(!label.empty(), "a facing label has glyphs");

        float yaw = label[0].yawDegrees;
        for (const Sprite& sprite : label) {
            check(nearly(sprite.yawDegrees, yaw, 1e-3f), "every glyph shares the label's yaw");
        }

        // Collinear: the cross product of successive offsets is zero if the
        // glyphs lie on one straight line rather than each spinning in place.
        if (label.size() >= 3) {
            Vec3 first = label[1].position - label[0].position;
            Vec3 second = label[2].position - label[1].position;
            check(length(cross(normalize(first), normalize(second))) < 1e-3f,
                  "the glyphs lie along one straight line");
        }

        // And that line is square to the viewer.
        Vec3 toViewer = normalize(viewer - anchor);
        Vec3 along = normalize(label.back().position - label.front().position);
        check(nearly(dot(along, toViewer), 0.0f, 1e-3f),
              "the run of glyphs is perpendicular to the view direction");
    }

    {   // A label with no font produces nothing rather than crashing.
        Font empty;
        std::vector<Sprite> label = text::build(empty, "NOTHING", {0.0f, 0.0f, 0.0f}, style);
        check(label.empty(), "no font means no geometry");
    }
}

// ------------------------------------------------------------------ scatter
void testScatter() {
    std::printf("scatter\n");

    {   // Everything lands inside the box it was asked for.
        std::vector<Sprite> motes = scatter::inBox({-5.0f, 0.0f, -5.0f}, {5.0f, 10.0f, 5.0f},
                                                   500, 1234u);
        check(motes.size() == 500, "inBox places the count asked for");

        bool outside = false;
        for (const Sprite& sprite : motes) {
            if (sprite.position.x < -5.0f || sprite.position.x > 5.0f) outside = true;
            if (sprite.position.y < 0.0f || sprite.position.y > 10.0f) outside = true;
            if (sprite.position.z < -5.0f || sprite.position.z > 5.0f) outside = true;
        }
        check(!outside, "and every one inside it");
    }

    {   // A ball, and nothing outside its radius.
        std::vector<Sprite> motes = scatter::inSphere({2.0f, 3.0f, 4.0f}, 6.0f, 800, 77u);
        check(motes.size() == 800, "inSphere places the count asked for");

        float furthest = 0.0f;
        for (const Sprite& sprite : motes) {
            furthest = std::max(furthest, length(sprite.position - Vec3{2.0f, 3.0f, 4.0f}));
        }
        check(furthest <= 6.0f + 1e-3f, "and none outside the radius");
        check(furthest > 5.0f, "while still reaching most of the way out");
    }

    {   // A beam stays within its cylinder.
        Vec3 from{0.0f, 20.0f, 0.0f};
        Vec3 axis = normalize(Vec3{0.3f, -1.0f, 0.2f});
        std::vector<Sprite> motes = scatter::inBeam(from, axis, 18.0f, 2.0f, 600, 5u);
        check(motes.size() == 600, "inBeam places the count asked for");

        bool strayed = false;
        for (const Sprite& sprite : motes) {
            Vec3 offset = sprite.position - from;
            float along = dot(offset, axis);
            float radial = length(offset - axis * along);
            if (along < -1e-3f || along > 18.0f + 1e-3f) strayed = true;
            if (radial > 2.0f + 1e-3f) strayed = true;
        }
        check(!strayed, "and every one inside the cylinder");
    }

    {   // Size jitter varies the sprites without inverting any of them.
        scatter::ParticleStyle style;
        style.size = {0.2f, 0.2f};
        style.sizeJitter = 0.5f;
        std::vector<Sprite> motes = scatter::inBox({0, 0, 0}, {1, 1, 1}, 300, 9u, style);

        float smallest = 1e30f, largest = 0.0f;
        for (const Sprite& sprite : motes) {
            smallest = std::min(smallest, sprite.size.x);
            largest = std::max(largest, sprite.size.x);
        }
        check(smallest > 0.0f, "jitter never produces a degenerate sprite");
        check(largest > smallest * 1.5f, "and does vary the size");
    }

    {   // The world-aware helpers.
        World world(palette::registry());
        world.fillBox({-10, 0, -10}, {10, 4, 10}, palette::Stone);
        world.fillBox({-3, 5, -3}, {3, 5, 3}, palette::Lava);

        std::vector<Sprite> settled =
            scatter::onSurface(world, {-10, 0, -10}, {10, 0, 10}, 40, 200, 3u);
        check(!settled.empty(), "onSurface finds the ground");
        bool belowGround = false;
        for (const Sprite& sprite : settled) {
            if (sprite.position.y < 5.0f) belowGround = true;
        }
        check(!belowGround, "and settles on top of it, not inside");

        std::vector<Sprite> embers =
            scatter::above(world, {-5, 4, -5}, {5, 6, 5}, palette::Lava, 3.0f, 4, 11u);
        check(!embers.empty(), "above finds the lava");
        check(embers.size() == 7u * 7u * 4u, "one batch per exposed block");
        bool wrongHeight = false;
        for (const Sprite& sprite : embers) {
            if (sprite.position.y < 6.0f || sprite.position.y > 9.0f + 1e-3f) wrongHeight = true;
        }
        check(!wrongHeight, "and places them in the column above it");

        // Buried motes are removed, floating ones are kept.
        std::vector<Sprite> mixed = scatter::inBox({-8, 0, -8}, {8, 12, 8}, 400, 42u);
        size_t before = mixed.size();
        scatter::removeInsideSolid(mixed, world);
        check(mixed.size() < before, "removeInsideSolid drops the buried ones");
        check(!mixed.empty(), "and keeps the rest");
        for (const Sprite& sprite : mixed) {
            check(!world.isOpaque(floorToInt(sprite.position)), "nothing is left inside a block");
        }
    }
}

// ------------------------------------------------------------------- scene
void testSceneIntegration() {
    std::printf("scene integration\n");

    Texture white = solidTexture(255, 255, 255, 255);

    Scene scene(palette::registry());
    scene.world.fillBox({-4, -4, -4}, {4, -1, 4}, palette::Stone);
    scene.world.set({0, 0, 0}, palette::Stone);

    SpriteSet sprites;
    Sprite sprite;
    sprite.position = {0.5f, 0.5f, 3.0f};  // between the camera side and the block
    sprite.size = {2.0f, 2.0f};
    sprite.texture = &white;
    sprite.tint = {1.0f, 0.0f, 0.0f};
    sprites.add(sprite);
    sprites.build();

    Ray ray{{0.5f, 0.5f, 10.0f}, {0.0f, 0.0f, -1.0f}};

    {   // Without the set attached, the ray reaches the block behind.
        SceneHit hit;
        check(intersectScene(scene, ray, 100.0f, RayFilter{}, hit), "the block is hit");
        check(hit.isBlock, "and it is a block");
    }

    scene.sprites = &sprites;

    {   // With it attached, the nearer quad wins.
        SceneHit hit;
        check(intersectScene(scene, ray, 100.0f, RayFilter{}, hit), "the sprite is hit");
        check(!hit.isBlock, "and it is not a block");
        check(nearly(hit.t, 7.0f, 1e-3f), "at the sprite's distance");
        check(nearly(hit.albedo.x, 1.0f, 1e-3f) && hit.albedo.y < 0.01f,
              "carrying the sprite's own colour");
    }

    {   // And it blocks light like any other surface.
        Vec3 transmittance = sceneTransmittance(scene, ray, 100.0f);
        check(maxComponent(transmittance) <= 0.0f, "an opaque sprite stops a shadow ray");
    }

    {   // An unbuilt set is inert rather than dangerous.
        SpriteSet unbuilt;
        unbuilt.add(sprite);
        scene.sprites = &unbuilt;

        SceneHit hit;
        check(intersectScene(scene, ray, 100.0f, RayFilter{}, hit), "the scene still traces");
        check(hit.isBlock, "an unbuilt set contributes nothing");
    }
}

} // namespace

int main() {
    testBvh();
    testQuad();
    testOrientation();
    testFont();
    testText();
    testScatter();
    testSceneIntegration();

    if (gFailures == 0) {
        std::printf("\nall sprite tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
