// Tests for the rig: the skeleton, parenting, and the tools that cut a model
// into finer pieces.
//
// The one that matters most is `splitting a part changes nothing`. Cutting a
// box in half means cutting its six skin rectangles in half, along the right
// texture axis, in the right direction, per face -- and getting that wrong
// does not crash or look broken. It looks like the skin is very slightly off,
// which is invisible until someone stares at a render and cannot say why it
// is wrong.
//
// So the check is exact and end-to-end: cast several thousand rays at the
// model, split it, cast the same rays again, and demand the same surface with
// the same colour every time. The skin is a coordinate pattern, so any
// misalignment at all changes a colour.
//
// Needs no game files.
#include "engine/core/png.hpp"
#include "engine/entity/attach.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/rigging.hpp"
#include "engine/prop/prop_rig.hpp"
#include "engine/rig/rig.hpp"

#include <cstdio>
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
bool nearlyVec(Vec3 a, Vec3 b, float tolerance) {
    return nearly(a.x, b.x, tolerance) && nearly(a.y, b.y, tolerance) &&
           nearly(a.z, b.z, tolerance);
}

// A skin whose every texel encodes its own coordinates. Any shift in the UV
// mapping, however small, comes back as a different colour.
Skin coordinateSkin() {
    ImageU8 image(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            image.set(x, y, {uint8_t(x * 4), uint8_t(y * 4), 200, 255});
        }
    }
    std::vector<uint8_t> png = pngEncode(image);

    Skin skin;
    skin.loadFromPng(png.data(), png.size(), nullptr);
    return skin;
}

// ------------------------------------------------------------------ skeleton
void testSkeleton() {
    std::printf("skeleton\n");

    Skeleton skeleton;
    int root = skeleton.add("root", -1, {0.0f, 0.0f, 0.0f});
    int body = skeleton.add("body", root, {0.0f, 12.0f, 0.0f});
    int head = skeleton.add("head", body, {0.0f, 24.0f, 0.0f});

    check(root == 0 && body == 1 && head == 2, "joints are numbered in order");
    check(skeleton.size() == 3, "and counted");
    check(skeleton.find("head") == head, "lookup by name works");
    check(skeleton.find("tail") == -1, "an unknown name is -1");

    // A parent has to exist already: that rule is what makes the storage
    // topological and resolve a single forward pass.
    check(skeleton.add("bad", 9, {}) == -1, "a forward parent is refused");
    check(skeleton.add("bad", -2, {}) == -1, "a nonsense parent is refused");
    check(skeleton.size() == 3, "and neither was added");

    check(skeleton.descendsFrom(head, root), "a grandchild descends from the root");
    check(skeleton.descendsFrom(head, head), "a joint descends from itself");
    check(!skeleton.descendsFrom(body, head), "a parent does not descend from its child");

    {   // At rest every joint is the identity: the pivot translation cancels.
        std::vector<Mat4> rest;
        skeleton.resolveRest(rest);
        check(rest.size() == 3, "rest resolves one matrix per joint");
        Vec3 point{3.0f, 27.0f, 1.0f};
        check(nearlyVec(transformPoint(rest[size_t(head)], point), point, 1e-4f),
              "a rest pose moves nothing");
    }

    {   // The headline: a parent carries its children.
        Pose pose;
        pose[body].rotationDegrees = {0.0f, 90.0f, 0.0f};

        std::vector<Mat4> matrices;
        skeleton.resolve(pose, matrices);

        // The head sits above the body pivot on the axis of rotation, so its
        // own pivot does not move...
        Vec3 headPivot{0.0f, 24.0f, 0.0f};
        check(nearlyVec(transformPoint(matrices[size_t(head)], headPivot), headPivot, 1e-3f),
              "a point on the axis stays put");

        // ...but a point in front of the face swings a quarter turn. Yaw of
        // +90 sends -Z to -X, matching the entity convention.
        Vec3 nose{0.0f, 28.0f, -4.0f};
        Vec3 turned = transformPoint(matrices[size_t(head)], nose);
        check(nearlyVec(turned, Vec3{-4.0f, 28.0f, 0.0f}, 1e-3f),
              "turning the body carries the head with it");
    }

    {   // An offset shifts a joint without turning it.
        Pose pose;
        pose[head].offset = {0.0f, 3.0f, 0.0f};
        std::vector<Mat4> matrices;
        skeleton.resolve(pose, matrices);
        check(nearlyVec(transformPoint(matrices[size_t(head)], Vec3{1.0f, 24.0f, 0.0f}),
                        Vec3{1.0f, 27.0f, 0.0f}, 1e-4f),
              "a joint offset translates the part");
    }
}

