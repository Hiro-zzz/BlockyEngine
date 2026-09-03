#pragma once
// Turning a rendered frame sequence into an MP4.
//
// This is the seam the animation layer calls. It reads the PNGs a take left
// on disk, encodes them, and writes one file -- which means the whole video
// path inherits the take's resume behaviour for free: the expensive part is
// already done and on disk before any of this runs.
//
// Runs of byte-identical frames become one sample with a longer duration,
// exactly as the APNG assembler does it, so animation on twos costs half the
// samples here too. It is the same argument in a different container: a
// format that lets a frame say how long it lasts should be told.
#include <string>
#include <vector>

namespace blocky {

struct Mp4Stats {
    int    samples = 0;      // coded frames, after identical runs collapse
    int    keyframes = 0;    // of those, the ones a player may seek to
    int    inputFrames = 0;  // frames on disk
    size_t bytes = 0;
    int    width = 0, height = 0;

    // How many macroblocks predicted from the previous picture, and how many
    // of those reached a position between its samples. The second number is
    // the only way to see whether the interpolation filter is earning its
    // place on a given sequence: motion that happens to be whole gets whole
    // vectors, and then the filter is dead weight rather than a gain.
    long long interMacroblocks = 0;
    long long fractionalVectors = 0;
};

// `pngPaths` in playback order. Every frame must be the same size.
//
// One frame in every second of playback is coded as an IDR, so a scrub bar
// never has to walk more than a second of prediction to land somewhere. The
// decision lives here rather than in the encoder because it is about seeking,
// not about compression, and it needs the clock: a frame held on twos is one
// sample lasting twice as long, and a keyframe every N *samples* would drift
// against the wall clock as soon as any frame is held.
//
// `qp` is the quantisation parameter, 0..51. Lower is finer and larger, and
// every six steps doubles the step size. Around 22 is visually lossless on
// this kind of flat, hard-edged artwork; 30 starts to show.
bool encodeMp4(const std::vector<std::string>& pngPaths, int fps, const std::string& outPath,
               int qp = 24, Mp4Stats* stats = nullptr, std::string* error = nullptr);

} // namespace blocky
