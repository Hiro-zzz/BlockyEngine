#include "engine/render/gpu/denoise_gpu.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace blocky {
namespace gpu {
namespace {

// Transliterated from render/post/denoise.cpp. Every difference from that file
// is either forced by GLSL or is a bug.
const char* kDecls = R"GLSL(
#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;

layout(std430, binding = 0) readonly buffer ColorBuf  { vec4 colorIn[]; };
layout(std430, binding = 1) readonly buffer AlbedoBuf { vec4 albedoIn[]; };
layout(std430, binding = 2) readonly buffer NormalBuf { vec4 normalDepth[]; };
layout(std430, binding = 3)          buffer PingBuf   { vec4 ping[]; };
layout(std430, binding = 4)          buffer PongBuf   { vec4 pong[]; };

uniform ivec2 uSize;
uniform int   uStep;
uniform float uColorSigma;
uniform float uNormalSigma;
uniform float uDepthSigma;
uniform float uAlbedoSigma;
uniform float uStrength;

// Albedo is a divisor, so it is floored: demodulating by a black surface
// would send the lighting to infinity and bring it back as a white pixel.
vec3 safeAlbedo(uint i) { return max(albedoIn[i].rgb, vec3(0.02)); }

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
)GLSL";

// Demodulate: keep the lighting, set the texture aside. Dividing the render
// by the albedo leaves just the illumination, which is smooth and safe to
// blur; multiplying the texture back afterwards means the filter never had a
// chance to erase it.
const char* kPrepare = R"GLSL(
void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= uSize.x || xy.y >= uSize.y) return;
    uint i = uint(xy.y) * uint(uSize.x) + uint(xy.x);
    ping[i] = vec4(colorIn[i].rgb / safeAlbedo(i), 0.0);
}
)GLSL";

const char* kAtrous = R"GLSL(
// The B3-spline kernel the a-trous scheme is built on.
const float kKernel[5] = float[5](1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);

float depthAt(int px, int py, float centreDepth) {
    px = clamp(px, 0, uSize.x - 1);
    py = clamp(py, 0, uSize.y - 1);
    float d = normalDepth[uint(py) * uint(uSize.x) + uint(px)].w;
    return d < 0.0 ? centreDepth : d;
}

void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= uSize.x || xy.y >= uSize.y) return;
    uint centre = uint(xy.y) * uint(uSize.x) + uint(xy.x);

    float centreDepth = normalDepth[centre].w;
    if (centreDepth < 0.0) {
        // Background: nothing to filter against, leave it alone.
        pong[centre] = ping[centre];
        return;
    }

    vec3 centreColor = ping[centre].rgb;
    vec3 centreNormal = normalDepth[centre].xyz;
    vec3 centreAlbedo = albedoIn[centre].rgb;

    // Depth gradient, so a sloped surface is not mistaken for an edge.
    // Clamped, because a central difference taken across an actual edge
    // reports a nonsensical slope.
    const float maxSlope = 3.0;
    float dzdx = clamp(0.5 * (depthAt(xy.x + 1, xy.y, centreDepth) -
                              depthAt(xy.x - 1, xy.y, centreDepth)), -maxSlope, maxSlope);
    float dzdy = clamp(0.5 * (depthAt(xy.x, xy.y + 1, centreDepth) -
                              depthAt(xy.x, xy.y - 1, centreDepth)), -maxSlope, maxSlope);

    vec3 sum = vec3(0.0);
    float weightSum = 0.0;

    for (int ky = 0; ky < 5; ++ky) {
        int sy = xy.y + (ky - 2) * uStep;
        if (sy < 0 || sy >= uSize.y) continue;

        for (int kx = 0; kx < 5; ++kx) {
            int sx = xy.x + (kx - 2) * uStep;
            if (sx < 0 || sx >= uSize.x) continue;

            uint tap = uint(sy) * uint(uSize.x) + uint(sx);
            float tapDepth = normalDepth[tap].w;
            if (tapDepth < 0.0) continue;

            float normalCloseness = max(0.0, dot(centreNormal, normalDepth[tap].xyz));
            float normalWeight = pow(normalCloseness, 1.0 / max(uNormalSigma, 1e-3));

            // Depth, compared against what the gradient predicts rather than
            // against the centre, and deliberately *not* scaled by the filter
            // width: an earlier version was, and the wide iterations blurred
            // straight across a two-block terrace step.
            float expected = centreDepth + dzdx * float(sx - xy.x) + dzdy * float(sy - xy.y);
            float depthScale = uDepthSigma * max(1.0, centreDepth * 0.02);
            float depthDelta = abs(tapDepth - expected) / max(depthScale, 1e-3);
            float depthWeight = exp(-depthDelta * depthDelta);

            vec3 albedoDelta = albedoIn[tap].rgb - centreAlbedo;
            float albedoWeight = exp(-dot(albedoDelta, albedoDelta) /
                                     (uAlbedoSigma * uAlbedoSigma));

            float colorDelta = abs(luminance(ping[tap].rgb) - luminance(centreColor));
            float colorWeight = exp(-colorDelta / max(uColorSigma, 1e-3));

            float weight = kKernel[ky] * kKernel[kx] *
                           normalWeight * depthWeight * albedoWeight * colorWeight;

            sum += ping[tap].rgb * weight;
            weightSum += weight;
        }
    }

    pong[centre] = vec4(weightSum > 1e-6 ? sum / weightSum : centreColor, 0.0);
}
)GLSL";