void testPose() {
    std::printf("pose\n");

    Pose pose;
    check(pose.size() == 0, "a fresh pose holds nothing");

    const Pose& constPose = pose;
    check(nearlyVec(constPose[5].rotationDegrees, Vec3{0.0f}, 1e-6f),
          "an unset joint reads back at rest");
    check(nearlyVec(constPose[-1].rotationDegrees, Vec3{0.0f}, 1e-6f),
          "and so does a negative index");
    check(pose.size() == 0, "reading does not grow the pose");

    pose[4].rotationDegrees = {10.0f, 0.0f, 0.0f};
    check(pose.size() == 5, "writing grows it to fit");
    check(nearly(constPose[4].rotationDegrees.x, 10.0f, 1e-4f), "and keeps the value");

    {   // A pose written for a rig with a jaw still applies to one without.
        Skeleton skeleton;
        skeleton.add("root", -1, {});
        Pose named;
        named.at(skeleton, "jaw").rotationDegrees = {30.0f, 0.0f, 0.0f};
        check(named.size() == 0, "posing a joint the skeleton lacks changes nothing");

        named.at(skeleton, "root").rotationDegrees = {5.0f, 0.0f, 0.0f};
        check(named.size() == 1, "and posing one it has does");
    }

    {   // The humanoid factories address the fixed indices.
        Pose stride = Pose::striding(30.0f);
        check(nearly(stride[joint::RightArm].rotationDegrees.x, -30.0f, 1e-4f),
              "striding swings the right arm back");
        check(nearly(stride[joint::LeftLeg].rotationDegrees.x, -30.0f, 1e-4f),
              "and the left leg with it");

        Pose t = Pose::tPose();
        check(nearly(t[joint::RightArm].rotationDegrees.z, 90.0f, 1e-4f),
              "a T-pose lifts the right arm outwards");
    }
}

// Which way a joint turns depends on which side of its pivot the part sits,
// and that is the single easiest thing to get backwards in a rig. A limb
// hangs *below* its pivot, so a positive turn about X carries it forward to
// -Z. The torso rises *above* the waist, so the same positive turn lays it
// backwards. Both are pinned down here on the real player model.
void testPlayerHierarchy() {
    std::printf("player hierarchy\n");

    Skin skin = coordinateSkin();
    EntityModel model = buildPlayerModel(skin);

    check(model.skeleton.size() == size_t(joint::HumanoidCount),
          "the player rig has the seven standard joints");
    check(model.skeleton.find("body") == joint::Body, "and they are where the constants say");
    check(model.skeleton.find("leftLeg") == joint::LeftLeg, "all of them");
    check(model.skeleton[joint::Head].parent == joint::Body, "the head hangs off the torso");
    check(model.skeleton[joint::RightArm].parent == joint::Body, "and so do the arms");
    check(model.skeleton[joint::RightLeg].parent == joint::Root, "the legs hang off the root");
    check(nearly(model.skeleton[joint::Body].pivot.y, 12.0f, 1e-4f),
          "the torso turns about the waist, not the neck");

    {   // Folding forward at the waist.
        Pose pose;
        pose[joint::Body].rotationDegrees = {-34.0f, 0.0f, 0.0f};

        std::vector<Mat4> matrices;
        model.skeleton.resolve(pose, matrices);

        Vec3 crown{0.0f, 32.0f, 0.0f};
        Vec3 moved = transformPoint(matrices[size_t(joint::Head)], crown);
        check(moved.z < -8.0f, "a negative turn at the waist throws the head forward");
        check(moved.y < 30.0f, "and brings it down");

        // The shoulder, not the hand: a hand at y = 12 sits exactly on the
        // waist pivot's axis and is invariant under a turn about X, which
        // says nothing either way.
        Vec3 shoulder{5.0f, 22.0f, 0.0f};
        Vec3 movedShoulder = transformPoint(matrices[size_t(joint::RightArm)], shoulder);
        check(movedShoulder.z < -4.0f, "the arms come forward with it");

        Vec3 foot{2.0f, 0.0f, 0.0f};
        check(nearlyVec(transformPoint(matrices[size_t(joint::RightLeg)], foot), foot, 1e-4f),
              "while the legs stay planted");
    }

    {   // And the opposite sign for a part that hangs below its pivot.
        Pose pose;
        pose[joint::RightArm].rotationDegrees = {30.0f, 0.0f, 0.0f};

        std::vector<Mat4> matrices;
        model.skeleton.resolve(pose, matrices);

        Vec3 hand{5.0f, 12.0f, 0.0f};
        Vec3 moved = transformPoint(matrices[size_t(joint::RightArm)], hand);
        check(moved.z < -3.0f, "a positive turn swings a hanging arm forward");
    }
}

