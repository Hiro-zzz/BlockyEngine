#include "engine/anim/take.hpp"

#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/video/encode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace blocky {
namespace {

using Clock = std::chrono::steady_clock;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string takeDir(const Take& take) {
    return take.outputDir.empty() ? ("out/" + take.name) : take.outputDir;
}

std::string apngPath(const Take& take) {
    return take.apngPath.empty() ? ("out/" + take.name + ".png") : take.apngPath;
}

std::string mp4Path(const Take& take) {
    return take.mp4Path.empty() ? ("out/" + take.name + ".mp4") : take.mp4Path;
}

int stepOf(const Take& take) {
    return take.timing.stepEvery < 1 ? 1 : take.timing.stepEvery;
}

// Rendered frames in [0, index]. Used to turn "frames left" into "traces
// left", which is the number an estimate has to be built on.
int tracesUpTo(int index, int step) {
    return index < 0 ? 0 : index / step + 1;
}

std::string formatDuration(double seconds) {
    char buf[32];
    if (seconds < 90.0) {
        std::snprintf(buf, sizeof(buf), "%.0fs", seconds);
    } else if (seconds < 5400.0) {
        std::snprintf(buf, sizeof(buf), "%dm %02ds", int(seconds) / 60, int(seconds) % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%dh %02dm", int(seconds) / 3600,
                      (int(seconds) % 3600) / 60);
    }
    return buf;
}

// What the frames on disk were rendered with. Resume works by asking whether
// a frame is already there, and a file cannot say what settings made it -- so
// without this, raising the resolution and running again would skip all 96
// stale frames and report success. The sequence carries a note of its own
// settings, and a run that disagrees with it stops instead of half-filling a
// directory with two different pictures.
std::string signatureOf(const Take& take) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "blocky-take 1\n%dx%d spp=%d bounces=%d seed=%llu\n"
                  "fps=%d step=%d duration=%.6f\n",
                  take.settings.width, take.settings.height, take.settings.samplesPerPixel,
                  take.settings.maxBounces, (unsigned long long)take.settings.seed,
                  take.timing.fps, take.timing.stepEvery < 1 ? 1 : take.timing.stepEvery,
                  double(take.timing.duration));
    return buf;
}

std::string signaturePath(const Take& take) { return takeDir(take) + "/take.txt"; }

} // namespace

std::string takeFramePath(const Take& take, int index) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d", index);
    return takeDir(take) + "/" + buf + ".png";
}

