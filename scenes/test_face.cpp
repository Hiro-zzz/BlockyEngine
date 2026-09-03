// What the eye scanner actually finds, measured rather than asserted.
//
// Detection on real data is the part of the face rig that can quietly be
// wrong, and "it worked on the two skins I tried" is not a result. So this
// runs the scanner over every player skin in the game jar plus whatever is
// passed on the command line, and prints a hit rate.
//
//   scene_test_face                 the jar's own skins
//   scene_test_face <png> <png>...  those files as well
#include "engine/assets/asset_source.hpp"
#include "engine/core/file.hpp"
#include "engine/entity/face.hpp"
#include "engine/entity/model.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace blocky;

namespace {

void report(const std::string& name, const Skin& skin) {
    face::EyeScan scan = face::scanFace(skin);

    std::printf("  %-34s ", name.c_str());
    if (!scan.found) {
        std::printf("MISS  (%s)\n", scan.rejection);
        return;
    }
    std::printf("%-6s %dx%d at (%d,%d)  conf %.2f  sclera %02X%02X%02X  iris %02X%02X%02X\n",
                face::shapeName(scan.shape), scan.right.width, scan.right.height, scan.right.x,
                scan.right.y, scan.confidence, scan.sclera.r, scan.sclera.g, scan.sclera.b,
                scan.iris.r, scan.iris.g, scan.iris.b);
}

} // namespace