// ------------------------------------------------------------ sampled rays
struct Sample {
    bool  hit = false;
    Vec3  albedo{};
    float t = 0.0f;
};

// Cast a fixed grid of rays at the model from five directions and record what
// each one found. Two models that look the same produce the same list.
std::vector<Sample> sampleModel(const EntityModel& model, const Skin& skin, const Pose& pose) {
    Entity entity;
    entity.model = &model;
    entity.skin = &skin;
    entity.position = {0.0f, 0.0f, 0.0f};
    entity.pose = pose;

    EntitySet set;
    set.add(entity);

    std::vector<Sample> samples;
    const int kGrid = 36;
    const Vec3 directions[] = {{0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f},
                              {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}};

    for (Vec3 direction : directions) {
        // Build the sweep plane explicitly rather than from orthonormalBasis:
        // that returns *some* perpendicular pair, so "up" could come back
        // horizontal and the grid would sweep a sliver past the model. It did.
        Vec3 up{0.0f, 1.0f, 0.0f};
        if (std::fabs(direction.y) > 0.9f) up = Vec3{0.0f, 0.0f, 1.0f};
        Vec3 across = normalize(cross(direction, up));
        Vec3 along = normalize(cross(across, direction));

        for (int i = 0; i < kGrid; ++i) {
            for (int j = 0; j < kGrid; ++j) {
                // The odd fractions keep the grid off exact texel edges, so a
                // sample never lands on a boundary and flips on a rounding.
                float u = -1.10f + 2.20f * (float(i) + 0.317f) / float(kGrid);
                float v = -1.30f + 2.60f * (float(j) + 0.613f) / float(kGrid);

                Vec3 origin = Vec3{0.0f, 1.05f, 0.0f} + across * u + along * v -
                              direction * 12.0f;

                EntityHit hit;
                Sample sample;
                if (set.intersect({origin, direction}, 60.0f, hit)) {
                    sample.hit = true;
                    sample.albedo = hit.albedo;
                    sample.t = hit.t;
                }
                samples.push_back(sample);
            }
        }
    }
    return samples;
}

// `withColour` false compares only the silhouette: what was hit and how far
// away. That is the weaker of the two claims, and the one that still holds
// across an inflated shell -- see the note in testSplitIsInvisible.
int compareSamples(const std::vector<Sample>& before, const std::vector<Sample>& after,
                   bool withColour, int* hitCount) {
    int differences = 0;
    int hits = 0;
    size_t count = before.size() < after.size() ? before.size() : after.size();

    for (size_t i = 0; i < count; ++i) {
        if (before[i].hit) ++hits;
        if (before[i].hit != after[i].hit) { ++differences; continue; }
        if (!before[i].hit) continue;
        if (!nearly(before[i].t, after[i].t, 1e-3f)) { ++differences; continue; }
        if (withColour && !nearlyVec(before[i].albedo, after[i].albedo, 1e-4f)) ++differences;
    }
    if (hitCount) *hitCount = hits;
    return differences;
}

// Drop the outer layers, leaving the solid base boxes. Those tile a cut
// exactly, so they are what the byte-for-byte claim is made about.
void stripOuter(EntityModel& model) {
    std::vector<ModelBox> kept;
    kept.reserve(model.boxes.size());
    for (const ModelBox& box : model.boxes) {
        if (box.layer == LayerOuter) continue;
        kept.push_back(box);
    }
    model.boxes = std::move(kept);
}