// Remodulate and blend back towards the original.
const char* kFinish = R"GLSL(
void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (xy.x >= uSize.x || xy.y >= uSize.y) return;
    uint i = uint(xy.y) * uint(uSize.x) + uint(xy.x);
    vec3 filtered = ping[i].rgb * safeAlbedo(i);
    pong[i] = vec4(mix(colorIn[i].rgb, filtered, uStrength), 0.0);
}
)GLSL";

void setError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

gl::GLuint compile(const char* stage, const char* name, std::string* error) {
    const char* sources[] = {kDecls, stage};

    const gl::GLuint shader = gl::CreateShader(gl::COMPUTE_SHADER);
    gl::ShaderSource(shader, 2, sources, nullptr);
    gl::CompileShader(shader);

    gl::GLint ok = 0;
    gl::GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetShaderInfoLog(shader, length, nullptr, log.data());
        setError(error, std::string("denoise ") + name + ": " + log);
        gl::DeleteShader(shader);
        return 0;
    }

    const gl::GLuint program = gl::CreateProgram();
    gl::AttachShader(program, shader);
    gl::LinkProgram(program);
    gl::DeleteShader(shader);

    gl::GetProgramiv(program, gl::LINK_STATUS, &ok);
    if (!ok) {
        gl::GLint length = 0;
        gl::GetProgramiv(program, gl::INFO_LOG_LENGTH, &length);
        std::string log(size_t(length > 0 ? length : 1), '\0');
        gl::GetProgramInfoLog(program, length, nullptr, log.data());
        setError(error, std::string("denoise link ") + name + ": " + log);
        gl::DeleteProgram(program);
        return 0;
    }
    return program;
}

void makeBuffer(gl::GLuint& buffer, const void* data, size_t bytes) {
    if (buffer == 0) gl::GenBuffers(1, &buffer);
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, buffer);
    gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(bytes), data, gl::DYNAMIC_DRAW);
}

} // namespace

Denoiser::~Denoiser() { destroy(); }

bool Denoiser::build(std::string* error) {
    destroy();
    prepare_ = compile(kPrepare, "prepare", error);
    if (!prepare_) return false;
    atrous_ = compile(kAtrous, "atrous", error);
    if (!atrous_) return false;
    finish_ = compile(kFinish, "finish", error);
    return finish_ != 0;
}

bool Denoiser::resize(int width, int height) {
    if (width == width_ && height == height_) return true;
    const size_t bytes = size_t(width) * size_t(height) * 4 * sizeof(float);
    makeBuffer(colorBuf_, nullptr, bytes);
    makeBuffer(albedoBuf_, nullptr, bytes);
    makeBuffer(normalBuf_, nullptr, bytes);
    makeBuffer(pingBuf_, nullptr, bytes);
    makeBuffer(pongBuf_, nullptr, bytes);
    width_ = width;
    height_ = height;
    return true;
}

