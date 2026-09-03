#include "engine/video/h264.hpp"

#include "engine/video/bitstream.hpp"
#include "engine/video/cavlc.hpp"
#include "engine/video/deblock.hpp"
#include "engine/video/intra4x4.hpp"
#include "engine/video/inter.hpp"
#include "engine/video/transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace blocky {
namespace {

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

uint8_t clampSample(float v) {
    const int i = int(std::lround(v));
    return uint8_t(i < 1 ? 1 : (i > 254 ? 254 : i));
}

uint8_t clip8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

std::vector<uint8_t> makeNal(uint8_t refIdc, uint8_t type, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> nal;
    nal.push_back(uint8_t((refIdc << 5) | type));
    const std::vector<uint8_t> escaped = rbspEscape(rbsp);
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

void appendLengthPrefixed(std::vector<uint8_t>& out, const std::vector<uint8_t>& nal) {
    const uint32_t size = uint32_t(nal.size());
    out.push_back(uint8_t(size >> 24));
    out.push_back(uint8_t(size >> 16));
    out.push_back(uint8_t(size >> 8));
    out.push_back(uint8_t(size));
    out.insert(out.end(), nal.begin(), nal.end());
}

// A plane of samples with a stride, so prediction can read neighbours without
// every call passing three numbers around.
struct Plane {
    uint8_t* data = nullptr;
    int width = 0, height = 0;

    uint8_t at(int x, int y) const { return data[size_t(y) * size_t(width) + size_t(x)]; }
    void set(int x, int y, uint8_t v) { data[size_t(y) * size_t(width) + size_t(x)] = v; }
};

// The sixteen 4x4 luma blocks of a macroblock are not in raster order. They
// run in a Z through each 8x8 quadrant and then through the quadrants, so
// that a block's neighbours have usually been coded already.
void luma4x4Position(int blkIdx, int& x4, int& y4) {
    const int quadrant = blkIdx / 4;
    const int within = blkIdx % 4;
    x4 = (quadrant % 2) * 2 + (within % 2);
    y4 = (quadrant / 2) * 2 + (within / 2);
}

// ------------------------------------------------------------------ prediction
//
// Intra prediction fills a block from the samples along its top and left
// edges -- the ones already reconstructed. There is no clever search here:
// the modes are few, so every legal one is tried and the closest wins.

enum : int { kPredVertical = 0, kPredHorizontal = 1, kPredDc = 2, kPredPlane = 3 };

void predict16x16(const Plane& rec, int px, int py, int mode, bool left, bool above,
                  uint8_t pred[256]) {
    switch (mode) {
        case kPredVertical:
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) pred[y * 16 + x] = rec.at(px + x, py - 1);
            return;

        case kPredHorizontal:
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) pred[y * 16 + x] = rec.at(px - 1, py + y);
            return;

        case kPredDc: {
            int value;
            if (left && above) {
                int sum = 0;
                for (int i = 0; i < 16; ++i) sum += rec.at(px + i, py - 1) + rec.at(px - 1, py + i);
                value = (sum + 16) >> 5;
            } else if (above) {
                int sum = 0;
                for (int i = 0; i < 16; ++i) sum += rec.at(px + i, py - 1);
                value = (sum + 8) >> 4;
            } else if (left) {
                int sum = 0;
                for (int i = 0; i < 16; ++i) sum += rec.at(px - 1, py + i);
                value = (sum + 8) >> 4;
            } else {
                // Nothing to predict from at all: the middle of the range.
                value = 128;
            }
            for (int i = 0; i < 256; ++i) pred[i] = uint8_t(value);
            return;
        }

        default: {
            // Plane: fit a tilted flat surface through the edges. This is the
            // mode that earns its keep on gradients, where vertical and
            // horizontal both leave a visible step.
            int h = 0, v = 0;
            for (int i = 0; i < 8; ++i) {
                h += (i + 1) * (rec.at(px + 8 + i, py - 1) - rec.at(px + 6 - i, py - 1));
                v += (i + 1) * (rec.at(px - 1, py + 8 + i) - rec.at(px - 1, py + 6 - i));
            }
            const int a = 16 * (rec.at(px - 1, py + 15) + rec.at(px + 15, py - 1));
            const int b = (5 * h + 32) >> 6;
            const int c = (5 * v + 32) >> 6;
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    pred[y * 16 + x] = clip8((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
                }
            }
            return;
        }
    }
}

// Chroma numbers its modes differently from luma -- DC is first here -- which
// is a trap worth naming rather than a thing to remember.
enum : int { kChromaDc = 0, kChromaHorizontal = 1, kChromaVertical = 2, kChromaPlane = 3 };

void predictChroma8x8(const Plane& rec, int px, int py, int mode, bool left, bool above,
                      uint8_t pred[64]) {
    switch (mode) {
        case kChromaHorizontal:
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) pred[y * 8 + x] = rec.at(px - 1, py + y);
            return;

        case kChromaVertical:
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) pred[y * 8 + x] = rec.at(px + x, py - 1);
            return;

        case kChromaPlane: {
            int h = 0, v = 0;
            for (int i = 0; i < 4; ++i) {
                h += (i + 1) * (rec.at(px + 4 + i, py - 1) - rec.at(px + 2 - i, py - 1));
                v += (i + 1) * (rec.at(px - 1, py + 4 + i) - rec.at(px - 1, py + 2 - i));
            }
            const int a = 16 * (rec.at(px - 1, py + 7) + rec.at(px + 7, py - 1));
            const int b = (34 * h + 32) >> 6;
            const int c = (34 * v + 32) >> 6;
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    pred[y * 8 + x] = clip8((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
                }
            }
            return;
        }

        default: {
            // Chroma DC is decided per 4x4 quarter, and which edge each
            // quarter prefers is not symmetric: the top right quarter looks
            // up first, the bottom left looks left first. Averaging both
            // everywhere would be simpler and would not be this mode.
            for (int q = 0; q < 4; ++q) {
                const int ox = (q % 2) * 4;
                const int oy = (q / 2) * 4;

                int sumAbove = 0, sumLeft = 0;
                for (int i = 0; i < 4; ++i) {
                    sumAbove += above ? rec.at(px + ox + i, py - 1) : 0;
                    sumLeft += left ? rec.at(px - 1, py + oy + i) : 0;
                }

                int value;
                const bool corner = (ox == 0 && oy == 0) || (ox > 0 && oy > 0);
                if (corner && left && above)      value = (sumAbove + sumLeft + 4) >> 3;
                else if (ox > 0 && oy == 0)       value = above ? (sumAbove + 2) >> 2
                                                          : (left ? (sumLeft + 2) >> 2 : 128);
                else if (ox == 0 && oy > 0)       value = left ? (sumLeft + 2) >> 2
                                                          : (above ? (sumAbove + 2) >> 2 : 128);
                else if (left)                    value = (sumLeft + 2) >> 2;
                else if (above)                   value = (sumAbove + 2) >> 2;
                else                              value = 128;

                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) pred[(oy + y) * 8 + ox + x] = uint8_t(value);
                }
            }
            return;
        }
    }
}

// Whether the block up and to the right of `blkIdx` has been decoded yet.
//
// Inside a macroblock this is pure scan order: the Z shape means five of the
// sixteen blocks have no such neighbour however complete the picture is. On
// the top row of a macroblock it is the macroblock above, and for the last
// block of that row it is the macroblock above and to the right.
bool aboveRightAvailable(int blkIdx, int mbx, int mby, int mbWidth) {
    switch (blkIdx) {
        case 3: case 7: case 11: case 13: case 15:
            return false;
        case 5:
            return mby > 0 && mbx + 1 < mbWidth;
        case 0: case 1: case 4:
            return mby > 0;
        default:
            return true;
    }
}

