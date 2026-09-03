#pragma once
// Timing curves, arranged around the kind of motion this engine is for.
//
// The usual easing library is a collection of ways to be smooth, because the
// usual target is a user interface sliding a panel. A blocky character is not
// that. What reads well on boxes is **hold, then snap**: a pose sits still
// long enough to be read, then covers the whole distance in a step or two.
// Smoothness is the option here, not the default.
//
// Every curve maps [0,1] to roughly [0,1]. Roughly, because `back` and
// `anticipate` deliberately leave the range -- that overshoot is the entire
// point of them, and a Track will happily extrapolate a value past its key.
#include "engine/core/math.hpp"

#include <functional>

namespace blocky {
namespace ease {

using Curve = std::function<float(float)>;

inline float linear(float t) { return saturate(t); }

// Smoothstep. The gentle one, for a camera that should not call attention.
inline float smooth(float t) { t = saturate(t); return t * t * (3.0f - 2.0f * t); }

inline float in(float t)  { t = saturate(t); return t * t; }
inline float out(float t) { t = saturate(t); return 1.0f - (1.0f - t) * (1.0f - t); }

// No interpolation at all: the value jumps at the far end. A track built from
// these is a slideshow of poses, which on twos is exactly stop motion.
inline float step(float t) { return t >= 1.0f ? 1.0f : 0.0f; }

// ------------------------------------------------------------ the snappy set

// Sit still for `fraction` of the segment, then cover the distance over what
// is left. The workhorse: a pose that holds and then moves reads as decided,
// while the same move spread evenly reads as drifting.
inline Curve hold(float fraction = 0.6f) {
    const float f = saturate(fraction);
    return [f](float t) {
        t = saturate(t);
        if (t <= f) return 0.0f;
        return smooth((t - f) / (1.0f - f));
    };
}

// Overshoot the target and settle back onto it. On a limb this is follow
// through; without it a stepped move stops dead and looks like a hinge
// running into its end stop.
inline Curve back(float overshoot = 1.70158f) {
    const float s = overshoot;
    return [s](float t) {
        t = saturate(t);
        const float u = t - 1.0f;
        return u * u * ((s + 1.0f) * u + s) + 1.0f;
    };
}

// Pull away from the target before moving to it. The other half of the
// cartoon vocabulary: a wind-up tells the eye a move is coming, which on
// twos is most of what sells the move at all.
inline Curve anticipate(float amount = 0.18f) {
    const float a = amount;
    return [a](float t) {
        t = saturate(t);
        // A short dip below zero, then a fast run to one.
        const float dip = 0.30f;
        if (t < dip) return -a * smooth(t / dip);
        const float u = (t - dip) / (1.0f - dip);
        return -a + (1.0f + a) * smooth(u);
    };
}

// Wind up, move, overshoot, settle -- all three at once, for a move that has
// to carry a whole beat on its own.
inline Curve snap(float wind = 0.14f, float overshoot = 1.3f) {
    Curve a = anticipate(wind);
    Curve b = back(overshoot);
    return [a, b](float t) {
        t = saturate(t);
        // Anticipation owns the first third, the overshoot the rest.
        if (t < 0.34f) return a(t / 0.34f) * 0.34f;
        return lerp(a(1.0f) * 0.34f, 1.0f, b((t - 0.34f) / 0.66f));
    };
}

// A decaying bounce, for something landing.
inline Curve bounce(int bounces = 3, float decay = 0.42f) {
    const int n = bounces < 1 ? 1 : bounces;
    const float d = saturate(decay);
    return [n, d](float t) {
        t = saturate(t);
        if (t >= 1.0f) return 1.0f;
        const float wave = std::fabs(std::cos(kPi * float(n) * t));
        return 1.0f - (1.0f - t) * wave * std::pow(d, t * float(n));
    };
}

} // namespace ease
} // namespace blocky
