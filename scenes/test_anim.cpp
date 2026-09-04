// Tests for the animation layer: timing, easing, tracks, blending, the take
// driver and the APNG writer.
//
// Two of these are worth explaining, because they are the ones that catch the
// bugs this module can actually have.
//
// **The APNG is parsed by code that is not the writer.** A writer checked by
// its own reader agrees with itself and proves nothing; the chunk parser here
// walks the bytes from scratch, recomputes every CRC, and knows the layout
// from the specification rather than from png.cpp. It then hands the same
// bytes to our *still* decoder and demands frame one back -- which is the
// whole promise of APNG, that a viewer which has never heard of animation
// still sees a picture.
//
// **A hold has to be free and it has to be exact.** Animating on twos is
// worth having because the second frame of a pair costs nothing, and that is
// only true if it is a byte copy rather than a second trace. So the take test
// asserts both halves: that the driver traced exactly half the frames, and
// that each held frame is byte-identical to the one it holds. Either of those
// can break silently -- a hold that is quietly re-traced still produces a
// correct animation, just at twice the price, and nobody would notice.
//
// The collapse has a matching invariant. Runs of identical frames become one
// APNG frame with a longer delay, and losing time there would be invisible in
// any single frame: the animation would simply run short. So the delays are
// summed and compared against the take's duration.
//
// Needs no game files.
#include "engine/anim/blend.hpp"
#include "engine/anim/take.hpp"
#include "engine/anim/track.hpp"
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/scene/scene.hpp"
#include "scenes/common/palette.hpp"

#include <cmath>
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

bool nearly(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }
bool nearlyVec(Vec3 a, Vec3 b, float tolerance = 1e-5f) {
    return nearly(a.x, b.x, tolerance) && nearly(a.y, b.y, tolerance) &&
           nearly(a.z, b.z, tolerance);
}

// ------------------------------------------------------------------- timing
void testTiming() {
    std::printf("timing\n");

    Timing timing;
    timing.duration = 4.0f;
    timing.fps = 24;
    timing.stepEvery = 2;

    check(timing.frameCount() == 96, "four seconds at 24 fps is 96 frames");
    check(timing.renderedFrameCount() == 48, "on twos, half of them are traced");

    // The defining property: a pair shares one instant, and the second of the
    // pair is flagged as a repeat of the first.
    const Frame a = timing.frameAt(0);
    const Frame b = timing.frameAt(1);
    const Frame c = timing.frameAt(2);
    check(!a.held && b.held && !c.held, "on twos, every second frame is held");
    check(nearly(a.time, b.time), "a held frame is handed the same instant as its leader");
    check(!nearly(b.time, c.time), "the next pair is handed a different instant");
    check(nearly(c.time, 2.0f / 24.0f), "the instant is the leader index over the frame rate");
    check(nearly(a.dt, 2.0f / 24.0f), "dt is one step, not one frame");

    // Counting the held flags independently must agree with the arithmetic.
    int traced = 0;
    for (int i = 0; i < timing.frameCount(); ++i) {
        if (!timing.frameAt(i).held) ++traced;
    }
    check(traced == timing.renderedFrameCount(), "held flags agree with renderedFrameCount");

    check(nearly(timing.frameAt(0).t01, 0.0f), "t01 starts at zero");
    check(timing.frameAt(95).t01 < 1.0f, "the last frame lands short of one, so a loop closes");

    timing.stepEvery = 1;
    int heldOnOnes = 0;
    for (int i = 0; i < timing.frameCount(); ++i) {
        if (timing.frameAt(i).held) ++heldOnOnes;
    }
    check(heldOnOnes == 0, "on ones nothing is held");
    check(timing.renderedFrameCount() == 96, "on ones every frame is traced");

    // Threes over a count that does not divide by three: the last group is
    // short, and the arithmetic still has to agree with the flags.
    timing.stepEvery = 3;
    timing.duration = 1.0f;   // 24 frames -> 8 groups of 3
    traced = 0;
    for (int i = 0; i < timing.frameCount(); ++i) {
        if (!timing.frameAt(i).held) ++traced;
    }
    check(traced == timing.renderedFrameCount(), "threes: flags agree with the count");

    timing.duration = 1.0f;
    timing.fps = 25;   // 25 frames, 3 does not divide it
    traced = 0;
    for (int i = 0; i < timing.frameCount(); ++i) {
        if (!timing.frameAt(i).held) ++traced;
    }
    check(traced == timing.renderedFrameCount(), "a ragged last group still counts right");

    // Degenerate settings must not divide by zero or produce nothing.
    Timing broken;
    broken.fps = 0;
    broken.stepEvery = 0;
    broken.duration = 0.0f;
    check(broken.frameCount() >= 1, "a degenerate timing still yields at least one frame");
    const Frame only = broken.frameAt(0);
    check(!only.held && only.dt > 0.0f, "and that frame is sane");
}

