// Smoke test for the bootstrap layer: build a gradient, write it through our
// own PNG encoder, read it back with our own decoder, and verify every pixel
// survived the round trip.
#include "engine/core/image.hpp"
#include "engine/core/png.hpp"

#include <cstdio>

using namespace blocky;

int main() {
    constexpr int kWidth = 512, kHeight = 512;

    Image hdr(kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            float u = float(x) / float(kWidth - 1);
            float v = float(y) / float(kHeight - 1);
            // A gradient plus a hard-edged checker, so filtering bugs and
            // compression bugs both show up as visible artefacts.
            bool checker = ((x / 32) + (y / 32)) % 2 == 0;
            Vec3 c{u, v, 0.35f + 0.4f * float(checker)};
            hdr.at(x, y) = c * (checker ? 1.0f : 0.55f);
        }
    }

    ToneParams tone;
    tone.curve = Tonemap::None;  // exact values, so the comparison is meaningful
    ImageU8 written = tonemapToU8(hdr, tone);

    std::string error;
    if (!pngSave("out/hello.png", written, &error)) {
        std::printf("save failed: %s\n", error.c_str());
        return 1;
    }

    ImageU8 readBack;
    if (!pngLoad("out/hello.png", readBack, &error)) {
        std::printf("load failed: %s\n", error.c_str());
        return 1;
    }

    if (readBack.width() != written.width() || readBack.height() != written.height()) {
        std::printf("round trip changed dimensions: %dx%d -> %dx%d\n",
                    written.width(), written.height(), readBack.width(), readBack.height());
        return 1;
    }

    long long mismatches = 0;
    for (int y = 0; y < written.height(); ++y) {
        for (int x = 0; x < written.width(); ++x) {
            ImageU8::RGBA a = written.get(x, y), b = readBack.get(x, y);
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) ++mismatches;
        }
    }

    if (mismatches != 0) {
        std::printf("FAIL: %lld pixels differ after round trip\n", mismatches);
        return 1;
    }

    std::vector<uint8_t> encoded = pngEncode(written);
    double rawSize = double(kWidth) * kHeight * 4.0;
    std::printf("OK  %dx%d round trip is lossless\n", kWidth, kHeight);
    std::printf("    out/hello.png  %.1f KiB  (%.1f%% of raw RGBA)\n",
                double(encoded.size()) / 1024.0, 100.0 * double(encoded.size()) / rawSize);
    return 0;
}
