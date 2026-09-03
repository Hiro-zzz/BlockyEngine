#include "engine/platform/window.hpp"

#include <windows.h>

#include <cstring>
#include <vector>

namespace blocky {
namespace {

const wchar_t* kClassName = L"BlockyEngineWindow";

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), n);
    return w;
}

// windowsx.h defines these, but pulling in the whole header for two macros is
// not worth it. The coordinates are signed 16-bit and can go negative when the
// mouse is captured outside the client area.
int mouseX(LPARAM lparam) { return int(short(LOWORD(lparam))); }
int mouseY(LPARAM lparam) { return int(short(HIWORD(lparam))); }

} // namespace

// A plain Win32 callback, so it recovers the Window instance from the
// handle's user data. The instance pointer is planted at WM_NCCREATE, the
// first message a window ever receives.
long long __stdcall Window::windowProc(void* hwndRaw, unsigned msg, unsigned long long wparam,
                                       long long lparam) {
    HWND hwnd = static_cast<HWND>(hwndRaw);

    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, WPARAM(wparam), LPARAM(lparam));
    }

    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, WPARAM(wparam), LPARAM(lparam));

    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            self->open_ = false;
            return 0;

        case WM_SIZE:
            self->width_ = int(LOWORD(lparam));
            self->height_ = int(HIWORD(lparam));
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Bit 30 of lparam is set on auto-repeat, which is not an edge.
            self->onKey(int(wparam), (lparam & (1LL << 30)) == 0);
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wparam < 256) self->keyDown_[wparam] = false;
            return 0;

        case WM_LBUTTONDOWN:
            if (!self->mouseLeft_) self->mouseLeftPressed_ = true;
            self->mouseLeft_ = true;
            SetCapture(hwnd);
            return 0;

        case WM_LBUTTONUP:
            self->mouseLeft_ = false;
            if (!self->mouseRight_) ReleaseCapture();
            return 0;

        case WM_RBUTTONDOWN:
            if (!self->mouseRight_) self->mouseRightPressed_ = true;
            self->mouseRight_ = true;
            SetCapture(hwnd);
            return 0;

        case WM_RBUTTONUP:
            self->mouseRight_ = false;
            ReleaseCapture();
            return 0;

        case WM_SETFOCUS:
            self->focused_ = true;
            return 0;

        case WM_KILLFOCUS:
            // Every key is released as far as we know: holding W and alt-tabbing
            // away must not leave the player walking for ever.
            self->focused_ = false;
            for (bool& down : self->keyDown_) down = false;
            self->mouseLeft_ = false;
            self->mouseRight_ = false;
            return 0;

        case WM_MOUSEWHEEL:
            self->wheelDelta_ += float(GET_WHEEL_DELTA_WPARAM(WPARAM(wparam))) / float(WHEEL_DELTA);
            return 0;

        case WM_INPUT: {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &size,
                                sizeof(RAWINPUTHEADER)) == UINT(-1))
                break;
            if (input.header.dwType != RIM_TYPEMOUSE) break;

            // A tablet or a remote desktop reports absolute positions instead
            // of relative counts. Those are rare and would need the previous
            // absolute point to difference against, so they are ignored rather
            // than guessed at -- WM_MOUSEMOVE still tracks the cursor for
            // anything that wants to know where it is.
            if ((input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) break;

            self->mouseDelta_.x += float(input.data.mouse.lLastX);
            self->mouseDelta_.y += float(input.data.mouse.lLastY);
            break;   // WM_INPUT must reach DefWindowProc so the system cleans up
        }

        case WM_MOUSEMOVE: {
            // Position only. The look delta comes from WM_INPUT above.
            self->lastMouseX_ = mouseX(LPARAM(lparam));
            self->lastMouseY_ = mouseY(LPARAM(lparam));
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, WPARAM(wparam), LPARAM(lparam));
}

void Window::onKey(int virtualKey, bool freshPress) {
    if (virtualKey < 0 || virtualKey >= 256) return;
    if (freshPress && !keyDown_[virtualKey]) keyPressed_[virtualKey] = true;
    keyDown_[virtualKey] = true;
}

Window::~Window() {
    destroy();

    // Freed here rather than in `destroy`, which `create` calls on itself
    // first: an icon set before the window existed has to survive that, or
    // setting one up front would silently do nothing.
    if (icon_) DestroyIcon(static_cast<HICON>(icon_));
    icon_ = nullptr;
}

