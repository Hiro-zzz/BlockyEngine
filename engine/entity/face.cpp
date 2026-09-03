#include "engine/entity/face.hpp"
#include "engine/entity/rigging.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace blocky {
namespace face {
namespace {

using RGBA = ImageU8::RGBA;

// Distance over the sRGB cube, 0..1. Kept in sRGB rather than linear on
// purpose: this is a question about what a texel looks like, not about how
// much light it carries, and linearising would pull every dark colour
// together exactly where eyes tend to live.
float distance(RGBA a, RGBA b) {
    float dr = (float(a.r) - float(b.r)) / 255.0f;
    float dg = (float(a.g) - float(b.g)) / 255.0f;
    float db = (float(a.b) - float(b.b)) / 255.0f;
    return std::sqrt((dr * dr + dg * dg + db * db) / 3.0f);
}

float brightness(RGBA c) {
    return (0.2126f * float(c.r) + 0.7152f * float(c.g) + 0.0722f * float(c.b)) / 255.0f;
}

// A candidate eye, in coordinates local to the face rect.
struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    int area() const { return w * h; }
};

RGBA medianColour(std::vector<RGBA>& samples) {
    if (samples.empty()) return RGBA{0, 0, 0, 255};

    // Median by brightness, then take that whole sample: averaging would
    // invent a colour that is nowhere in the skin, and on a two-tone face
    // that shows as a seam.
    std::sort(samples.begin(), samples.end(),
              [](RGBA a, RGBA b) { return brightness(a) < brightness(b); });
    return samples[samples.size() / 2];
}

} // namespace

const char* shapeName(EyeShape shape) { return shape == EyeShape::Wide ? "wide" : "narrow"; }