// ------------------------------------------------------------------- easing
void testEasing() {
    std::printf("easing\n");

    // Endpoints. Every curve has to start where it starts and arrive.
    struct Named { const char* name; ease::Curve curve; };
    const Named curves[] = {
        {"linear", &ease::linear},   {"smooth", &ease::smooth},
        {"in", &ease::in},           {"out", &ease::out},
        {"hold", ease::hold(0.6f)},  {"back", ease::back()},
        {"anticipate", ease::anticipate()}, {"snap", ease::snap()},
        {"bounce", ease::bounce()},
    };
    for (const Named& n : curves) {
        check(nearly(n.curve(0.0f), 0.0f, 1e-4f), "curve starts at zero");
        check(nearly(n.curve(1.0f), 1.0f, 1e-4f), "curve arrives at one");
    }

    check(nearly(ease::step(0.0f), 0.0f) && nearly(ease::step(0.999f), 0.0f),
          "step holds all the way to the end");
    check(nearly(ease::step(1.0f), 1.0f), "step arrives only at the end");

    // hold(f) is defined by being exactly still for the first f of the
    // segment. If that stopped being true the curve would just be an ease.
    const ease::Curve held = ease::hold(0.6f);
    check(nearly(held(0.0f), 0.0f) && nearly(held(0.3f), 0.0f) && nearly(held(0.599f), 0.0f),
          "hold(0.6) does not move for the first six tenths");
    check(held(0.8f) > 0.0f, "and then it moves");

    // Anticipation that never pulls back is not anticipation, and an
    // overshoot that never passes the target is not an overshoot. Endpoints
    // alone would let both degrade into a plain ease without complaint.
    float lowest = 1.0f;
    const ease::Curve anticipate = ease::anticipate(0.2f);
    for (int i = 0; i <= 100; ++i) lowest = std::min(lowest, anticipate(float(i) / 100.0f));
    check(lowest < -0.02f, "anticipate really goes backwards first");

    float highest = 0.0f;
    const ease::Curve back = ease::back();
    for (int i = 0; i <= 100; ++i) highest = std::max(highest, back(float(i) / 100.0f));
    check(highest > 1.02f, "back really overshoots the target");

    // Monotone curves must not wobble.
    const ease::Curve monotone[] = {&ease::linear, &ease::smooth, &ease::in, &ease::out,
                                    ease::hold(0.4f)};
    for (const ease::Curve& curve : monotone) {
        bool rising = true;
        float previous = curve(0.0f);
        for (int i = 1; i <= 200; ++i) {
            const float value = curve(float(i) / 200.0f);
            if (value < previous - 1e-6f) rising = false;
            previous = value;
        }
        check(rising, "a monotone curve never goes backwards");
    }

    // Out of range input is clamped rather than extrapolated.
    check(nearly(ease::smooth(-1.0f), 0.0f) && nearly(ease::smooth(2.0f), 1.0f),
          "curves clamp their input");
}

