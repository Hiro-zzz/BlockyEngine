#pragma once
// The frame loop: what turns a shot into a sequence on disk and an animation
// you can open.
//
// -------------------------------------------------------------- stage and shot
//
// There is no Stage type here, and there should not be. The expensive half of
// a scene -- the world, the textures, the light set -- is built once by the
// caller, in ordinary code, before the take starts. The cheap half is whatever
// `shot` reaches out and changes. So the split costs no new vocabulary:
//
//     Scene scene;
//     scene.world = generateIsland();          // once
//     scene.blockTextures = &textures;         // once
//
//     Take take;
//     take.scene = &scene;
//     take.shot = [&](Scene& s, const Frame& f) {
//         s.camera.lookAt(orbit(f.time), centre);   // per frame
//         entities.clear();
//         figure.pose = walk.at(f.time);
//         entities.add(figure);
//         s.entities = &entities;
//     };
//
// EntitySet has to be rebuilt inside the shot because add() flattens the pose
// into world-space boxes on the spot -- that is what makes the tracer's job
// cheap, and it means a new pose needs a new set.
//
// ------------------------------------------------------------------ on disk
//
// Every frame is written to `out/<name>/0000.png`, including the held ones,
// which are byte copies of the frame they hold rather than a second render.
// That costs a little disk and buys a lot:
//
//   - the run resumes; a frame already on disk is not traced again,
//   - a single bad frame can be re-rendered by deleting just that file,
//   - two processes can split the range and work at once,
//   - the directory is an ordinary image sequence, readable by anything.
//
// The APNG is then assembled from those files without decoding or recompressing
// any of them: the frames' compressed data is lifted straight out and runs of
// identical frames collapse into one frame with a longer delay.
#include "engine/anim/timing.hpp"
#include "engine/core/image.hpp"
#include "engine/render/post/denoise.hpp"
#include "engine/render/trace/pathtrace.hpp"
#include "engine/scene/scene.hpp"

#include <functional>
#include <string>

namespace blocky {

struct Take {
    std::string name = "take";
    Timing      timing;
    PathSettings settings;

    // Built once by the caller and mutated by the shot. Not owned.
    Scene* scene = nullptr;

    // The seam. Must be a pure function of `Frame` -- see timing.hpp for why
    // that is a rule and not a style note.
    std::function<void(Scene&, const Frame&)> shot;

    // Optional. Runs after the denoiser and turns the render into the frame
    // that gets written: cel shading, grading, vignette. Without it the
    // denoised colour buffer is used as-is.
    std::function<Image(RenderTargets&, const Frame&)> finish;

    DenoiseSettings denoiseSettings;
    bool            denoiseFrames = true;
    ToneParams      tone;

    // ------------------------------------------------------ where it renders
    //
    // Two seams, both optional, both defaulting to the CPU. They are
    // std::function rather than a flag because anim/ has no business knowing
    // that OpenGL exists: a caller that wants the GPU hands one in, and this
    // file stays a loop over frames.
    //
    // `renderer` must fill `targets` -- colour and the three auxiliary
    // buffers -- exactly as renderPath does. `filter` replaces the denoiser
    // and is only consulted when denoiseFrames is set.
    std::function<bool(const Scene&, const PathSettings&, RenderTargets&)> renderer;
    std::function<Image(const RenderTargets&, const DenoiseSettings&)> filter;

    // Empty means "out/<name>" and "out/<name>.png".
    std::string outputDir;
    std::string apngPath;
    bool writeApng = true;
    int  apngPlays = 0;      // 0 = loop forever

    // MP4, encoded by engine/video. Off by default while the encoder is
    // still coding every macroblock as raw samples: the file plays anywhere
    // but is enormous, and nobody wants that by surprise.
    std::string mp4Path;     // empty means out/<name>.mp4
    bool writeMp4 = false;
    int  videoQp = 24;       // 0..51, lower is finer and larger

    // Skip frames already on disk. Turn off to force a full re-render.
    bool resume = true;

    // Render only part of the take. -1 means "to the end".
    int  firstFrame = 0;
    int  lastFrame = -1;

    bool progress = true;
};

struct TakeStats {
    int    framesTraced = 0;   // actually went through the path tracer
    int    framesHeld = 0;     // copied from the frame they hold
    int    framesSkipped = 0;  // already on disk
    double seconds = 0.0;
};

// Renders the take and, unless told otherwise, assembles the animation.
// Returns false and fills `error` on the first frame that fails to write.
bool renderTake(const Take& take, TakeStats* stats = nullptr, std::string* error = nullptr);

// Encode the sequence to MP4. Like assembleApng, it works off the files and
// so can be run on its own after an interrupted render.
bool assembleMp4(const Take& take, std::string* error = nullptr);

// Build the APNG from a sequence already on disk. Useful on its own after an
// interrupted run, and it is what renderTake calls at the end.
bool assembleApng(const Take& take, std::string* error = nullptr);

// Where a given frame lives. Exposed because scenes sometimes want to poke at
// one frame by hand.
std::string takeFramePath(const Take& take, int index);

} // namespace blocky
