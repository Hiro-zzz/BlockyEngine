// Smoke test for the platform layer: open a Win32 window, put a real
// OpenGL 4.6 core context on it, clear it to a known colour, read the pixels
// back and check they are what we asked for.
//
// A GUI program cannot be eyeballed from a build script, so the verification
// path is glReadPixels rather than a screenshot -- and that same path is what
// gives the viewport its snapshot mode.
#include "engine/core/png.hpp"
#include "engine/platform/window.hpp"
#include "engine/render/gl/gl_loader.hpp"

#include <cstdio>
#include <vector>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

} // namespace

int main() {
    constexpr int kWidth = 320, kHeight = 200;

    Window window;
    std::string error;
    if (!window.create("BlockyEngine GL smoke test", kWidth, kHeight, &error)) {
        std::printf("  FAIL  %s\n", error.c_str());
        return 1;
    }
    check(window.width() == kWidth && window.height() == kHeight, "client area is the requested size");

    gl::ContextSettings settings;
    settings.samples = 0;   // read-back is simpler without a multisampled buffer
    settings.debug = true;

    void* context = nullptr;
    if (!gl::createContext(window.deviceContext(), settings, &context, &error)) {
        std::printf("  FAIL  %s\n", error.c_str());
        return 1;
    }

    std::printf("vendor   : %s\n", gl::vendorString().c_str());
    std::printf("renderer : %s\n", gl::rendererString().c_str());
    std::printf("version  : %s\n", gl::versionString().c_str());

    check(!gl::versionString().empty() && gl::versionString() != "?", "driver reports a version");
    check(gl::Clear != nullptr && gl::CreateShader != nullptr && gl::TexImage3D != nullptr,
          "the function table is populated across all GL generations");

    // Clear to a colour that is unmistakable and not a component-swap of
    // itself, so a channel-order bug cannot pass.
    const float kR = 0.20f, kG = 0.55f, kB = 0.85f;
    gl::Viewport(0, 0, kWidth, kHeight);
    gl::ClearColor(kR, kG, kB, 1.0f);
    gl::Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
    gl::Finish();

    check(gl::GetError() == gl::NO_ERROR_, "no GL error after clearing");

    std::vector<uint8_t> pixels(size_t(kWidth) * size_t(kHeight) * 4);
    gl::PixelStorei(gl::PACK_ALIGNMENT, 1);
    gl::ReadPixels(0, 0, kWidth, kHeight, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());
    check(gl::GetError() == gl::NO_ERROR_, "no GL error after read-back");

    auto expected = [](float v) { return uint8_t(v * 255.0f + 0.5f); };
    int wrong = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (std::abs(int(pixels[i + 0]) - int(expected(kR))) > 2) { ++wrong; continue; }
        if (std::abs(int(pixels[i + 1]) - int(expected(kG))) > 2) { ++wrong; continue; }
        if (std::abs(int(pixels[i + 2]) - int(expected(kB))) > 2) { ++wrong; continue; }
    }
    check(wrong == 0, "every pixel came back as the colour we cleared to");
    if (wrong) {
        std::printf("        %d of %d pixels differ; first pixel is (%u %u %u)\n",
                    wrong, kWidth * kHeight, pixels[0], pixels[1], pixels[2]);
    }

    // Save it too, which exercises the same path the viewport's snapshot uses:
    // GL reads bottom-up, images are stored top-down.
    ImageU8 image(kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y) {
        const uint8_t* row = pixels.data() + size_t(kHeight - 1 - y) * size_t(kWidth) * 4;
        for (int x = 0; x < kWidth; ++x) {
            image.set(x, y, {row[x * 4], row[x * 4 + 1], row[x * 4 + 2], 255});
        }
    }
    if (!pngSave("out/test_gl.png", image, &error)) {
        std::printf("  FAIL  save: %s\n", error.c_str());
        ++gFailures;
    }

    gl::destroyContext(window.deviceContext(), context);
    window.destroy();

    if (gFailures == 0) {
        std::printf("\nall gl tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