EyeScan scanFace(const Skin& skin, const FaceScanOptions& options) {
    EyeScan scan;

    if (!skin.valid()) {
        scan.rejection = "the skin is empty";
        return scan;
    }

    const SkinRect faceRect = skin.faceRect(PartHead, LayerBase, SkinFaceFront);
    if (!faceRect.valid() || faceRect.width < 4 || faceRect.height < 4) {
        scan.rejection = "the head has no usable front face";
        return scan;
    }

    const ImageU8& image = skin.image();
    const int w = faceRect.width;
    const int h = faceRect.height;
    const int half = w / 2;

    auto at = [&](int x, int y) { return image.get(faceRect.x + x, faceRect.y + y); };

    // Every rectangle in the left half of the face is scored, and the best one
    // wins. The search space is tiny -- a few hundred boxes on an eight-pixel
    // face -- and searching it beats growing regions outright: an eye sits one
    // texel below a brow and one above a cheek, both of which are edges too, so
    // anything that grows merges all three into a band and loses the eye.
    const int bandTop = std::max(0, int(float(h) * options.bandTop));
    const int bandBottom = std::min(h, int(float(h) * options.bandBottom));
    const int maxHeight = std::max(1, h / 2);

    auto mirrors = [&](int x, int y) {
        return distance(at(x, y), at(w - 1 - x, y)) <= options.symmetryTolerance;
    };

    Box best;
    float bestScore = 0.0f;
    bool anyMirrored = false;

    // An explicit answer skips the search entirely. Everything below it --
    // the mirror, the colours, the fill -- is still derived, so overriding
    // costs the caller one rectangle and not a whole hand-built rig.
    if (options.rightEyeOverride.valid()) {
        const SkinRect& given = options.rightEyeOverride;
        best = {given.x - faceRect.x, given.y - faceRect.y, given.width, given.height};

        if (best.x < 0 || best.y < 0 || best.x + best.w > half || best.y + best.h > h) {
            scan.rejection = "the override is not inside the left half of the face";
            return scan;
        }
        bestScore = 1.0f;
        anyMirrored = true;
    } else {
    for (int y = bandTop; y < bandBottom; ++y) {
        for (int boxH = 1; boxH <= maxHeight && y + boxH <= bandBottom; ++boxH) {
            for (int x = 0; x < half - 1; ++x) {
                // The last column before the centre line is left out on
                // purpose: every face carries a nose, a muzzle or simply skin
                // between the eyes, and a candidate that reaches the midline
                // has swallowed it. Vanilla, and both hand-drawn skins tested,
                // stop one column short.
                for (int boxW = 1; x + boxW <= half - 1; ++boxW) {
                    Box box{x, y, boxW, boxH};

                    // ---- must mirror, all of it
                    bool symmetric = true;
                    for (int by = y; by < y + boxH && symmetric; ++by) {
                        for (int bx = x; bx < x + boxW; ++bx) {
                            if (!mirrors(bx, by)) { symmetric = false; break; }
                        }
                    }
                    if (!symmetric) continue;
                    anyMirrored = true;

                    // ---- an eye holds a bright part and a dark part
                    // This is what kills the single stray texel: one texel has
                    // no contrast with itself, so it scores zero however
                    // perfectly it mirrors.
                    float lightest = 0.0f, darkest = 1.0f;
                    for (int by = y; by < y + boxH; ++by) {
                        for (int bx = x; bx < x + boxW; ++bx) {
                            float b = brightness(at(bx, by));
                            lightest = std::max(lightest, b);
                            darkest = std::min(darkest, b);
                        }
                    }
                    const float span = lightest - darkest;
                    // Softened rather than used raw: the difference between a
                    // vanilla eye and a pale drawn one is threefold in raw
                    // span and should not be threefold in score.
                    const float contrast = span / (span + 0.25f);

                    // ---- and it has a boundary all the way round
                    // A box drawn one column too wide includes a texel whose
                    // outward neighbour is the same skin colour, and this is
                    // what notices.
                    int borders = 0, edges = 0;
                    auto outward = [&](int bx, int by, int dx, int dy) {
                        const int nx = bx + dx, ny = by + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;   // clipped, no claim
                        ++borders;
                        if (distance(at(bx, by), at(nx, ny)) > options.edgeThreshold) ++edges;
                    };
                    for (int by = y; by < y + boxH; ++by) {
                        outward(x, by, -1, 0);
                        outward(x + boxW - 1, by, 1, 0);
                    }
                    for (int bx = x; bx < x + boxW; ++bx) {
                        outward(bx, y, 0, -1);
                        outward(bx, y + boxH - 1, 0, 1);
                    }
                    const float edgeness = borders > 0 ? float(edges) / float(borders) : 0.0f;

                    // ---- and it is small
                    const float sizePrior =
                        std::max(0.4f, 1.0f - float(box.area() - 2) / float(std::max(4, w * h / 4)));

                    const float score = contrast * edgeness * sizePrior;
                    if (score > bestScore) {
                        bestScore = score;
                        best = box;
                    }
                }
            }
        }
    }

    }

    if (!anyMirrored) {
        scan.rejection = "nothing on this face mirrors about its centre";
        return scan;
    }

    scan.confidence = bestScore;
    if (bestScore < options.minConfidence || best.area() == 0) {
        scan.rejection = "no candidate looked enough like an eye";
        return scan;
    }

    // The entity's right eye is the one in the *left* half of the texture: a
    // skin stores each face as a viewer sees it, so +X reads the left half.
    scan.right = {faceRect.x + best.x, faceRect.y + best.y, best.w, best.h};
    scan.left = {faceRect.x + (w - best.x - best.w), faceRect.y + best.y, best.w, best.h};

    scan.shape = best.h > best.w ? EyeShape::Narrow : EyeShape::Wide;

    RGBA lightest = at(best.x, best.y);
    RGBA darkest = lightest;
    for (int by = best.y; by < best.y + best.h; ++by) {
        for (int bx = best.x; bx < best.x + best.w; ++bx) {
            RGBA c = at(bx, by);
            if (brightness(c) > brightness(lightest)) lightest = c;
            if (brightness(c) < brightness(darkest)) darkest = c;
        }
    }
    scan.sclera = lightest;
    scan.iris = darkest;

    // The face an eye is set into, for filling the hole it leaves behind.
    //
    // **Not** the whole ring around it. The row directly above an eye is a
    // brow or a fringe on almost every humanoid skin, and on one whose hair is
    // cut close to the eyes it wins the median outright. That single texel row
    // then decides three things at once: what the painted-out original is
    // filled with, what the five hidden faces of the new eye box are clothed
    // in, and therefore what colour appears the instant anything moves an eye
    // or shuts it. A character ends up with a band of hair colour across the
    // middle of its face, and the cause is nowhere near the symptom.
    //
    // What is reliably *face* is the cheek directly below and the temple
    // either side at eye level. Those are what this takes.
    auto sample = [&](std::vector<RGBA>& into, int bx, int by) {
        if (bx >= 0 && by >= 0 && bx < w && by < h) into.push_back(at(bx, by));
    };

    std::vector<RGBA> ring;
    for (int bx = best.x - 1; bx <= best.x + best.w; ++bx) sample(ring, bx, best.y + best.h);
    for (int by = best.y; by < best.y + best.h; ++by) {
        sample(ring, best.x - 1, by);
        sample(ring, best.x + best.w, by);
    }

    // An eye hard against the edge of the face rectangle has neither. Falling
    // back to the full ring is worse than the cheek and better than nothing,
    // and the scan's own band limits make it a case that should not arise.
    if (ring.empty()) {
        for (int by = best.y - 1; by <= best.y + best.h; ++by) {
            for (int bx = best.x - 1; bx <= best.x + best.w; ++bx) {
                if (bx >= best.x && bx < best.x + best.w && by >= best.y &&
                    by < best.y + best.h) {
                    continue;
                }
                sample(ring, bx, by);
            }
        }
    }
    scan.surround = medianColour(ring);

    scan.found = true;
    return scan;
}

