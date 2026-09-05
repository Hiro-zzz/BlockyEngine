#include "plugins/forge/selftest.hpp"

#include "engine/core/file.hpp"
#include "engine/edit/emit.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/scene/camera.hpp"

#include "plugins/forge/sculptview.hpp"
#include "plugins/forge/studio.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace forge {

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

float worstDifference(const Mat4& a, const Mat4& b) {
    float worst = 0.0f;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            worst = std::max(worst, std::fabs(a.m[column][row] - b.m[column][row]));
    return worst;
}

// ------------------------------------------------------------ one transform
void testTransformIsShared() {
    std::printf("transform\n");

    Studio studio;
    studio.begin();
    studio.sculpt.place({8, 0, 8});

    const IVec3 dims = studio.sculpt.dims();
    const Mat4 toWorld = modelToWorld(dims);

    PropSet props;
    props.addTransformed(&studio.sculpt.model(), toWorld);
    props.build();

    check(props.flats().size() == 1, "the model reaches the set");
    if (props.flats().empty()) return;

    // The renderer's forward matrix has to be the one it was handed, not one
    // reconstructed from a position and three angles.
    check(worstDifference(props.flats()[0].toWorld, toWorld) == 0.0f,
          "the renderer places the model by exactly the matrix it was given");

    // And the cursor's inverse has to be the renderer's inverse. Not
    // bit-identical -- both are computed, and by different code -- but
    // agreeing far past anything a voxel could tell apart.
    const Mat4 picking = inverseAffine(toWorld);
    check(worstDifference(props.flats()[0].toLocal, picking) < 1e-5f,
          "and the cursor inverts the same matrix the renderer did");

    // The round trip, which is the property both of them actually rely on.
    const Vec3 probe{3.25f, 7.5f, 11.75f};
    const Vec3 back = transformPoint(picking, transformPoint(toWorld, probe));
    check(std::fabs(back.x - probe.x) < 1e-4f && std::fabs(back.y - probe.y) < 1e-4f &&
              std::fabs(back.z - probe.z) < 1e-4f,
          "a voxel coordinate survives the trip out to the world and back");
}

// ---------------------------------------------------------- plate and model
void testPlateCarriesTheModel() {
    std::printf("plate\n");

    const IVec3 dims{16, 16, 16};

    // The top of the plate and the bottom of the model are the same plane:
    // the model stands on it, and neither is inside the other.
    const float plateTop = transformPoint(plateToWorld(dims), Vec3{0.0f, 1.0f, 0.0f}).y;
    const float modelBottom = transformPoint(modelToWorld(dims), Vec3{0.0f, 0.0f, 0.0f}).y;
    check(std::fabs(plateTop - modelBottom) < 1e-6f, "the model stands on the plate");

    const float plateBottom = transformPoint(plateToWorld(dims), Vec3{0.0f, 0.0f, 0.0f}).y;
    check(std::fabs(plateBottom) < 1e-6f, "and the plate stands on the floor");

    // Footprints match, which is the whole reason the plate is a prop.
    VoxelModel plate;
    buildPlate(plate, dims);
    check(plate.dims() == IVec3(dims.x, 1, dims.z), "the plate is the model's footprint");

    const Vec3 plateCorner = transformPoint(plateToWorld(dims), Vec3{float(dims.x), 0.0f, 0.0f});
    const Vec3 modelCorner = transformPoint(modelToWorld(dims), Vec3{float(dims.x), 0.0f, 0.0f});
    check(std::fabs(plateCorner.x - modelCorner.x) < 1e-6f &&
              std::fabs(plateCorner.z - modelCorner.z) < 1e-6f,
          "and lines up with it in x and z");
}