bool Window::create(const std::string& title, int width, int height, std::string* error) {
    destroy();

    HINSTANCE instance = GetModuleHandleW(nullptr);

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // OWNDC gives the window a private device context, which is what a GL
        // context wants bound for the window's whole life.
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::windowProc);
        wc.hInstance = instance;
        // IDC_ARROW resolves to the ANSI resource id unless UNICODE is defined
        // project-wide, so name the wide form explicitly.
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            if (error) *error = "window: RegisterClassExW failed";
            return false;
        }
        classRegistered = true;
    }

    // Size the frame so the *client* area is the requested resolution.
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance, this);
    if (!hwnd) {
        if (error) *error = "window: CreateWindowExW failed";
        return false;
    }

    hwnd_ = hwnd;
    hdc_ = GetDC(hwnd);
    width_ = width;
    height_ = height;
    open_ = true;

    // An icon set before the window existed. Both messages, because Windows
    // keeps two: the small one for the title bar and Alt-Tab, the big one for
    // the task bar and the switcher.
    if (icon_) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(icon_));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icon_));
    }

    // Mouse look comes from raw input, not from where the cursor is.
    //
    // The obvious way -- read the cursor position, subtract the last one, put
    // the cursor back in the middle -- is what this used to do, and it is
    // quietly wrong. Windows coalesces mouse moves, so the synthetic move
    // caused by re-centring and the player's real move can arrive as one
    // message or out of order; the flag that skips "the move we caused
    // ourselves" then eats a real one instead. Centring once per drag hides
    // it well enough to ship a fly camera. Centring every frame, which is what
    // a captured cursor needs, turns it into half the input going missing.
    //
    // Raw input reports the device's own relative counts. Nothing is
    // subtracted, nothing is synthetic, and the cursor's position stops being
    // part of the answer at all.
    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = 0x01;   // generic desktop
    mouse.usUsage = 0x02;       // mouse
    mouse.dwFlags = 0;          // only while this window has focus
    mouse.hwndTarget = hwnd;
    rawMouse_ = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;

    return true;
}

void Window::destroy() {
    setCursorCaptured(false);

    if (hwnd_) {
        if (cursorCaptured_) setCursorCaptured(false);
        if (hdc_) ReleaseDC(static_cast<HWND>(hwnd_), static_cast<HDC>(hdc_));
        DestroyWindow(static_cast<HWND>(hwnd_));
    }
    hwnd_ = nullptr;
    hdc_ = nullptr;
    open_ = false;
}

void Window::show() {
    if (!hwnd_) return;
    ShowWindow(static_cast<HWND>(hwnd_), SW_SHOW);
    UpdateWindow(static_cast<HWND>(hwnd_));
    SetForegroundWindow(static_cast<HWND>(hwnd_));
}

bool Window::pumpEvents() {
    // Per-frame state resets before the queue is drained.
    for (bool& pressed : keyPressed_) pressed = false;
    mouseLeftPressed_ = false;
    mouseRightPressed_ = false;
    mouseDelta_ = Vec2{0.0f, 0.0f};
    wheelDelta_ = 0.0f;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (cursorCaptured_ && open_ && focused_) centreCursor();
    return open_;
}

void Window::centreCursor() {
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (!hwnd) return;

    RECT client;
    GetClientRect(hwnd, &client);

    // Only to keep the pointer from wandering out of the window and clicking
    // on something else. Nothing reads its position for movement any more, so
    // there is no synthetic move to compensate for.
    POINT centre{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
    lastMouseX_ = int(centre.x);
    lastMouseY_ = int(centre.y);

    ClientToScreen(hwnd, &centre);
    SetCursorPos(centre.x, centre.y);
}

void Window::setCursorCaptured(bool captured) {
    if (captured == cursorCaptured_) return;
    cursorCaptured_ = captured;
    ShowCursor(captured ? FALSE : TRUE);
    if (captured) centreCursor();
}

void Window::setTitle(const std::string& title) {
    if (hwnd_) SetWindowTextW(static_cast<HWND>(hwnd_), widen(title).c_str());
}

bool Window::setIcon(const ImageU8& image) {
    if (image.empty()) return false;

    // Windows wants bottom-up BGRA; ours is top-down RGBA. Both conversions
    // are done here rather than asked of the caller, because "supply your
    // pixels upside down and with two channels swapped" is not an interface
    // anybody should have to remember.
    const int w = image.width(), h = image.height();
    std::vector<uint8_t> bgra(size_t(w) * size_t(h) * 4);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const ImageU8::RGBA c = image.get(x, h - 1 - y);
            uint8_t* out = &bgra[(size_t(y) * size_t(w) + size_t(x)) * 4];
            out[0] = c.b;
            out[1] = c.g;
            out[2] = c.r;
            out[3] = c.a;
        }
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP colour = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!colour || !bits) {
        if (colour) DeleteObject(colour);
        return false;
    }
    std::memcpy(bits, bgra.data(), bgra.size());

    // A mask is still required even for a 32-bit icon, and is still ignored
    // when the colour bitmap carries alpha. An empty one is the usual answer.
    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);

    ICONINFO icon{};
    icon.fIcon = TRUE;
    icon.hbmColor = colour;
    icon.hbmMask = mask;

    HICON handle = CreateIconIndirect(&icon);
    DeleteObject(colour);
    DeleteObject(mask);
    if (!handle) return false;

    if (icon_) DestroyIcon(static_cast<HICON>(icon_));
    icon_ = handle;

    if (hwnd_) {
        SendMessageW(static_cast<HWND>(hwnd_), WM_SETICON, ICON_SMALL, LPARAM(handle));
        SendMessageW(static_cast<HWND>(hwnd_), WM_SETICON, ICON_BIG, LPARAM(handle));
    }
    return true;
}

bool Window::keyDown(int virtualKey) const {
    return virtualKey >= 0 && virtualKey < 256 && keyDown_[virtualKey];
}

bool Window::keyPressed(int virtualKey) const {
    return virtualKey >= 0 && virtualKey < 256 && keyPressed_[virtualKey];
}

} // namespace blocky
