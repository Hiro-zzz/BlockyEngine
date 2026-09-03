#pragma once
// Textures for world blocks: loading them, and answering which one belongs on
// which face of which block.
//
// The directory used to be called `assets/game/`, which stopped being true the
// day the engine gave up having a palette. Nothing here knows what a block is
// called; it is handed rules and it resolves them.
//
// Deliberately separate from the entity skin path. Block textures are many
// small independent images addressed by block and face, authored in a
// resource pack under textures/block/, some of them animated strips, some of
// them needing a biome tint multiplied on. An entity skin is one image with a
// fixed internal layout. The two share nothing but Texture and AssetSource,
// so they get their own libraries.
#include "engine/assets/asset_source.hpp"
#include "engine/assets/texture.hpp"
#include "engine/core/math.hpp"
#include "engine/world/block.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace blocky {

// Face ordering used everywhere in this module.
enum BlockFace { FaceNegX = 0, FacePosX, FaceNegY, FacePosY, FaceNegZ, FacePosZ, FaceCount };

// Map a hit normal (one component +/-1) to a face index.
inline int blockFaceIndex(IVec3 normal) {
    if (normal.x < 0) return FaceNegX;
    if (normal.x > 0) return FacePosX;
    if (normal.y < 0) return FaceNegY;
    if (normal.y > 0) return FacePosY;
    if (normal.z < 0) return FaceNegZ;
    return FacePosZ;
}

// One block's answer to "which texture on which face", and how to tint it.
//
// The shape of the answer is the engine's; the table of answers is not. It
// used to be a fixed list in here, naming vanilla textures for a palette the
// engine also owned -- so a caller with its own blocks could not be resolved
// at all, and the engine carried a list of Minecraft names for no reason of
// its own. The table now comes from whoever owns the palette; for the scenes
// that is `palette::minecraftRules()`.
struct BlockTextureRule {
    BlockId id = 0;

    // Names as they appear under textures/block/, without the directory and
    // without the .png. Null means the face keeps its flat albedo.
    const char* top = nullptr;
    const char* side = nullptr;
    const char* bottom = nullptr;
    const char* sideOverlay = nullptr;  // alpha-composited over `side`, tinted

    Vec3 topTint{1.0f, 1.0f, 1.0f};
    Vec3 overlayTint{1.0f, 1.0f, 1.0f};
    Vec3 allTint{1.0f, 1.0f, 1.0f};
};

class BlockTextureLibrary {
public:
    // Resolve `rules` against `source`, for a world using `registry`. Blocks
    // whose textures are missing simply keep their flat albedo, so a partial
    // or exotic resource pack degrades instead of failing.
    bool load(const AssetSource& source, const BlockRegistry& registry,
              const std::vector<BlockTextureRule>& rules, std::string* error = nullptr);

    // Bind vanilla texture names to a block the built-in table knows nothing
    // about -- one a scene added to its own registry -- or override one it
    // does. Names are as they appear under textures/block/, without the
    // directory and without the .png.
    //
    // This is the seam a model loader would come in through: resolving
    // blockstate and model JSON comes down to "which texture on which face",
    // and that is exactly what this takes. Until then it is how a scene gets
    // at any of the thousand textures the table does not name.
    bool bind(const AssetSource& source, BlockId id, const char* topName, const char* sideName,
              const char* bottomName, Vec3 tint = {1.0f, 1.0f, 1.0f},
              std::string* error = nullptr);

    // ------------------------------------------------- textures built in code
    // The same seam as `bind`, minus the file. A generator calls these in a
    // loop; so would a model loader. Nothing above this class notices where a
    // texture came from, which is the point of the seam existing at all.
    //
    // `addTexture` returns an index to hand to `bindIndices`, or -1 for an
    // empty texture. Registering the same name twice replaces it, so a game
    // can regenerate one tile without rebuilding the set.
    int addTexture(const std::string& name, const Texture& texture);

    // `overlay` is composited over the face texture and alpha-blended, which
    // is how a grass side gets its fringe -- the same mechanism the resource
    // pack path uses, so the two cannot drift.
    bool bindIndices(BlockId id, int top, int side, int bottom, Vec3 tint = {1.0f, 1.0f, 1.0f},
                     int overlay = -1, Vec3 overlayTint = {1.0f, 1.0f, 1.0f});

    // Width of the widest registered texture, or 0 when there are none. The
    // viewport's texture array asks, so that generating at 32 pixels a face
    // does not get sampled back down to 16 on the way to the GPU.
    int tileSize() const;

    bool   loaded() const { return !textures_.empty(); }
    size_t textureCount() const { return textures_.size(); }
    size_t texturedBlockCount() const { return texturedBlocks_; }

    bool hasTexture(BlockId id, int face) const;

    // Linear-light albedo at a point on a face. `fallback` is returned when
    // this block or face has no texture.
    Vec3 sampleAlbedo(BlockId id, int face, Vec2 uv, Vec3 fallback) const;

    // Mean colour of a face, for previews and for any code that wants one
    // value per face rather than a texture lookup.
    Vec3 averageAlbedo(BlockId id, int face, Vec3 fallback) const;

    // Names that could not be found, for reporting.
    const std::vector<std::string>& missing() const { return missing_; }

private:
    struct FaceEntry {
        int  texture = -1;
        int  overlay = -1;  // composited over `texture`, alpha-blended
        Vec3 tint{1.0f, 1.0f, 1.0f};
        Vec3 overlayTint{1.0f, 1.0f, 1.0f};
    };
    struct BlockEntry {
        FaceEntry faces[FaceCount];
        bool any = false;
    };

    int loadTexture(const AssetSource& source, const std::string& name);

    std::vector<Texture> textures_;
    std::unordered_map<std::string, int> textureByName_;
    std::vector<BlockEntry> blocks_;
    std::vector<std::string> missing_;
    size_t texturedBlocks_ = 0;
};

} // namespace blocky
