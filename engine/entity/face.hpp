#pragma once
// Reading a face out of a skin, so the eyes can be given joints.
//
// The eyes of a Minecraft character are *painted*, not modelled: two or three
// texels on the flat front of the head box. Rigging them therefore takes three
// steps that rigging.hpp cannot do on its own --
//
//   1. find them, because every skin puts them somewhere slightly different;
//   2. paint them out, because attach() only ever *adds* geometry and a moved
//      eye would otherwise leave the original still staring forward;
//   3. build new ones, as real boxes on real joints.
//
// Step 1 is the whole difficulty. What follows deliberately reports failure
// instead of guessing: a wrong guess does not look like a bug, it looks like a
// character whose face has been vandalised.
#include "engine/assets/entity/skin.hpp"
#include "engine/entity/model.hpp"

namespace blocky {
namespace face {

// The two shapes the scanner sorts eyes into. Everything downstream -- the
// generated texels, the box proportions, the sensible range of movement --
// follows from which of these it is.
enum class EyeShape {
    Wide,    // as wide as it is tall, or wider: the vanilla two-by-one
    Narrow,  // taller than it is wide: the large eye of a drawn-style skin
};

const char* shapeName(EyeShape shape);

struct EyeScan {
    bool found = false;

    EyeShape shape = EyeShape::Wide;

    // Where the eyes were found, in skin-image pixels. `right` is the
    // entity's own right eye, which lives in the *left* half of the texture
    // rectangle -- a skin stores each face as a viewer sees it.
    SkinRect right{};
    SkinRect left{};

    // Pulled out of the eye itself, so a generated eye still belongs to this
    // character rather than to a default palette. sRGB, as stored.
    ImageU8::RGBA sclera{};
    ImageU8::RGBA iris{};

    // Median of the ring of texels around the eyes: what the hole gets filled
    // with when they are painted out.
    ImageU8::RGBA surround{};

    // 0..1. How much of the found box is actually eye-like, and how well the
    // two halves mirror. Below `FaceScanOptions::minConfidence` the scan
    // reports nothing at all.
    float confidence = 0.0f;

    // Why it gave up, for a scene that wants to say so. Empty on success.
    const char* rejection = "";
};

struct FaceScanOptions {
    // Colour distance, 0..1 over the sRGB cube, for the two tests the scan
    // rests on: how equal a texel and its mirror must be, and how different a
    // texel must be from a neighbour to count as an edge.
    float symmetryTolerance = 0.06f;
    float edgeThreshold = 0.16f;

    // The vertical band of the face searched, as a fraction of its height.
    // Above is hair and forehead, below is mouth and chin; an eye that sits
    // outside this band is not one the scanner claims to handle.
    float bandTop = 0.25f;
    float bandBottom = 0.88f;

    float minConfidence = 0.30f;

    // Say where the entity's right eye is and skip the search entirely. The
    // scanner is exact on every vanilla skin, but "almost always" is not
    // "always", and a scene that can see the render knows better than a
    // heuristic that cannot. Left invalid, the search runs.
    SkinRect rightEyeOverride{};
};

// Look for a mirrored pair of eyes on the head's front face.
//
// The scan does not try to identify skin tone. Two real skins broke that idea
// immediately: on one the most common colour of the face is the *hair* that
// covers it, and on the other the hair is split across so many near-identical
// shades that nothing is common at all. What both skins do have -- and what
// this looks for instead -- is a mirror-symmetric pair of texels that stand
// out from what sits directly above and below them.
EyeScan scanFace(const Skin& skin, const FaceScanOptions& options = {});

// --------------------------------------------------------------- the rig
struct EyeRigOptions {
    FaceScanOptions scan;

    // How far the eye boxes stand out from the face, in model pixels.
    //
    // They only have to clear it, not sit on it. An eye flush with the head is
    // coplanar with it and which of the two a ray finds first is decided by
    // whatever order the boxes happen to sit in -- but a twentieth of a pixel
    // settles that, and it is the number that matters here. Six tenths, the
    // first value tried, is nearly four centimetres of relief on a twelve
    // centimetre eye: the sides of the box catch their own light and the
    // character ends up with eyes stuck onto its face rather than in it.
    float relief = 0.05f;

    // And a hair more for the pupil, so it clears its own white.
    float pupilRelief = 0.03f;

    // Depth of the boxes. Comfortably more than the relief, so the back of an
    // eye stays buried in the head however far the pupil is moved.
    float depth = 0.5f;
};

struct EyeRig {
    bool built = false;
    EyeScan scan;

    // The whites, parented to the head. Move these and the whole eye slides
    // across the cheek -- which is a deformed face, not a glance.
    int right = -1;
    int left = -1;

    // The pupils, parented to their own eye. These are what a glance moves:
    // a look is the dark part travelling *inside* the white, and an eye drawn
    // as a single box with both painted on it cannot do that at all.
    int rightPupil = -1;
    int leftPupil = -1;

    // How far a pupil can travel before it leaves the white, in model pixels.
    // `gaze` works in fractions of this.
    Vec2 gazeTravel{};

    const char* rejection = "";
};

// Point both pupils, in fractions of their travel: x from -1 (the entity's
// own left) to +1 (its right), y from -1 (down) to +1 (up). Values outside
// that are clamped, because a pupil past the edge of its white is not a
// harder stare, it is a hole in the face.
void gaze(const EyeRig& rig, Pose& pose, float x, float y = 0.0f);

// Scan, paint the found eyes out of the skin, and hang two new eye boxes off
// the head joint.
//
// `skin` is modified. It has to be: attach() only ever *adds* geometry, so
// leaving the painted eyes where they are gives a character who glances
// sideways four of them. The originals are filled in with the colour that
// surrounds them, and a freshly drawn pair -- built from this character's own
// sclera and iris, not from a default palette -- is written into a region of
// the image that no part of the model was using.
//
// Returns with `built == false` and the skin untouched when the scan found
// nothing it believed, or when the skin has no free room for the new texels.
EyeRig buildEyeRig(EntityModel& model, Skin& skin, int headJoint = joint::Head,
                   const EyeRigOptions& options = {});

} // namespace face
} // namespace blocky