bool Denoiser::run(const RenderTargets& targets, const DenoiseSettings& settings, Image& out,
                   std::string* error) {
    if (prepare_ == 0) { setError(error, "gpu denoiser: not built"); return false; }
    if (!targets.valid() || targets.color.empty()) {
        out = targets.color;
        return true;
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();

    const int width = targets.width;
    const int height = targets.height;
    const size_t count = size_t(width) * size_t(height);
    resize(width, height);

    // Three vec3 buffers become three vec4 ones on the way over: std430 pads
    // a vec3 to sixteen bytes anyway, so packing them tighter would buy
    // nothing and cost an indexing rule to get wrong.
    std::vector<float> staging(count * 4);

    auto upload = [&](gl::GLuint buffer, const Vec3* src, const float* w) {
        for (size_t i = 0; i < count; ++i) {
            staging[i * 4 + 0] = src[i].x;
            staging[i * 4 + 1] = src[i].y;
            staging[i * 4 + 2] = src[i].z;
            staging[i * 4 + 3] = w ? w[i] : 0.0f;
        }
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, buffer);
        gl::BufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                          gl::GLsizeiptr(staging.size() * sizeof(float)), staging.data());
    };

    upload(colorBuf_, targets.color.data(), nullptr);
    upload(albedoBuf_, targets.albedo.data(), nullptr);
    upload(normalBuf_, targets.normal.data(), targets.depth.data());

    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 0, colorBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 1, albedoBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 2, normalBuf_);

    const gl::GLuint groupsX = gl::GLuint((width + 7) / 8);
    const gl::GLuint groupsY = gl::GLuint((height + 7) / 8);

    auto setCommon = [&](gl::GLuint p) {
        gl::UseProgram(p);
        gl::Uniform2i(gl::GetUniformLocation(p, "uSize"), width, height);
        gl::Uniform1f(gl::GetUniformLocation(p, "uNormalSigma"), settings.normalSigma);
        gl::Uniform1f(gl::GetUniformLocation(p, "uDepthSigma"), settings.depthSigma);
        gl::Uniform1f(gl::GetUniformLocation(p, "uAlbedoSigma"), settings.albedoSigma);
        gl::Uniform1f(gl::GetUniformLocation(p, "uStrength"), saturate(settings.strength));
    };

    gl::Finish();
    const auto uploaded = Clock::now();

    // ---- demodulate into ping
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 3, pingBuf_);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 4, pongBuf_);
    setCommon(prepare_);
    gl::DispatchCompute(groupsX, groupsY, 1);
    gl::MemoryBarrier(gl::SHADER_STORAGE_BARRIER_BIT);

    // ---- the passes, each one twice as wide as the last
    gl::GLuint ping = pingBuf_, pong = pongBuf_;
    for (int iteration = 0; iteration < settings.iterations; ++iteration) {
        const int step = 1 << iteration;

        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 3, ping);
        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 4, pong);

        setCommon(atrous_);
        gl::Uniform1i(gl::GetUniformLocation(atrous_, "uStep"), step);
        // The colour tolerance loosens as the filter widens, which is what
        // stops the later passes from re-introducing blotches.
        gl::Uniform1f(gl::GetUniformLocation(atrous_, "uColorSigma"),
                      settings.colorSigma * float(step));

        gl::DispatchCompute(groupsX, groupsY, 1);
        gl::MemoryBarrier(gl::SHADER_STORAGE_BARRIER_BIT);

        const gl::GLuint tmp = ping;
        ping = pong;
        pong = tmp;
    }

    // ---- remodulate, and blend back towards the original
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 3, ping);
    gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 4, pong);
    setCommon(finish_);
    gl::DispatchCompute(groupsX, groupsY, 1);
    gl::MemoryBarrier(gl::SHADER_STORAGE_BARRIER_BIT | gl::BUFFER_UPDATE_BARRIER_BIT);
    gl::Finish();

    const auto filtered = Clock::now();

    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, pong);
    gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0,
                         gl::GLsizeiptr(staging.size() * sizeof(float)), staging.data());

    out = Image(width, height);
    for (size_t i = 0; i < count; ++i) {
        out.data()[i] = Vec3{staging[i * 4], staging[i * 4 + 1], staging[i * 4 + 2]};
    }

    const auto done = Clock::now();
    lastUpload_ = std::chrono::duration<double>(uploaded - started).count();
    lastFilter_ = std::chrono::duration<double>(filtered - uploaded).count();
    lastReadback_ = std::chrono::duration<double>(done - filtered).count();

    const gl::GLenum err = gl::GetError();
    if (err != gl::NO_ERROR_) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "gpu denoiser: GL error 0x%04X", unsigned(err));
        setError(error, buf);
        return false;
    }
    return true;
}

void Denoiser::destroy() {
    const gl::GLuint programs[] = {prepare_, atrous_, finish_};
    for (gl::GLuint p : programs) {
        if (p) gl::DeleteProgram(p);
    }
    prepare_ = atrous_ = finish_ = 0;

    const gl::GLuint buffers[] = {colorBuf_, albedoBuf_, normalBuf_, pingBuf_, pongBuf_};
    for (gl::GLuint b : buffers) {
        if (b) gl::DeleteBuffers(1, &b);
    }
    colorBuf_ = albedoBuf_ = normalBuf_ = pingBuf_ = pongBuf_ = 0;
    width_ = height_ = 0;
}

} // namespace gpu
} // namespace blocky