// ------------------------------------------------------------------ cutting
void testSplitIsInvisible() {
    std::printf("splitting a part at rest\n");

    Skin skin = coordinateSkin();
    check(!skin.texture().empty(), "the coordinate skin loaded");

    // Two references: the whole model, and the base layer alone.
    EntityModel reference = buildPlayerModel(skin);
    std::vector<Sample> before = sampleModel(reference, skin, Pose::standing());

    EntityModel baseReference = buildPlayerModel(skin);
    stripOuter(baseReference);
    std::vector<Sample> baseBefore = sampleModel(baseReference, skin, Pose::standing());

    int hits = 0;
    {
        int selfDifferences = compareSamples(before, before, true, &hits);
        check(selfDifferences == 0, "the sampler is deterministic");
        check(hits > 800, "the sample grid actually hits the model");
    }

    struct Case {
        const char* what;
        int joint;
        int axis;
        int segments;
        rigging::Chain chain;
    };
    const Case cases[] = {
        {"an arm cut in two across its length", joint::RightArm, 1, 2, rigging::Chain::FromHigh},
        {"an arm cut in three", joint::LeftArm, 1, 3, rigging::Chain::FromHigh},
        {"a leg cut in two", joint::RightLeg, 1, 2, rigging::Chain::FromHigh},
        {"a head cut across X", joint::Head, 0, 2, rigging::Chain::Siblings},
        {"a head cut across Z", joint::Head, 2, 2, rigging::Chain::Siblings},
        {"a head cut across Y in four", joint::Head, 1, 4, rigging::Chain::FromLow},
        {"a torso cut across X in two", joint::Body, 0, 2, rigging::Chain::Siblings},
    };

    for (const Case& test : cases) {
        EntityModel model = buildPlayerModel(skin);
        size_t boxesBefore = model.boxes.size();
        size_t onJoint = model.boxesOn(test.joint).size();

        std::vector<int> pieces =
            rigging::split(model, test.joint, test.axis, test.segments, test.chain, "seg");

        check(pieces.size() == size_t(test.segments), "the split returns one joint per segment");
        check(model.boxes.size() == boxesBefore - onJoint + onJoint * size_t(test.segments),
              "and one box per segment per original box");
        check(model.boxesOn(test.joint).empty(), "the original joint keeps no boxes");

        // Claim one, exact: the base layer is unchanged, colour and all.
        // Solid boxes tile a cut with no overlap, so there is nothing here to
        // be approximate about.
        EntityModel baseModel = buildPlayerModel(skin);
        stripOuter(baseModel);
        rigging::split(baseModel, test.joint, test.axis, test.segments, test.chain, "seg");

        std::vector<Sample> baseAfter = sampleModel(baseModel, skin, Pose::standing());
        int baseDifferences = compareSamples(baseBefore, baseAfter, true, nullptr);
        if (baseDifferences != 0) {
            std::printf("        %s (base): %d of %zu samples differ\n", test.what,
                        baseDifferences, baseBefore.size());
        }
        check(baseDifferences == 0, test.what);

        // Claim two, also exact but weaker: with the outer shells on, the
        // silhouette is untouched -- same surface, same distance, everywhere.
        //
        // Colour is not claimed there, deliberately. Each segment's shell is
        // inflated on all six sides, so at a cut two shells overlap in a band
        // half a pixel wide, and inside it they disagree by one texel row
        // about which part of the sleeve they wear. That follows from inflate
        // being one scalar per box; the alternative is per-face inflation for
        // a difference nobody can see.
        std::vector<Sample> after = sampleModel(model, skin, Pose::standing());
        int silhouette = compareSamples(before, after, false, nullptr);
        if (silhouette != 0) {
            std::printf("        %s (silhouette): %d of %zu samples differ\n", test.what,
                        silhouette, before.size());
        }
        check(silhouette == 0, "the shelled silhouette survives the cut too");
    }
}

void testSplitPoses() {
    std::printf("posing a cut part\n");

    Skin skin = coordinateSkin();
    EntityModel model = buildPlayerModel(skin);

    int elbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
    check(elbow > 0, "the arm gains an elbow");
    check(model.skeleton.find("rightElbow0") == elbow, "named as asked");

    // The forearm must hang below the upper arm, and the elbow must sit
    // between them rather than at the shoulder.
    check(nearly(model.skeleton[elbow].pivot.y, 18.0f, 1e-3f),
          "the elbow sits halfway down the arm");
    check(model.skeleton.descendsFrom(elbow, joint::RightArm),
          "and hangs off the shoulder joint");

    Vec3 restLo, restHi;
    {
        Entity entity;
        entity.model = &model;
        entity.skin = &skin;
        EntitySet set;
        set.add(entity);
        check(set.bounds(restLo, restHi), "the rested model has bounds");
    }

    {   // Bending the elbow moves the forearm and leaves the shoulder alone.
        // Positive about X takes a limb forward, to -Z; see conventions.md.
        Pose pose;
        pose[elbow].rotationDegrees = {90.0f, 0.0f, 0.0f};

        Entity entity;
        entity.model = &model;
        entity.skin = &skin;
        entity.pose = pose;
        EntitySet set;
        set.add(entity);

        Vec3 lo, hi;
        check(set.bounds(lo, hi), "the bent model has bounds");
        // A forearm swung forwards reaches further towards -Z than anything
        // on a standing figure does.
        // The forearm swings out to z = -6.25 pixels; the furthest a
        // standing figure reaches is the hat brim at -4.5. That is 0.11 of a
        // block, which is what the threshold has to respect.
        check(lo.z < restLo.z - 0.05f, "a bent elbow reaches forward");
        check(nearly(hi.y, restHi.y, 1e-3f), "and does not change the height");
    }

    {   // The shoulder still carries the whole arm, forearm included.
        Pose pose;
        pose[joint::RightArm].rotationDegrees = {0.0f, 0.0f, 90.0f};

        Entity entity;
        entity.model = &model;
        entity.skin = &skin;
        entity.pose = pose;
        EntitySet set;
        set.add(entity);

        Vec3 lo, hi;
        set.bounds(lo, hi);
        check(hi.x > restHi.x + 0.3f, "raising the shoulder takes the forearm with it");
    }
}