// ----------------------------------------------------------------- pointing
//
// The one that would actually catch a wrong cursor. Put a voxel somewhere,
// point the camera at it the way the orbit does, fire the ray the way the
// view does, and demand the pick name that voxel.
void testPointingHitsWhatIsShown() {
    std::printf("pointing\n");

    Studio studio;
    studio.begin();

    // A single column in the middle of the grid, so a ray through the centre
    // of the screen has exactly one thing it can be pointing at.
    const uint16_t material = studio.voxelMaterial();
    for (int y = 0; y < 6; ++y) studio.sculpt.place({8, y, 8}, material);

    Orbit orbit;
    orbit.frame(studio.sculpt);

    Camera camera;
    orbit.apply(camera, 16.0f / 9.0f);

    const Mat4 toLocal = inverseAffine(modelToWorld(studio.sculpt.dims()));
    const Ray ray = camera.generateRay(0.5f, 0.5f);

    const edit::Pick pick =
        studio.sculpt.pick(transformPoint(toLocal, ray.origin), transformDir(toLocal, ray.direction));

    check(pick.hit, "the centre of the screen points at the model");
    if (pick.hit) {
        check(pick.voxel.x == 8 && pick.voxel.z == 8, "at the column that is there");
        check(pick.voxel.y >= 0 && pick.voxel.y < 6, "and within its height");

        // The placement cell is one step away and empty -- the property the
        // whole place/erase pair rests on.
        const IVec3 step = pick.adjacent - pick.voxel;
        check(std::abs(step.x) + std::abs(step.y) + std::abs(step.z) == 1,
              "the placement cell is one step off");
        check(studio.sculpt.at(pick.adjacent) == VoxelModel::kEmpty ||
                  !studio.sculpt.inside(pick.adjacent),
              "and is somewhere a voxel can go");
    }

    // A ray out of the corner of the same camera must miss: a picker that
    // hits everywhere is as wrong as one that hits nowhere, and only the
    // second is obvious.
    const Ray corner = camera.generateRay(0.02f, 0.02f);
    const edit::Pick missed = studio.sculpt.pick(transformPoint(toLocal, corner.origin),
                                                 transformDir(toLocal, corner.direction));
    check(!missed.hit, "and the corner of the screen points at nothing");
}

// ------------------------------------------------------------------- export
//
// `scene_test_edit` proves the emitter. This proves the **button**: that the
// directory gets made, that both files are written under the names the rest
// of the world will look for, and that a refusal is reported instead of
// passing for success. Those are this program's, not the core's.
//
// It writes into `out/`, along with everything else the engine produces, and
// leaves the files there: that directory is output and is not kept.
void testExportWritesBothFiles() {
    std::printf("export\n");

    Studio studio;
    studio.begin();
    studio.name = "selftest";

    const uint16_t material = studio.voxelMaterial();
    studio.sculpt.box({6, 0, 6}, {9, 2, 9}, material);

    const std::string stem = std::string(kOutputDirectory) + "/selftest";
    const std::string sourcePath = stem + ".hpp";
    const std::string sheetPath =
        std::string(kOutputDirectory) + "/" + edit::sheetFileName("selftest", studio.sculpt.dims());

    removeFile(sourcePath);
    removeFile(sheetPath);

    studio.exportDocument();

    check(fileExists(sourcePath), "exporting a model writes the source");
    check(fileExists(sheetPath), "and the sheet, under the name that carries the size");
    check(studio.status.find("failed") == std::string::npos &&
              studio.status.find("refused") == std::string::npos,
          "and says so rather than reporting a failure");

    // The canvas half writes one file, and to the name it was given.
    studio.mode = Mode::Canvas;
    studio.canvas.pencil(3, 3, studio.pixelColour());

    const std::string canvasPath = stem + ".png";
    const std::string canvasSource = stem + ".hpp";
    removeFile(canvasPath);
    removeFile(canvasSource);
    studio.exportDocument();
    check(fileExists(canvasPath), "exporting a canvas writes the PNG");
    check(fileExists(canvasSource), "and the source beside it");

    // Sprites go out as source and only as source: a list of quads has no
    // picture form, and the button has to say so rather than write half of
    // something.
    studio.mode = Mode::Sprites;
    Sprite mote;
    mote.position = {0.5f, 0.75f, 0.25f};
    mote.size = {0.1f, 0.1f};
    studio.sprites.add(mote);

    const std::string spritePath = stem + "_sprites.hpp";
    removeFile(spritePath);
    studio.exportDocument();
    check(fileExists(spritePath), "exporting sprites writes the source");

    studio.exportForMinecraft();
    check(studio.status.find("no format") != std::string::npos,
          "and the pack button refuses them with a reason");

    // And the other button, which writes somebody else's format.
    studio.mode = Mode::Sculpt;
    const std::string pack = std::string(kOutputDirectory) + "/pack";
    const std::string packModel = pack + "/assets/forge/models/item/selftest.json";
    removeFile(packModel);

    studio.exportForMinecraft();
    check(fileExists(packModel), "the pack button writes a model");
    check(fileExists(pack + "/assets/forge/textures/item/selftest.png"), "and its palette");
    check(fileExists(pack + "/pack.mcmeta"), "and the mcmeta that makes it loadable");
    check(studio.status.find("refused") == std::string::npos &&
              studio.status.find("failed") == std::string::npos,
          "and reports what it wrote");
}

} // namespace

int runSelfTest() {
    gFailures = 0;

    testTransformIsShared();
    testPlateCarriesTheModel();
    testPointingHitsWhatIsShown();
    testExportWritesBothFiles();

    if (gFailures == 0) {
        std::printf("\nall forge checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}

} // namespace forge
