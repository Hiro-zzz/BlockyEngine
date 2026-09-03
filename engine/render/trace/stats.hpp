#pragma once
#include <cstdint>

namespace blocky {

struct RenderStats {
    // Tracing only: the clock starts after the scene is prepared and stops
    // when the last tile is done.
    double   seconds = 0.0;

    // What renderPath does before a single ray is fired -- copying the scene
    // and gathering emissive faces into the light set. Nil for one still and
    // worth knowing for a take, because it is paid again on every frame.
    double   setupSeconds = 0.0;

    uint64_t primaryRays = 0;   // camera rays fired
    uint64_t totalRays = 0;     // including bounce and shadow rays
    int      threadsUsed = 0;
    size_t   emissiveFaces = 0;
};

} // namespace blocky
