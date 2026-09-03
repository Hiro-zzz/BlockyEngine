#pragma once
// The worlds a game can start in.
//
// Two kinds, and the difference is not how they are stored -- both are code,
// because this engine has no world file and deliberately no scene format. The
// difference is what they are *for*.
//
//   - **A generated world** is different every time and the same for a given
//     seed. It is where you go to be somewhere new.
//   - **A named map** is the same for everyone, every time. It is where you go
//     to build, to test something, or to know that the thing you are about to
//     show somebody will be where you left it.
//
// The named ones are written the way `gm_construct` is: not as landscapes that
// happen to be fixed, but as places arranged on purpose, with a flat plain to
// build on and geometry that answers questions -- how high can I step, how far
// can I fall, what does water do.
//
// The seam is `buildMap`. Everything downstream asks the `World`, never the
// generator, exactly as `terrain.hpp` already promised.
#include "engine/core/math.hpp"
#include "engine/world/world.hpp"

#include "game/terrain.hpp"

#include <string>

namespace game {

enum class MapKind {
    Random,      // a fresh world from the seed
    Construct,   // be_construct: the flat build yard
    Lake,        // be_lake: a bowl of water in a ring of forest
    Valley,      // be_valley: a river between two ridges
};

struct MapInfo {
    const char* id;      // what the command line calls it
    const char* name;    // what the menu shows
    const char* blurb;   // one line under it
};

// Fixed order, so a menu can walk them without a table of its own.
inline constexpr MapKind kMapOrder[] = {MapKind::Random, MapKind::Construct, MapKind::Lake,
                                        MapKind::Valley};
inline constexpr int kMapCount = int(sizeof(kMapOrder) / sizeof(kMapOrder[0]));

const MapInfo& mapInfo(MapKind kind);

// "be_lake" or "lake" both work; anything unknown comes back as Random, which
// is the answer that always produces a world.
MapKind mapFromName(const std::string& name);

// Fills `world` and puts the feet position of a sensible start in `spawn`.
//
// Named maps say where to start rather than searching for it: the point of a
// build yard is that everyone arrives at the same place, facing the same way.
// `settings` is only fully consulted by `Random`; the named maps take the seed
// from it for the details they scatter, and ignore the rest, because a map
// whose shape depended on the extent would not be the same map twice.
uint64_t buildMap(blocky::World& world, MapKind kind, const TerrainSettings& settings,
                  blocky::Vec3& spawn, float& spawnYawDegrees);

} // namespace game