int sad(const uint8_t* a, const Plane& source, int px, int py, int size) {
    int total = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            total += std::abs(int(a[y * size + x]) - int(source.at(px + x, py + y)));
        }
    }
    return total;
}

long long ssd(const uint8_t* a, const Plane& source, int px, int py, int size) {
    long long total = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const long long d = int(a[y * size + x]) - int(source.at(px + x, py + y));
            total += d * d;
        }
    }
    return total;
}

// How many bits an Exp-Golomb code would take, without writing it. Motion
// estimation asks this thousands of times per macroblock and never wants the
// bits themselves.
int ueBits(uint32_t value) {
    int n = 0;
    for (uint32_t x = value + 1; x > 1; x >>= 1) ++n;
    return 2 * n + 1;
}

int seBits(int value) {
    return ueBits(value > 0 ? uint32_t(2 * value - 1) : uint32_t(-2 * value));
}

int median3(int a, int b, int c) { return a + b + c - std::min(a, std::min(b, c)) -
                                          std::max(a, std::max(b, c)); }

// What a neighbouring macroblock contributes to the motion vector predictor.
//
// The two flags are not the same question and conflating them is a classic
// way to get the predictor subtly wrong. `available` is about the picture:
// whether there is a macroblock there at all. `hasMotion` is about the
// macroblock: an intra one is perfectly available and still contributes a
// zero vector with no reference index, which is a different thing from not
// being there -- the two are treated differently by the rule below.
struct MvNeighbour {
    bool available = false;
    bool hasMotion = false;
    int  x = 0, y = 0;
};

} // namespace

YuvFrame rgbToYuv420(const ImageU8& image, int codedWidth, int codedHeight) {
    YuvFrame out;
    if (image.empty() || codedWidth <= 0 || codedHeight <= 0) return out;

    out.width = codedWidth;
    out.height = codedHeight;
    out.y.assign(size_t(codedWidth) * size_t(codedHeight), 16);

    const int chromaWidth = codedWidth / 2;
    const int chromaHeight = codedHeight / 2;
    out.cb.assign(size_t(chromaWidth) * size_t(chromaHeight), 128);
    out.cr.assign(out.cb.size(), 128);

    constexpr float kR = 0.2126f, kB = 0.0722f;
    constexpr float kG = 1.0f - kR - kB;

    std::vector<float> fullCb(size_t(codedWidth) * size_t(codedHeight), 0.0f);
    std::vector<float> fullCr(fullCb.size(), 0.0f);

    const int sourceWidth = image.width();
    const int sourceHeight = image.height();

    for (int y = 0; y < codedHeight; ++y) {
        const int sy = std::min(y, sourceHeight - 1);
        for (int x = 0; x < codedWidth; ++x) {
            const int sx = std::min(x, sourceWidth - 1);
            const ImageU8::RGBA c = image.get(sx, sy);

            const float r = float(c.r) / 255.0f;
            const float g = float(c.g) / 255.0f;
            const float b = float(c.b) / 255.0f;

            const float luma = kR * r + kG * g + kB * b;
            const size_t i = size_t(y) * size_t(codedWidth) + size_t(x);

            out.y[i] = clampSample(16.0f + 219.0f * luma);
            fullCb[i] = (b - luma) / (2.0f * (1.0f - kB));
            fullCr[i] = (r - luma) / (2.0f * (1.0f - kR));
        }
    }

    for (int y = 0; y < chromaHeight; ++y) {
        for (int x = 0; x < chromaWidth; ++x) {
            const size_t a = size_t(y * 2) * size_t(codedWidth) + size_t(x * 2);
            const size_t b = a + 1;
            const size_t c = a + size_t(codedWidth);
            const size_t d = c + 1;

            const float cb = (fullCb[a] + fullCb[b] + fullCb[c] + fullCb[d]) * 0.25f;
            const float cr = (fullCr[a] + fullCr[b] + fullCr[c] + fullCr[d]) * 0.25f;

            const size_t i = size_t(y) * size_t(chromaWidth) + size_t(x);
            out.cb[i] = clampSample(128.0f + 224.0f * cb);
            out.cr[i] = clampSample(128.0f + 224.0f * cr);
        }
    }
    return out;
}

bool H264Encoder::begin(int displayWidth, int displayHeight, int fps, int qp, std::string* error) {
    if (displayWidth <= 0 || displayHeight <= 0) {
        setError(error, "h264: refusing to encode a frame with no size");
        return false;
    }
    if (displayWidth % 2 != 0 || displayHeight % 2 != 0) {
        setError(error, "h264: 4:2:0 needs even dimensions; " + std::to_string(displayWidth) +
                            "x" + std::to_string(displayHeight) + " is not");
        return false;
    }
    if (qp < 0 || qp > 51) {
        setError(error, "h264: qp must be between 0 and 51");
        return false;
    }

    displayWidth_ = displayWidth;
    displayHeight_ = displayHeight;
    mbWidth_ = (displayWidth + 15) / 16;
    mbHeight_ = (displayHeight + 15) / 16;
    fps_ = fps < 1 ? 1 : fps;
    qp_ = qp;
    frameIndex_ = 0;

    sps_ = buildSps();
    pps_ = buildPps();
    started_ = true;
    return true;
}

std::vector<uint8_t> H264Encoder::buildSps() const {
    BitWriter w;

    w.u(8, 66);    // profile_idc: Baseline
    w.flag(true);  // constraint_set0
    w.flag(true);  // constraint_set1
    w.flag(false); // constraint_set2
    w.flag(false); w.flag(false); w.flag(false);
    w.u(2, 0);
    w.u(8, 51);    // level_idc 5.1

    w.ue(0);       // seq_parameter_set_id
    w.ue(0);       // log2_max_frame_num_minus4
    w.ue(2);       // pic_order_cnt_type 2: display order is decode order
    w.ue(1);       // max_num_ref_frames
    w.flag(false); // gaps_in_frame_num_value_allowed_flag

    w.ue(uint32_t(mbWidth_ - 1));
    w.ue(uint32_t(mbHeight_ - 1));
    w.flag(true);  // frame_mbs_only_flag
    w.flag(true);  // direct_8x8_inference_flag

    const int cropRight = (mbWidth_ * 16 - displayWidth_) / 2;
    const int cropBottom = (mbHeight_ * 16 - displayHeight_) / 2;
    const bool cropping = cropRight != 0 || cropBottom != 0;
    w.flag(cropping);
    if (cropping) {
        w.ue(0);
        w.ue(uint32_t(cropRight));
        w.ue(0);
        w.ue(uint32_t(cropBottom));
    }

    w.flag(true);   // vui_parameters_present_flag
    {
        w.flag(false);  // aspect_ratio_info_present_flag
        w.flag(false);  // overscan_info_present_flag
        w.flag(true);   // video_signal_type_present_flag
        w.u(3, 5);      // video_format: unspecified
        w.flag(false);  // video_full_range_flag: limited range
        w.flag(true);   // colour_description_present_flag
        w.u(8, 1);      // colour_primaries: BT.709
        w.u(8, 1);      // transfer_characteristics: BT.709
        w.u(8, 1);      // matrix_coefficients: BT.709
        w.flag(false);  // chroma_loc_info_present_flag
        w.flag(true);   // timing_info_present_flag
        w.u(32, 1000);
        w.u(32, uint32_t(fps_) * 2000u);
        w.flag(true);   // fixed_frame_rate_flag
        w.flag(false);  // nal_hrd_parameters_present_flag
        w.flag(false);  // vcl_hrd_parameters_present_flag
        w.flag(false);  // pic_struct_present_flag
        w.flag(false);  // bitstream_restriction_flag
    }

    w.trailingBits();
    return w.data();
}