// ------------------------------------------------------------------- tracks
void testTracks() {
    std::printf("tracks\n");

    Track<float> empty;
    check(empty.empty() && nearly(empty.at(0.5f), 0.0f), "an empty track reads as zero");

    Track<float> one(7.0f);
    check(nearly(one.at(-100.0f), 7.0f) && nearly(one.at(100.0f), 7.0f),
          "a single key holds forever in both directions");

    Track<float> ramp;
    ramp.key(1.0f, 10.0f, &ease::linear);
    ramp.key(3.0f, 20.0f);

    check(nearly(ramp.at(1.0f), 10.0f) && nearly(ramp.at(3.0f), 20.0f),
          "a track passes exactly through its keys");
    check(nearly(ramp.at(2.0f), 15.0f), "and interpolates linearly between them");
    check(nearly(ramp.at(0.0f), 10.0f), "before the first key it holds, it does not extrapolate");
    check(nearly(ramp.at(9.0f), 20.0f), "after the last key it holds too");

    // Keys given out of order have to sort themselves, or everything after
    // the first mistake reads from the wrong segment.
    Track<float> shuffled;
    shuffled.key(3.0f, 30.0f, &ease::linear);
    shuffled.key(1.0f, 10.0f, &ease::linear);
    shuffled.key(2.0f, 20.0f, &ease::linear);
    check(nearly(shuffled.at(1.5f), 15.0f) && nearly(shuffled.at(2.5f), 25.0f),
          "keys added out of order still read in order");

    // The curve belongs to the segment that starts at its key, so two
    // segments of one track can have different timing.
    Track<float> mixed;
    mixed.key(0.0f, 0.0f, &ease::step);
    mixed.key(1.0f, 10.0f, &ease::linear);
    mixed.key(2.0f, 20.0f);
    check(nearly(mixed.at(0.5f), 0.0f), "a stepped segment holds its start value");
    check(nearly(mixed.at(1.5f), 15.0f), "while the next segment eases normally");

    Track<float> slideshow;
    slideshow.stepped();
    slideshow.key(0.0f, 0.0f);
    slideshow.key(1.0f, 5.0f);
    check(nearly(slideshow.at(0.99f), 0.0f), "stepped() makes the whole track a slideshow");

    // Two keys at one instant are a cut. The danger is a divide by zero in
    // the segment length, which would come back as a NaN and poison whatever
    // it was driving.
    Track<float> cut;
    cut.key(0.0f, 0.0f, &ease::linear);
    cut.key(1.0f, 1.0f, &ease::linear);
    cut.key(1.0f, 50.0f, &ease::linear);
    cut.key(2.0f, 60.0f);
    const float atCut = cut.at(1.0f);
    check(atCut == atCut, "a zero length segment does not produce a NaN");
    check(nearly(atCut, 50.0f), "a cut takes the later value");

    // Vec3 goes through the same machinery, which is the point of animLerp
    // being a customisation point rather than a member.
    Track<Vec3> path;
    path.key(0.0f, Vec3{0.0f, 0.0f, 0.0f}, &ease::linear);
    path.key(2.0f, Vec3{10.0f, 20.0f, -4.0f});
    check(nearlyVec(path.at(1.0f), Vec3{5.0f, 10.0f, -2.0f}), "a Vec3 track interpolates");
}

