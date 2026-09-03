#pragma once
// A film is a list of shots. This is what turns that list into one sequence
// on disk and one video.
//
// ------------------------------------------------------- why not one Take
//
// `Take` renders one continuous shot: one scene, one camera, one lambda. A
// five-minute film is thirty of those, and half of them stand in a different
// place. The obvious move -- one Take whose shot swaps the world by time --
// breaks the one contract Take asks for, because the world is the expensive
// half and swapping it per frame is exactly what the seam exists to avoid.
//
// So a film is many Takes, and what makes them one film is that they all
// write into one directory with one global frame numbering. `firstFrame` and
// `lastFrame` already carve a range out of a take, and `assembleApng` and
// `assembleMp4` already work off files rather than off memory. Nothing new
// was needed in the engine: a shot is a Take restricted to its own frames,
// and the film is the assemble at the end.
//
// The signature guard in take.cpp turns out to be the thing that makes this
// safe rather than merely possible. Every take writes the same take.txt, so
// two shots rendered at different resolutions cannot land in one directory --
// the second one refuses. A five-minute render is long enough that finding
// that out at the end instead would cost a day.
//
// ------------------------------------------------------------------ sets
//
// Shots naming the same set share a world, and consecutive ones share a Take.
// Everything else -- camera, light, cast, props, labels -- is set by the shot
// itself, every frame, because the shot before it left the scene arranged for
// something else entirely.
#include "engine/anim/take.hpp"
#include "engine/core/file.hpp"
#include "engine/scene/scene.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace film {

using namespace blocky;

// Where a shot is in its own time. `global` exists for the few things that
// span shots -- a sun that keeps rising across a cut.
struct Beat {
    float local = 0.0f;   // seconds since this shot started
    float t01 = 0.0f;     // local / duration, in [0, 1]
    float global = 0.0f;  // seconds since the film started
    float dt = 0.0f;      // seconds between rendered steps
};

struct Shot {
    std::string name;
    int   set = 0;         // index into Reel::sets
    float seconds = 4.0f;
    bool  card = false;    // an intertitle: no cel shading, no grade

    std::function<void(Scene&, const Beat&)> play;

    // Filled in by Reel::layout().
    int firstFrame = 0;
    int lastFrame = 0;     // inclusive
    float startTime = 0.0f;
};

class Reel {
public:
    std::string name = "film";
    std::string outputDir;          // empty means out/<name>

    int fps = 24;
    int stepEvery = 2;

    PathSettings settings;
    ToneParams   tone;

    DenoiseSettings denoiseSettings;
    bool denoiseFrames = true;

    // Handed to every Take. Null leaves the CPU path tracer and the CPU
    // denoiser in place, which is what the film has always done.
    std::function<bool(const Scene&, const PathSettings&, RenderTargets&)> renderer;
    std::function<Image(const RenderTargets&, const DenoiseSettings&)> filter;

    // Applied to every shot that is not a card. Takes the shot as well as the
    // frame, so a look can key off which shot it is in.
    std::function<Image(RenderTargets&, const Frame&, const Shot&)> finish;

    bool writeApng = false;         // a five-minute APNG is enormous
    bool writeMp4 = true;
    int  videoQp = 24;

    // Scenes, indexed by Shot::set. Built once by the caller, before any
    // rendering: these are the expensive halves.
    std::vector<Scene*> sets;
    std::vector<Shot>   shots;

    // Render only these shots, by name. Empty means all of them.
    std::vector<std::string> only;

    bool resume = true;

    // ------------------------------------------------------------- layout
    // Assign every shot its frame range. A shot's first frame is snapped up
    // to a multiple of `stepEvery` so that no shot starts inside a hold --
    // otherwise the first frame of a shot could be a held copy of the last
    // frame of the one before it, which is a different picture entirely.
    void layout() {
        const int step = stepEvery < 1 ? 1 : stepEvery;
        int cursor = 0;
        for (Shot& shot : shots) {
            int frames = int(std::lround(double(shot.seconds) * double(fps)));
            if (frames < step) frames = step;
            frames = ((frames + step - 1) / step) * step;   // whole holds only

            shot.firstFrame = cursor;
            shot.lastFrame = cursor + frames - 1;
            shot.startTime = float(cursor) / float(fps);
            shot.seconds = float(frames) / float(fps);
            cursor += frames;
        }
        totalFrames_ = cursor;
    }

    int   totalFrames() const { return totalFrames_; }
    float duration() const { return float(totalFrames_) / float(fps < 1 ? 1 : fps); }

    // Which shot a global frame index falls in, or nullptr past the end.
    const Shot* shotAt(int frameIndex) const {
        for (const Shot& shot : shots) {
            if (frameIndex >= shot.firstFrame && frameIndex <= shot.lastFrame) return &shot;
        }
        return nullptr;
    }