void testJawAndAttachments() {
    std::printf("faces and attachments\n");

    Skin skin = coordinateSkin();

    {   // A jaw is a slice off the bottom of the head that hinges.
        EntityModel model = buildPlayerModel(skin);
        int jaw = rigging::addJaw(model, joint::Head, 3.0f, "jaw");
        check(jaw > 0, "the head gains a jaw");
        check(model.skeleton[jaw].parent == joint::Head, "hung off the head");
        check(nearly(model.skeleton[jaw].pivot.y, 27.0f, 1e-3f),
              "hinged at the top of the jaw slice");
        check(nearly(model.skeleton[jaw].pivot.z, 4.0f, 1e-3f),
              "and at the back of the head, where a jaw turns");

        // Everything above the jaw went back onto the head joint, so turning
        // the head still moves the whole skull.
        check(!model.boxesOn(joint::Head).empty(), "the skull is back on the head joint");
        check(!model.boxesOn(jaw).empty(), "and the jaw has boxes of its own");

        // Opening it must move something, and only downwards at the front.
        Entity shut;
        shut.model = &model;
        shut.skin = &skin;
        EntitySet shutSet;
        shutSet.add(shut);
        Vec3 shutLo, shutHi;
        shutSet.bounds(shutLo, shutHi);

        Pose open;
        open[jaw].rotationDegrees = {35.0f, 0.0f, 0.0f};
        Entity wide = shut;
        wide.pose = open;
        EntitySet wideSet;
        wideSet.add(wide);
        Vec3 openLo, openHi;
        wideSet.bounds(openLo, openHi);

        check(openLo.z < shutLo.z - 0.01f || openLo.y < shutLo.y - 0.01f,
              "opening the jaw moves it out of the head");
        check(nearly(openHi.y, shutHi.y, 1e-3f), "without moving the top of the skull");
    }

    {   // A small feature hung off the head, following it.
        EntityModel model = buildPlayerModel(skin);
        size_t before = model.boxes.size();

        SkinRect headFront = skin.faceRect(PartHead, LayerBase, SkinFaceFront);
        rigging::Attachment brow = rigging::attachmentFrom(
            "browRight", joint::Head, {1.0f, 29.0f, -4.5f}, {2.0f, 1.0f, 1.0f},
            rigging::subRect(headFront, 2, 2, 2, 1));
        int browJoint = rigging::attach(model, brow);

        check(browJoint > 0, "the brow attaches");
        check(model.boxes.size() == before + 1, "and adds exactly one box");
        check(model.boxesOn(browJoint).size() == 1, "bound to its own joint");

        // Turning the head must take the brow with it.
        Pose turned;
        turned[joint::Head].rotationDegrees = {0.0f, 90.0f, 0.0f};

        std::vector<Mat4> matrices;
        model.skeleton.resolve(turned, matrices);
        Vec3 browPoint{2.0f, 29.5f, -4.0f};
        Vec3 moved = transformPoint(matrices[size_t(browJoint)], browPoint);
        check(!nearlyVec(moved, browPoint, 0.1f), "a turned head carries the brow");

        // A degenerate attachment is refused rather than added.
        rigging::Attachment flat = brow;
        flat.size = {0.0f, 1.0f, 1.0f};
        check(rigging::attach(model, flat) == -1, "a zero-sized attachment is refused");
    }

    {   // subRect stays inside its parent.
        SkinRect rect{10, 20, 8, 8};
        SkinRect inner = rigging::subRect(rect, 2, 3, 4, 4);
        check(inner.x == 12 && inner.y == 23, "subRect offsets from the corner");
        check(inner.width == 4 && inner.height == 4, "and keeps the asked size");

        SkinRect clipped = rigging::subRect(rect, 6, 6, 8, 8);
        check(clipped.width == 2 && clipped.height == 2, "clipping to the parent");
    }
}

