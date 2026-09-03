#include "engine/assets/blocks/block_textures.hpp"

namespace blocky {
namespace {

const char* kTexturePrefix = "assets/minecraft/textures/block/";

// Everything that used to sit here -- the biome tints, the Rule struct and the
// table of vanilla texture names for the built-in palette -- moved out with
// the palette itself. This file loads and composites what it is handed; which
// texture belongs on which face is a fact about somebody's blocks.

} // namespace

int BlockTextureLibrary::loadTexture(const AssetSource& source, const std::string& name) {
    auto existing = textureByName_.find(name);
    if (existing != textureByName_.end()) return existing->second;

    std::string path = kTexturePrefix + name + ".png";
    std::vector<uint8_t> bytes;
    if (!source.read(path, bytes, nullptr)) {
        missing_.push_back(name);
        textureByName_[name] = -1;
        return -1;
    }

    Texture texture;
    if (!texture.loadFromPng(bytes.data(), bytes.size(), nullptr)) {
        missing_.push_back(name);
        textureByName_[name] = -1;
        return -1;
    }
    // Lava and water ship as vertical animation strips.
    texture.cropToFirstSquareFrame();

    int index = int(textures_.size());
    textures_.push_back(std::move(texture));
    textureByName_[name] = index;
    return index;
}

bool BlockTextureLibrary::load(const AssetSource& source, const BlockRegistry& registry,
                               const std::vector<BlockTextureRule>& rules, std::string* error) {
    textures_.clear();
    textureByName_.clear();
    missing_.clear();
    texturedBlocks_ = 0;

    blocks_.assign(registry.size(), BlockEntry{});

    if (!source.isOpen()) {
        if (error) *error = "block textures: asset source is not open";
        return false;
    }

    for (const BlockTextureRule& rule : rules) {
        if (rule.id >= blocks_.size()) continue;

        int top    = rule.top ? loadTexture(source, rule.top) : -1;
        int side   = rule.side ? loadTexture(source, rule.side) : -1;
        int bottom = rule.bottom ? loadTexture(source, rule.bottom) : -1;
        int overlay = rule.sideOverlay ? loadTexture(source, rule.sideOverlay) : -1;

        BlockEntry& entry = blocks_[rule.id];
        for (int face = 0; face < FaceCount; ++face) {
            FaceEntry& target = entry.faces[face];
            if (face == FacePosY) {
                target.texture = top;
                target.tint = rule.topTint * rule.allTint;
            } else if (face == FaceNegY) {
                target.texture = bottom;
                target.tint = rule.allTint;
            } else {
                target.texture = side;
                target.tint = rule.allTint;
                target.overlay = overlay;
                target.overlayTint = rule.overlayTint;
            }
            if (target.texture >= 0) entry.any = true;
        }
        if (entry.any) ++texturedBlocks_;
    }

    if (textures_.empty()) {
        if (error) *error = "block textures: nothing could be loaded from " + source.description();
        return false;
    }
    return true;
}

bool BlockTextureLibrary::bind(const AssetSource& source, BlockId id, const char* topName,
                               const char* sideName, const char* bottomName, Vec3 tint,
                               std::string* error) {
    if (!source.isOpen()) {
        if (error) *error = "block textures: asset source is not open";
        return false;
    }
    if (id >= blocks_.size()) blocks_.resize(size_t(id) + 1);

    int top    = topName ? loadTexture(source, topName) : -1;
    int side   = sideName ? loadTexture(source, sideName) : -1;
    int bottom = bottomName ? loadTexture(source, bottomName) : -1;

    if (top < 0 && side < 0 && bottom < 0) {
        if (error) *error = "block textures: none of the named textures could be read";
        return false;
    }

    BlockEntry& entry = blocks_[id];
    bool wasTextured = entry.any;
    entry = BlockEntry{};

    for (int face = 0; face < FaceCount; ++face) {
        FaceEntry& target = entry.faces[face];
        target.texture = face == FacePosY ? top : (face == FaceNegY ? bottom : side);
        target.tint = tint;
        if (target.texture >= 0) entry.any = true;
    }
    if (entry.any && !wasTextured) ++texturedBlocks_;
    return entry.any;
}

int BlockTextureLibrary::addTexture(const std::string& name, const Texture& texture) {
    if (texture.empty()) return -1;

    auto found = textureByName_.find(name);
    if (found != textureByName_.end()) {
        textures_[size_t(found->second)] = texture;
        return found->second;
    }

    textures_.push_back(texture);
    int index = int(textures_.size()) - 1;
    textureByName_.emplace(name, index);
    return index;
}

bool BlockTextureLibrary::bindIndices(BlockId id, int top, int side, int bottom, Vec3 tint,
                                      int overlay, Vec3 overlayTint) {
    if (top < 0 && side < 0 && bottom < 0) return false;
    if (id >= blocks_.size()) blocks_.resize(size_t(id) + 1);

    BlockEntry& entry = blocks_[id];
    bool wasTextured = entry.any;
    entry = BlockEntry{};

    for (int face = 0; face < FaceCount; ++face) {
        FaceEntry& target = entry.faces[face];
        target.texture = face == FacePosY ? top : (face == FaceNegY ? bottom : side);
        target.tint = tint;

        // An overlay belongs on the sides only: it is the fringe where a top
        // surface wraps over an edge, and a top or a bottom has no edge.
        if (overlay >= 0 && face != FacePosY && face != FaceNegY) {
            target.overlay = overlay;
            target.overlayTint = overlayTint;
        }
        if (target.texture >= 0) entry.any = true;
    }
    if (entry.any && !wasTextured) ++texturedBlocks_;
    return entry.any;
}

int BlockTextureLibrary::tileSize() const {
    int widest = 0;
    for (const Texture& texture : textures_) widest = std::max(widest, texture.width());
    return widest;
}

bool BlockTextureLibrary::hasTexture(BlockId id, int face) const {
    if (id >= blocks_.size() || face < 0 || face >= FaceCount) return false;
    return blocks_[id].faces[face].texture >= 0;
}

Vec3 BlockTextureLibrary::sampleAlbedo(BlockId id, int face, Vec2 uv, Vec3 fallback) const {
    if (id >= blocks_.size() || face < 0 || face >= FaceCount) return fallback;

    const FaceEntry& entry = blocks_[id].faces[face];
    if (entry.texture < 0) return fallback;

    Vec3 color = textures_[size_t(entry.texture)].sample(uv) * entry.tint;

    if (entry.overlay >= 0) {
        const Texture& overlay = textures_[size_t(entry.overlay)];
        float alpha = overlay.sampleAlpha(uv);
        if (alpha > 0.0f) {
            color = lerp(color, overlay.sample(uv) * entry.overlayTint, alpha);
        }
    }
    return color;
}

Vec3 BlockTextureLibrary::averageAlbedo(BlockId id, int face, Vec3 fallback) const {
    if (id >= blocks_.size() || face < 0 || face >= FaceCount) return fallback;

    const FaceEntry& entry = blocks_[id].faces[face];
    if (entry.texture < 0) return fallback;
    return textures_[size_t(entry.texture)].averageColor() * entry.tint;
}

} // namespace blocky