// ----------------------------------------------------------------- blending
void testBlending() {
    std::printf("blending\n");

    JointPose a;
    a.rotationDegrees = {0.0f, 0.0f, 0.0f};
    a.scale = 1.0f;
    JointPose b;
    b.rotationDegrees = {90.0f, -30.0f, 10.0f};
    b.offset = {2.0f, 0.0f, 0.0f};
    b.scale = 2.0f;

    const JointPose mid = animLerp(a, b, 0.5f);
    check(nearlyVec(mid.rotationDegrees, Vec3{45.0f, -15.0f, 5.0f}), "joint rotations blend");
    check(nearlyVec(mid.offset, Vec3{1.0f, 0.0f, 0.0f}), "joint offsets blend");
    check(nearly(mid.scale, 1.5f), "joint scale blends");

    Pose wide;
    wide[joint::Head].rotationDegrees = {20.0f, 0.0f, 0.0f};
    wide[40].rotationDegrees = {60.0f, 0.0f, 0.0f};   // a joint only this pose knows

    Pose narrow;
    narrow[joint::Head].rotationDegrees = {0.0f, 0.0f, 0.0f};

    const Pose blended = animLerp(narrow, wide, 0.5f);
    check(nearly(blended[joint::Head].rotationDegrees.x, 10.0f), "shared joints blend");
    check(nearly(blended[40].rotationDegrees.x, 30.0f),
          "a joint the other pose has never heard of blends against rest");

    check(nearly(animLerp(narrow, wide, 0.0f)[40].rotationDegrees.x, 0.0f), "t=0 is the first pose");
    check(nearly(animLerp(narrow, wide, 1.0f)[40].rotationDegrees.x, 60.0f),
          "t=1 is the second pose");

    // Additive layering: rotations sum, scales multiply, and the weight
    // scales the overlay rather than blending towards it.
    Pose base;
    base[joint::Body].rotationDegrees = {10.0f, 0.0f, 0.0f};
    base[joint::Body].scale = 2.0f;
    Pose overlay;
    overlay[joint::Body].rotationDegrees = {4.0f, 0.0f, 0.0f};
    overlay[joint::Body].scale = 1.5f;

    const Pose sum = poseAdd(base, overlay, 1.0f);
    check(nearly(sum[joint::Body].rotationDegrees.x, 14.0f), "poseAdd sums rotations");
    check(nearly(sum[joint::Body].scale, 3.0f), "poseAdd multiplies scales");

    const Pose halfSum = poseAdd(base, overlay, 0.5f);
    check(nearly(halfSum[joint::Body].rotationDegrees.x, 12.0f), "weight scales the overlay");
    check(nearly(poseAdd(base, overlay, 0.0f)[joint::Body].scale, 2.0f),
          "zero weight leaves the base alone, scale included");

    Camera first;
    first.lookAt({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
    first.fovY = radians(30.0f);
    first.aperture = 0.0f;
    Camera second;
    second.lookAt({10.0f, 4.0f, 0.0f}, {0.0f, 2.0f, 0.0f});
    second.fovY = radians(50.0f);
    second.aperture = 0.2f;
    second.projection = Camera::Projection::Orthographic;

    const Camera between = animLerp(first, second, 0.5f);
    check(nearlyVec(between.position, Vec3{5.0f, 2.0f, 0.0f}), "camera position blends");
    check(nearly(between.fovY, radians(40.0f), 1e-4f), "camera field of view blends");
    check(nearly(between.aperture, 0.1f), "camera aperture blends");
    check(between.projection == Camera::Projection::Perspective,
          "projection is not blended -- switching it is a cut, so the first one wins");
}

// -------------------------------------------------------------- apng parsing
// A reader written from the specification rather than from png.cpp, so the
// writer is checked by something that does not share its assumptions.
struct Chunk {
    std::string tag;
    std::vector<uint8_t> payload;
    bool crcOk = false;
};

uint32_t beU32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t beU16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }

bool parseChunks(const std::vector<uint8_t>& bytes, std::vector<Chunk>& out) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) return false;

    size_t pos = 8;
    while (pos + 12 <= bytes.size()) {
        const uint32_t length = beU32(bytes.data() + pos);
        if (pos + 12 + length > bytes.size()) return false;

        Chunk chunk;
        chunk.tag.assign(reinterpret_cast<const char*>(bytes.data() + pos + 4), 4);
        chunk.payload.assign(bytes.begin() + long(pos) + 8, bytes.begin() + long(pos) + 8 + long(length));
        chunk.crcOk = beU32(bytes.data() + pos + 8 + length) ==
                      crc32Bytes(bytes.data() + pos + 4, size_t(length) + 4);
        out.push_back(std::move(chunk));
        pos += 12 + length;
    }
    return pos == bytes.size();
}