void testLayersAndReparenting() {
    std::printf("layers and reparenting\n");

    Skin skin = coordinateSkin();
    EntityModel model = buildPlayerModel(skin);

    int hat = rigging::detachLayer(model, PartHead, LayerOuter, "hat");
    check(hat > 0, "the hat layer gets its own joint");
    check(model.skeleton[hat].parent == joint::Head, "hung off the head");
    check(nearlyVec(model.skeleton[hat].pivot, model.skeleton[joint::Head].pivot, 1e-4f),
          "sharing the head's pivot, so detaching alone moves nothing");

    for (const ModelBox& box : model.boxes) {
        if (box.part == PartHead && box.layer == LayerOuter) {
            check(box.joint == hat, "every hat box moved to the new joint");
        }
        if (box.part == PartHead && box.layer == LayerBase) {
            check(box.joint == joint::Head, "and the scalp did not");
        }
    }

    {   // Detaching alone leaves the render untouched.
        EntityModel plain = buildPlayerModel(skin);
        std::vector<Sample> before = sampleModel(plain, skin, Pose::standing());
        std::vector<Sample> after = sampleModel(model, skin, Pose::standing());
        check(compareSamples(before, after, true, nullptr) == 0,
              "detaching a layer changes nothing");
    }

    {   // But now the hat can tilt on its own.
        Pose pose;
        pose[hat].rotationDegrees = {0.0f, 0.0f, 25.0f};

        std::vector<Mat4> matrices;
        model.skeleton.resolve(pose, matrices);
        Vec3 crown{0.0f, 32.0f, 0.0f};
        Vec3 tilted = transformPoint(matrices[size_t(hat)], crown);
        check(!nearlyVec(tilted, crown, 0.05f), "the hat tilts");
        check(nearlyVec(transformPoint(matrices[size_t(joint::Head)], crown), crown, 1e-4f),
              "while the head under it stays put");
    }

    {   // A missing layer is reported rather than guessed at.
        EntityModel other = buildPlayerModel(skin);
        check(rigging::detachLayer(other, PartHead, LayerOuter, "a") > 0, "first detach works");
        // Detaching again finds the boxes on the new joint and makes another.
        check(rigging::detachLayer(other, PartHead, LayerOuter, "b") > 0, "and again");
    }

    {   // Reparenting refuses anything that would break the ordering.
        EntityModel other = buildPlayerModel(skin);
        check(!rigging::reparent(other, 0, -1), "the root cannot be reparented");
        check(!rigging::reparent(other, joint::Head, joint::LeftLeg),
              "a forward parent is refused, since parents come first");
        check(!rigging::reparent(other, joint::Body, joint::Body), "and so is a self-parent");
        check(rigging::reparent(other, joint::Head, joint::Root),
              "hanging the head off the root is fine");
        check(other.skeleton[joint::Head].parent == joint::Root, "and takes effect");
    }
}

// --------------------------------------------------------------- prop rigs
void testPropRig() {
    std::printf("prop rigs\n");

    VoxelModel post;
    post.resize({2, 12, 2});
    uint16_t wood = post.addMaterial(VoxelMaterial{});
    for (int y = 0; y < 12; ++y)
        for (int z = 0; z < 2; ++z)
            for (int x = 0; x < 2; ++x) post.set({x, y, z}, wood);

    VoxelModel lamp;
    lamp.resize({4, 4, 4});
    VoxelMaterial glow;
    glow.emission = {5.0f, 4.0f, 2.0f};
    uint16_t glowSlot = lamp.addMaterial(glow);
    for (int y = 0; y < 4; ++y)
        for (int z = 0; z < 4; ++z)
            for (int x = 0; x < 4; ++x) lamp.set({x, y, z}, glowSlot);

    PropRig rig;
    int base = rig.addPart("base", -1, {1.0f, 0.0f, 1.0f}, &post);
    int arm = rig.addPart("arm", base, {1.0f, 12.0f, 1.0f}, &lamp, {0.0f, 10.0f, 0.0f});
    check(base == 0 && arm == 1, "the rig builds its skeleton");
    check(rig.parts.size() == 2, "with one part each");

    {   // At rest, both parts land where they were authored.
        PropSet set;
        PropPlacement placement;
        placement.position = {0.0f, 0.0f, 0.0f};
        placement.voxelSize = 1.0f;
        addRigged(set, rig, Pose{}, placement);
        set.build();

        check(set.size() == 2, "both parts are placed");
        check(set.voxelCount() == 12u * 4u + 4u * 4u * 4u, "and all their voxels counted");

        // The lamp is a 4-cube lifted ten voxels, so it spans y 10..14.
        PropHit hit;
        check(set.intersect({{1.5f, 12.5f, 20.0f}, {0.0f, 0.0f, -1.0f}}, 100.0f, hit),
              "the lamp is where the offset put it");
        check(hit.emission.x > 1.0f, "and it is the emissive part");
    }

    {   // Turning the base carries the lamp on the end of the arm.
        Pose pose;
        pose[base].rotationDegrees = {0.0f, 90.0f, 0.0f};

        Vec3 restLo, restHi, turnedLo, turnedHi;
        check(rigBounds(rig, Pose{}, restLo, restHi), "rest bounds");
        check(rigBounds(rig, pose, turnedLo, turnedHi), "turned bounds");

        check(nearly(restHi.y, turnedHi.y, 1e-3f), "a yaw does not change the height");
        check(!nearlyVec(restLo, turnedLo, 0.5f), "but it does move the lamp");
    }

    {   // A part with no model, and an empty rig, are ignored quietly.
        PropRig empty;
        PropSet set;
        addRigged(set, empty, Pose{}, {});
        set.build();
        check(set.empty(), "an empty rig places nothing");

        PropRig partial;
        partial.addPart("only", -1, {}, nullptr);
        PropSet set2;
        addRigged(set2, partial, Pose{}, {});
        check(set2.empty(), "a joint with no model places nothing");
    }
}