    // ------------------------------------------------------------- render
    bool render(std::string* error = nullptr) {
        if (shots.empty()) { setError(error, "reel: no shots"); return false; }
        layout();

        const std::string dir = outputDir.empty() ? ("out/" + name) : outputDir;

        printPlan();

        // Consecutive shots standing in the same set share one Take, so a
        // world is not rebuilt for a cut that does not leave the room.
        size_t i = 0;
        while (i < shots.size()) {
            size_t j = i;
            while (j + 1 < shots.size() && shots[j + 1].set == shots[i].set &&
                   wanted(shots[j + 1]) == wanted(shots[i])) {
                ++j;
            }

            if (wanted(shots[i])) {
                if (!renderRun(i, j, dir, error)) return false;
            }
            i = j + 1;
        }

        return assemble(dir, error);
    }

    // Assemble whatever is on disk into the finished film. Split out because
    // a five-minute render gets interrupted, and the frames are the work.
    bool assemble(const std::string& dir, std::string* error = nullptr) {
        if (totalFrames_ == 0) layout();

        Take take = templateTake(dir);
        take.name = name;

        for (int i = 0; i < totalFrames_; ++i) {
            if (!fileExists(takeFramePath(take, i))) {
                std::printf("[reel] frame %04d of %d is missing -- nothing to assemble yet\n", i,
                            totalFrames_);
                return true;
            }
        }

        if (writeApng) {
            std::printf("[reel] assembling apng...\n");
            if (!assembleApng(take, error)) return false;
            std::printf("[reel] wrote out/%s.png\n", name.c_str());
        }
        if (writeMp4) {
            std::printf("[reel] encoding mp4...\n");
            if (!assembleMp4(take, error)) return false;
            std::printf("[reel] wrote out/%s.mp4\n", name.c_str());
        }
        return true;
    }

    void printPlan() const {
        std::printf("[reel] %s: %zu shots, %d frames, %.1f s at %d fps on %s\n", name.c_str(),
                    shots.size(), totalFrames_, duration(), fps,
                    stepEvery == 1 ? "ones" : "twos");
        std::printf("[reel] %dx%d, %d spp, %d bounces -- %d frames to trace\n", settings.width,
                    settings.height, settings.samplesPerPixel, settings.maxBounces,
                    (totalFrames_ + stepEvery - 1) / stepEvery);
        for (const Shot& shot : shots) {
            std::printf("       %-18s set %d  %6.2f-%6.2f  frames %4d-%4d%s\n", shot.name.c_str(),
                        shot.set, shot.startTime, shot.startTime + shot.seconds, shot.firstFrame,
                        shot.lastFrame, shot.card ? "  card" : "");
        }
    }

private:
    static void setError(std::string* error, const std::string& message) {
        if (error) *error = message;
    }

    bool wanted(const Shot& shot) const {
        if (only.empty()) return true;
        for (const std::string& want : only) {
            if (want == shot.name) return true;
        }
        return false;
    }

    Take templateTake(const std::string& dir) const {
        Take take;
        take.outputDir = dir;
        take.timing.duration = duration();
        take.timing.fps = fps;
        take.timing.stepEvery = stepEvery;
        take.settings = settings;
        take.tone = tone;
        take.denoiseSettings = denoiseSettings;
        take.denoiseFrames = denoiseFrames;
        take.renderer = renderer;
        take.filter = filter;
        take.resume = resume;
        take.writeApng = false;   // the film is assembled once, at the end
        take.writeMp4 = false;
        take.videoQp = videoQp;
        return take;
    }

    bool renderRun(size_t first, size_t last, const std::string& dir, std::string* error) {
        Take take = templateTake(dir);
        take.name = shots[first].name;
        if (last > first) take.name += ".." + shots[last].name;

        take.scene = sets[size_t(shots[first].set)];
        take.firstFrame = shots[first].firstFrame;
        take.lastFrame = shots[last].lastFrame;

        // One dispatcher for the whole run: find the shot this frame belongs
        // to and hand it its own local time. A shot never learns that it is
        // sharing a Take with its neighbours.
        const Reel* self = this;
        take.shot = [self, first, last](Scene& scene, const Frame& f) {
            for (size_t k = first; k <= last; ++k) {
                const Shot& shot = self->shots[k];
                if (f.index < shot.firstFrame || f.index > shot.lastFrame) continue;
                Beat beat;
                beat.local = f.time - shot.startTime;
                beat.t01 = shot.seconds > 0.0f ? saturate(beat.local / shot.seconds) : 0.0f;
                beat.global = f.time;
                beat.dt = f.dt;
                if (shot.play) shot.play(scene, beat);
                return;
            }
        };

        if (finish) {
            take.finish = [self](RenderTargets& targets, const Frame& f) -> Image {
                const Shot* shot = self->shotAt(f.index);
                static const Shot fallback;
                return self->finish(targets, f, shot ? *shot : fallback);
            };
        }

        return renderTake(take, nullptr, error);
    }

    int totalFrames_ = 0;
};

} // namespace film
