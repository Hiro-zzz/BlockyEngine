#pragma once
// Blending, for the things a track holds keys of.
//
// This lives in anim/ and not in rig/ on purpose. rig.hpp opens by saying
// nothing in it moves over time and nothing in it needs to, and that is still
// true: a skeleton knows about parenting and pivots, not about seconds. What
// moves is a *pose*, and blending two of them is animation's business.
//
// ------------------------------------------------- why Euler angles are fine
//
// Interpolating Euler angles is normally a compromise -- gimbal lock, uneven
// angular speed -- and the usual answer is a quaternion. Here it is exact.
// The joints this rig grows are hinges: an elbow, a knee, a jaw all turn
// about one axis, and `rigging::addHinge` builds them that way. Interpolating
// one angle about one axis is not an approximation of the right answer, it is
// the right answer.
//
// The exception is a joint genuinely turning about two axes at once -- a head
// tracking something as the body turns under it. That wants a quaternion, and
// it can have one the day a scene needs it, without disturbing anything here.
#include "engine/core/math.hpp"
#include "engine/rig/rig.hpp"
#include "engine/scene/camera.hpp"

namespace blocky {

// The customisation point a Track resolves. Anything you want to key needs an
// overload of this, and everything the engine ships with has one.
inline float animLerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec3  animLerp(Vec3 a, Vec3 b, float t)   { return a + (b - a) * t; }

JointPose animLerp(const JointPose& a, const JointPose& b, float t);

// Joints either side knows nothing about read back as rest, so a pose written
// for a rig with a jaw still blends against one without.
Pose animLerp(const Pose& a, const Pose& b, float t);

// Everything about a camera except the projection mode, which is an enum and
// has no in-between: the result keeps `a`'s. Cutting between an orthographic
// and a perspective camera is a cut, not a move.
Camera animLerp(const Camera& a, const Camera& b, float t);

// ------------------------------------------------------------------ helpers

// Add one pose on top of another: rotations and offsets sum, scales multiply.
// What a breathing idle or a hand tremor wants, layered over a keyed pose
// without either knowing about the other.
Pose poseAdd(const Pose& base, const Pose& overlay, float weight = 1.0f);

} // namespace blocky