int main(int argc, char** argv) {
    int total = 0, found = 0;

    std::string jar = AssetSource::findClientJar();
    AssetSource source;
    if (!jar.empty() && source.open(jar, nullptr)) {
        std::printf("skins from %s\n", source.description().c_str());

        std::vector<std::string> paths;
        for (const std::string& entry : source.list("assets/minecraft/textures/entity/player/")) {
            if (entry.size() > 4 && entry.compare(entry.size() - 4, 4, ".png") == 0) {
                paths.push_back(entry);
            }
        }

        for (const std::string& path : paths) {
            Skin skin;
            if (!skin.loadFromSource(source, path, nullptr)) continue;
            ++total;

            std::string leaf = path.substr(path.rfind('/') + 1);
            report(leaf, skin);
            if (face::scanFace(skin).found) ++found;
        }
    } else {
        std::printf("no game assets found; command-line skins only\n");
    }

    for (int i = 1; i < argc; ++i) {
        std::vector<uint8_t> bytes;
        Skin skin;
        std::string error;
        if (!readFileBytes(argv[i], bytes, &error) ||
            !skin.loadFromPng(bytes.data(), bytes.size(), &error)) {
            std::printf("  %-34s could not load: %s\n", argv[i], error.c_str());
            continue;
        }
        ++total;

        std::string leaf = std::string(argv[i]);
        size_t cut = leaf.find_last_of("/\\");
        if (cut != std::string::npos) leaf = leaf.substr(cut + 1);
        report(leaf, skin);
        if (face::scanFace(skin).found) ++found;
    }

    std::printf("\n%d of %d skins scanned successfully (%.0f%%)\n", found, total,
                total > 0 ? 100.0 * double(found) / double(total) : 0.0);

    // ---- the part that is a test rather than a measurement
    //
    // A hit rate is not a correctness claim: a scanner that answers "2x1 at
    // (9,12)" for everything would score 100%. Vanilla Steve is ground truth
    // read straight out of the jar, so it can be asserted exactly.
    int failures = 0;
    auto check = [&](bool condition, const char* what) {
        if (!condition) {
            std::printf("  FAIL  %s\n", what);
            ++failures;
        }
    };

    Skin steve;
    if (!jar.empty() && steve.loadFromSource(
                            source, "assets/minecraft/textures/entity/player/wide/steve.png",
                            nullptr)) {
        std::printf("\nexact checks on vanilla steve\n");

        face::EyeScan scan = face::scanFace(steve);
        check(scan.found, "steve's eyes are found at all");
        // Read off the skin by hand: the eye is the two texels at (9,12),
        // white beside a purple iris, and its mirror at (13,12).
        check(scan.right.x == 9 && scan.right.y == 12, "the right eye is at (9,12)");
        check(scan.right.width == 2 && scan.right.height == 1, "it is two texels by one");
        check(scan.left.x == 13 && scan.left.y == 12, "the left eye is its mirror at (13,12)");
        check(scan.shape == face::EyeShape::Wide, "a two-by-one eye is wide, not narrow");
        check(scan.sclera.r == 0xFF && scan.sclera.g == 0xFF && scan.sclera.b == 0xFF,
              "the sclera is the white of the eye");
        check(scan.iris.r == 0x52 && scan.iris.g == 0x3D && scan.iris.b == 0x89,
              "the iris is steve's purple");

        // The entity's right is +X, and +X reads the *left* half of the face
        // rectangle. Getting this backwards is invisible on a symmetric face
        // and wrong on every other one.
        SkinRect faceRect = steve.faceRect(PartHead, LayerBase, SkinFaceFront);
        check(scan.right.x < faceRect.x + faceRect.width / 2,
              "the entity's right eye sits in the left half of the texture");

        // ---- and the rig
        Skin working = steve;
        EntityModel model = buildPlayerModel(working);
        const size_t jointsBefore = model.skeleton.size();
        const size_t boxesBefore = model.boxes.size();

        face::EyeRig rig = face::buildEyeRig(model, working);
        check(rig.built, "the rig is built");
        check(model.skeleton.size() == jointsBefore + 4, "four joints are added: two eyes, two pupils");
        check(model.boxes.size() == boxesBefore + 4, "and four boxes");
        check(rig.right >= 0 && rig.left >= 0 && rig.right != rig.left,
              "the two eyes are distinct joints");
        check(rig.rightPupil >= 0 && rig.leftPupil >= 0, "and both have a pupil");

        // A pupil hangs off its own eye, not off the head. That is what lets a
        // glance move the dark part while the white stays put; parent it to the
        // head instead and the whole eye slides across the cheek, which is a
        // deformed face rather than a look.
        check(model.skeleton[rig.rightPupil].parent == rig.right,
              "the right pupil is parented to the right eye");
        check(model.skeleton[rig.leftPupil].parent == rig.left,
              "the left pupil is parented to the left eye");

        // And gaze moves exactly those two joints, clamped to the white.
        Pose look;
        face::gaze(rig, look, -3.0f, 0.0f);
        check(look[rig.rightPupil].offset.x < 0.0f, "a negative gaze moves the pupil to -X");
        check(std::fabs(look[rig.rightPupil].offset.x) <= rig.gazeTravel.x + 1e-5f,
              "and never past the edge of its own white");
        check(look[rig.right].offset.x == 0.0f, "the white itself does not move");

        // The claim the whole design rests on: the painted eyes are *gone*.
        // attach() only ever adds, so if this fails a character who glances
        // sideways ends up with four eyes and nothing crashes to say so.
        ImageU8::RGBA repainted = working.image().get(9, 12);
        check(!(repainted.r == 0xFF && repainted.g == 0xFF && repainted.b == 0xFF),
              "the original right eye has been painted out");
        ImageU8::RGBA mirrored = working.image().get(13, 12);
        check(!(mirrored.r == 0xFF && mirrored.g == 0xFF && mirrored.b == 0xFF),
              "so has the original left eye");
        check(working.image().get(9, 12).r == working.image().get(13, 12).r,
              "both were filled with the same surrounding colour");

        // The generated pair went somewhere no part of the model was reading.
        check(working.model() == steve.model(), "repainting did not change the arm width");

        // Nothing was disturbed that should not have been: the mouth is one
        // row below the eyes and must survive untouched.
        check(working.image().get(11, 13).r == steve.image().get(11, 13).r,
              "the rest of the face is untouched");
    } else {
        std::printf("\nno game assets: the exact checks need vanilla steve, skipping\n");
    }

    if (failures == 0) {
        std::printf("\nall face tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", failures);
    return 1;
}
