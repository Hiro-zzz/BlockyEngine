#pragma once
// A value with keys in time.
//
// Tracks are a convenience above the seam, not the seam itself. A shot is a
// function of time, so `sin(f.time * 3.0f)` is already a perfectly good
// animation and needs none of this. What a track adds is the other way of
// thinking -- this pose here, that pose there -- and the two mix freely
// inside one shot.
//
// -------------------------------------------- cadence and timing are not the
// same knob
//
// Two separate things make motion read as snappy, and it is worth keeping
// them apart in your head:
//
//   Timing::stepEvery  quantises *when* the scene is sampled. On twos, the
//                      whole frame holds for two frames. This is the one that
//                      halves the render.
//   the curve here     shapes *how* a value crosses its segment. `ease::hold`
//                      keeps it still and then throws it across.
//
// Either alone helps; together they are stop motion.
#include "engine/anim/blend.hpp"
#include "engine/anim/ease.hpp"

#include <algorithm>
#include <vector>

namespace blocky {

template <class T>
class Track {
public:
    Track() = default;

    // A track with one key holds one value forever, which is a tidy way to
    // leave something animatable that is not animated yet.
    explicit Track(const T& constant) { key(0.0f, constant); }

    // The curve given with a key governs the segment that *starts* there --
    // the run from this key to the next. The last key's curve is never used.
    Track& key(float time, const T& value) { return key(time, value, defaultCurve_); }

    Track& key(float time, const T& value, ease::Curve curve) {
        Key k;
        k.time = time;
        k.value = value;
        k.curve = std::move(curve);
        auto at = std::upper_bound(keys_.begin(), keys_.end(), time,
                                   [](float t, const Key& other) { return t < other.time; });
        keys_.insert(at, std::move(k));
        return *this;
    }

    // Every later key jumps instead of sliding. A track of these is a
    // slideshow of poses, which on twos is exactly stop motion.
    Track& stepped() { defaultCurve_ = &ease::step; return *this; }

    Track& curve(ease::Curve c) { defaultCurve_ = std::move(c); return *this; }

    // Outside the keyed range the track holds its end values rather than
    // extrapolating: running off the end of a shot should freeze, not fly.
    T at(float time) const {
        if (keys_.empty()) return T{};
        if (time <= keys_.front().time) return keys_.front().value;
        if (time >= keys_.back().time)  return keys_.back().value;

        size_t i = 0;
        while (i + 2 < keys_.size() && keys_[i + 1].time <= time) ++i;

        const Key& a = keys_[i];
        const Key& b = keys_[i + 1];
        const float span = b.time - a.time;

        // Two keys at the same instant are a cut, not a division by zero.
        if (span <= 0.0f) return b.value;

        const float u = (time - a.time) / span;
        return animLerp(a.value, b.value, a.curve ? a.curve(u) : ease::smooth(u));
    }

    bool   empty() const { return keys_.empty(); }
    size_t size()  const { return keys_.size(); }
    float  startTime() const { return keys_.empty() ? 0.0f : keys_.front().time; }
    float  endTime()   const { return keys_.empty() ? 0.0f : keys_.back().time; }

private:
    struct Key {
        float time = 0.0f;
        T value{};
        ease::Curve curve;
    };

    std::vector<Key> keys_;
    ease::Curve defaultCurve_ = &ease::smooth;
};

} // namespace blocky