ImageU8 solid(int width, int height, uint8_t r, uint8_t g, uint8_t b) {
    ImageU8 image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) image.set(x, y, {r, g, b, 255});
    }
    return image;
}

void testApng() {
    std::printf("apng\n");

    const ImageU8 red = solid(8, 6, 220, 40, 40);
    const ImageU8 green = solid(8, 6, 40, 200, 60);
    const std::vector<uint8_t> greenPng = pngEncode(green);

    std::string error;
    ApngWriter writer;
    check(writer.begin(8, 6, 0, &error), "begin accepts a sane size");
    check(writer.addFrame(red, 1, 24, &error), "an image frame is accepted");
    check(writer.addEncodedFrame(greenPng.data(), greenPng.size(), 2, 24, &error),
          "an already encoded frame is accepted");
    check(writer.frameCount() == 2, "two frames went in");

    // Wrong size, and something that is not a PNG at all.
    const ImageU8 wrongSize = solid(9, 6, 0, 0, 0);
    check(!writer.addFrame(wrongSize, 1, 24, nullptr), "a frame of the wrong size is refused");
    const uint8_t junk[16] = {0};
    check(!writer.addEncodedFrame(junk, sizeof(junk), 1, 24, nullptr), "junk is refused");

    const std::string path = "out/test_anim/pair.png";
    check(writer.save(path, &error), "the animation saves");

    std::vector<uint8_t> bytes;
    check(readFileBytes(path, bytes, &error), "and reads back");

    std::vector<Chunk> chunks;
    check(parseChunks(bytes, chunks), "the file is a well formed chunk stream");

    int fctl = 0, fdat = 0, idat = 0, actlFrames = -1;
    size_t actlAt = 0, firstIdatAt = 0;
    std::vector<uint32_t> sequences;
    float duration = 0.0f;

    for (size_t i = 0; i < chunks.size(); ++i) {
        const Chunk& chunk = chunks[i];
        check(chunk.crcOk, "every chunk carries a correct crc");
        if (chunk.tag == "acTL") {
            actlAt = i;
            actlFrames = int(beU32(chunk.payload.data()));
            check(beU32(chunk.payload.data() + 4) == 0, "num_plays of zero means loop forever");
        } else if (chunk.tag == "fcTL") {
            ++fctl;
            sequences.push_back(beU32(chunk.payload.data()));
            check(beU32(chunk.payload.data() + 4) == 8 && beU32(chunk.payload.data() + 8) == 6,
                  "each frame declares the full canvas");
            const uint16_t delayNum = beU16(chunk.payload.data() + 20);
            const uint16_t delayDen = beU16(chunk.payload.data() + 22);
            check(delayDen != 0, "a delay denominator of zero would mean a tenth of a second");
            duration += float(delayNum) / float(delayDen);
        } else if (chunk.tag == "fdAT") {
            ++fdat;
            sequences.push_back(beU32(chunk.payload.data()));
        } else if (chunk.tag == "IDAT") {
            if (idat == 0) firstIdatAt = i;
            ++idat;
        }
    }

    check(chunks.front().tag == "IHDR", "IHDR comes first");
    check(chunks.back().tag == "IEND", "IEND comes last");
    check(actlFrames == fctl && fctl == 2, "acTL agrees with the number of frames present");
    check(actlAt < firstIdatAt, "acTL precedes the image data, as the format requires");
    check(idat == 1 && fdat == 1, "the first frame is an IDAT and the rest are fdAT");
    check(nearly(duration, 1.0f / 24.0f + 2.0f / 24.0f, 1e-6f), "the delays sum to the real length");

    bool contiguous = true;
    for (size_t i = 0; i < sequences.size(); ++i) {
        if (sequences[i] != uint32_t(i)) contiguous = false;
    }
    check(contiguous, "sequence numbers run 0, 1, 2 across fcTL and fdAT together");

    // No recompression: the frame handed in as an encoded PNG must appear
    // with its compressed bytes untouched, or the writer is quietly doing the
    // work twice on every frame of every take.
    std::vector<uint8_t> sourceIdat;
    std::vector<Chunk> sourceChunks;
    parseChunks(greenPng, sourceChunks);
    for (const Chunk& chunk : sourceChunks) {
        if (chunk.tag == "IDAT") {
            sourceIdat.insert(sourceIdat.end(), chunk.payload.begin(), chunk.payload.end());
        }
    }
    std::vector<uint8_t> carried;
    for (const Chunk& chunk : chunks) {
        if (chunk.tag == "fdAT") {
            carried.assign(chunk.payload.begin() + 4, chunk.payload.end());
        }
    }
    check(!sourceIdat.empty() && carried == sourceIdat,
          "an encoded frame is carried through without being recompressed");

    // The promise of the format: a reader that knows nothing about animation
    // still sees a picture, and that picture is frame one.
    ImageU8 still;
    check(pngDecode(bytes.data(), bytes.size(), still, &error),
          "our own still decoder reads the animation");
    check(still.width() == 8 && still.height() == 6, "and gets the right size");
    bool matchesFirst = true;
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) {
            const ImageU8::RGBA got = still.get(x, y);
            const ImageU8::RGBA want = red.get(x, y);
            if (got.r != want.r || got.g != want.g || got.b != want.b || got.a != want.a) {
                matchesFirst = false;
            }
        }
    }
    check(matchesFirst, "the still it sees is exactly frame one");

    ApngWriter empty;
    check(!empty.save("out/test_anim/never.png", nullptr), "an animation with no frames is refused");
}

