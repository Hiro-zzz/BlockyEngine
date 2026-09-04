#pragma once
// The four characters, and the poses they are put through.
//
// ----------------------------------------------------------------- signs
//
// Everything below rests on docs/conventions.md, and getting it wrong folds a
// limb into the torso, which reads as missing geometry rather than as a wrong
// angle. Written out once here so the poses can be read without going back:
//
//   A limb hangs BELOW its pivot.
//     +X swings it forward, toward -Z.
//     +Z swings it toward +X: outward for the right arm, across the body for
//     the left.
//   An elbow bends with a POSITIVE X -- the forearm hangs below it and the
//   hand comes forward.
//   A knee bends with a NEGATIVE X -- the shin hangs below it and the heel
//   goes backward.
//   The torso and head rise ABOVE their pivots, so the same sign takes them
//   the other way: folding forward at the waist is a NEGATIVE X.
//
// The entity looks toward -Z and its own right hand is at +X, which means a
// camera in front of it sees that right hand on the left of frame.
#include "engine/assets/entity/skin.hpp"
#include "engine/core/file.hpp"
#include "engine/entity/entity.hpp"
#include "engine/entity/face.hpp"
#include "engine/entity/rigging.hpp"
#include "scenes/common/skins.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace film {

using namespace blocky;

// ------------------------------------------------------------------ figures

// The joints a plain player model does not come with. Held separately from
// the model because every pose needs them and none of them needs the model.
struct Rig {
    int rightElbow = -1;
    int leftElbow = -1;
    int rightKnee = -1;
    int leftKnee = -1;
};

// One character: the skin, the model cut to take a rig, and the eyes.
//
// Not copyable in practice -- an Entity holds bare pointers to the model and
// the skin, so these must sit still. The film keeps them as named members of
// one struct for exactly that reason.
struct Figure {
    Skin        skin;
    EntityModel model;
    Rig         rig;
    face::EyeRig eyes;
    std::string label;

    // `name` is both what this figure is called in the shot list and the name
    // of the look drawn for it when no file is given -- which is why the four
    // in `skins.hpp` are called what the film calls them. A path still wins,
    // so pointing the film at real skins is one argument away.
    bool load(const std::string& name, const std::string& path = "", bool wantEyes = true) {
        label = name;

        std::string note;
        if (!skins::loadOrDraw(skin, path, name, &note)) {
            std::printf("[cast] %s: %s\n", name.c_str(), note.c_str());
            return false;
        }
        std::printf("[cast] %-8s %s\n", name.c_str(), note.c_str());

        model = buildPlayerModel(skin);
        rig.rightElbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
        rig.leftElbow  = rigging::addHinge(model, joint::LeftArm, "leftElbow");
        rig.rightKnee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
        rig.leftKnee   = rigging::addHinge(model, joint::LeftLeg, "leftKnee");

        // No jaw anywhere in this film -- it is a silent one, and the cards
        // do the talking.

        if (wantEyes) eyes = face::buildEyeRig(model, skin);

        std::printf("[cast] %-8s %s, %zu boxes, eyes %s\n", name.c_str(),
                    skin.model() == SkinModel::Slim ? "slim" : "classic", model.boxes.size(),
                    eyes.built ? face::shapeName(eyes.scan.shape) : eyes.rejection);
        return true;
    }

    bool ok() const { return !model.boxes.empty(); }

    Entity at(Vec3 position, float yawDegrees) const {
        Entity e;
        e.model = &model;
        e.skin = &skin;
        e.position = position;
        e.yawDegrees = yawDegrees;
        return e;
    }

    void look(Pose& pose, float x, float y = 0.0f) const {
        if (eyes.built) face::gaze(eyes, pose, x, y);
    }
};

