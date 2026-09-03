#pragma once
// An H.264 encoder, written from the specification.
//
// -------------------------------------------------------------- where it is
//
// Baseline. The first frame is an IDR, the rest are P pictures, and another
// IDR comes round once a second of shown time so that seeking has somewhere
// to land.
//
// An intra macroblock picks between I_16x16 (four luma modes), I_NxN with all
// nine I_4x4 modes and most-probable-mode signalling, and I_PCM as a floor.
// An inter one is P_Skip or P_L0_16x16: one quarter-sample vector for the
// whole macroblock against one reference. Intra stays available inside a P slice --
// its macroblock type numbers shift by five -- and wins wherever the picture
// grew something the previous one did not have. Residual goes through the 4x4
// integer transform, with a Hadamard over the DC coefficients where I_16x16
// makes them worth collecting, and is coded with CAVLC.
//
// I_PCM began as a bisection and stayed as a floor. A container and an
// entropy coder written together give one verdict for two pieces of work: if
// the file does not play, nothing says which half is wrong. Coding every
// macroblock raw made the stream correct by construction, so anything that
// broke was the container. It stays because a coded macroblock is only
// *almost* always smaller than the raw samples: past 384 bytes of bits the
// macroblock goes in raw, so this encoder cannot do worse than remembering
// the pixels.
//
// ------------------------------------------------- what a reference changes
//
// Reference pictures are the deblocked ones, so the filter stopped being an
// output stage the decoder could be left to do, and became part of encoding.
// And the cost of getting reconstruction wrong changed in kind, not degree:
// an all-intra encoder that reconstructs differently from the decoder spoils
// one macroblock, while this one spoils the reference, predicts the next
// frame from a picture the decoder never had, and compounds. The signature is
// a PSNR that slides down a run of P frames and jumps back at every IDR. A
// flat one means the two agree.
//
// Measured on the four-second `sway_draft` take at QP 24, 96 frames on twos
// and so 48 coded pictures: all-intra 476 KiB at 47.06 dB, P pictures with
// whole-sample vectors 299 KiB at 46.31 dB, and quarter-sample vectors 277
// KiB at the same quality. x264 constrained to the same profile and the same
// keyframe spacing reaches that quality in 140 KiB, so the remainder is a
// factor of 1.97 -- and where it lives is now an open question rather than a
// list. Quarter samples were the largest named item and returned 7%.
//
// ------------------------------------------------------------------- colour
//
// Frames arrive as 8-bit sRGB and leave as BT.709 limited-range Y'CbCr at
// 4:2:0. The stream says so in its VUI rather than leaving a player to guess
// from the frame size, which is the usual reason a render comes out of a
// video file looking slightly wrong in a way nobody can name.
#include "engine/core/image.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace blocky {

// Planar 4:2:0. Dimensions are the *coded* size, padded up to whole
// macroblocks; the display size travels separately in the cropping fields.
struct YuvFrame {
    int width = 0, height = 0;
    std::vector<uint8_t> y;
    std::vector<uint8_t> cb, cr;

    bool valid() const {
        return width > 0 && height > 0 && y.size() == size_t(width) * size_t(height) &&
               cb.size() == y.size() / 4 && cr.size() == cb.size();
    }
};

// Convert and pad in one step. Padding replicates the edge pixels rather than
// filling with black, so the samples a decoder throws away on crop do not
// drag a dark fringe into the ones it keeps.
YuvFrame rgbToYuv420(const ImageU8& image, int codedWidth, int codedHeight);

class H264Encoder {
public:
    // `fps` only reaches the VUI timing fields; the container carries the
    // authoritative clock. `qp` is the quantisation parameter, 0..51: lower
    // is finer and larger, and every six steps doubles the step size, so 24
    // is twice the detail of 30 and half of 18.
    bool begin(int displayWidth, int displayHeight, int fps, int qp = 24,
               std::string* error = nullptr);

