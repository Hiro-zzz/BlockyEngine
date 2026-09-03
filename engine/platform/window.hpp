#pragma once
// A window, on raw Win32. No GLFW, no SDL.
//
// Deliberately minimal: create a window, pump its messages, and report what
// the keyboard and mouse did since the last frame. Everything the viewport
// needs and nothing else.
#include "engine/core/image.hpp"
#include "engine/core/math.hpp"

#include <cstdint>
#include <string>

namespace blocky {

// Key codes are Win32 virtual-key codes. The handful the viewport binds are
// named here so scene code does not have to include windows.h.
namespace key {
inline constexpr int W = 'W', A = 'A', S = 'S', D = 'D';
inline constexpr int Q = 'Q', E = 'E', F = 'F', G = 'G', R = 'R', P = 'P', T = 'T';
inline constexpr int Space  = 0x20;
inline constexpr int Shift  = 0x10;
inline constexpr int Control = 0x11;
inline constexpr int Escape = 0x1B;
inline constexpr int Digit1 = '1', Digit2 = '2', Digit3 = '3';
inline constexpr int Minus = 0xBD, Plus = 0xBB;   // the OEM -/= pair
inline constexpr int F1 = 0x70, F2 = 0x71, F3 = 0x72, F4 = 0x73, F5 = 0x74;
inline constexpr int Enter = 0x0D;
inline constexpr int Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
} // namespace key

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const std::string& title, int width, int height, std::string* error = nullptr);
    void destroy();

    // Created hidden, so the GL context can be set up before anything is
    // painted. Snapshot mode never calls this at all.
    void show();

    // Drains the message queue. Returns false once the window should close.
    bool pumpEvents();

    bool isOpen() const { return open_; }
    void requestClose() { open_ = false; }

    int width()  const { return width_; }
    int height() const { return height_; }

    // Held down right now.
    bool keyDown(int virtualKey) const;
    // Went down during the last pumpEvents() -- edge triggered.
    bool keyPressed(int virtualKey) const;

    bool mouseRightDown() const { return mouseRight_; }
    bool mouseLeftDown() const { return mouseLeft_; }

    // Edge-triggered, cleared by pumpEvents like keyPressed. Breaking a block
    // wants the click, not the hold -- a held button would mine the whole
    // column in a second.
    bool mouseLeftPressed() const { return mouseLeftPressed_; }
    bool mouseRightPressed() const { return mouseRightPressed_; }
    // Cursor movement since the last frame, in pixels.
    // Relative mouse movement since the last pumpEvents, in the device's own
    // counts. From raw input, so it is independent of the cursor's position,
    // of the screen edge, and of any pointer acceleration the desktop applies.
    Vec2 mouseDelta() const { return mouseDelta_; }

    // Where the cursor is, in pixels from the top-left of the client area.
    //
    // This is the other kind of mouse, and the two do not substitute for each
    // other. Looking around wants `mouseDelta`, which no screen edge can stop;
    // pointing at a button wants this, which no acceleration curve can drift.
    // Meaningless while the cursor is captured, since capture keeps yanking it
    // back to the middle.
    Vec2 mousePosition() const { return {float(lastMouseX_), float(lastMouseY_)}; }

    // Wheel notches since the last frame.
    float wheelDelta() const { return wheelDelta_; }

    // While captured the cursor is hidden and re-centred every frame, which
    // is what makes a fly camera feel right.
    void setCursorCaptured(bool captured);
    bool cursorCaptured() const { return cursorCaptured_; }

    // Whether this window currently has keyboard focus. A captured cursor
    // must be let go when it does not: a window that keeps yanking the
    // pointer back to its middle while you are using something else is not a
    // game, it is a hostage situation.
    bool focused() const { return focused_; }

    // Native handle, as void* so this header stays free of windows.h.
    void* nativeHandle() const { return hwnd_; }
    void* deviceContext() const { return hdc_; }

    void setTitle(const std::string& title);

    // The icon in the title bar, the task bar and Alt-Tab, from RGBA pixels.
    //
    // Pixels rather than a resource file, because a `.ico` needs an `.rc` and
    // `rc.exe` in the build -- a second toolchain step and a second file
    // format, for an image this project would rather draw the way it draws
    // everything else. `image` should be square; 32 or 64 across is plenty,
    // since Windows scales what it is given.
    //
    // Safe to call before `create`: the icon is remembered and applied to the
    // window when there is one.
    bool setIcon(const ImageU8& image);

private:
    static long long __stdcall windowProc(void* hwnd, unsigned msg, unsigned long long wparam,
                                          long long lparam);
    void onKey(int virtualKey, bool down);
    void centreCursor();

    void* hwnd_ = nullptr;
    void* hdc_ = nullptr;

    int width_ = 0, height_ = 0;
    bool open_ = false;

    bool keyDown_[256] = {};
    bool keyPressed_[256] = {};
    bool mouseRight_ = false;
    bool mouseLeft_ = false;
    bool mouseLeftPressed_ = false;
    bool mouseRightPressed_ = false;

    Vec2 mouseDelta_{};
    float wheelDelta_ = 0.0f;
    bool cursorCaptured_ = false;
    bool rawMouse_ = false;
    bool focused_ = true;
    int lastMouseX_ = 0, lastMouseY_ = 0;

    // Owned: destroyed with the window, and replaced rather than leaked when
    // `setIcon` is called twice.
    void* icon_ = nullptr;
};

} // namespace blocky