std::vector<uint8_t> H264Encoder::buildPps() const {
    BitWriter w;

    w.ue(0);        // pic_parameter_set_id
    w.ue(0);        // seq_parameter_set_id
    w.flag(false);  // entropy_coding_mode_flag: CAVLC
    w.flag(false);  // bottom_field_pic_order_in_frame_present_flag
    w.ue(0);        // num_slice_groups_minus1
    w.ue(0);        // num_ref_idx_l0_default_active_minus1
    w.ue(0);        // num_ref_idx_l1_default_active_minus1
    w.flag(false);  // weighted_pred_flag
    w.u(2, 0);      // weighted_bipred_idc
    w.se(0);        // pic_init_qp_minus26
    w.se(0);        // pic_init_qs_minus26
    w.se(0);        // chroma_qp_index_offset
    w.flag(true);   // deblocking_filter_control_present_flag
    w.flag(false);  // constrained_intra_pred_flag
    w.flag(false);  // redundant_pic_cnt_present_flag

    w.trailingBits();
    return w.data();
}

std::vector<uint8_t> H264Encoder::encode(const ImageU8& image, bool forceKeyframe) {
    std::vector<uint8_t> out;
    if (!started_ || image.empty()) return out;

    const YuvFrame frame = rgbToYuv420(image, codedWidth(), codedHeight());
    if (!frame.valid()) return out;

    const int lumaWidth = frame.width;
    const int lumaHeight = frame.height;
    const int chromaWidth = lumaWidth / 2;
    const int chromaHeight = lumaHeight / 2;
    const size_t mbCount = size_t(mbWidth_) * size_t(mbHeight_);

    // An IDR on the first frame, when asked for one, or when there is nothing
    // to predict from. Everything else is a P picture.
    const bool idr = frameIndex_ == 0 || forceKeyframe || !haveReference_ ||
                     refY_.size() != frame.y.size();
    const bool pSlice = !idr;

    if (idr) frameNum_ = 0;
    lastWasIdr_ = idr;

    reconY_.assign(size_t(lumaWidth) * size_t(lumaHeight), 0);
    reconCb_.assign(size_t(chromaWidth) * size_t(chromaHeight), 0);
    reconCr_.assign(reconCb_.size(), 0);

    const int blocksWide = mbWidth_ * 4;
    const int blocksHigh = mbHeight_ * 4;
    coeffY_.assign(size_t(blocksWide) * size_t(blocksHigh), 0);
    coeffCb_.assign(size_t(mbWidth_ * 2) * size_t(mbHeight_ * 2), 0);
    coeffCr_.assign(coeffCb_.size(), 0);
    modeY_.assign(size_t(blocksWide) * size_t(blocksHigh), uint8_t(h264::kIntra4Dc));
    pcmFlags_.assign(mbCount, 0);
    mbIntra_.assign(mbCount, 1);
    mvX_.assign(mbCount, 0);
    mvY_.assign(mbCount, 0);

    Plane recY{reconY_.data(), lumaWidth, lumaHeight};
    Plane recCb{reconCb_.data(), chromaWidth, chromaHeight};
    Plane recCr{reconCr_.data(), chromaWidth, chromaHeight};

    Plane srcY{const_cast<uint8_t*>(frame.y.data()), lumaWidth, lumaHeight};
    Plane srcCb{const_cast<uint8_t*>(frame.cb.data()), chromaWidth, chromaHeight};
    Plane srcCr{const_cast<uint8_t*>(frame.cr.data()), chromaWidth, chromaHeight};

    h264::RefPicture ref;
    if (pSlice) {
        ref.y = refY_.data();
        ref.cb = refCb_.data();
        ref.cr = refCr_.data();
        ref.width = lumaWidth;
        ref.height = lumaHeight;
    }

    const int qp = qp_;
    const int qpc = h264::chromaQp(qp);

    // What a mode signal is worth against a unit of distortion. Rough, but
    // without it the encoder happily spends four bits to save two of residual.
    const int lambda = 2 << (qp / 6);

    // The P slice decisions need a different currency. Comparing two codings
    // of the same macroblock by bits alone works while both reconstruct to
    // roughly the same quality, which is true of two intra codings at one
    // quantiser. It is not true of P_Skip, which spends almost no bits and
    // accepts whatever error the previous frame leaves behind. So a P slice
    // weighs squared error against bits, with the usual weight: distortion
    // and rate meet at a slope that doubles every three steps of QP.
    const double lambdaRdReal = 0.85 * std::pow(2.0, double(qp - 12) / 3.0);
    const long long lambdaRd = std::max(1LL, (long long)(lambdaRdReal + 0.5));
    // Motion estimation compares sums of absolute differences, not squares,
    // so its weight is the square root of the other one.
    const int lambdaMv = std::max(1, int(std::sqrt(double(lambdaRd)) + 0.5));

    BitWriter w;

    // ---- slice header
    w.ue(0);                            // first_mb_in_slice
    w.ue(pSlice ? 5u : 7u);             // slice_type: every slice in this picture is P, or I
    w.ue(0);                            // pic_parameter_set_id
    w.u(4, uint32_t(frameNum_ & 15));   // frame_num
    if (idr) w.ue(uint32_t(frameIndex_ & 1));   // idr_pic_id: only has to differ from the last
    if (pSlice) {
        w.flag(false);  // num_ref_idx_active_override_flag: the one from the PPS will do
        w.flag(false);  // ref_pic_list_modification_flag_l0: one reference, no reordering
    }
    if (idr) {
        w.flag(false);  // no_output_of_prior_pics_flag
        w.flag(false);  // long_term_reference_flag
    } else {
        w.flag(false);  // adaptive_ref_pic_marking_mode_flag: sliding window, one frame deep
    }
    w.se(qp - 26);      // slice_qp_delta

    // ---- deblocking: on
    //
    // While every frame was intra the filter was purely an output stage:
    // intra prediction is defined to read the reconstructed samples *before*
    // it runs, so a decoder did all of the work. That stopped being true here.
    // A reference picture is the filtered one, so the encoder now filters its
    // own reconstruction and predicts from that -- an encoder predicting from
    // an unfiltered copy would drift away from the decoder exactly the way a
    // wrong rescaling does, a little more with every frame.
    w.ue(0);        // disable_deblocking_filter_idc: filter every edge
    w.se(0);        // slice_alpha_c0_offset_div2
    w.se(0);        // slice_beta_offset_div2

    auto lumaNc = [&](int bx, int by) {
        const int a = bx > 0 ? int(coeffY_[size_t(by) * size_t(blocksWide) + size_t(bx - 1)]) : -1;
        const int b = by > 0 ? int(coeffY_[size_t(by - 1) * size_t(blocksWide) + size_t(bx)]) : -1;
        if (a >= 0 && b >= 0) return (a + b + 1) >> 1;
        if (a >= 0) return a;
        if (b >= 0) return b;
        return 0;
    };
    auto chromaNc = [&](const std::vector<uint8_t>& counts, int bx, int by) {
        const int stride = mbWidth_ * 2;
        const int a = bx > 0 ? int(counts[size_t(by) * size_t(stride) + size_t(bx - 1)]) : -1;
        const int b = by > 0 ? int(counts[size_t(by - 1) * size_t(stride) + size_t(bx)]) : -1;
        if (a >= 0 && b >= 0) return (a + b + 1) >> 1;
        if (a >= 0) return a;
        if (b >= 0) return b;
        return 0;
    };

    // ---------------------------------------------------------------- chroma
    //
    // Chroma is coded the same way whatever the luma path decided, so this is
    // one function called once per candidate prediction: intra chroma for the
    // intra paths, motion compensated chroma for the inter one.
    //
    // It writes the coefficient counts into the picture as it goes, because
    // the blocks after the first take their code book from the ones before
    // it, and then puts back what it found. A candidate that loses must leave
    // no trace.
    struct ChromaCoding {
        BitWriter bits;
        int       cbp = 0;
        uint8_t   recon[2][64] = {};
        uint8_t   counts[2][4] = {};
    };

    auto codeChroma = [&](int mbx, int mby, const uint8_t pred[2][64], bool intra,
                          ChromaCoding& coding) {
        const int cx = mbx * 8, cy = mby * 8;
        const int stride = mbWidth_ * 2;

        uint8_t saved[2][4];
        for (int b = 0; b < 4; ++b) {
            const size_t i = size_t(mby * 2 + b / 2) * size_t(stride) + size_t(mbx * 2 + b % 2);
            saved[0][b] = coeffCb_[i];
            saved[1][b] = coeffCr_[i];
        }

        int32_t dcQuant[2][4];
        int32_t acQuant[2][4][16];
        bool anyDc = false, anyAc = false;

        for (int plane = 0; plane < 2; ++plane) {
            const Plane& src = plane == 0 ? srcCb : srcCr;
            int32_t planeDc[4];

            for (int b = 0; b < 4; ++b) {
                const int ox = (b % 2) * 4, oy = (b / 2) * 4;
                int32_t block[16];
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        block[y * 4 + x] = int32_t(src.at(cx + ox + x, cy + oy + y)) -
                                           int32_t(pred[plane][(oy + y) * 8 + ox + x]);
                    }
                }
                h264::forward4x4(block);
                planeDc[b] = block[0];
                h264::quantize4x4(block, acQuant[plane][b], qpc, intra);
                acQuant[plane][b][0] = 0;
                for (int i = 1; i < 16; ++i) {
                    if (acQuant[plane][b][i] != 0) anyAc = true;
                }
            }

            h264::hadamard2x2(planeDc);
            for (int b = 0; b < 4; ++b) {
                dcQuant[plane][b] = h264::quantizeDcChroma(planeDc[b], qpc, intra);
                if (dcQuant[plane][b] != 0) anyDc = true;
            }
        }

        coding.cbp = anyAc ? 2 : (anyDc ? 1 : 0);

        if (coding.cbp != 0) {
            for (int plane = 0; plane < 2; ++plane) {
                h264::writeResidualBlock(coding.bits, dcQuant[plane], h264::BlockKind::ChromaDc4,
                                         -1);
            }
        }
        if (coding.cbp == 2) {
            for (int plane = 0; plane < 2; ++plane) {
                std::vector<uint8_t>& counts = plane == 0 ? coeffCb_ : coeffCr_;
                for (int b = 0; b < 4; ++b) {
                    const int bx4 = mbx * 2 + (b % 2);
                    const int by4 = mby * 2 + (b / 2);

                    int32_t zig[15];
                    for (int k = 1; k < 16; ++k) zig[k - 1] = acQuant[plane][b][h264::kZigZag4x4[k]];

                    const int count = h264::writeResidualBlock(
                        coding.bits, zig, h264::BlockKind::ChromaAc15,
                        chromaNc(counts, bx4, by4));
                    counts[size_t(by4) * size_t(stride) + size_t(bx4)] = uint8_t(count);
                    coding.counts[plane][b] = uint8_t(count);
                }
            }
        }

        for (int plane = 0; plane < 2; ++plane) {
            int32_t dcr[4];
            for (int i = 0; i < 4; ++i) dcr[i] = dcQuant[plane][i];
            h264::hadamard2x2(dcr);
            h264::dequantizeDcChroma(dcr, qpc);

            for (int b = 0; b < 4; ++b) {
                const int ox = (b % 2) * 4, oy = (b / 2) * 4;
                int32_t block[16];
                h264::dequantize4x4(acQuant[plane][b], block, qpc);
                block[0] = dcr[b];
                h264::inverse4x4(block);
                for (int y = 0; y < 4; ++y) {
                    for (int x = 0; x < 4; ++x) {
                        const int p = pred[plane][(oy + y) * 8 + ox + x];
                        coding.recon[plane][(oy + y) * 8 + ox + x] =
                            clip8(p + block[y * 4 + x]);
                    }
                }
            }
        }

        for (int b = 0; b < 4; ++b) {
            const size_t i = size_t(mby * 2 + b / 2) * size_t(stride) + size_t(mbx * 2 + b % 2);
            coeffCb_[i] = saved[0][b];
            coeffCr_[i] = saved[1][b];
        }
    };

    // ------------------------------------------------- the motion predictor
    //
    // A vector is written as its difference from a prediction made out of the
    // neighbours, because neighbouring macroblocks in a moving picture nearly
    // always move together -- which is the whole reason a vector is cheap.
    auto neighbourAt = [&](int mbx, int mby, bool inBounds) {
        MvNeighbour n;
        if (!inBounds) return n;
        n.available = true;
        const size_t i = size_t(mby) * size_t(mbWidth_) + size_t(mbx);
        if (mbIntra_[i]) return n;   // available, but with no reference index
        n.hasMotion = true;
        n.x = int(mvX_[i]);
        n.y = int(mvY_[i]);
        return n;
    };

    auto predictorOf = [&](const MvNeighbour& a, const MvNeighbour& b, const MvNeighbour& c,
                           int& outX, int& outY) {
        MvNeighbour A = a, B = b, C = c;
        // With nothing above, the left neighbour stands in for all three,
        // which makes the median below return it.
        if (!B.available && !C.available && A.available) {
            B = A;
            C = A;
        }
        const int matches = int(A.hasMotion) + int(B.hasMotion) + int(C.hasMotion);
        if (matches == 1) {
            const MvNeighbour& only = A.hasMotion ? A : (B.hasMotion ? B : C);
            outX = only.x;
            outY = only.y;
            return;
        }
        outX = median3(A.x, B.x, C.x);
        outY = median3(A.y, B.y, C.y);
    };

    int skipRun = 0;
    const uint32_t intraTypeOffset = pSlice ? 5u : 0u;   // I_NxN is 0 in an I slice, 5 in a P

    for (int mby = 0; mby < mbHeight_; ++mby) {
        for (int mbx = 0; mbx < mbWidth_; ++mbx) {
            const int px = mbx * 16, py = mby * 16;
            const int cx = mbx * 8, cy = mby * 8;
            const bool left = mbx > 0;
            const bool above = mby > 0;
            const size_t mbIndex = size_t(mby) * size_t(mbWidth_) + size_t(mbx);

            // ================= intra chroma, shared by both intra luma paths
            int bestChromaMode = kChromaDc, bestChromaCost = -1;
            uint8_t chromaPred[2][64];
            for (int mode = 0; mode < 4; ++mode) {
                if (mode == kChromaHorizontal && !left) continue;
                if (mode == kChromaVertical && !above) continue;
                if (mode == kChromaPlane && !(left && above)) continue;

                uint8_t pb[64], pr[64];
                predictChroma8x8(recCb, cx, cy, mode, left, above, pb);
                predictChroma8x8(recCr, cx, cy, mode, left, above, pr);
                const int cost = sad(pb, srcCb, cx, cy, 8) + sad(pr, srcCr, cx, cy, 8);
                if (bestChromaCost < 0 || cost < bestChromaCost) {
                    bestChromaCost = cost;
                    bestChromaMode = mode;
                    std::copy(pb, pb + 64, chromaPred[0]);
                    std::copy(pr, pr + 64, chromaPred[1]);
                }
            }

            ChromaCoding chromaIntra;
            codeChroma(mbx, mby, chromaPred, true, chromaIntra);
            const int cbpChroma = chromaIntra.cbp;
            const long long chromaIntraSsd = ssd(chromaIntra.recon[0], srcCb, cx, cy, 8) +
                                             ssd(chromaIntra.recon[1], srcCr, cx, cy, 8);

            // ================= snapshot, so no candidate can see another's work
            uint8_t savedRec[256];
            uint8_t savedCounts[16];
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) savedRec[y * 16 + x] = recY.at(px + x, py + y);
            }
            for (int i = 0; i < 16; ++i) {
                savedCounts[i] = coeffY_[size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                                         size_t(mbx * 4 + i % 4)];
            }
            auto restoreLuma = [&]() {
                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) recY.set(px + x, py + y, savedRec[y * 16 + x]);
                }
                for (int i = 0; i < 16; ++i) {
                    coeffY_[size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                            size_t(mbx * 4 + i % 4)] = savedCounts[i];
                }
            };

            // ================= path A: one intra prediction for the whole block
            BitWriter bitsA;
            uint8_t reconA[256];
            uint8_t countsA[16] = {0};
            {
                uint8_t pred[256], best[256];
                int bestMode = kPredDc, bestCost = -1;
                for (int mode = 0; mode < 4; ++mode) {
                    if (mode == kPredVertical && !above) continue;
                    if (mode == kPredHorizontal && !left) continue;
                    if (mode == kPredPlane && !(left && above)) continue;
                    predict16x16(recY, px, py, mode, left, above, pred);
                    const int cost = sad(pred, srcY, px, py, 16);
                    if (bestCost < 0 || cost < bestCost) {
                        bestCost = cost;
                        bestMode = mode;
                        std::copy(pred, pred + 256, best);
                    }
                }

                int32_t blocks[16][16];
                int32_t dc[16];
                for (int by = 0; by < 4; ++by) {
                    for (int bx = 0; bx < 4; ++bx) {
                        int32_t* block = blocks[by * 4 + bx];
                        for (int y = 0; y < 4; ++y) {
                            for (int x = 0; x < 4; ++x) {
                                block[y * 4 + x] =
                                    int32_t(srcY.at(px + bx * 4 + x, py + by * 4 + y)) -
                                    int32_t(best[(by * 4 + y) * 16 + bx * 4 + x]);
                            }
                        }
                        h264::forward4x4(block);
                        dc[by * 4 + bx] = block[0];
                    }
                }

                h264::hadamard4x4(dc);
                int32_t dcQuant[16];
                for (int i = 0; i < 16; ++i) dcQuant[i] = h264::quantizeDcLuma(dc[i], qp, true);

                int32_t acQuant[16][16];
                bool anyAc = false;
                for (int b = 0; b < 16; ++b) {
                    h264::quantize4x4(blocks[b], acQuant[b], qp, true);
                    acQuant[b][0] = 0;
                    for (int i = 1; i < 16; ++i) {
                        if (acQuant[b][i] != 0) anyAc = true;
                    }
                }
                const int cbpLuma = anyAc ? 15 : 0;

                const int mbType = 1 + bestMode + 4 * cbpChroma + 12 * (cbpLuma != 0 ? 1 : 0);
                bitsA.ue(uint32_t(mbType) + intraTypeOffset);
                bitsA.ue(uint32_t(bestChromaMode));
                bitsA.se(0);   // mb_qp_delta

                for (int i = 0; i < 16; ++i) {
                    coeffY_[size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                            size_t(mbx * 4 + i % 4)] = 0;
                }

                {
                    int32_t zig[16];
                    for (int k = 0; k < 16; ++k) zig[k] = dcQuant[h264::kZigZag4x4[k]];
                    h264::writeResidualBlock(bitsA, zig, h264::BlockKind::Luma16,
                                             lumaNc(mbx * 4, mby * 4));
                }

                if (cbpLuma != 0) {
                    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
                        int bx4, by4;
                        luma4x4Position(blkIdx, bx4, by4);
                        const int32_t* block = acQuant[by4 * 4 + bx4];

                        int32_t zig[15];
                        for (int k = 1; k < 16; ++k) zig[k - 1] = block[h264::kZigZag4x4[k]];

                        const int count = h264::writeResidualBlock(
                            bitsA, zig, h264::BlockKind::LumaAc15,
                            lumaNc(mbx * 4 + bx4, mby * 4 + by4));
                        countsA[by4 * 4 + bx4] = uint8_t(count);
                        coeffY_[size_t(mby * 4 + by4) * size_t(blocksWide) +
                                size_t(mbx * 4 + bx4)] = uint8_t(count);
                    }
                }
                bitsA.append(chromaIntra.bits);

                // Reconstruct into a buffer rather than into the picture: this
                // path may lose.
                int32_t dcRecon[16];
                for (int i = 0; i < 16; ++i) dcRecon[i] = dcQuant[i];
                h264::hadamard4x4(dcRecon);
                h264::dequantizeDcLuma(dcRecon, qp);

                for (int by = 0; by < 4; ++by) {
                    for (int bx = 0; bx < 4; ++bx) {
                        int32_t block[16];
                        h264::dequantize4x4(acQuant[by * 4 + bx], block, qp);
                        block[0] = dcRecon[by * 4 + bx];
                        h264::inverse4x4(block);
                        for (int y = 0; y < 4; ++y) {
                            for (int x = 0; x < 4; ++x) {
                                const int p = best[(by * 4 + y) * 16 + bx * 4 + x];
                                reconA[(by * 4 + y) * 16 + bx * 4 + x] =
                                    clip8(p + block[y * 4 + x]);
                            }
                        }
                    }
                }
            }
            const long long ssdA = ssd(reconA, srcY, px, py, 16) + chromaIntraSsd;

            restoreLuma();

            // ================= path C: predict from the previous picture
            //
            // One vector for the whole macroblock, in quarter samples,
            // against one reference. What is left of the format's inter
            // toolbox -- partitions smaller than a macroblock, several
            // references -- buys more, and each one costs another search.
            BitWriter bitsC;
            uint8_t reconC[256] = {};
            uint8_t countsC[16] = {0};
            ChromaCoding chromaInter;
            int mvcx = 0, mvcy = 0;
            long long ssdC = 0;

            // The skip candidate: no vector of its own, no residual at all,
            // and about one bit. On a held frame or a still background this
            // is the whole macroblock.
            uint8_t skipPredY[256] = {};
            uint8_t skipPredC[2][64] = {};
            int skipX = 0, skipY = 0;
            long long ssdSkip = 0;

            if (pSlice) {
                const MvNeighbour nA = neighbourAt(mbx - 1, mby, left);
                const MvNeighbour nB = neighbourAt(mbx, mby - 1, above);
                MvNeighbour nC = neighbourAt(mbx + 1, mby - 1, above && mbx + 1 < mbWidth_);
                if (!nC.available) nC = neighbourAt(mbx - 1, mby - 1, above && left);

                int mvpx = 0, mvpy = 0;
                predictorOf(nA, nB, nC, mvpx, mvpy);

                // ---- the skip vector, which is the predictor except where a
                // neighbour is missing or standing still. The exception is
                // what makes a still background cost nothing: it forces the
                // zero vector rather than inheriting a moving neighbour's.
                if (!nA.available || !nB.available || (nA.hasMotion && nA.x == 0 && nA.y == 0) ||
                    (nB.hasMotion && nB.x == 0 && nB.y == 0)) {
                    skipX = 0;
                    skipY = 0;
                } else {
                    skipX = mvpx;
                    skipY = mvpy;
                }

                // A chroma vector is the luma one unchanged: quarter of a luma
                // sample and eighth of a chroma sample are the same distance.
                h264::predictLuma(ref, px, py, skipX, skipY, 16, skipPredY);
                h264::predictChroma(ref.cb, chromaWidth, chromaHeight, cx, cy, skipX, skipY, 8,
                                    skipPredC[0]);
                h264::predictChroma(ref.cr, chromaWidth, chromaHeight, cx, cy, skipX, skipY, 8,
                                    skipPredC[1]);
                ssdSkip = ssd(skipPredY, srcY, px, py, 16) +
                          ssd(skipPredC[0], srcCb, cx, cy, 8) +
                          ssd(skipPredC[1], srcCr, cx, cy, 8);

                // ---- the search
                uint8_t scratch[256];
                int bestX = 0, bestY = 0, bestCost = -1;
                auto consider = [&](int mx, int my) {
                    // Quarter samples, so the range is four times the number
                    // of whole samples it stands for.
                    if (mx < -256 || mx > 256 || my < -256 || my > 256) return;
                    h264::predictLuma(ref, px, py, mx, my, 16, scratch);
                    const int cost =
                        h264::blockSad(scratch, frame.y.data(), lumaWidth, px, py, 16) +
                        lambdaMv * (seBits(mx - mvpx) + seBits(my - mvpy));
                    if (bestCost < 0 || cost < bestCost) {
                        bestCost = cost;
                        bestX = mx;
                        bestY = my;
                    }
                };

                consider(0, 0);
                consider(mvpx, mvpy);
                if (nA.hasMotion) consider(nA.x, nA.y);
                if (nB.hasMotion) consider(nB.x, nB.y);
                if (nC.hasMotion) consider(nC.x, nC.y);

                // A halving diamond from the best candidate. A full search
                // would be twenty times the work for a fraction of a decibel:
                // the vectors that matter are near a neighbour's or near zero,
                // and the candidates above have already been there.
                //
                // In quarter samples the same halving carries the search all
                // the way down and needs no separate sub-sample pass: steps 32
                // to 4 are whole samples, 2 is the half, 1 the quarter. The
                // diamond does not care that its last two steps cost an
                // interpolation where the ones above cost a copy.
                //
                // Two more elaborate searches were tried here and neither paid
                // for itself: eight-point refinement at the half and the
                // quarter instead of the diamond, and a Hadamard-transformed
                // cost in place of the plain sum of differences -- the usual
                // remedy for a fractional position winning on the noise its
                // interpolation removed rather than on the residual it saved.
                // On sway_draft the three agree to within 0.13% of size and
                // 0.01 dB, which is a move along the rate-distortion curve
                // rather than off it. The measurements are in api-video.md.
                for (int step = 32; step >= 1; step >>= 1) {
                    for (;;) {
                        const int fromX = bestX, fromY = bestY;
                        consider(fromX - step, fromY);
                        consider(fromX + step, fromY);
                        consider(fromX, fromY - step);
                        consider(fromX, fromY + step);
                        if (bestX == fromX && bestY == fromY) break;
                    }
                }

                mvcx = bestX;
                mvcy = bestY;

                uint8_t pred[256];
                uint8_t predC[2][64];
                h264::predictLuma(ref, px, py, mvcx, mvcy, 16, pred);
                h264::predictChroma(ref.cb, chromaWidth, chromaHeight, cx, cy, mvcx, mvcy, 8,
                                    predC[0]);
                h264::predictChroma(ref.cr, chromaWidth, chromaHeight, cx, cy, mvcx, mvcy, 8,
                                    predC[1]);

                codeChroma(mbx, mby, predC, false, chromaInter);

                // Residual: sixteen ordinary 4x4 blocks. No Hadamard over the
                // DC coefficients -- that belongs to I_16x16, where one flat
                // prediction covers the macroblock and the DC terms are
                // therefore correlated. Here each block predicts from its own
                // piece of the reference and they are not.
                int32_t quant[16][16];
                int cbpLuma = 0;
                for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
                    int x4, y4;
                    luma4x4Position(blkIdx, x4, y4);

                    int32_t block[16];
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            block[y * 4 + x] =
                                int32_t(srcY.at(px + x4 * 4 + x, py + y4 * 4 + y)) -
                                int32_t(pred[(y4 * 4 + y) * 16 + x4 * 4 + x]);
                        }
                    }
                    h264::forward4x4(block);
                    h264::quantize4x4(block, quant[blkIdx], qp, false);

                    bool any = false;
                    for (int i = 0; i < 16; ++i) {
                        if (quant[blkIdx][i] != 0) any = true;
                    }
                    if (any) cbpLuma |= 1 << ((y4 / 2) * 2 + (x4 / 2));

                    int32_t rebuilt[16];
                    h264::dequantize4x4(quant[blkIdx], rebuilt, qp);
                    h264::inverse4x4(rebuilt);
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            const int p = pred[(y4 * 4 + y) * 16 + x4 * 4 + x];
                            reconC[(y4 * 4 + y) * 16 + x4 * 4 + x] =
                                clip8(p + rebuilt[y * 4 + x]);
                        }
                    }
                }

                bitsC.ue(0);   // mb_type 0 in a P slice: P_L0_16x16
                // ref_idx_l0 is not in the bitstream: there is one reference
                // and num_ref_idx_l0_active_minus1 is zero.
                bitsC.se(mvcx - mvpx);   // mvd_l0, in quarter samples
                bitsC.se(mvcy - mvpy);

                const int cbp = cbpLuma | (chromaInter.cbp << 4);
                const int codeNum = h264::codedBlockPatternCodeNum(cbp, false);
                bitsC.ue(uint32_t(codeNum < 0 ? 0 : codeNum));

                for (int i = 0; i < 16; ++i) {
                    coeffY_[size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                            size_t(mbx * 4 + i % 4)] = 0;
                }

                if (cbp != 0) {
                    bitsC.se(0);   // mb_qp_delta
                    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
                        int x4, y4;
                        luma4x4Position(blkIdx, x4, y4);
                        if ((cbpLuma & (1 << ((y4 / 2) * 2 + (x4 / 2)))) == 0) continue;

                        int32_t zig[16];
                        for (int k = 0; k < 16; ++k) zig[k] = quant[blkIdx][h264::kZigZag4x4[k]];

                        const int count = h264::writeResidualBlock(
                            bitsC, zig, h264::BlockKind::Luma16,
                            lumaNc(mbx * 4 + x4, mby * 4 + y4));
                        countsC[y4 * 4 + x4] = uint8_t(count);
                        coeffY_[size_t(mby * 4 + y4) * size_t(blocksWide) +
                                size_t(mbx * 4 + x4)] = uint8_t(count);
                    }
                }
                bitsC.append(chromaInter.bits);

                ssdC = ssd(reconC, srcY, px, py, 16) +
                       ssd(chromaInter.recon[0], srcCb, cx, cy, 8) +
                       ssd(chromaInter.recon[1], srcCr, cx, cy, 8);

                restoreLuma();
            }

            // ================= path B: an intra prediction per 4x4 block
            //
            // Blocks are decided, coded and reconstructed in the Z scan order
            // one at a time, because each one predicts from the one before it.
            // That is also why the residual cannot be written in this loop:
            // coded_block_pattern has to precede it in the bitstream and is
            // not known until every block has been quantised.
            BitWriter bitsB;
            uint8_t countsB[16] = {0};
            int modesB[16] = {0};
            {
                int32_t quant[16][16];
                int cbpLuma = 0;

                for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
                    int x4, y4;
                    luma4x4Position(blkIdx, x4, y4);
                    const int bx = px + x4 * 4, by = py + y4 * 4;

                    h264::Neighbours4x4 n;
                    n.haveAbove = y4 > 0 || above;
                    n.haveLeft = x4 > 0 || left;
                    n.haveAboveRight = aboveRightAvailable(blkIdx, mbx, mby, mbWidth_);

                    if (n.haveAbove) {
                        for (int i = 0; i < 4; ++i) n.above[i] = recY.at(bx + i, by - 1);
                    }
                    if (n.haveAboveRight) {
                        for (int i = 4; i < 8; ++i) n.above[i] = recY.at(bx + i, by - 1);
                    }
                    n.fillAboveRight();
                    if (n.haveLeft) {
                        for (int i = 0; i < 4; ++i) n.left[i] = recY.at(bx - 1, by + i);
                    }
                    if (n.haveCorner()) n.corner = recY.at(bx - 1, by - 1);

                    // The most probable mode: the milder of what the two
                    // neighbours chose, with an absent or non-I_NxN neighbour
                    // counting as DC.
                    const int gx = mbx * 4 + x4, gy = mby * 4 + y4;
                    const int modeA = (x4 > 0 || left)
                                          ? int(modeY_[size_t(gy) * size_t(blocksWide) +
                                                       size_t(gx - 1)])
                                          : h264::kIntra4Dc;
                    const int modeB = (y4 > 0 || above)
                                          ? int(modeY_[size_t(gy - 1) * size_t(blocksWide) +
                                                       size_t(gx)])
                                          : h264::kIntra4Dc;
                    const int predictedMode = std::min(modeA, modeB);

                    uint8_t pred[16], bestPred[16];
                    int bestMode = h264::kIntra4Dc, bestCost = -1;
                    for (int mode = 0; mode < h264::kIntra4ModeCount; ++mode) {
                        if (!h264::predict4x4(mode, n, pred)) continue;
                        int cost = 0;
                        for (int y = 0; y < 4; ++y) {
                            for (int x = 0; x < 4; ++x) {
                                cost += std::abs(int(pred[y * 4 + x]) -
                                                 int(srcY.at(bx + x, by + y)));
                            }
                        }
                        if (mode != predictedMode) cost += lambda;
                        if (bestCost < 0 || cost < bestCost) {
                            bestCost = cost;
                            bestMode = mode;
                            std::copy(pred, pred + 16, bestPred);
                        }
                    }

                    modesB[blkIdx] = bestMode;
                    modeY_[size_t(gy) * size_t(blocksWide) + size_t(gx)] = uint8_t(bestMode);

                    // Signal the mode now: these bits precede the pattern.
                    if (bestMode == predictedMode) {
                        bitsB.flag(true);
                    } else {
                        bitsB.flag(false);
                        bitsB.u(3, uint32_t(bestMode < predictedMode ? bestMode : bestMode - 1));
                    }

                    int32_t block[16];
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            block[y * 4 + x] = int32_t(srcY.at(bx + x, by + y)) -
                                               int32_t(bestPred[y * 4 + x]);
                        }
                    }
                    h264::forward4x4(block);
                    h264::quantize4x4(block, quant[blkIdx], qp, true);

                    bool any = false;
                    for (int i = 0; i < 16; ++i) {
                        if (quant[blkIdx][i] != 0) any = true;
                    }
                    if (any) cbpLuma |= 1 << ((y4 / 2) * 2 + (x4 / 2));

                    // Reconstruct immediately: the next block predicts from it.
                    int32_t rebuilt[16];
                    h264::dequantize4x4(quant[blkIdx], rebuilt, qp);
                    h264::inverse4x4(rebuilt);
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            recY.set(bx + x, by + y,
                                     clip8(int(bestPred[y * 4 + x]) + rebuilt[y * 4 + x]));
                        }
                    }
                }

                // Now the pattern is known, and the residual can follow it.
                BitWriter head;
                head.ue(intraTypeOffset);   // I_NxN
                head.append(bitsB);
                head.ue(uint32_t(bestChromaMode));

                const int cbp = cbpLuma | (cbpChroma << 4);
                const int codeNum = h264::codedBlockPatternCodeNum(cbp, true);
                head.ue(uint32_t(codeNum < 0 ? 0 : codeNum));

                for (int i = 0; i < 16; ++i) {
                    coeffY_[size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                            size_t(mbx * 4 + i % 4)] = 0;
                }

                if (cbp != 0) {
                    head.se(0);   // mb_qp_delta
                    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
                        int x4, y4;
                        luma4x4Position(blkIdx, x4, y4);
                        if ((cbpLuma & (1 << ((y4 / 2) * 2 + (x4 / 2)))) == 0) continue;

                        int32_t zig[16];
                        for (int k = 0; k < 16; ++k) zig[k] = quant[blkIdx][h264::kZigZag4x4[k]];

                        const int count = h264::writeResidualBlock(
                            head, zig, h264::BlockKind::Luma16,
                            lumaNc(mbx * 4 + x4, mby * 4 + y4));
                        countsB[y4 * 4 + x4] = uint8_t(count);
                        coeffY_[size_t(mby * 4 + y4) * size_t(blocksWide) +
                                size_t(mbx * 4 + x4)] = uint8_t(count);
                    }
                }
                head.append(chromaIntra.bits);
                bitsB = head;
            }
            uint8_t reconB[256];
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) reconB[y * 16 + x] = recY.at(px + x, py + y);
            }
            const long long ssdB = ssd(reconB, srcY, px, py, 16) + chromaIntraSsd;

            // ================= choose
            //
            // I_PCM is the floor: past this many bits, remembering the samples
            // is cheaper than coding them, and an encoder that can do worse
            // than that is an encoder with a hole in it.
            const size_t pcmBits = 9 + 7 + 384 * 8;
            const size_t bitsAcount = bitsA.bitCount();
            const size_t bitsBcount = bitsB.bitCount();

            enum { kPathA, kPathB, kPathC, kPathSkip, kPathPcm };
            int winner;

            if (!pSlice) {
                // Two intra codings of the same macroblock at the same
                // quantiser land at about the same quality, so bits decide.
                winner = bitsAcount <= bitsBcount ? kPathA : kPathB;
                if (std::min(bitsAcount, bitsBcount) > pcmBits) winner = kPathPcm;
            } else {
                // A skip spends no bits on residual and takes whatever error
                // that leaves, so quality has to be in the comparison. The
                // skip's own cost is the one bit it adds to the run around it.
                long long bestJ = ssdA + lambdaRd * (long long)bitsAcount;
                winner = kPathA;

                const long long jB = ssdB + lambdaRd * (long long)bitsBcount;
                if (jB < bestJ) { bestJ = jB; winner = kPathB; }

                const long long jC = ssdC + lambdaRd * (long long)bitsC.bitCount();
                if (jC < bestJ) { bestJ = jC; winner = kPathC; }

                const long long jSkip = ssdSkip + lambdaRd;
                if (jSkip < bestJ) { bestJ = jSkip; winner = kPathSkip; }

                const long long jPcm = lambdaRd * (long long)pcmBits;
                if (jPcm < bestJ) { bestJ = jPcm; winner = kPathPcm; }
            }

            // A skipped macroblock writes nothing here: it lengthens the run
            // that the next coded macroblock carries.
            if (winner == kPathSkip) {
                restoreLuma();
                ++skipRun;

                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) {
                        recY.set(px + x, py + y, skipPredY[y * 16 + x]);
                    }
                }
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        recCb.set(cx + x, cy + y, skipPredC[0][y * 8 + x]);
                        recCr.set(cx + x, cy + y, skipPredC[1][y * 8 + x]);
                    }
                }
                for (int i = 0; i < 16; ++i) {
                    const size_t at = size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                                      size_t(mbx * 4 + i % 4);
                    coeffY_[at] = 0;
                    modeY_[at] = uint8_t(h264::kIntra4Dc);
                }
                for (int b = 0; b < 4; ++b) {
                    const size_t i = size_t(mby * 2 + b / 2) * size_t(mbWidth_ * 2) +
                                     size_t(mbx * 2 + b % 2);
                    coeffCb_[i] = 0;
                    coeffCr_[i] = 0;
                }
                mbIntra_[mbIndex] = 0;
                mvX_[mbIndex] = int16_t(skipX);
                mvY_[mbIndex] = int16_t(skipY);
                continue;
            }

            if (pSlice) {
                w.ue(uint32_t(skipRun));
                skipRun = 0;
            }

            if (winner == kPathPcm) {
                restoreLuma();
                w.ue(25u + intraTypeOffset);
                w.alignTo(false);

                uint8_t raw[256];
                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) raw[y * 16 + x] = srcY.at(px + x, py + y);
                }
                w.bytes(raw, 256);
                for (int plane = 0; plane < 2; ++plane) {
                    const Plane& src = plane == 0 ? srcCb : srcCr;
                    uint8_t c[64];
                    for (int y = 0; y < 8; ++y) {
                        for (int x = 0; x < 8; ++x) c[y * 8 + x] = src.at(cx + x, cy + y);
                    }
                    w.bytes(c, 64);
                }

                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) recY.set(px + x, py + y, srcY.at(px + x, py + y));
                }
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        recCb.set(cx + x, cy + y, srcCb.at(cx + x, cy + y));
                        recCr.set(cx + x, cy + y, srcCr.at(cx + x, cy + y));
                    }
                }
                for (int i = 0; i < 16; ++i) {
                    const size_t at = size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                                      size_t(mbx * 4 + i % 4);
                    coeffY_[at] = 16;
                    modeY_[at] = uint8_t(h264::kIntra4Dc);
                }
                for (int b = 0; b < 4; ++b) {
                    const size_t i = size_t(mby * 2 + b / 2) * size_t(mbWidth_ * 2) +
                                     size_t(mbx * 2 + b % 2);
                    coeffCb_[i] = 16;
                    coeffCr_[i] = 16;
                }
                pcmFlags_[mbIndex] = 1;
                continue;
            }

            const ChromaCoding& chroma = winner == kPathC ? chromaInter : chromaIntra;

            if (winner == kPathA) {
                // The 4x4 path already wrote itself into the picture and the
                // counts; put back what it overwrote and lay path A on top.
                restoreLuma();
                w.append(bitsA);
                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) recY.set(px + x, py + y, reconA[y * 16 + x]);
                }
                for (int i = 0; i < 16; ++i) {
                    const size_t at = size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                                      size_t(mbx * 4 + i % 4);
                    coeffY_[at] = countsA[i];
                    // A macroblock that was not I_NxN offers DC to whoever
                    // asks it what mode it used.
                    modeY_[at] = uint8_t(h264::kIntra4Dc);
                }
            } else if (winner == kPathC) {
                restoreLuma();
                w.append(bitsC);
                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) recY.set(px + x, py + y, reconC[y * 16 + x]);
                }
                for (int i = 0; i < 16; ++i) {
                    const size_t at = size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                                      size_t(mbx * 4 + i % 4);
                    coeffY_[at] = countsC[i];
                    modeY_[at] = uint8_t(h264::kIntra4Dc);
                }
                mbIntra_[mbIndex] = 0;
                mvX_[mbIndex] = int16_t(mvcx);
                mvY_[mbIndex] = int16_t(mvcy);
            } else {
                w.append(bitsB);
                for (int i = 0; i < 16; ++i) {
                    coeffY_[size_t(mby * 4 + i / 4) * size_t(blocksWide) +
                            size_t(mbx * 4 + i % 4)] = countsB[i];
                }
                (void)modesB;
            }

            // Chroma: whichever prediction won, its reconstruction and its
            // coefficient counts go into the picture together.
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 8; ++x) {
                    recCb.set(cx + x, cy + y, chroma.recon[0][y * 8 + x]);
                    recCr.set(cx + x, cy + y, chroma.recon[1][y * 8 + x]);
                }
            }
            for (int b = 0; b < 4; ++b) {
                const size_t i = size_t(mby * 2 + b / 2) * size_t(mbWidth_ * 2) +
                                 size_t(mbx * 2 + b % 2);
                coeffCb_[i] = chroma.counts[0][b];
                coeffCr_[i] = chroma.counts[1][b];
            }
        }
    }

    // A run of skipped macroblocks at the end of the slice still has to be
    // written, or the decoder stops one macroblock short of the picture.
    if (pSlice && skipRun > 0) w.ue(uint32_t(skipRun));

    // How much of this picture came from between the samples. Nothing depends
    // on it -- it is here to be looked at, because a filter that never runs
    // agrees with every decoder there is.
    fractionalVectors_ = 0;
    interMacroblocks_ = 0;
    for (size_t i = 0; i < mbCount; ++i) {
        if (mbIntra_[i]) continue;
        ++interMacroblocks_;
        if ((mvX_[i] & 3) != 0 || (mvY_[i] & 3) != 0) ++fractionalVectors_;
    }

    // The output picture: the reconstruction with the filter run over it.
    // Prediction above used the unfiltered samples, which is what the format
    // says to do; this copy is what a viewer sees -- and what the next frame
    // predicts from.
    filtered_.width = lumaWidth;
    filtered_.height = lumaHeight;
    filtered_.y = reconY_;
    filtered_.cb = reconCb_;
    filtered_.cr = reconCr_;
    {
        h264::DeblockPicture dp;
        dp.mbWidth = mbWidth_;
        dp.mbHeight = mbHeight_;
        dp.qp = qp;
        dp.qpChroma = qpc;
        dp.pcm = pcmFlags_;
        dp.intra = mbIntra_;
        dp.mvx = mvX_;
        dp.mvy = mvY_;
        dp.nonZero.assign(coeffY_.size(), 0);
        for (size_t i = 0; i < coeffY_.size(); ++i) dp.nonZero[i] = coeffY_[i] != 0 ? 1 : 0;
        h264::deblock(filtered_.y.data(), filtered_.cb.data(), filtered_.cr.data(), lumaWidth,
                      lumaHeight, dp);
    }

    refY_ = filtered_.y;
    refCb_ = filtered_.cb;
    refCr_ = filtered_.cr;
    haveReference_ = true;

    w.trailingBits();

    // An IDR is nal_unit_type 5 and the highest reference priority; a P
    // picture is type 1, and still a reference, because the frame after it
    // will predict from it.
    appendLengthPrefixed(out, makeNal(idr ? 3 : 2, idr ? 5 : 1, w.data()));
    frameNum_ = (frameNum_ + 1) & 15;
    ++frameIndex_;
    return out;
}

std::vector<uint8_t> H264Encoder::avcc() const {
    std::vector<uint8_t> out;
    if (!started_) return out;

    const std::vector<uint8_t> spsNal = makeNal(3, 7, sps_);
    const std::vector<uint8_t> ppsNal = makeNal(3, 8, pps_);
    if (spsNal.size() < 4) return out;

    out.push_back(1);
    out.push_back(spsNal[1]);
    out.push_back(spsNal[2]);
    out.push_back(spsNal[3]);
    out.push_back(0xFF);
    out.push_back(0xE1);

    out.push_back(uint8_t(spsNal.size() >> 8));
    out.push_back(uint8_t(spsNal.size()));
    out.insert(out.end(), spsNal.begin(), spsNal.end());

    out.push_back(1);
    out.push_back(uint8_t(ppsNal.size() >> 8));
    out.push_back(uint8_t(ppsNal.size()));
    out.insert(out.end(), ppsNal.begin(), ppsNal.end());

    return out;
}

} // namespace blocky