// ---------------------------------------------------------------- the rig
namespace {

// Which texels of the skin no part of the model reads.
//
// Computed from the layout rather than assumed, because "the 64x64 skin has
// spare room at the top left" is the kind of fact that is true until it is
// not. Every rectangle the Skin can hand out is marked, and what is left over
// is genuinely free.
std::vector<uint8_t> coverageMask(const Skin& skin) {
    const ImageU8& image = skin.image();
    std::vector<uint8_t> used(size_t(image.width()) * size_t(image.height()), 0);

    for (int part = 0; part < PartCount; ++part) {
        for (int layer = 0; layer < LayerCount; ++layer) {
            if (!skin.hasLayer(SkinPart(part), SkinLayer(layer))) continue;
            for (int f = 0; f < SkinFaceCount; ++f) {
                SkinRect r = skin.faceRect(SkinPart(part), SkinLayer(layer), SkinFace(f));
                if (!r.valid()) continue;
                for (int y = r.y; y < r.y + r.height; ++y) {
                    for (int x = r.x; x < r.x + r.width; ++x) {
                        if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) continue;
                        used[size_t(y) * size_t(image.width()) + size_t(x)] = 1;
                    }
                }
            }
        }
    }
    return used;
}

bool findFreeRegion(const Skin& skin, int width, int height, SkinRect& out) {
    const ImageU8& image = skin.image();
    const std::vector<uint8_t> used = coverageMask(skin);

    for (int y = 0; y + height <= image.height(); ++y) {
        for (int x = 0; x + width <= image.width(); ++x) {
            bool clear = true;
            for (int dy = 0; dy < height && clear; ++dy) {
                for (int dx = 0; dx < width; ++dx) {
                    if (used[size_t(y + dy) * size_t(image.width()) + size_t(x + dx)]) {
                        clear = false;
                        break;
                    }
                }
            }
            if (clear) {
                out = {x, y, width, height};
                return true;
            }
        }
    }
    return false;
}

} // namespace