// ----------------------------------------------- voxel elements on a figure
//
// The claim worth testing is not "the matrix multiplies". It is that a thing
// hung off a joint agrees with the limb the *renderer* draws -- and those are
// two different paths: `EntitySet` flattens boxes, an attachment builds a
// transform. What keeps them together is that both go through
// `resolveEntityJoints`, so the test compares the two against each other
// rather than either against a number typed in by hand.
//
// Scenes used to do this arithmetic themselves, and two of them carried a
// copy of the same fifteen lines.
void testVoxelAttachments() {
    std::printf("voxel attachments\n");

    Skin skin = coordinateSkin();
    EntityModel model = buildPlayerModel(skin);

    // A tool: a thin handle low down and off-centre, a head at the top,
    // nothing between. Its grip is nowhere near the middle of its bounds,
    // which is the whole reason gripVoxel exists.
    VoxelModel tool;
    tool.resize({4, 16, 2});
    const uint16_t iron = tool.addMaterial(VoxelMaterial{});
    for (int y = 0; y < 6; ++y) tool.set({1, y, 0}, iron);
    for (int y = 13; y < 16; ++y)
        for (int x = 0; x < 4; ++x) tool.set({x, y, 0}, iron);

    const Vec3 grip = gripVoxel(tool);
    check(nearly(grip.x, 1.5f, 1e-4f), "the grip sits on the handle in x");
    check(grip.y < 3.0f, "and low down, not in the middle of the bounds");

    const int arm = joint::RightArm;
    const Vec3 fist = jointTip(model, arm);
    check(nearly(fist.y, 12.0f, 1e-4f), "the fist is the bottom of the arm box");
    check(fist.x > 4.0f, "and on the entity's own right");
    check(nearlyVec(jointTip(model, -1), {}, 1e-6f), "a joint that is not there has no tip");

    Entity figure;
    figure.model = &model;
    figure.skin = &skin;
    figure.position = {3.0f, 1.0f, -2.0f};
    figure.yawDegrees = 37.0f;
    figure.pose[joint::Body].rotationDegrees = {0.0f, 15.0f, 0.0f};
    figure.pose[arm].rotationDegrees = {-40.0f, 0.0f, 12.0f};

    // ---- one implementation of the placement, not two
    {
        EntitySet set;
        set.add(figure);

        Mat4 armJoint;
        check(entityJointToWorld(figure, arm, armJoint), "the arm joint resolves");

        const ModelBox* source = nullptr;
        for (const ModelBox& box : model.boxes) {
            if (box.part == PartRightArm && box.layer == LayerBase) source = &box;
        }
        check(source != nullptr, "the model has a right arm");

        const EntitySet::FlatBox* flat = nullptr;
        for (const EntitySet::FlatBox& box : set.boxes()) {
            if (box.part == PartRightArm && box.layer == LayerBase) flat = &box;
        }
        check(flat != nullptr, "and it was flattened");

        if (source && flat) {
            const Mat4 rebuilt = armJoint * translate(source->origin - Vec3{source->inflate});
            bool same = true;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) same = same && rebuilt.m[c][r] == flat->toWorld.m[c][r];
            check(same, "the public joint matrix is exactly the one the renderer used");
        }
    }

    // ---- the anchor voxel lands on the limb, under any pose
    for (int variant = 0; variant < 2; ++variant) {
        Entity posed = figure;
        if (variant == 1) posed.pose[arm].rotationDegrees = {95.0f, 0.0f, -30.0f};

        VoxelAttachment held;
        held.model = &tool;
        held.joint = arm;
        held.offset = fist;
        held.anchor = grip;

        Mat4 toWorld;
        check(attachmentToWorld(posed, held, toWorld), "the attachment resolves");

        EntitySet set;
        set.add(posed);

        // Bottom centre of the flattened arm box, straight out of what the
        // tracer walks. The fist is that point by definition, so the two
        // paths have to land on the same spot in the world.
        const EntitySet::FlatBox* flat = nullptr;
        for (const EntitySet::FlatBox& box : set.boxes()) {
            if (box.part == PartRightArm && box.layer == LayerBase) flat = &box;
        }
        if (flat) {
            const Vec3 boxBottomCentre =
                transformPoint(flat->toWorld, {flat->size.x * 0.5f, 0.0f, flat->size.z * 0.5f});
            check(nearlyVec(transformPoint(toWorld, grip), boxBottomCentre, 1e-4f),
                  "the grip voxel sits on the fist the renderer drew");
        }

        // Rigid plus a *uniform* scale, which is what PropSet demands: a
        // non-uniform one would stretch the local ray direction unevenly and
        // hits would mis-sort silently.
        const Vec3 origin = transformPoint(toWorld, {});
        const Vec3 ax = transformPoint(toWorld, {1.0f, 0.0f, 0.0f}) - origin;
        const Vec3 ay = transformPoint(toWorld, {0.0f, 1.0f, 0.0f}) - origin;
        const Vec3 az = transformPoint(toWorld, {0.0f, 0.0f, 1.0f}) - origin;
        const float unit = figure.scale / 16.0f;
        check(nearly(length(ax), unit, 1e-5f) && nearly(length(ay), unit, 1e-5f) &&
                  nearly(length(az), unit, 1e-5f),
              "one voxel is one model pixel on every axis");
        check(nearly(dot(ax, ay), 0.0f, 1e-6f) && nearly(dot(ay, az), 0.0f, 1e-6f) &&
                  nearly(dot(ax, az), 0.0f, 1e-6f),
              "and the axes stay orthogonal");

        // Sixteen voxels to a block is the reason the default is one: an item
        // from the game comes out the size the game draws it.
        check(nearly(length(transformPoint(toWorld, {0.0f, 16.0f, 0.0f}) - origin), 1.0f, 1e-4f),
              "a sixteen-voxel model is one block tall on a scale-1 figure");
    }

    // ---- entity scale carries through
    {
        Entity big = figure;
        big.scale = 2.0f;

        VoxelAttachment held;
        held.model = &tool;
        held.joint = arm;
        held.offset = fist;

        Mat4 toWorld;
        check(attachmentToWorld(big, held, toWorld), "a scaled figure still resolves");

        const Vec3 origin = transformPoint(toWorld, {});
        check(nearly(length(transformPoint(toWorld, {0.0f, 16.0f, 0.0f}) - origin), 2.0f, 1e-4f),
              "a twice-sized figure holds a twice-sized tool");
    }

    // ---- what is skipped, and what that costs
    {
        VoxelModel empty;

        VoxelAttachment noModel;
        noModel.joint = arm;

        VoxelAttachment emptyModel;
        emptyModel.model = &empty;
        emptyModel.joint = arm;

        VoxelAttachment noJoint;
        noJoint.model = &tool;
        noJoint.joint = 99;

        VoxelAttachment good;
        good.model = &tool;
        good.joint = arm;
        good.offset = fist;
        good.anchor = grip;

        PropSet props;
        const int placed = addAttachments(props, figure, {noModel, emptyModel, noJoint, good});
        props.build();

        check(placed == 1, "three broken attachments are skipped and the good one is not");
        check(props.size() == 1, "and only that one reached the set");
        check(props.voxelCount() == tool.solidCount(), "with all of the tool's voxels");
    }

    // ---- an entity with no model answers rather than crashing
    {
        Entity bare;
        Mat4 out;
        check(!entityJointToWorld(bare, 0, out), "a modelless entity has no joints");

        std::vector<Mat4> joints;
        resolveEntityJoints(bare, joints);
        check(joints.empty(), "and resolving it gives nothing");
    }
}

} // namespace

int main() {
    testSkeleton();
    testPose();
    testPlayerHierarchy();
    testSplitIsInvisible();
    testSplitPoses();
    testJawAndAttachments();
    testLayersAndReparenting();
    testPropRig();
    testVoxelAttachments();

    if (gFailures == 0) {
        std::printf("\nall rig tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
