#pragma once
// An MP4 muxer: one video track, AVC, nothing else.
//
// MP4 is ISO Base Media Format, which is a tree of length-tagged boxes and
// genuinely simple -- the difficulty in writing video is entirely in the
// codec, and none of it is here. What this file has to get right is
// bookkeeping: every sample's size, its duration, and its absolute byte
// offset in the finished file.
//
// ------------------------------------------------------------ why it matters
//
// The sample table is the only thing that says where a frame is. Get an
// offset wrong by one and the file is not slightly wrong, it is undecodable
// from that point on -- and nothing in the writing of it will complain.
//
// -------------------------------------------------------------- on twos again
//
// `stts` maps samples to durations as a run-length list, so a frame that is
// held for two frame periods is one sample with twice the duration rather
// than two copies of the same picture. The same saving APNG gives, from a
// completely different mechanism, and for the same reason: both formats let a
// frame say how long it lasts instead of assuming.
#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

class Mp4Writer {
public:
    // `timescale` is the clock samples are measured in, in units per second.
    // Choosing fps*1000 makes one frame exactly 1000 ticks, so no frame rate
    // ever lands on a repeating fraction.
    //
    // `avcc` is the AVCDecoderConfigurationRecord: the parameter sets a
    // decoder needs before it can read the first frame. In MP4 they live in
    // the header rather than in the stream.
    bool begin(int width, int height, uint32_t timescale, const std::vector<uint8_t>& avcc,
               std::string* error = nullptr);

    // One coded frame, as length-prefixed NAL units. `duration` is in
    // timescale units.
    //
    // `syncSample` is whether a player may seek straight to this frame, which
    // is true of an IDR and of nothing else. It is the only reason the
    // container needs to know anything about the codec: the sync sample table
    // is what a scrub bar reads, and a P frame listed in it sends the decoder
    // off predicting from a picture it never decoded.
    void addSample(const std::vector<uint8_t>& sample, uint32_t duration,
                   bool syncSample = true);

    bool save(const std::string& path, std::string* error = nullptr);

    int    sampleCount() const { return int(sizes_.size()); }
    size_t payloadBytes() const { return mdat_.size(); }

private:
    std::vector<uint8_t>  mdat_;
    std::vector<uint32_t> sizes_;
    std::vector<uint32_t> durations_;
    std::vector<uint32_t> offsets_;   // relative to the start of the mdat payload
    std::vector<uint8_t>  sync_;      // one per sample: seekable or not
    std::vector<uint8_t>  avcc_;

    int      width_ = 0, height_ = 0;
    uint32_t timescale_ = 24000;
    bool     started_ = false;
};

} // namespace blocky