// ------------------------------------------------------------------- poses
namespace pose {

// Standing at ease. Arms a few degrees off the ribs on purpose: two boxes
// that share a shading band stop reading as two boxes.
inline Pose rest(const Rig& r) {
    Pose p;
    p[joint::Body].rotationDegrees     = { -1.0f,  0.0f,  0.0f};
    p[joint::Head].rotationDegrees     = {  1.0f,  0.0f,  0.0f};
    p[joint::RightArm].rotationDegrees = { -4.0f,  0.0f,  6.0f};
    p[r.rightElbow].rotationDegrees    = {  9.0f,  0.0f,  0.0f};
    p[joint::LeftArm].rotationDegrees  = { -4.0f,  0.0f, -6.0f};
    p[r.leftElbow].rotationDegrees     = {  9.0f,  0.0f,  0.0f};
    p[joint::RightLeg].rotationDegrees = {  1.0f,  0.0f,  1.5f};
    p[r.rightKnee].rotationDegrees     = { -4.0f,  0.0f,  0.0f};
    p[joint::LeftLeg].rotationDegrees  = { -1.0f,  0.0f, -1.5f};
    p[r.leftKnee].rotationDegrees      = { -4.0f,  0.0f,  0.0f};
    return p;
}

// Weight on one hip, the other knee soft. What a person actually stands like
// when they have been standing a while.
inline Pose slouch(const Rig& r, float sign = 1.0f) {
    Pose p = rest(r);
    p[joint::Root].rotationDegrees     = {  0.0f,  0.0f,  2.5f * sign};
    p[joint::Root].offset              = {  0.0f, -0.35f, 0.0f};
    p[joint::Body].rotationDegrees     = { -2.0f,  0.0f, -1.5f * sign};
    p[joint::Head].rotationDegrees     = { -3.0f, -4.0f * sign, 0.0f};
    p[joint::RightLeg].rotationDegrees = {  2.0f,  0.0f,  3.0f};
    p[r.rightKnee].rotationDegrees     = { -9.0f,  0.0f,  0.0f};
    p[joint::LeftLeg].rotationDegrees  = { -2.0f,  0.0f, -2.0f};
    p[r.leftKnee].rotationDegrees      = { -3.0f,  0.0f,  0.0f};
    return p;
}

// A walk, as a function of phase in [0,1). One full cycle is two steps.
//
// The arms swing against the legs, which is the whole of what makes a walk
// read; without it a figure looks like it is being slid along the ground.
inline Pose walk(const Rig& r, float phase, float stride = 26.0f) {
    const float th = phase * kTwoPi;
    const float s = std::sin(th);
    const float c = std::cos(th);

    Pose p;
    // Legs. Right forward on the first half of the cycle.
    p[joint::RightLeg].rotationDegrees = {  stride * s, 0.0f,  1.5f};
    p[joint::LeftLeg].rotationDegrees  = { -stride * s, 0.0f, -1.5f};

    // Knees bend on the way through, not at the extremes -- a straight leg
    // at the back of the stride is what a knee is for.
    const float bendR = std::max(0.0f, -s * 0.7f + c * 0.5f);
    const float bendL = std::max(0.0f,  s * 0.7f - c * 0.5f);
    p[r.rightKnee].rotationDegrees = { -stride * 1.5f * bendR, 0.0f, 0.0f};
    p[r.leftKnee].rotationDegrees  = { -stride * 1.5f * bendL, 0.0f, 0.0f};

    // Arms, opposed to the legs.
    p[joint::RightArm].rotationDegrees = { -stride * 0.62f * s, 0.0f,  5.0f};
    p[joint::LeftArm].rotationDegrees  = {  stride * 0.62f * s, 0.0f, -5.0f};
    p[r.rightElbow].rotationDegrees    = { 14.0f + 10.0f * std::max(0.0f, -s), 0.0f, 0.0f};
    p[r.leftElbow].rotationDegrees     = { 14.0f + 10.0f * std::max(0.0f,  s), 0.0f, 0.0f};

    // The body rises twice per cycle, at each mid-stance, and rolls a little
    // toward the standing leg.
    p[joint::Root].offset          = {0.0f, 0.55f * std::fabs(c) - 0.30f, 0.0f};
    p[joint::Root].rotationDegrees = {0.0f, -4.0f * s, 1.8f * c};
    p[joint::Body].rotationDegrees = {-2.5f, 5.0f * s, 0.0f};
    p[joint::Head].rotationDegrees = {1.5f, -2.0f * s, 0.0f};
    return p;
}

// Wiping a counter: the near arm sweeps across, the body follows it a little.
inline Pose wipe(const Rig& r, float phase) {
    const float s = std::sin(phase * kTwoPi);

    Pose p = rest(r);
    p[joint::Body].rotationDegrees     = { -9.0f,  10.0f * s,  0.0f};
    p[joint::Head].rotationDegrees     = { -7.0f,   7.0f * s,  0.0f};
    // Reaching forward and down onto the counter, sweeping side to side.
    p[joint::RightArm].rotationDegrees = { 58.0f,  16.0f * s, 14.0f + 10.0f * s};
    p[r.rightElbow].rotationDegrees    = { 26.0f,   8.0f * s,  0.0f};
    p[joint::LeftArm].rotationDegrees  = { -2.0f,   0.0f,     -7.0f};
    p[r.leftElbow].rotationDegrees     = { 12.0f,   0.0f,      0.0f};
    p[joint::RightLeg].rotationDegrees = {  2.0f,   0.0f,      2.0f};
    p[joint::LeftLeg].rotationDegrees  = { -2.0f,   0.0f,     -2.0f};
    return p;
}

// Standing, but folded in on itself. Shoulders forward, head down.
inline Pose weary(const Rig& r) {
    Pose p = rest(r);
    p[joint::Root].offset              = {  0.0f, -0.7f,  0.0f};
    p[joint::Body].rotationDegrees     = { -9.0f,  0.0f,  0.0f};   // negative folds forward
    p[joint::Head].rotationDegrees     = {-12.0f,  0.0f,  0.0f};
    p[joint::RightArm].rotationDegrees = {  6.0f,  0.0f,  3.0f};
    p[r.rightElbow].rotationDegrees    = { 16.0f,  0.0f,  0.0f};
    p[joint::LeftArm].rotationDegrees  = {  6.0f,  0.0f, -3.0f};
    p[r.leftElbow].rotationDegrees     = { 16.0f,  0.0f,  0.0f};
    p[joint::RightLeg].rotationDegrees = {  1.0f,  0.0f,  2.0f};
    p[r.rightKnee].rotationDegrees     = { -7.0f,  0.0f,  0.0f};
    p[joint::LeftLeg].rotationDegrees  = { -1.0f,  0.0f, -2.0f};
    p[r.leftKnee].rotationDegrees      = { -7.0f,  0.0f,  0.0f};
    return p;
}

// Sitting: thighs forward, shins down. The root drops by half a leg, which
// the caller has to match by putting the figure on the seat.
inline Pose sit(const Rig& r, float lean = 0.0f) {
    Pose p;
    p[joint::Root].offset              = {  0.0f, -6.0f,  1.0f};
    p[joint::Body].rotationDegrees     = { lean,   0.0f,  0.0f};
    p[joint::Head].rotationDegrees     = { -4.0f,  0.0f,  0.0f};
    p[joint::RightLeg].rotationDegrees = { 84.0f,  0.0f,  3.0f};
    p[r.rightKnee].rotationDegrees     = {-86.0f,  0.0f,  0.0f};
    p[joint::LeftLeg].rotationDegrees  = { 84.0f,  0.0f, -3.0f};
    p[r.leftKnee].rotationDegrees      = {-86.0f,  0.0f,  0.0f};
    p[joint::RightArm].rotationDegrees = { 12.0f,  0.0f,  7.0f};
    p[r.rightElbow].rotationDegrees    = { 34.0f,  0.0f,  0.0f};
    p[joint::LeftArm].rotationDegrees  = { 12.0f,  0.0f, -7.0f};
    p[r.leftElbow].rotationDegrees     = { 34.0f,  0.0f,  0.0f};
    return p;
}

// One arm held out, palm up-ish: offering something, or being handed it.
// `side` is +1 for the right arm, -1 for the left.
inline Pose offer(const Rig& r, float side, float reach = 1.0f) {
    Pose p = rest(r);
    const int arm = side > 0.0f ? joint::RightArm : joint::LeftArm;
    const int elbow = side > 0.0f ? r.rightElbow : r.leftElbow;

    p[joint::Body].rotationDegrees = { -5.0f, -7.0f * side, 3.0f * side};
    p[joint::Head].rotationDegrees = { -5.0f, -5.0f * side, 0.0f};
    p[arm].rotationDegrees   = { 52.0f * reach, 10.0f * side, side * (16.0f + 8.0f * reach)};
    p[elbow].rotationDegrees = { 30.0f * reach,  6.0f * side, 0.0f};
    return p;
}

// An arm raised, high and open. The white figure's one gesture.
inline Pose raise(const Rig& r, float side, float amount = 1.0f) {
    Pose p = rest(r);
    const int arm = side > 0.0f ? joint::RightArm : joint::LeftArm;
    const int elbow = side > 0.0f ? r.rightElbow : r.leftElbow;

    p[joint::Body].rotationDegrees = { -3.0f, 0.0f, -2.0f * side * amount};
    p[joint::Head].rotationDegrees = {  6.0f * amount, 0.0f, 0.0f};
    p[arm].rotationDegrees   = { 4.0f, 0.0f, side * (6.0f + 156.0f * amount)};
    p[elbow].rotationDegrees = { 10.0f + 8.0f * amount, 0.0f, side * 12.0f * amount};
    return p;
}

// Turning to look over a shoulder without moving the feet. The torso carries
// the head and both arms, which is the whole reason the rig is parented.
inline Pose turnToLook(const Rig& r, float degrees) {
    Pose p = rest(r);
    p[joint::Body].rotationDegrees = { -2.0f, degrees * 0.45f, 0.0f};
    p[joint::Head].rotationDegrees = {  0.0f, degrees * 0.55f, 0.0f};
    return p;
}

// Breathing, to be added on top of anything else. A still figure that is
// perfectly still reads as a prop.
inline Pose breathe(float time, float period = 4.0f, float sign = 1.0f, float amount = 1.0f) {
    Pose p;
    const float s = std::sin(time * kTwoPi / period) * sign * amount;
    p[joint::Root].rotationDegrees = {0.0f, 0.0f, 1.6f * s};
    p[joint::Body].rotationDegrees = {0.0f, 0.0f, -1.2f * s};
    p[joint::Head].rotationDegrees = {0.0f, -1.1f * s, 0.6f * s};
    p[joint::Root].offset = {0.0f, -0.14f * std::fabs(s), 0.0f};
    return p;
}

// A slow drift, for the figure that is not quite standing on the ground.
inline Pose float_(float time, float period = 6.0f) {
    Pose p;
    const float s = std::sin(time * kTwoPi / period);
    const float c = std::cos(time * kTwoPi / (period * 1.37f));
    p[joint::Root].offset = {0.0f, 0.9f + 0.55f * s, 0.0f};
    p[joint::Root].rotationDegrees = {0.0f, 3.0f * c, 1.4f * s};
    p[joint::Head].rotationDegrees = {2.0f * s, 0.0f, 0.0f};
    return p;
}

} // namespace pose
} // namespace film
