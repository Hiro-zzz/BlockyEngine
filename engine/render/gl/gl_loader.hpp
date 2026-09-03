#pragma once
// OpenGL 4.6 core, loaded by hand.
//
// Windows ships an OpenGL 1.1 header and an import library to match; anything
// newer has to be fetched at runtime through wglGetProcAddress. Rather than
// mix a 1.1 header with runtime pointers, every function used here is loaded
// into this table -- including the 1.1 ones -- so there is exactly one way to
// call GL and no chance of a stale prototype.
//
// Functions live in blocky::gl without their "gl" prefix: gl::Clear,
// gl::DrawArrays, gl::UseProgram.
#include <cstdint>
#include <string>

namespace blocky {
namespace gl {

// ------------------------------------------------------------------- types
using GLenum     = unsigned int;
using GLboolean  = unsigned char;
using GLbitfield = unsigned int;
using GLbyte     = signed char;
using GLubyte    = unsigned char;
using GLshort    = short;
using GLushort   = unsigned short;
using GLint      = int;
using GLuint     = unsigned int;
using GLsizei    = int;
using GLfloat    = float;
using GLdouble   = double;
using GLchar     = char;
using GLintptr   = std::intptr_t;
using GLsizeiptr = std::intptr_t;

// ---------------------------------------------------------------- constants
inline constexpr GLbitfield COLOR_BUFFER_BIT = 0x4000;
inline constexpr GLbitfield DEPTH_BUFFER_BIT = 0x0100;

inline constexpr GLenum DEPTH_TEST = 0x0B71;
inline constexpr GLenum CULL_FACE  = 0x0B44;
inline constexpr GLenum BLEND      = 0x0BE2;
inline constexpr GLenum MULTISAMPLE = 0x809D;
inline constexpr GLenum FRAMEBUFFER_SRGB = 0x8DB9;
inline constexpr GLenum DEBUG_OUTPUT = 0x92E0;
inline constexpr GLenum DEBUG_OUTPUT_SYNCHRONOUS = 0x8242;

inline constexpr GLenum LESS   = 0x0201;
inline constexpr GLenum LEQUAL = 0x0203;
inline constexpr GLenum BACK   = 0x0405;
inline constexpr GLenum FRONT  = 0x0404;
inline constexpr GLenum CCW    = 0x0901;
inline constexpr GLenum CW     = 0x0900;

inline constexpr GLenum SRC_ALPHA = 0x0302;
inline constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;

inline constexpr GLenum TRIANGLES = 0x0004;

inline constexpr GLenum ARRAY_BUFFER         = 0x8892;
inline constexpr GLenum ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum STATIC_DRAW          = 0x88E4;

inline constexpr GLenum BYTE           = 0x1400;
inline constexpr GLenum UNSIGNED_BYTE  = 0x1401;
inline constexpr GLenum SHORT          = 0x1402;
inline constexpr GLenum UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum INT            = 0x1404;
inline constexpr GLenum UNSIGNED_INT   = 0x1405;
inline constexpr GLenum FLOAT          = 0x1406;

inline constexpr GLenum VERTEX_SHADER   = 0x8B31;
inline constexpr GLenum FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum COMPILE_STATUS  = 0x8B81;
inline constexpr GLenum LINK_STATUS     = 0x8B82;
inline constexpr GLenum INFO_LOG_LENGTH = 0x8B84;

inline constexpr GLenum TEXTURE_2D       = 0x0DE1;
inline constexpr GLenum TEXTURE_2D_ARRAY = 0x8C1A;
inline constexpr GLenum TEXTURE0         = 0x84C0;
inline constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum TEXTURE_WRAP_R = 0x8072;
inline constexpr GLenum NEAREST = 0x2600;
inline constexpr GLenum LINEAR  = 0x2601;
inline constexpr GLenum NEAREST_MIPMAP_LINEAR = 0x2702;
inline constexpr GLenum CLAMP_TO_EDGE = 0x812F;

inline constexpr GLenum RGBA           = 0x1908;
inline constexpr GLenum RGBA8          = 0x8058;
inline constexpr GLenum RGBA16F        = 0x881A;
inline constexpr GLenum RGBA32F        = 0x8814;
inline constexpr GLenum SRGB8_ALPHA8   = 0x8C43;

// ------------------------------------------------------------ framebuffers
// Rendering somewhere other than the window. Everything the viewport does
// between the world and the screen -- outlines, bloom, the tone curve -- needs
// the frame as a texture first, and a texture is what these attach.
inline constexpr GLenum FRAMEBUFFER          = 0x8D40;
inline constexpr GLenum READ_FRAMEBUFFER     = 0x8CA8;
inline constexpr GLenum DRAW_FRAMEBUFFER     = 0x8CA9;
inline constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum COLOR_ATTACHMENT0    = 0x8CE0;
inline constexpr GLenum COLOR_ATTACHMENT1    = 0x8CE1;
inline constexpr GLenum DEPTH_ATTACHMENT     = 0x8D00;
inline constexpr GLenum DEPTH_COMPONENT      = 0x1902;
inline constexpr GLenum DEPTH_COMPONENT24    = 0x81A6;
inline constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum PACK_ALIGNMENT   = 0x0D05;

inline constexpr GLenum VENDOR   = 0x1F00;
inline constexpr GLenum RENDERER = 0x1F01;
inline constexpr GLenum VERSION  = 0x1F02;
inline constexpr GLenum NO_ERROR_ = 0;

// ------------------------------------------------------------------ compute
// Compute shaders are core since 4.3 and shader storage buffers since 4.3, so
// the 4.6 context the viewport already creates carries both. That is the
// whole reason a GPU tracer costs no new dependency here: no Vulkan, no CUDA,
// nothing to fetch -- the context is already open.
inline constexpr GLenum COMPUTE_SHADER        = 0x91B9;
inline constexpr GLenum SHADER_STORAGE_BUFFER = 0x90D2;

inline constexpr GLbitfield SHADER_STORAGE_BARRIER_BIT = 0x00002000;
inline constexpr GLbitfield BUFFER_UPDATE_BARRIER_BIT  = 0x00000200;

inline constexpr GLenum DYNAMIC_DRAW = 0x88E8;
inline constexpr GLenum DYNAMIC_READ = 0x88E9;
inline constexpr GLenum STATIC_READ  = 0x88E5;

inline constexpr GLenum MAX_COMPUTE_WORK_GROUP_INVOCATIONS = 0x90EB;
inline constexpr GLenum MAX_COMPUTE_SHADER_STORAGE_BLOCKS = 0x90DB;

// Indirect dispatch: the work group count comes out of a buffer instead of
// out of the call. That is what lets a stage be sized by a number the GPU
// itself just computed, without the host stalling to read it back.
inline constexpr GLenum DISPATCH_INDIRECT_BUFFER = 0x90EE;
inline constexpr GLbitfield COMMAND_BARRIER_BIT  = 0x00000040;

// ------------------------------------------------------- function pointers
// One X-macro list, expanded into typedefs, declarations and the loader.
#define BLOCKY_GL_FUNCTIONS(X)                                                                    \
  X(void,   Viewport,     (GLint x, GLint y, GLsizei w, GLsizei h))                               \
  X(void,   ClearColor,   (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                           \
  X(void,   Clear,        (GLbitfield mask))                                                      \
  X(void,   Enable,       (GLenum cap))                                                           \
  X(void,   Disable,      (GLenum cap))                                                           \
  X(void,   DepthFunc,    (GLenum func))                                                          \
  X(void,   CullFace,     (GLenum mode))                                                          \
  X(void,   FrontFace,    (GLenum mode))                                                          \
  X(void,   BlendFunc,    (GLenum src, GLenum dst))                                               \
  X(void,   PixelStorei,  (GLenum name, GLint param))                                             \
  X(void,   ReadPixels,   (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* p)) \
  X(void,   Finish,       ())                                                                     \
  X(GLenum, GetError,     ())                                                                     \
  X(const GLubyte*, GetString, (GLenum name))                                                     \
  X(void,   DrawArrays,   (GLenum mode, GLint first, GLsizei count))                              \
  X(void,   DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))         \
  X(void,   GenTextures,  (GLsizei n, GLuint* textures))                                          \
  X(void,   DeleteTextures, (GLsizei n, const GLuint* textures))                                  \
  X(void,   BindTexture,  (GLenum target, GLuint texture))                                        \
  X(void,   TexParameteri, (GLenum target, GLenum name, GLint param))                             \
  X(void,   TexImage2D,   (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,      \
                           GLint border, GLenum fmt, GLenum type, const void* pixels))            \
  X(void,   GenBuffers,   (GLsizei n, GLuint* buffers))                                           \
  X(void,   DeleteBuffers, (GLsizei n, const GLuint* buffers))                                    \
  X(void,   BindBuffer,   (GLenum target, GLuint buffer))                                         \
  X(void,   BufferData,   (GLenum target, GLsizeiptr size, const void* data, GLenum usage))       \
  X(void,   GenVertexArrays, (GLsizei n, GLuint* arrays))                                         \
  X(void,   DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                \
  X(void,   BindVertexArray, (GLuint array))                                                      \
  X(void,   EnableVertexAttribArray, (GLuint index))                                              \
  X(void,   VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized,    \
                                  GLsizei stride, const void* offset))                            \
  X(void,   VertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride,         \
                                   const void* offset))                                           \
  X(GLuint, CreateShader, (GLenum type))                                                          \
  X(void,   ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* strings,            \
                           const GLint* lengths))                                                 \
  X(void,   CompileShader, (GLuint shader))                                                       \
  X(void,   GetShaderiv,  (GLuint shader, GLenum name, GLint* params))                            \
  X(void,   GetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei* len, GLchar* log))            \
  X(void,   DeleteShader, (GLuint shader))                                                        \
  X(GLuint, CreateProgram, ())                                                                    \
  X(void,   AttachShader, (GLuint program, GLuint shader))                                        \
  X(void,   LinkProgram,  (GLuint program))                                                       \
  X(void,   GetProgramiv, (GLuint program, GLenum name, GLint* params))                           \
  X(void,   GetProgramInfoLog, (GLuint program, GLsizei max, GLsizei* len, GLchar* log))          \
  X(void,   UseProgram,   (GLuint program))                                                       \
  X(void,   DeleteProgram, (GLuint program))                                                      \
  X(GLint,  GetUniformLocation, (GLuint program, const GLchar* name))                             \
  X(void,   Uniform1i,    (GLint location, GLint v0))                                             \
  X(void,   Uniform1f,    (GLint location, GLfloat v0))                                           \
  X(void,   Uniform3fv,   (GLint location, GLsizei count, const GLfloat* value))                  \
  X(void,   UniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose,                \
                               const GLfloat* value))                                             \
  X(void,   ActiveTexture, (GLenum texture))                                                      \
  X(void,   TexImage3D,   (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,      \
                           GLsizei depth, GLint border, GLenum fmt, GLenum type,                  \
                           const void* pixels))                                                   \
  X(void,   TexSubImage3D, (GLenum target, GLint level, GLint xoff, GLint yoff, GLint zoff,       \
                            GLsizei w, GLsizei h, GLsizei depth, GLenum fmt, GLenum type,         \
                            const void* pixels))                                                  \
  X(void,   GenerateMipmap, (GLenum target))                                                      \
  X(void,   GetIntegerv,  (GLenum name, GLint* data))                                             \
  X(void,   Uniform1ui,   (GLint location, GLuint v0))                                            \
  X(void,   Uniform2i,    (GLint location, GLint v0, GLint v1))                                   \
  X(void,   Uniform3i,    (GLint location, GLint v0, GLint v1, GLint v2))                         \
  X(void,   BindBufferBase, (GLenum target, GLuint index, GLuint buffer))                         \
  X(void,   BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data))   \
  X(void,   GetBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, void* data))      \
  X(void,   DispatchCompute, (GLuint x, GLuint y, GLuint z))                                      \
  X(void,   DispatchComputeIndirect, (GLintptr indirect))                                         \
  X(void,   MemoryBarrier, (GLbitfield barriers))                                                 \
  X(void,   GenFramebuffers, (GLsizei n, GLuint* framebuffers))                                   \
  X(void,   DeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                          \
  X(void,   BindFramebuffer, (GLenum target, GLuint framebuffer))                                 \
  X(void,   FramebufferTexture2D,                                                                 \
            (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))    \
  X(GLenum, CheckFramebufferStatus, (GLenum target))                                              \
  X(void,   DrawBuffers,  (GLsizei n, const GLenum* bufs))

#define BLOCKY_GL_DECLARE(ret, name, args) \
  using PFN_##name = ret(__stdcall*) args; \
  extern PFN_##name name;
BLOCKY_GL_FUNCTIONS(BLOCKY_GL_DECLARE)
#undef BLOCKY_GL_DECLARE

// ------------------------------------------------------------------ context
struct ContextSettings {
    int  depthBits = 24;
    int  samples = 4;      // MSAA; 0 disables
    bool debug = false;    // request a debug context and install a callback
    int  majorVersion = 4;
    int  minorVersion = 6;
};

// Creates a core-profile context on the window's device context and loads the
// function table. `hdc` and the returned handle are opaque Win32 types.
bool createContext(void* hdc, const ContextSettings& settings, void** outContext,
                   std::string* error = nullptr);

void destroyContext(void* hdc, void* context);
bool makeCurrent(void* hdc, void* context);
void swapBuffers(void* hdc);

// 0 = no vsync, 1 = wait for one refresh. Silently ignored if unsupported.
void setSwapInterval(int interval);

// Populated by createContext, for logging.
const std::string& vendorString();
const std::string& rendererString();
const std::string& versionString();

} // namespace gl
} // namespace blocky
