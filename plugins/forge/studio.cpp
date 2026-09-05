#include "plugins/forge/studio.hpp"

#include "engine/core/file.hpp"
#include "engine/core/png.hpp"
#include "engine/edit/emit.hpp"

#include "plugins/forge/palette.hpp"

#include <algorithm>
#include <utility>

namespace forge {

using namespace blocky;

void Studio::begin() {
    sculpt.create(16);
    canvas.create(edit::CanvasPreset::BlockTexture);
    say("new 16-cube model. Tab switches to the canvas.", 6.0f);
}

void Studio::tick(float dt) {
    if (statusSeconds <= 0.0f) return;
    statusSeconds -= dt;
    if (statusSeconds <= 0.0f) status.clear();
}

void Studio::say(std::string line, float seconds) {
    status = std::move(line);
    statusSeconds = seconds;
}

void Studio::toggleMode() {
    switch (mode) {
        case Mode::Sculpt:  mode = Mode::Canvas;  say("canvas", 1.5f);  break;
        case Mode::Canvas:  mode = Mode::Sprites; say("sprites", 1.5f); break;
        case Mode::Sprites: mode = Mode::Sculpt;  say("model", 1.5f);   break;
    }
}

void Studio::undo() {
    // Three documents, three stacks, and the name of the step comes from
    // whichever one just moved. The sprite list keeps snapshots rather than
    // cell differences -- see `edit/sprite_doc.hpp` for why that is the same
    // question answered twice, not an inconsistency.
    if (mode == Mode::Sprites) {
        const std::string step = sprites.undoName();
        say(sprites.undo() ? "undo: " + step : "nothing to undo", 1.5f);
        return;
    }

    const bool stepped = mode == Mode::Sculpt ? sculpt.undo() : canvas.undo();
    const edit::History& history = mode == Mode::Sculpt ? sculpt.history() : canvas.history();
    say(stepped ? "undo: " + history.redoName() : "nothing to undo", 1.5f);
}

void Studio::redo() {
    if (mode == Mode::Sprites) {
        const std::string step = sprites.redoName();
        say(sprites.redo() ? "redo: " + step : "nothing to redo", 1.5f);
        return;
    }

    const bool stepped = mode == Mode::Sculpt ? sculpt.redo() : canvas.redo();
    const edit::History& history = mode == Mode::Sculpt ? sculpt.history() : canvas.history();
    say(stepped ? "redo: " + history.undoName() : "nothing to redo", 1.5f);
}

uint16_t Studio::voxelMaterial() { return sculpt.addMaterial(swatchMaterial(swatch)); }

ImageU8::RGBA Studio::pixelColour() const { return swatchPixel(swatch); }

void Studio::exportDocument() {
    std::string error;
    if (!createDirectories(kOutputDirectory)) {
        say("could not make " + std::string(kOutputDirectory), 8.0f);
        return;
    }

    if (mode == Mode::Sprites) {
        // One file, and only one: a list of quads has no picture form. The
        // sheet and the PNG exist because a grid *is* an image; this is not.
        std::string source;
        if (!edit::emitSpriteSource(sprites.sprites(), source, {edit::identifierFor(name, "place")},
                                    &error)) {
            say("export refused: " + error, 8.0f);
            return;
        }

        const std::string path = std::string(kOutputDirectory) + "/" + name + "_sprites.hpp";
        if (!writeFileBytes(path, reinterpret_cast<const uint8_t*>(source.data()), source.size(),
                            &error)) {
            say("export failed: " + error, 8.0f);
            return;
        }
        say("wrote " + path, 8.0f);
        return;
    }

    if (mode == Mode::Canvas) {
        // Both halves go out twice, by the same rule: the source is what the
        // repository keeps, the picture is what you hand to something else.
        const std::string path = std::string(kOutputDirectory) + "/" + name + ".png";
        if (!canvas.save(path, &error)) {
            say("export failed: " + error, 8.0f);
            return;
        }

        std::string source;
        if (!edit::emitCanvasSource(canvas.image(), source, {edit::identifierFor(name)}, &error)) {
            // Too many colours is the usual reason, and it is not a failure of
            // the export: the PNG is written and is the right answer for a
            // picture that dense.
            say("wrote " + path + "; source refused: " + error, 10.0f);
            return;
        }

        const std::string sourcePath = std::string(kOutputDirectory) + "/" + name + ".hpp";
        if (!writeFileBytes(sourcePath, reinterpret_cast<const uint8_t*>(source.data()),
                            source.size(), &error)) {
            say("wrote " + path + "; " + sourcePath + " failed: " + error, 10.0f);
            return;
        }

        say("wrote " + sourcePath + " and " + path, 8.0f);
        return;
    }

    // A model goes out twice, and the two are not alternatives. The source is
    // exact and belongs in a repository; the sheet is a picture and opens in
    // anything. Writing both means never having to have decided in advance.
    const std::string stem = std::string(kOutputDirectory) + "/" + name;

    std::string source;
    const std::string sheetPath =
        std::string(kOutputDirectory) + "/" + edit::sheetFileName(name, sculpt.dims());

    if (!pngSave(sheetPath, edit::emitLayerSheet(sculpt.model()), &error)) {
        say("sheet failed: " + error, 8.0f);
        return;
    }

    if (!edit::emitSource(sculpt.model(), source, {edit::identifierFor(name)}, &error)) {
        // The sheet is already written, and saying so matters: half an export
        // that reports only the failure looks like nothing was written.
        say("wrote " + sheetPath + "; source refused: " + error, 10.0f);
        return;
    }

    const std::string sourcePath = stem + ".hpp";
    if (!writeFileBytes(sourcePath, reinterpret_cast<const uint8_t*>(source.data()), source.size(),
                        &error)) {
        say("wrote " + sheetPath + "; " + sourcePath + " failed: " + error, 10.0f);
        return;
    }

    say("wrote " + sourcePath + " and " + sheetPath, 8.0f);
}

void Studio::exportForMinecraft() {
    std::string error;

    edit::McExportOptions options = minecraft;
    options.name = edit::resourceName(name);

    const std::string root = std::string(kOutputDirectory) + "/pack";

    if (mode == Mode::Canvas) {
        // A texture is already the format. All the pack adds is the path it
        // has to sit at and the mcmeta that makes the folder loadable.
        const std::string path = root + "/assets/" + options.space + "/textures/" +
                                 options.folder + "/" + options.name + ".png";

        if (!createParentDirectories(path) || !canvas.save(path, &error)) {
            say("pack failed: " + error, 8.0f);
            return;
        }

        const std::string meta = "{\n  \"pack\": {\n    \"pack_format\": " +
                                 std::to_string(options.packFormat) + ",\n    \"description\": \"" +
                                 options.description + "\"\n  }\n}\n";
        if (!writeFileBytes(root + "/pack.mcmeta", reinterpret_cast<const uint8_t*>(meta.data()),
                            meta.size(), &error)) {
            say("wrote " + path + "; pack.mcmeta failed: " + error, 10.0f);
            return;
        }

        say("pack: " + path, 8.0f);
        return;
    }

    if (mode == Mode::Sprites) {
        // Nothing to write. Minecraft has no notion of a loose quad standing
        // in the world -- particles there are a hard-coded system, not a
        // model -- so this is a refusal with a reason rather than a stub.
        say("Minecraft has no format for loose sprites; export them as source", 8.0f);
        return;
    }

    edit::McStats stats;
    if (!edit::writeMinecraftPack(sculpt.model(), root, options, &stats, &error)) {
        say("pack refused: " + error, 10.0f);
        return;
    }

    say("pack: " + root + "  (" + std::to_string(stats.elements) + " elements, " +
            std::to_string(stats.faces) + " faces, " + std::to_string(stats.materials) +
            " colours)",
        9.0f);
}

void Studio::importFile(const std::string& path) {
    std::string error;

    ImageU8 image;
    if (!pngLoad(path, image, &error)) {
        say("could not read " + path + ": " + error, 8.0f);
        return;
    }

    // The size in the name is what says this picture is a stack of slices
    // rather than a texture. Without it there is no way to tell, and guessing
    // would turn a mistyped file name into a model nobody asked for.
    IVec3 dims{};
    if (edit::parseSheetFileName(path, dims)) {
        VoxelModel model;
        if (!edit::readLayerSheet(image, dims, model, 0, &error)) {
            say("not a sheet of that size: " + error, 8.0f);
            return;
        }
        sculpt.adopt(std::move(model));
        mode = Mode::Sculpt;
        say("loaded " + path + " as a model", 6.0f);
        return;
    }

    canvas.create(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) canvas.pencil(x, y, image.get(x, y));
    canvas.history().clear();

    mode = Mode::Canvas;
    say("loaded " + path + " as a canvas", 6.0f);
}

} // namespace forge
