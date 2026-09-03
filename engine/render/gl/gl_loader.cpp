#include "engine/render/gl/gl_loader.hpp"

#include <windows.h>

// The third Win32 collision in this file's history, after FLOAT and BYTE.
// winnt.h spells the compiler's store fence `MemoryBarrier`, and on the
// expansions below that would rewrite the table entry for glMemoryBarrier
// into a fence. The GL name is fixed by the API, so the macro is what gives.
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

#include <cstdio>

namespace blocky {
namespace gl {

// Definitions for the pointer table declared in the header.
#define BLOCKY_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
BLOCKY_GL_FUNCTIONS(BLOCKY_GL_DEFINE)
#undef BLOCKY_GL_DEFINE

namespace {

// ---------------------------------------------------------------- WGL bits
constexpr int WGL_DRAW_TO_WINDOW_ARB = 0x2001;
constexpr int WGL_ACCELERATION_ARB   = 0x2003;
constexpr int WGL_SUPPORT_OPENGL_ARB = 0x2010;
constexpr int WGL_DOUBLE_BUFFER_ARB  = 0x2011;
constexpr int WGL_PIXEL_TYPE_ARB     = 0x2013;
constexpr int WGL_COLOR_BITS_ARB     = 0x2014;
constexpr int WGL_DEPTH_BITS_ARB     = 0x2022;
constexpr int WGL_STENCIL_BITS_ARB   = 0x2023;
constexpr int WGL_FULL_ACCELERATION_ARB = 0x2027;
constexpr int WGL_TYPE_RGBA_ARB      = 0x202B;
constexpr int WGL_SAMPLE_BUFFERS_ARB = 0x2041;
constexpr int WGL_SAMPLES_ARB        = 0x2042;

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_FLAGS_ARB         = 0x2094;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB  = 0x9126;
constexpr int WGL_CONTEXT_DEBUG_BIT_ARB     = 0x0001;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;

// ::FLOAT and ::BYTE have to be qualified: this namespace defines GL enums
// with those exact names, and unqualified lookup finds those first.
using PFN_wglChoosePixelFormatARB =
    BOOL(__stdcall*)(HDC, const int*, const ::FLOAT*, UINT, int*, UINT*);
using PFN_wglCreateContextAttribsARB = HGLRC(__stdcall*)(HDC, HGLRC, const int*);
using PFN_wglSwapIntervalEXT = BOOL(__stdcall*)(int);

PFN_wglChoosePixelFormatARB choosePixelFormatARB = nullptr;
PFN_wglCreateContextAttribsARB createContextAttribsARB = nullptr;
PFN_wglSwapIntervalEXT swapIntervalEXT = nullptr;

HMODULE gOpenGlModule = nullptr;
std::string gVendor, gRenderer, gVersion;

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// wglGetProcAddress only knows about extensions; the 1.1 entry points live in
// opengl32.dll itself. Try both, in that order.
void* loadEntry(const char* name) {
    if (auto* p = reinterpret_cast<void*>(wglGetProcAddress(name))) {
        // Some drivers historically returned these sentinels for "not found".
        auto value = reinterpret_cast<std::intptr_t>(p);
        if (value != 0 && value != 1 && value != 2 && value != 3 && value != -1) return p;
    }
    if (!gOpenGlModule) gOpenGlModule = LoadLibraryW(L"opengl32.dll");
    if (!gOpenGlModule) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(gOpenGlModule, name));
}

bool loadFunctionTable(std::string* error) {
    std::string missing;

#define BLOCKY_GL_LOAD(ret, name, args)                                        \
    name = reinterpret_cast<PFN_##name>(loadEntry("gl" #name));                \
    if (!name) { if (!missing.empty()) missing += ", "; missing += "gl" #name; }
    BLOCKY_GL_FUNCTIONS(BLOCKY_GL_LOAD)
#undef BLOCKY_GL_LOAD

    if (!missing.empty()) {
        setError(error, "gl: could not load " + missing);
        return false;
    }
    return true;
}

// A throwaway window whose only job is to host a legacy context, because
// SetPixelFormat may be called exactly once per window and the modern format
// can only be chosen through an extension that needs a live context first.
struct BootstrapContext {
    HWND  window = nullptr;
    HDC   dc = nullptr;
    HGLRC rc = nullptr;

    ~BootstrapContext() {
        if (rc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc); }
        if (dc && window) ReleaseDC(window, dc);
        if (window) DestroyWindow(window);
    }

    bool create(std::string* error) {
        HINSTANCE instance = GetModuleHandleW(nullptr);

        static bool registered = false;
        const wchar_t* className = L"BlockyEngineGlBootstrap";
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = instance;
            wc.lpszClassName = className;
            if (!RegisterClassExW(&wc)) {
                setError(error, "gl: bootstrap class registration failed");
                return false;
            }
            registered = true;
        }

        window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32,
                                 nullptr, nullptr, instance, nullptr);
        if (!window) { setError(error, "gl: bootstrap window failed"); return false; }

        dc = GetDC(window);

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;

        int format = ChoosePixelFormat(dc, &pfd);
        if (!format || !SetPixelFormat(dc, format, &pfd)) {
            setError(error, "gl: no usable bootstrap pixel format");
            return false;
        }

