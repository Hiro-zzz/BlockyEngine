// Exercises the ZIP reader and the PNG decoder together, against a real
// Minecraft client jar: 22k entries, deflate-compressed, CRC-checked.
#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/core/zip.hpp"

#include <cstdio>
#include <cstdlib>

using namespace blocky;

namespace {

int gFailures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++gFailures;
    }
}

std::string defaultJarPath() {
    const char* appData = std::getenv("APPDATA");
    if (!appData) return {};
    return std::string(appData) + "/.tlauncher/legacy/Minecraft/game/versions/1.20.4/1.20.4.jar";
}

} // namespace

int main(int argc, char** argv) {
    std::string jarPath = argc > 1 ? argv[1] : defaultJarPath();

    if (jarPath.empty() || !fileExists(jarPath)) {
        std::printf("no client jar at %s\n", jarPath.c_str());
        std::printf("pass one as an argument to run this test\n");
        return 0;  // not a failure: the engine must not require game files
    }

    std::printf("opening %s\n", jarPath.c_str());

    ZipArchive archive;
    std::string error;
    if (!archive.openFile(jarPath, &error)) {
        std::printf("  FAIL  open: %s\n", error.c_str());
        return 1;
    }
    std::printf("  %zu entries\n", archive.entryCount());
    check(archive.entryCount() > 1000, "archive has a plausible number of entries");

    // Count what matters to us.
    size_t blockTextures = 0, entityTextures = 0;
    for (const std::string& name : archive.names()) {
        if (name.rfind("assets/minecraft/textures/block/", 0) == 0) ++blockTextures;
        if (name.rfind("assets/minecraft/textures/entity/", 0) == 0) ++entityTextures;
    }
    std::printf("  %zu block textures, %zu entity textures\n", blockTextures, entityTextures);
    check(blockTextures > 500, "block textures found");
    check(entityTextures > 100, "entity textures found");

    // Decode a representative set. Every one goes through our inflate, our
    // CRC check and our PNG decoder.
    const char* samples[] = {
        "assets/minecraft/textures/block/stone.png",
        "assets/minecraft/textures/block/dirt.png",
        "assets/minecraft/textures/block/grass_block_top.png",
        "assets/minecraft/textures/block/grass_block_side.png",
        "assets/minecraft/textures/block/oak_log.png",
        "assets/minecraft/textures/block/oak_leaves.png",
        "assets/minecraft/textures/block/glowstone.png",
        "assets/minecraft/textures/block/sand.png",
        "assets/minecraft/textures/entity/player/wide/steve.png",
        "assets/minecraft/textures/entity/player/slim/alex.png",
    };

    for (const char* name : samples) {
        if (!archive.contains(name)) {
            std::printf("  FAIL  missing entry %s\n", name);
            ++gFailures;
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!archive.read(name, bytes, &error)) {
            std::printf("  FAIL  read %s: %s\n", name, error.c_str());
            ++gFailures;
            continue;
        }
        ImageU8 image;
        if (!pngDecode(bytes.data(), bytes.size(), image, &error)) {
            std::printf("  FAIL  decode %s: %s\n", name, error.c_str());
            ++gFailures;
            continue;
        }
        std::printf("    %-58s %3d x %-3d\n",
                    name + 24, image.width(), image.height());
    }

    // Decode every block texture, to be sure nothing in the set trips up the
    // decoder: animated textures are tall strips, some are 16-bit, some are
    // paletted.
    size_t decoded = 0, failed = 0;
    int minWidth = 1 << 20, maxHeight = 0;
    for (const std::string& name : archive.names()) {
        if (name.rfind("assets/minecraft/textures/block/", 0) != 0) continue;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".png") != 0) continue;

        std::vector<uint8_t> bytes;
        ImageU8 image;
        if (archive.read(name, bytes, nullptr) &&
            pngDecode(bytes.data(), bytes.size(), image, &error)) {
            ++decoded;
            minWidth = std::min(minWidth, image.width());
            maxHeight = std::max(maxHeight, image.height());
        } else {
            if (failed < 5) std::printf("  FAIL  %s: %s\n", name.c_str(), error.c_str());
            ++failed;
        }
    }
    std::printf("  decoded %zu/%zu block textures (smallest width %d, tallest %d)\n",
                decoded, decoded + failed, minWidth, maxHeight);
    check(failed == 0, "every block texture decodes");

    if (gFailures == 0) {
        std::printf("\nall zip/png tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
