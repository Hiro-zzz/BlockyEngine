#pragma once
// The interactive viewport.
//
// Its job is not to be a renderer -- the path tracer is the renderer. Its job
// is to let you stand somewhere, look at something, and get those numbers
// back out as code you can paste into a scene. Everything else is in service
// of that: no shadows, no reflections, just enough shading to read shape.
#include "engine/render/gl/overlay.hpp"
#include "engine/scene/scene.hpp"

#include <functional>
#include <string>

namespace blocky {

class Window;

// What the per-frame hook is told. Time is measured from the first frame, and
// `dt` is already clamped -- a stall must not teleport anything.
struct ViewportFrame {
    float  dt = 0.0f;
    double time = 0.0;
    int    index = 0;

    // Keys and mouse, for a host that is driving. Null in snapshot mode,
    // where there is no window to read.
    //
    // The window itself rather than a second input abstraction: a game needs
    // more of it than a viewport could guess, and inventing an intermediate
    // struct would mean adding a field to it every time the game wanted
    // another key.
    const Window* input = nullptr;
};

// What the post pass does to a finished frame.
//
// The viewport draws the world into a linear HDR buffer and then runs one pass
// over it, which is the arrangement the CPU renderers have always had: `Image`
// is linear, `post/` filters it, `tonemap` runs once at the end. These are the
// knobs of that pass.
//
// Cel is the odd one out and belongs in this struct anyway. Its **bands** are
// applied while shading, because quantising illumination needs the light, and
// by the time a frame exists the light has already been multiplied by albedo.
// Its **outline** is applied here, because an edge needs the neighbours. That
// is the same two-seam split `docs/architecture.md` describes for the tracer,
// and the reason a style cannot be one enum with one branch.
struct ViewportEffects {
    // 0 turns cel off. Otherwise the number of steps the illumination may
    // take: three or four read as cel, ten reads as a slightly odd renderer.
    float celBands = 0.0f;

    // How dark an edge goes, 0 to 1. Independent of `celBands` on purpose --
    // an outline over ordinary shading is a legitimate look, and cel without
    // one is what a rasteriser gives you if you forget.
    float outline = 0.0f;
    float outlineDepthTolerance = 0.055f;   // relative, so distance is fair
    float outlineNormalTolerance = 0.35f;   // 1 - dot(n0, n1)

    // Above this luminance a pixel starts to glow. Lava, glowstone and the
    // sun off water are what this is for; the threshold sits above one so
    // ordinary lit surfaces cannot reach it.
    float bloomThreshold = 1.15f;
    float bloomSoftness = 0.7f;
    float bloomIntensity = 0.0f;   // 0 turns the whole chain off

    float vignette = 0.0f;
    float saturation = 1.0f;
    float contrast = 1.0f;

    // A colour the frame is multiplied by, and a haze it fades into with
    // distance. Both are off by default and both exist for the same case:
    // being *inside* something.
    //
    // A head underwater is not a grade -- it is a medium, and the two things
    // a medium does are absorb (the tint) and scatter (the fog). Doing it
    // here rather than in the world pass is the same seam the outline uses:
    // the depth buffer is already sitting in this shader, and the alternative
    // is teaching every draw call in the viewport about a fluid. The tracer
    // needs none of this -- water there is a real medium with a real
    // absorption coefficient, and a ray that entered it knows.
    Vec3  tint{1.0f, 1.0f, 1.0f};
    float fogDensity = 0.0f;             // per block; 0 turns it off
    Vec3  fogColour{0.10f, 0.24f, 0.34f};
};

struct ViewportSettings {
    int width = 1280;
    int height = 720;
    std::string title = "BlockyEngine viewport";

    bool vsync = true;
    int  msaa = 4;

    float moveSpeed = 8.0f;        // blocks per second
    float mouseSensitivity = 0.12f; // degrees per pixel
    float exposure = 1.0f;

    // When set, the viewport renders to this path and exits without ever
    // showing a window. This is what makes a GUI program testable from a
    // build script.
    std::string snapshotPath;

    // How many frames to run before the snapshot is taken. One is right for
    // a still world; anything proving that something *changes* needs more,
    // because the first frame's dt is microseconds and a fixed-step host will
    // not have advanced at all by then.
    int snapshotFrames = 1;

    // Called once per frame, before anything is drawn, with the scene free to
    // change underneath. Returning false ends the loop.
    //
    // This is the seam that turns a viewer into a host. Everything the
    // viewport does about a changing world -- remeshing only the chunks whose
    // stamp moved, re-reading the prop placements -- happens because of it,
    // and nothing above it needs to know whether the changes came from
    // physics, from a script, or from a key press.
    std::function<bool(const ViewportFrame&)> onFrame;

    // When set, the viewport stops flying its own camera and uses
    // `scene.camera` exactly as `onFrame` left it. Its own WASD and mouse-look
    // bindings go quiet, because a game wants those keys.
    //
    // The fly camera is not deleted, only stood down -- finding an angle and
    // pasting it into a scene is still what the viewport is *for*, and a game
    // built on it should be able to hand control back.
    bool hostCamera = false;

    // Appended to the window title when set. A host uses it to say what just
    // happened -- "reloaded", or the line a script failed on -- where the eye
    // already is, rather than in a console behind the window.
    const std::string* statusText = nullptr;

    // Called after the world is drawn, to put an interface on top of it. The
    // viewport owns the `Overlay` because it owns the context; the host owns
    // what goes in it.
    //
    // Skipped in snapshot mode by default -- a photograph of a scene is not
    // usually wanted with a menu across it. `snapshotOverlay` asks for it
    // anyway, which is how the interface itself gets checked from a script.
    std::function<void(Overlay&)> onOverlay;
    bool snapshotOverlay = false;

    // The window icon, as RGBA pixels. Empty leaves the system default.
    //
    // Pixels rather than a `.ico`, so the game can draw its own mark with the
    // same code it draws everything else -- see `Window::setIcon`.
    blocky::ImageU8 icon;

    // Points at a host-owned flag: while it is true the cursor is left alone.
    //
    // A pointer rather than a value for the same reason `statusText` is one:
    // the settings are handed over once and read every frame, and a menu has
    // to be able to change its mind halfway through the run. Capture is what
    // makes looking around work and what makes clicking a button impossible,
    // so something has to be able to turn it off.
    const bool* freeCursor = nullptr;

    // Post-processing, or null for none of it. A pointer for the same reason
    // `statusText` is one: a host that lets the player press a key to change
    // the look has to be able to change it after the settings were handed
    // over.
    const ViewportEffects* effects = nullptr;

    // Leaves Escape to the host instead of ending the loop on it.
    //
    // Off by default on purpose: a window that cannot be closed is not a
    // kindness, and a scene has no other way out. A host that turns this on is
    // promising it has one -- a menu with a way to quit -- and `onFrame`
    // returning false is how it takes it.
    bool hostEscape = false;
};

// Returns a process exit code. The camera the user flies to is printed on
// exit, ready to paste back into scene code.
//
// The scene is taken by reference and **may be modified**, but only by
// `settings.onFrame`: the viewport itself changes nothing about it.
int runViewport(Scene& scene, const ViewportSettings& settings);

} // namespace blocky