bool renderTake(const Take& take, TakeStats* stats, std::string* error) {
    if (!take.scene) { setError(error, "take: scene is null"); return false; }
    if (!take.shot)  { setError(error, "take: shot is not set"); return false; }

    const std::string dir = takeDir(take);
    if (!createDirectories(dir)) {
        setError(error, "take: cannot create " + dir);
        return false;
    }

    // Guard the sequence before a single frame is written.
    {
        const std::string want = signatureOf(take);
        std::vector<uint8_t> have;
        if (readFileBytes(signaturePath(take), have) && !have.empty()) {
            const std::string found(have.begin(), have.end());
            if (found != want) {
                setError(error, "take: " + dir +
                                    " was rendered with different settings -- delete it to start"
                                    " over, or render this take under another name.\nwanted:\n" +
                                    want + "found:\n" + found);
                return false;
            }
        } else if (!writeFileBytes(signaturePath(take),
                                   reinterpret_cast<const uint8_t*>(want.data()), want.size(),
                                   error)) {
            return false;
        }
    }

    const int step  = stepOf(take);
    const int total = take.timing.frameCount();
    const int first = std::max(0, take.firstFrame);
    const int last  = take.lastFrame < 0 ? total - 1 : std::min(take.lastFrame, total - 1);

    PathSettings settings = take.settings;

    // The tracer's own progress bar would print once per frame over the top of
    // ours, and one line per frame is the useful granularity here anyway.
    settings.progress = false;

    // Deliberately *not* varied per frame. A fixed seed keeps the noise
    // correlated between frames, so what is left of it sits still instead of
    // boiling; a per-frame seed is the instinctive choice and the wrong one.
    // (Within a hold the question does not arise -- the frame is copied.)

    TakeStats local;
    const auto started = Clock::now();
    double tracedSeconds = 0.0;

    if (take.progress) {
        std::printf("[%s] %d frames, %d fps, on %s -- %d to trace\n", take.name.c_str(), total,
                    take.timing.fps, step == 1 ? "ones" : (step == 2 ? "twos" : "threes or more"),
                    take.timing.renderedFrameCount());
    }

    for (int i = first; i <= last; ++i) {
        const Frame f = take.timing.frameAt(i);
        const std::string path = takeFramePath(take, i);

        if (take.resume && fileExists(path)) {
            ++local.framesSkipped;
            continue;
        }

        // A held frame is a byte copy of the one it holds. Reading and
        // rewriting a couple of hundred kilobytes against thirty seconds of
        // tracing is the entire saving of animating on twos.
        if (f.held) {
            const int leader = (i / step) * step;
            std::vector<uint8_t> bytes;
            if (readFileBytes(takeFramePath(take, leader), bytes) && !bytes.empty()) {
                if (!writeFileBytes(path, bytes.data(), bytes.size(), error)) return false;
                ++local.framesHeld;
                continue;
            }
            // The frame it holds is not on disk -- a part range starting mid
            // hold, say. Fall through and trace it: the time it is handed is
            // quantised, so the result is the same picture either way.
        }

        take.shot(*take.scene, f);

        const auto frameStart = Clock::now();
        RenderTargets targets;
        if (take.renderer) {
            if (!take.renderer(*take.scene, settings, targets)) {
                setError(error, "take: the renderer failed on frame " + std::to_string(i));
                return false;
            }
        } else {
            renderPath(*take.scene, settings, nullptr, &targets);
        }

        if (take.denoiseFrames) {
            targets.color = take.filter ? take.filter(targets, take.denoiseSettings)
                                        : denoise(targets, take.denoiseSettings);
        }
        Image frame = take.finish ? take.finish(targets, f) : targets.color;
        const double frameSeconds =
            std::chrono::duration<double>(Clock::now() - frameStart).count();

        if (!pngSave(path, frame, take.tone, error)) return false;

        ++local.framesTraced;
        tracedSeconds += frameSeconds;

        if (take.progress) {
            const int tracesLeft = tracesUpTo(last, step) - tracesUpTo(i, step);
            const double average = tracedSeconds / double(local.framesTraced);
            std::printf("  %04d/%04d  %5.1fs  eta %s\n", i, total - 1, frameSeconds,
                        formatDuration(average * double(tracesLeft)).c_str());
            std::fflush(stdout);
        }
    }

    local.seconds = std::chrono::duration<double>(Clock::now() - started).count();

    if (take.progress) {
        std::printf("[%s] %d traced, %d held, %d already there -- %s\n", take.name.c_str(),
                    local.framesTraced, local.framesHeld, local.framesSkipped,
                    formatDuration(local.seconds).c_str());
    }

    if (stats) *stats = local;

    if (take.writeApng) {
        // Only worth attempting when the whole sequence is on disk; a part
        // range is a deliberate half-finished state, not an error.
        if (first == 0 && last == total - 1) {
            if (!assembleApng(take, error)) return false;
            if (take.progress) std::printf("[%s] wrote %s\n", take.name.c_str(),
                                           apngPath(take).c_str());
            if (take.writeMp4) {
                if (!assembleMp4(take, error)) return false;
                if (take.progress) std::printf("[%s] wrote %s\n", take.name.c_str(),
                                               mp4Path(take).c_str());
            }
        } else if (take.progress) {
            std::printf("[%s] part range rendered; run again over the whole take to assemble\n",
                        take.name.c_str());
        }
    }
    return true;
}

bool assembleMp4(const Take& take, std::string* error) {
    std::vector<std::string> paths;
    paths.reserve(size_t(take.timing.frameCount()));
    for (int i = 0; i < take.timing.frameCount(); ++i) paths.push_back(takeFramePath(take, i));

    Mp4Stats stats;
    if (!encodeMp4(paths, take.timing.fps, mp4Path(take), take.videoQp, &stats, error)) return false;

    if (take.progress) {
        std::printf("[%s] mp4: %d frames as %d samples, %.1f MiB of payload\n",
                    take.name.c_str(), stats.inputFrames, stats.samples,
                    double(stats.bytes) / 1048576.0);
        if (stats.interMacroblocks > 0) {
            std::printf("[%s] mp4: %lld of %lld predicted macroblocks moved by a fraction "
                        "of a sample (%.0f%%)\n",
                        take.name.c_str(), stats.fractionalVectors, stats.interMacroblocks,
                        100.0 * double(stats.fractionalVectors) /
                            double(stats.interMacroblocks));
        }
    }
    return true;
}

bool assembleApng(const Take& take, std::string* error) {
    const int total = take.timing.frameCount();
    const int fps = take.timing.fps < 1 ? 1 : take.timing.fps;

    ApngWriter writer;
    if (!writer.begin(take.settings.width, take.settings.height, take.apngPlays, error)) {
        return false;
    }

    // A run of byte-identical frames becomes one frame with a longer delay.
    // Held frames are byte copies by construction, so this collapses every
    // hold -- and it would collapse a stretch that happens to be identical
    // for some other reason too, which costs nothing and is never wrong.
    std::vector<uint8_t> pending;
    int run = 0;

    for (int i = 0; i < total; ++i) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(takeFramePath(take, i), bytes) || bytes.empty()) {
            setError(error, "take: frame " + takeFramePath(take, i) +
                                " is missing; render the whole take before assembling");
            return false;
        }

        if (run > 0 && bytes == pending) { ++run; continue; }

        if (run > 0 && !writer.addEncodedFrame(pending.data(), pending.size(), run, fps, error)) {
            return false;
        }
        pending = std::move(bytes);
        run = 1;
    }

    if (run > 0 && !writer.addEncodedFrame(pending.data(), pending.size(), run, fps, error)) {
        return false;
    }
    return writer.save(apngPath(take), error);
}

} // namespace blocky
