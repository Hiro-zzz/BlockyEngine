#pragma once
// Where time enters the engine, and the only place that knows what a frame is.
//
// The renderer is not touched by any of this. It still takes a `const Scene&`
// and returns an image; animation is a loop that rebuilds the cheap parts of
// a scene and calls it again. Nothing below `anim/` has any idea time exists.
//
// ---------------------------------------------------------------- on twos
//
// `stepEvery` is the whole reason this file has a quantiser in it. Setting it
// to 2 holds each pose for two frames, which is the twelve-per-second rate of
// classic limited animation -- and on a blocky character it reads as a style
// rather than as a shortage. It is also, exactly, half the render: the second
// frame of a hold is *identical* to the first, so it is copied rather than
// traced.
//
// That identity is not detected, it is guaranteed: the time handed to a shot
// is quantised before the shot ever sees it, so two frames of one hold are
// handed the same number and must produce the same scene.
//
// Which is the one rule a shot has to keep: **be a pure function of Frame.**
// Read the clock, a global counter, or an unseeded rng inside a shot and the
// held frame stops matching the frame it was supposed to hold, while the
// engine goes on cheerfully copying it. Everything that varies must vary as a
// function of `Frame`, and everything that does not belongs outside the loop.
#include <algorithm>
#include <cmath>

namespace blocky {

// One frame's worth of time, as a shot sees it.
struct Frame {
    int   index = 0;      // 0 .. count-1, counting every frame of the output

    // Seconds, already quantised by `stepEvery`. This is the number a shot
    // should use for everything; `index / fps` is deliberately not offered,
    // because using it would defeat the hold.
    float time = 0.0f;

    // `time` as a fraction of the take's duration, in [0, 1]. What a Track
    // normalised to the whole shot wants.
    float t01 = 0.0f;

    // Seconds between two *rendered* steps -- one step of hold, not one
    // frame. This is the shutter a motion blur would open, and the step a
    // hand-rolled integration would take.
    float dt = 0.0f;

    // True when this frame repeats the one before it. The driver copies these
    // instead of tracing them; a shot never needs to look at it.
    bool  held = false;
};

struct Timing {
    float duration = 4.0f;   // seconds of finished animation
    int   fps = 24;          // frames written per second

    // 1 = a new pose every frame, 2 = on twos (the 12 fps look), 3 = on
    // threes. Anything above 1 costs proportionally less to render.
    int   stepEvery = 2;

    int frameCount() const {
        int n = int(std::lround(double(duration) * double(fps < 1 ? 1 : fps)));
        return n < 1 ? 1 : n;
    }

    Frame frameAt(int index) const {
        const int rate = fps < 1 ? 1 : fps;
        const int step = stepEvery < 1 ? 1 : stepEvery;

        // The frame this one is holding: itself, or the last one that began a
        // step. Everything downstream follows from this single rounding.
        const int leader = (index / step) * step;

        Frame f;
        f.index = index;
        f.time  = float(leader) / float(rate);
        f.dt    = float(step) / float(rate);
        f.held  = index != leader;
        f.t01   = duration > 0.0f ? std::min(1.0f, f.time / duration) : 0.0f;
        return f;
    }

    // How many frames actually get traced, which is what the clock in your
    // head should be multiplying by seconds-per-frame.
    int renderedFrameCount() const {
        const int step = stepEvery < 1 ? 1 : stepEvery;
        return (frameCount() + step - 1) / step;
    }
};

} // namespace blocky