EyeRig buildEyeRig(EntityModel& model, Skin& skin, int headJoint,
                   const EyeRigOptions& options) {
    EyeRig rig;

    if (headJoint < 0 || headJoint >= int(model.skeleton.size())) {
        rig.rejection = "the model has no such joint";
        return rig;
    }

    rig.scan = scanFace(skin, options.scan);
    if (!rig.scan.found) {
        rig.rejection = rig.scan.rejection;
        return rig;
    }

    const SkinRect& right = rig.scan.right;
    const SkinRect& left = rig.scan.left;
    const int eyeW = right.width;
    const int eyeH = right.height;

    // ---- somewhere to draw the new eyes
    //
    // Three patches: the white of the eye, one texel of iris for the pupil,
    // and one of the surrounding colour for the five faces of a box that are
    // not its front. Both eyes share the white, because a flat colour has no
    // handedness -- it is the pupil that will differ, and the pupil is its own
    // box.
    SkinRect scratch;
    if (!findFreeRegion(skin, eyeW + 2, eyeH, scratch)) {
        rig.rejection = "the skin has no free region for the generated eyes";
        return rig;
    }

    ImageU8 patched = skin.image();

    // Paint the originals out. attach() only ever adds, so a pupil that moves
    // over a painted eye still left in place gives the character two of them.
    for (const SkinRect* eye : {&right, &left}) {
        for (int y = eye->y; y < eye->y + eye->height; ++y) {
            for (int x = eye->x; x < eye->x + eye->width; ++x) {
                patched.set(x, y, rig.scan.surround);
            }
        }
    }

    const SkinRect scleraTarget{scratch.x, scratch.y, eyeW, eyeH};
    for (int y = 0; y < eyeH; ++y) {
        for (int x = 0; x < eyeW; ++x) {
            patched.set(scleraTarget.x + x, scleraTarget.y + y, rig.scan.sclera);
        }
    }

    const SkinRect irisTarget{scratch.x + eyeW, scratch.y, 1, 1};
    patched.set(irisTarget.x, irisTarget.y, rig.scan.iris);

    const SkinRect sideTarget{scratch.x + eyeW + 1, scratch.y, 1, 1};
    patched.set(sideTarget.x, sideTarget.y, rig.scan.surround);

    if (!skin.replaceImage(patched)) {
        rig.rejection = "the skin refused the repaint";
        return rig;
    }

    // ---- geometry
    //
    // The head box spans x [-4, 4] and y [24, 32], and its front face maps
    // u = 1 - x/8, v = 1 - y/8 (see boxFaceUv). So a texel at column `bx` of
    // the face sits at x = 4 - bx, and one at row `by` sits at y = 32 - by.
    // Getting this backwards puts the eyes on the back of the head, which at
    // least fails loudly.
    const SkinRect faceRect = skin.faceRect(PartHead, LayerBase, SkinFaceFront);
    const float faceX = float(faceRect.x);
    const float faceY = float(faceRect.y);

    // The pupil is a fraction of the eye, leaving white either side of it for
    // the gaze to travel across. A narrow eye keeps a tall pupil, which is
    // what a drawn skin paints; a vanilla two-by-one gets a single texel.
    const int pupilW = std::max(1, eyeW / 2);
    const int pupilH = rig.scan.shape == EyeShape::Narrow ? std::max(1, eyeH - 1)
                                                          : std::max(1, (eyeH + 1) / 2);
    rig.gazeTravel = {0.5f * float(eyeW - pupilW), 0.5f * float(eyeH - pupilH)};

    auto attachEye = [&](const SkinRect& eye, const char* eyeName, const char* pupilName,
                         int& eyeJoint, int& pupilJoint) {
        const float bx = float(eye.x) - faceX;
        const float by = float(eye.y) - faceY;

        const Vec3 origin{4.0f - bx - float(eyeW), 32.0f - by - float(eyeH),
                          -4.0f - options.relief};

        rigging::Attachment white;
        white.name = eyeName;
        white.parent = headJoint;
        white.origin = origin;
        white.size = {float(eyeW), float(eyeH), options.depth};
        white.pivot = origin + white.size * 0.5f;
        white.part = PartHead;
        white.layer = LayerBase;
        for (int f = 0; f < SkinFaceCount; ++f) white.faces[f] = sideTarget;
        white.faces[SkinFaceFront] = scleraTarget;

        eyeJoint = rigging::attach(model, white);
        if (eyeJoint < 0) return;

        // The pupil hangs off the eye, not off the head, so moving the eye
        // takes the pupil with it and moving the pupil leaves the white where
        // it is. That is the whole difference between a glance and a face
        // with its eyes slid sideways.
        rigging::Attachment pupil;
        pupil.name = pupilName;
        pupil.parent = eyeJoint;
        pupil.origin = {origin.x + 0.5f * float(eyeW - pupilW),
                        origin.y + 0.5f * float(eyeH - pupilH),
                        origin.z - options.pupilRelief};
        pupil.size = {float(pupilW), float(pupilH), options.depth};
        pupil.pivot = pupil.origin + pupil.size * 0.5f;
        pupil.part = PartHead;
        pupil.layer = LayerBase;
        for (int f = 0; f < SkinFaceCount; ++f) pupil.faces[f] = irisTarget;

        pupilJoint = rigging::attach(model, pupil);
    };

    attachEye(right, "rightEye", "rightPupil", rig.right, rig.rightPupil);
    attachEye(left, "leftEye", "leftPupil", rig.left, rig.leftPupil);

    if (rig.right < 0 || rig.left < 0 || rig.rightPupil < 0 || rig.leftPupil < 0) {
        rig.rejection = "the eye boxes could not be attached";
        return rig;
    }

    rig.built = true;
    return rig;
}

void gaze(const EyeRig& rig, Pose& pose, float x, float y) {
    if (!rig.built) return;

    const float clampedX = std::max(-1.0f, std::min(1.0f, x));
    const float clampedY = std::max(-1.0f, std::min(1.0f, y));
    const Vec3 shift{clampedX * rig.gazeTravel.x, clampedY * rig.gazeTravel.y, 0.0f};

    // Both pupils by the same amount. Real eyes converge on a near target and
    // these do not, which at a metre and a half of camera distance is a
    // difference nobody can see -- and giving each pupil its own angle would
    // cross them the moment the head turns.
    pose[rig.rightPupil].offset = shift;
    pose[rig.leftPupil].offset = shift;
}

} // namespace face
} // namespace blocky
