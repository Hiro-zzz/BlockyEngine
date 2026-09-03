#pragma once
// The player: a character, a camera, and the two verbs a voxel game is made
// of -- take a block away, put one back.
//
// The controller underneath is `blocky::Character`, which knows nothing about
// input, cameras or blocks. This is the layer that turns a keyboard into a
// wish direction and a mouse click into an edit, and it is deliberately the
// only place in the game that reads the window.
#include "engine/physics/character.hpp"
#include "engine/platform/window.hpp"
#include "engine/world/world.hpp"
#include "game/blocks.hpp"

namespace game {

struct Player {
    blocky::Character body;
    blocky::CharacterSettings settings;

    float mouseSensitivity = 0.11f;

    // How far away a block can be reached.
    float reach = 5.0f;

    // ---------------------------------------------------------- the hotbar
    // One row, and everything that can be in the hand is in it.
    //
    // The physgun used to be a key, and that was wrong twice over. A key is
    // invisible -- nothing on the screen says whether it is pressed, so the
    // tool could be out and indistinguishable from not being out -- and it
    // makes a tool a different *kind* of thing from a block, which it is not:
    // both are what the hand is holding, both decide what the mouse buttons
    // do, and there is exactly one at a time.
    //
    // So a slot is one or the other, the number keys and the wheel pick
    // between them all, and the row along the bottom of the screen is the
    // answer to "what am I holding" rather than to "what would I place".
    struct Slot {
        enum class Kind { Block, Physgun };

        Kind kind = Kind::Block;
        blocky::BlockId block = 0;

        static Slot ofBlock(blocky::BlockId id) { return {Kind::Block, id}; }
        static Slot tool(Kind which) { return {which, 0}; }
    };

    std::vector<Slot> hotbar;
    size_t held = 0;

    // Where the camera is. The controller does not know which of these is on
    // -- only the camera and the pose change, and both are downstream.
    //
    // The front view is not a mirror of the back one: it looks *at* the
    // character rather than past them, which is the only mode where the face
    // is visible and therefore the only one where the eye rig is worth
    // anything. Every game in the genre has all three for that reason.
    enum class View { First, ThirdBack, ThirdFront };
    View view = View::First;

    bool thirdPerson() const { return view != View::First; }

    // What the crosshair is on, valid only when `looking` is true.
    bool  looking = false;
    blocky::IVec3 target{};
    blocky::IVec3 targetNormal{};

    // Set for one frame when a block was removed or placed, so the host knows
    // the world changed without diffing it.
    bool edited = false;

    // Set for one frame when the hands did something worth animating --
    // including a swing that hit nothing. Missing is an action too: an arm
    // that only moves when a block was actually there tells the player they
    // are out of range by staying still, which reads as the game ignoring
    // them rather than as the block being far away.
    bool acted = false;

    // Holding the button keeps mining, which is what the genre does and what
    // makes the swing a cycle rather than a twitch. A tap is never swallowed:
    // the press acts at once and only the repeat waits.
    float mineInterval = 0.24f;
    float mineCooldown = 0.0f;

    static const char* viewName(View view) {
        return view == View::First ? "1ST" : (view == View::ThirdBack ? "3RD" : "FRONT");
    }

    const Slot* heldSlot() const {
        return hotbar.empty() ? nullptr : &hotbar[held % hotbar.size()];
    }

    // What a right click would place. Meaningless while a tool is held, and
    // the caller is expected to ask `holdingTool` first -- there is no block
    // that means "no block", and inventing one would put it in the palette.
    blocky::BlockId heldBlock() const {
        const Slot* slot = heldSlot();
        return slot && slot->kind == Slot::Kind::Block ? slot->block
                                                       : blocky::BlockId(block::Stone);
    }

    bool holdingTool() const {
        const Slot* slot = heldSlot();
        return slot && slot->kind != Slot::Kind::Block;
    }
};

// Mouse look, on its own because it belongs to the frame rather than to the
// fixed step: turning at the physics rate feels notchy on a fast machine.
void aimPlayer(Player& player, const blocky::Window& window, float dt);

// One fixed step of walking. Input is read here rather than passed in so the
// bindings live in one place.
//
// `mayRun` is asked rather than assumed because whether the run key means
// anything is not a question about input: an empty stamina bar answers it, and
// that lives above this layer. Passing it in keeps the binding here and the
// rule there. See `maySprint`.
void stepPlayer(Player& player, const blocky::World& world, const blocky::Window& window,
                bool mayRun, float dt);

// Where the crosshair is pointing, and what a click does. Called once a frame,
// with the frame's own `dt` -- the mining repeat is a thing that happens in
// time, unlike walking, which happens on the fixed step.
//
// The ray starts at the character's true eye, not at the animated camera: the
// crosshair must not wobble with the view bob. The two are a few centimetres
// apart at most, which at a reach of five blocks is nothing, and the
// alternative is a game that misses because it was mid-step.
void updateReach(Player& player, blocky::World& world, const blocky::Window& window, float dt);

} // namespace game