// --------------------------------------------------------------------- take
Take miniTake(Scene& scene, const char* name) {
    Take take;
    take.name = name;
    take.timing.duration = 0.5f;
    take.timing.fps = 24;      // 12 frames
    take.timing.stepEvery = 2; // 6 traced, 6 held
    take.settings.width = 24;
    take.settings.height = 16;
    take.settings.samplesPerPixel = 1;
    take.settings.maxBounces = 1;
    take.outputDir = std::string("out/test_anim/") + name;
    take.apngPath = std::string("out/test_anim/") + name + ".png";
    take.progress = false;
    take.scene = &scene;
    return take;
}

void clearTake(const Take& take) {
    for (int i = 0; i < take.timing.frameCount(); ++i) removeFile(takeFramePath(take, i));
    removeFile(take.outputDir + "/take.txt");
    removeFile(take.apngPath);
}

void testTake() {
    std::printf("take\n");

    Scene scene(palette::registry());
    scene.world.fillBox({-2, -1, -2}, {2, -1, 2}, palette::Stone);
    scene.world.set({0, 0, 0}, palette::GoldBlock);
    scene.overrideBackground = true;
    scene.background = Vec3{0.05f, 0.05f, 0.08f};

    std::string error;

    // ---- a moving shot: every traced frame is a different picture
    Take moving = miniTake(scene, "moving");
    clearTake(moving);
    moving.shot = [](Scene& s, const Frame& f) {
        const float angle = f.t01 * kTwoPi;
        s.camera.lookAt({std::sin(angle) * 6.0f, 3.0f, -std::cos(angle) * 6.0f}, {0.0f, 0.0f, 0.0f});
    };

    TakeStats stats;
    check(renderTake(moving, &stats, &error), "a take renders");
    check(stats.framesTraced == 6, "exactly half the frames went through the tracer");
    check(stats.framesHeld == 6, "and the other half were held");
    check(stats.framesSkipped == 0, "nothing was skipped on a clean run");

    check(takeFramePath(moving, 7).find("0007.png") != std::string::npos,
          "frames are named with a four digit index");

    // The hold has to be a copy, not a second trace. Assert it at the level
    // that matters: the bytes.
    for (int i = 0; i < moving.timing.frameCount(); i += 2) {
        std::vector<uint8_t> leader, heldFrame;
        check(readFileBytes(takeFramePath(moving, i), leader, nullptr), "leader frame exists");
        check(readFileBytes(takeFramePath(moving, i + 1), heldFrame, nullptr), "held frame exists");
        check(leader == heldFrame, "a held frame is byte identical to the frame it holds");
    }

    // And consecutive pairs must actually differ, or the take is a still and
    // the test above proves nothing.
    std::vector<uint8_t> first, third;
    readFileBytes(takeFramePath(moving, 0), first, nullptr);
    readFileBytes(takeFramePath(moving, 2), third, nullptr);
    check(first != third, "consecutive traced frames are genuinely different pictures");

    // ---- resume
    TakeStats again;
    check(renderTake(moving, &again, &error), "the same take runs again");
    check(again.framesTraced == 0 && again.framesHeld == 0 && again.framesSkipped == 12,
          "resume skips everything already on disk");

    // ---- the guard
    Take changed = moving;
    changed.settings.samplesPerPixel = 4;
    check(!renderTake(changed, nullptr, &error),
          "a take whose settings changed refuses to mix frames into the old sequence");
    check(error.find("different settings") != std::string::npos,
          "and says so in a way that names the problem");

    // ---- the collapse preserves time
    std::vector<uint8_t> bytes;
    check(readFileBytes(moving.apngPath, bytes, &error), "the take wrote an animation");
    std::vector<Chunk> chunks;
    check(parseChunks(bytes, chunks), "which parses");

    int frames = 0;
    float duration = 0.0f;
    for (const Chunk& chunk : chunks) {
        if (chunk.tag != "fcTL") continue;
        ++frames;
        duration += float(beU16(chunk.payload.data() + 20)) / float(beU16(chunk.payload.data() + 22));
    }
    check(frames == 6, "twelve frames on twos collapse into six");
    check(nearly(duration, 0.5f, 1e-5f), "and the collapse keeps the full running time");

    // ---- a shot that never changes: everything collapses to one frame, and
    // the running time still has to survive.
    Take stillLife = miniTake(scene, "still");
    clearTake(stillLife);
    stillLife.shot = [](Scene& s, const Frame&) {
        s.camera.lookAt({0.0f, 3.0f, -6.0f}, {0.0f, 0.0f, 0.0f});
    };
    check(renderTake(stillLife, nullptr, &error), "a motionless take renders");

    bytes.clear();
    chunks.clear();
    readFileBytes(stillLife.apngPath, bytes, &error);
    parseChunks(bytes, chunks);

    frames = 0;
    duration = 0.0f;
    for (const Chunk& chunk : chunks) {
        if (chunk.tag != "fcTL") continue;
        ++frames;
        duration += float(beU16(chunk.payload.data() + 20)) / float(beU16(chunk.payload.data() + 22));
    }
    check(frames == 1, "a motionless take collapses to a single frame");
    check(nearly(duration, 0.5f, 1e-5f), "which still runs for the whole take");

    // ---- refusals
    Take headless = miniTake(scene, "moving");
    headless.scene = nullptr;
    headless.shot = [](Scene&, const Frame&) {};
    check(!renderTake(headless, nullptr, &error), "a take with no scene is refused");

    Take mute = miniTake(scene, "moving");
    mute.shot = nullptr;
    check(!renderTake(mute, nullptr, &error), "a take with no shot is refused");
}

} // namespace

int main() {
    testTiming();
    testEasing();
    testTracks();
    testBlending();
    testApng();
    testTake();

    if (gFailures > 0) {
        std::printf("\n%d animation test(s) failed\n", gFailures);
        return 1;
    }
    std::printf("\nall animation tests passed\n");
    return 0;
}
