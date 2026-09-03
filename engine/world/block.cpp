#include "engine/world/block.hpp"

namespace blocky {

BlockRegistry::BlockRegistry() {
    // Air, and then whatever the caller adds. Id zero has to be air because
    // the storage layer treats zero as "nothing here" -- a chunk that was
    // never allocated reads as air without anybody deciding it should.
    //
    // Nothing else is registered, and that is the whole of the engine's
    // opinion about what a world contains. The twenty-six blocks that used to
    // follow this line were content: they made `physics/` able to ask whether
    // a block was water by comparing an id, and `assets/` able to carry a
    // table of Minecraft texture names. Both now ask the palette they were
    // given. See `scenes/common/palette.hpp` and `game/blocks.hpp`.
    BlockDef air;
    air.name = "air";
    air.albedo = Vec3{0.0f};
    air.opaque = false;
    air.solid = false;   // and not a fluid: passable, and holds nothing up
    add(air);
}

BlockId BlockRegistry::add(BlockDef def) {
    BlockId id = BlockId(defs_.size());
    byName_[def.name] = id;
    defs_.push_back(std::move(def));
    return id;
}

BlockId BlockRegistry::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? block::Air : it->second;
}

} // namespace blocky
