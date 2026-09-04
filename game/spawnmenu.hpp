#pragma once
// The spawn menu: hold nothing, press Q, and take something out of the shelf.
//
// The genre's answer to "what is there to play with", and the reason it is a
// menu rather than more hotbar slots: the hotbar is what the hand is holding
// and there is exactly one at a time, while this is a catalogue that will grow
// and has no business being nine things long.
//
// ------------------------------------------------------- the world keeps going
//
// Unlike the pause menu, this does not stop the fixed steps. A spawn menu that
// froze the world would make watching what you just spawned a two-step
// operation, and half of why you spawn a thing is to see it land. What it does
// take is the *pointer*: the cursor comes back, so looking and walking stop
// while it is open, which is the same trade every game in the genre makes.
//
// ------------------------------------------------------------ what it decides
//
// Nothing. It reports which tile was clicked and `main.cpp` does the spawning,
// exactly as `Menu` reports an action and `main.cpp` loads the world. The menu
// knowing how to build a ragdoll would be the menu knowing about the avatar,
// the physics world and the budget.
#include "engine/render/gl/overlay.hpp"

#include "game/props.hpp"

#include <vector>

namespace blocky {
class Window;
}

namespace game {

struct SpawnMenu {
    bool open = false;

    // Which tile the pointer is over, or -1. Kept between frames so the
    // highlight does not flicker on a frame where the mouse did not move.
    int hovered = -1;

    // Set for one frame after a spawn, to say so on screen. A tile that looks
    // identical before and after a click leaves the player wondering whether
    // it worked, and clicking again is how you end up with four.
    float flashSeconds = 0.0f;
    int   flashTile = -1;
};

// What the player asked for this frame.
struct SpawnRequest {
    bool made = false;

    // Index into the catalogue, or `kRagdoll` for the one entry that is not a
    // prop. The menu shows them in one grid because from where the player
    // stands they are the same verb -- put a thing in the world.
    int tile = -1;
};

// The tile index that means "a ragdoll", one past the catalogue.
inline int ragdollTile(const std::vector<PropKind>& catalogue) { return int(catalogue.size()); }

// One frame. Reads the window; returns what was clicked, if anything.
SpawnRequest updateSpawnMenu(SpawnMenu& menu, const blocky::Window& window,
                             const std::vector<PropKind>& catalogue, int width, int height,
                             float dt);

// Draws the grid over the finished frame.
void drawSpawnMenu(const SpawnMenu& menu, blocky::Overlay& overlay,
                   const std::vector<PropKind>& catalogue);

} // namespace game
