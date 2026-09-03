#include "engine/video/encode.hpp"

#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/video/h264.hpp"
#include "engine/video/mp4.hpp"

namespace blocky {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

} // namespace

bool encodeMp4(const std::vector<std::string>& pngPaths, int fps, const std::string& outPath,
               int qp, Mp4Stats* stats, std::string* error) {
    if (pngPaths.empty()) {
        setError(error, "mp4: no frames to encode");
        return false;
    }
    if (fps < 1) fps = 1;

    // A tick of 1000 per frame means any integer frame rate divides exactly,
    // so a held frame is 2000 ticks and nothing ever drifts.
    const uint32_t timescale = uint32_t(fps) * 1000u;
    const uint32_t frameTicks = 1000u;

    H264Encoder encoder;
    Mp4Writer writer;

    Mp4Stats local;
    local.inputFrames = int(pngPaths.size());

    std::vector<uint8_t> pendingBytes;   // the encoded sample waiting to be written
    std::vector<uint8_t> pendingSource;  // the PNG it came from, for comparison
    uint32_t pendingTicks = 0;
    bool     pendingSync = false;
    bool     started = false;

    // Presentation time from the last keyframe up to the pending sample. It
    // counts ticks rather than samples so that held frames pull their weight:
    // two seconds of a still image is still two seconds to scrub through.
    uint32_t ticksSinceKey = 0;

    for (const std::string& path : pngPaths) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(path, bytes) || bytes.empty()) {
            setError(error, "mp4: cannot read " + path);
            return false;
        }

        // A repeat of the frame before it does not need encoding again: it
        // extends how long the previous sample is shown.
        if (started && bytes == pendingSource) {
            pendingTicks += frameTicks;
            continue;
        }

        ImageU8 image;
        if (!pngDecode(bytes.data(), bytes.size(), image, error)) return false;

        if (!started) {
            if (!encoder.begin(image.width(), image.height(), fps, qp, error)) return false;
            if (!writer.begin(encoder.displayWidth(), encoder.displayHeight(), timescale,
                              encoder.avcc(), error)) {
                return false;
            }
            local.width = image.width();
            local.height = image.height();
            started = true;
        } else {
            if (image.width() != local.width || image.height() != local.height) {
                setError(error, "mp4: " + path + " is a different size from the first frame");
                return false;
            }
            writer.addSample(pendingBytes, pendingTicks, pendingSync);
            ++local.samples;
            if (pendingSync) ++local.keyframes;
            ticksSinceKey += pendingTicks;
        }

        pendingBytes = encoder.encode(image, ticksSinceKey >= timescale);
        if (pendingBytes.empty()) {
            setError(error, "mp4: the encoder produced nothing for " + path);
            return false;
        }
        local.interMacroblocks += encoder.lastFrameInterMacroblocks();
        local.fractionalVectors += encoder.lastFrameFractionalVectors();
        pendingSync = encoder.lastFrameWasKeyframe();
        if (pendingSync) ticksSinceKey = 0;
        pendingSource = std::move(bytes);
        pendingTicks = frameTicks;
    }

    if (!pendingBytes.empty()) {
        writer.addSample(pendingBytes, pendingTicks, pendingSync);
        ++local.samples;
        if (pendingSync) ++local.keyframes;
    }

    if (!writer.save(outPath, error)) return false;

    local.bytes = writer.payloadBytes();
    if (stats) *stats = local;
    return true;
}

} // namespace blocky