        rc = wglCreateContext(dc);
        if (!rc || !wglMakeCurrent(dc, rc)) {
            setError(error, "gl: bootstrap context failed");
            return false;
        }

        choosePixelFormatARB =
            reinterpret_cast<PFN_wglChoosePixelFormatARB>(wglGetProcAddress("wglChoosePixelFormatARB"));
        createContextAttribsARB =
            reinterpret_cast<PFN_wglCreateContextAttribsARB>(wglGetProcAddress("wglCreateContextAttribsARB"));
        swapIntervalEXT =
            reinterpret_cast<PFN_wglSwapIntervalEXT>(wglGetProcAddress("wglSwapIntervalEXT"));

        if (!createContextAttribsARB) {
            setError(error, "gl: driver has no wglCreateContextAttribsARB, so no core profile");
            return false;
        }
        return true;
    }
};

void __stdcall debugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei,
                             const GLchar* message, const void*) {
    // 0x826B is NOTIFICATION: driver chatter, not worth printing.
    if (severity == 0x826B) return;
    std::printf("[gl] type=0x%X id=%u severity=0x%X: %s\n", type, id, severity, message);
}

using PFN_DebugMessageCallback = void(__stdcall*)(void*, const void*);

} // namespace

bool createContext(void* hdcRaw, const ContextSettings& settings, void** outContext,
                   std::string* error) {
    HDC hdc = static_cast<HDC>(hdcRaw);
    if (!hdc) { setError(error, "gl: no device context"); return false; }

    {
        BootstrapContext bootstrap;
        if (!bootstrap.create(error)) return false;
    }
    // The bootstrap context is gone here, but the extension pointers it gave
    // us stay valid for the process.

    int attributes[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1,
        WGL_SUPPORT_OPENGL_ARB, 1,
        WGL_DOUBLE_BUFFER_ARB,  1,
        WGL_ACCELERATION_ARB,   WGL_FULL_ACCELERATION_ARB,
        WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
        WGL_COLOR_BITS_ARB,     32,
        WGL_DEPTH_BITS_ARB,     settings.depthBits,
        WGL_STENCIL_BITS_ARB,   0,
        WGL_SAMPLE_BUFFERS_ARB, settings.samples > 0 ? 1 : 0,
        WGL_SAMPLES_ARB,        settings.samples,
        0
    };

    int format = 0;
    UINT formatCount = 0;
    bool chosen = choosePixelFormatARB &&
                  choosePixelFormatARB(hdc, attributes, nullptr, 1, &format, &formatCount) &&
                  formatCount > 0;

    if (!chosen) {
        // Fall back to a plain format: MSAA is a nicety, a context is not.
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = static_cast<::BYTE>(settings.depthBits);
        format = ChoosePixelFormat(hdc, &pfd);
        if (!format) { setError(error, "gl: no usable pixel format"); return false; }
    }

    PIXELFORMATDESCRIPTOR chosenDescriptor{};
    DescribePixelFormat(hdc, format, sizeof(chosenDescriptor), &chosenDescriptor);
    if (!SetPixelFormat(hdc, format, &chosenDescriptor)) {
        setError(error, "gl: SetPixelFormat failed");
        return false;
    }

    int contextFlags = settings.debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0;
    int contextAttributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, settings.majorVersion,
        WGL_CONTEXT_MINOR_VERSION_ARB, settings.minorVersion,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        WGL_CONTEXT_FLAGS_ARB,         contextFlags,
        0
    };

    HGLRC rc = createContextAttribsARB(hdc, nullptr, contextAttributes);
    if (!rc) {
        setError(error, "gl: driver refused a " + std::to_string(settings.majorVersion) + "." +
                            std::to_string(settings.minorVersion) + " core context");
        return false;
    }
    if (!wglMakeCurrent(hdc, rc)) {
        wglDeleteContext(rc);
        setError(error, "gl: wglMakeCurrent failed");
        return false;
    }

    if (!loadFunctionTable(error)) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(rc);
        return false;
    }

    auto readString = [](GLenum name) {
        const GLubyte* s = GetString(name);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("?");
    };
    gVendor = readString(VENDOR);
    gRenderer = readString(RENDERER);
    gVersion = readString(VERSION);

    if (settings.debug) {
        if (auto* install = reinterpret_cast<PFN_DebugMessageCallback>(loadEntry("glDebugMessageCallback"))) {
            Enable(DEBUG_OUTPUT);
            Enable(DEBUG_OUTPUT_SYNCHRONOUS);
            install(reinterpret_cast<void*>(&debugCallback), nullptr);
        }
    }

    *outContext = rc;
    return true;
}

void destroyContext(void* hdc, void* context) {
    if (!context) return;
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(static_cast<HGLRC>(context));
    (void)hdc;
}

bool makeCurrent(void* hdc, void* context) {
    return wglMakeCurrent(static_cast<HDC>(hdc), static_cast<HGLRC>(context)) != FALSE;
}

void swapBuffers(void* hdc) { SwapBuffers(static_cast<HDC>(hdc)); }

void setSwapInterval(int interval) {
    if (swapIntervalEXT) swapIntervalEXT(interval);
}

const std::string& vendorString()   { return gVendor; }
const std::string& rendererString() { return gRenderer; }
const std::string& versionString()  { return gVersion; }

} // namespace gl
} // namespace blocky