    // One frame, as a length-prefixed NAL unit ready to drop into mdat.
    //
    // The first frame is an IDR; every frame after it is a P picture unless
    // `forceKeyframe` asks for another IDR. Where those go is a decision
    // about seeking, not about compression, and it needs a clock -- how long
    // each frame is shown -- which this class does not have. The caller does,
    // so the caller decides.
    std::vector<uint8_t> encode(const ImageU8& image, bool forceKeyframe = false);

    // Whether the frame just encoded was an IDR, and so a point a player can
    // seek to. This is what the container's sync sample table is built from.
    bool lastFrameWasKeyframe() const { return lastWasIdr_; }

    // The parameter sets, packed as an AVCDecoderConfigurationRecord for the
    // avcC box.
    std::vector<uint8_t> avcc() const;

    int codedWidth() const { return mbWidth_ * 16; }
    int codedHeight() const { return mbHeight_ * 16; }
    // The reconstructed, deblocked picture of the last frame encoded. This is
    // what a conformant decoder must produce, so comparing it against one is
    // the only real proof the filter is right.
    const YuvFrame& filteredPicture() const { return filtered_; }

    int displayWidth() const { return displayWidth_; }
    int displayHeight() const { return displayHeight_; }

    // How many macroblocks of the last picture predicted from a fractional
    // position, and how many predicted at all. A measurement, not a promise:
    // an I picture reports zero, and so does a P picture whose motion
    // happened to land on whole samples. It is here because agreement with an
    // outside decoder proves nothing about the interpolation filter on a
    // sequence that never asked for one.
    int lastFrameFractionalVectors() const { return fractionalVectors_; }
    int lastFrameInterMacroblocks() const { return interMacroblocks_; }

private:
    std::vector<uint8_t> buildSps() const;
    std::vector<uint8_t> buildPps() const;

    int displayWidth_ = 0, displayHeight_ = 0;
    int mbWidth_ = 0, mbHeight_ = 0;
    int fps_ = 24;
    int qp_ = 24;
    int frameIndex_ = 0;

    // The picture as a decoder will have it. Intra prediction reads
    // reconstructed neighbours, not source ones, so the encoder has to keep
    // its own copy and it has to match the decoder's exactly.
    std::vector<uint8_t> reconY_, reconCb_, reconCr_;

    // Non-zero coefficient counts per 4x4 block, which is the context that
    // picks the CAVLC code book for the blocks after them.
    std::vector<uint8_t> coeffY_, coeffCb_, coeffCr_;

    // The 4x4 prediction mode each luma block used, or DC for blocks in a
    // macroblock that was not coded as I_NxN. This is what the most probable
    // mode is derived from, so it has to outlive the macroblock.
    std::vector<uint8_t> modeY_;

    // Which macroblocks were stored raw. The deblocking filter treats those
    // as having a quantiser of zero, which switches it off across their
    // edges -- there is no quantisation error in a raw block to smooth.
    std::vector<uint8_t> pcmFlags_;

    // The picture after deblocking: what a decoder outputs, and what the next
    // frame predicts from. Kept separately because intra prediction must go
    // on reading the unfiltered samples.
    YuvFrame filtered_;

    // The reference picture: the previous frame's `filtered_`, held as its own
    // copy because `filtered_` is overwritten while the next frame is coded.
    // It has to be the filtered one -- a decoder has nothing else -- and that
    // is the whole reason the filter had to be written before any of this.
    std::vector<uint8_t> refY_, refCb_, refCr_;
    bool haveReference_ = false;

    // frame_num counts reference pictures since the last IDR and resets with
    // it. It is four bits wide here, so it wraps at sixteen, which is legal
    // and expected; a decoder tracks it modulo the same number.
    int  frameNum_ = 0;
    bool lastWasIdr_ = false;

    int fractionalVectors_ = 0;
    int interMacroblocks_ = 0;

    // Per macroblock, for the deblocking filter and for the motion vector
    // predictor: whether it was coded intra, and what it moved by, in quarter
    // samples. An intra macroblock holds a zero vector and is recognised by
    // the flag, not by the vector.
    std::vector<uint8_t> mbIntra_;
    std::vector<int16_t> mvX_, mvY_;

    std::vector<uint8_t> sps_, pps_;   // RBSP with the NAL header byte
    bool started_ = false;
};

} // namespace blocky
