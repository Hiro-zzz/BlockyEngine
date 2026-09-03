#pragma once
// The main menu: what the game is before it is a world.
//
// It runs inside the same viewport the game does, over a world that is
// already loaded and slowly turning. That is a decision rather than a
// shortcut. A menu drawn on a flat colour needs no renderer at all, and a
// menu drawn over a still image needs an artist; a menu drawn over the actual
// engine, orbiting an actual world, needs neither and is the only one of the
// three that cannot go stale when the renderer changes.
//
// -------------------------------------------------------------- what it owns
//
// Nothing but its own state. The menu says which screen it is on and what was
// chosen; `main.cpp` owns the world, the player and the decision to rebuild.
// So the menu can be opened over a running game and closed again without
// anything underneath it being torn down.
#include "engine/render/gl/overlay.hpp"
#include "engine/scene/camera.hpp"

#include "game/maps.hpp"

#include <string>

namespace blocky {
class Window;
class BlockRegistry;
}

namespace game {

struct Player;

enum class Screen {
    Title,      // the front page
    Play,       // choosing a world
    Paused,     // opened from inside a game
};

// What the menu is asking the host to do, once per frame and never twice.
enum class MenuAction {
    None,
    StartGame,   // `chosen` says which map
    Resume,
    ToTitle,     // leave the running game
    Quit,
};

struct Menu {
    Screen screen = Screen::Title;
    bool   open = true;

    // Which row the pointer or the keyboard is on. Kept per screen so moving
    // between them does not lose your place.
    int hovered = 0;

    MapKind chosen = MapKind::Random;
    uint32_t seed = 1337u;

    // Set while a world is being built, so the frame that does the work can
    // say so before it blocks. Generation takes a second and a half on a big
    // extent, and a window that simply stops for that long looks crashed.
    std::string busy;

    // The camera angle the title screen orbits at, in degrees, advanced by
    // the frame time.
    float orbit = 0.0f;

    // True while the menu wants the pointer. Handed to the viewport, which
    // reads it every frame -- see `ViewportSettings::freeCursor`.
    bool wantsCursor = true;
};

// One frame of the menu: reads the window, returns what it decided.
//
// Input is taken here rather than in `main` for the same reason `stepPlayer`
// takes it there: the bindings for one thing belong in one place, and a menu
// that could be driven from two of them would eventually be driven from two
// of them differently.
MenuAction updateMenu(Menu& menu, const blocky::Window& window, int width, int height, float dt);

// Draws it. Separated from the update because the viewport draws the world
// between the two, and an interface has to go on top of that.
void drawMenu(const Menu& menu, blocky::Overlay& overlay);

// The slow orbit the title screen is seen from.
//
// A real camera over the real world, rather than a picture. Every other menu
// background in the genre is an image somebody rendered once and has to
// re-render whenever the game stops looking like it; this one cannot go out
// of date, because it *is* the game looking like itself.
blocky::Camera menuCamera(const Menu& menu, const blocky::World& world, float aspect);

// The heads-up display for a running game: crosshair, hotbar, and the line of
// numbers that used to live in the window title.
struct Hud {
    bool showDebug = false;
    std::string debugLine;

    // A word in the middle of the screen that fades out: which look was just
    // switched to, which block was just picked. Cheaper than a settings panel
    // and, for one value that changes with one key, better.
    std::string toast;
    float toastSeconds = 0.0f;

    // What the buttons do with whatever is in the hand right now. Drawn under
    // the row, and it changes with what the tool is doing rather than only
    // with which slot is chosen.
    std::string hint;
};

struct Vitals;

void drawHud(const Hud& hud, blocky::Overlay& overlay, const Player& player, const Vitals& vitals,
             const blocky::BlockRegistry& registry);

} // namespace game
