#include "plugins/forge/sculptview.hpp"

#include "engine/platform/window.hpp"

#include "plugins/forge/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge {

Mat4 modelToWorld(IVec3 dims, float voxelSize) {
    // Lifted by one voxel, which is the plate's thickness: the model stands
    // on the plate and the plate stands on the floor. Standing both on y = 0
    // puts the plate's top face exactly on the floor block's top face, and
    // two coplanar faces are a z-fight -- it shows up as the plate boiling
    // into the tiles as the camera moves, which looks like a renderer bug
    // rather than a placement one.
    const Vec3 offset{-0.5f * float(dims.x) * voxelSize, voxelSize,
                      -0.5f * float(dims.z) * voxelSize};
    return translate(offset) * scale(Vec3{voxelSize, voxelSize, voxelSize});
}

void buildPlate(VoxelModel& plate, IVec3 dims) {
    plate.resize({std::max(1, dims.x), 1, std::max(1, dims.z)});

    VoxelMaterial amber;
    amber.albedo = srgbToLinear(Vec3{0.55f, 0.38f, 0.13f});
    const uint16_t slot = plate.addMaterial(amber);

    for (int z = 0; z < plate.dims().z; ++z)
        for (int x = 0; x < plate.dims().x; ++x) plate.set({x, 0, z}, slot);
}

Mat4 plateToWorld(IVec3 dims, float voxelSize) {
    // One voxel below the model, which after the lift above puts it flat on
    // the floor. Derived from the model's own transform rather than written
    // out again, so the two cannot drift apart.
    return translate(Vec3{0.0f, -voxelSize, 0.0f}) *
           modelToWorld({dims.x, 1, dims.z}, voxelSize);
}

void Orbit::frame(const edit::Sculpt& sculpt, float voxelSize) {
    const IVec3 dims = sculpt.dims();
    const float footprint = float(std::max(dims.x, dims.z)) * voxelSize;

    if (sculpt.model().empty()) {
        // Nothing to look at but the plate, so look at the plate.
        height = voxelSize * 1.5f;
        distance = std::max(0.6f, 1.9f * footprint);
        return;
    }

    const IVec3 lo = sculpt.model().minVoxel();
    const IVec3 hi = sculpt.model().maxVoxel();

    const float tall = float(hi.y - lo.y + 1) * voxelSize;
    const float wide = float(std::max(hi.x - lo.x, hi.z - lo.z) + 1) * voxelSize;

    // The middle of the occupied box, plus the plate underneath it.
    height = voxelSize + float(lo.y + hi.y + 1) * 0.5f * voxelSize;

    // Three times rather than two, because the palette band covers the bottom
    // of the window: a model framed to fill the frame is framed to sit behind
    // the interface.
    distance = std::max(0.6f, 3.0f * std::max(tall, wide));
}

void Orbit::apply(Camera& camera, float aspect) const {
    const float yaw = radians(yawDegrees);
    const float pitch = radians(pitchDegrees);

    const Vec3 target{0.0f, height, 0.0f};
    const Vec3 eye{target.x + distance * std::cos(pitch) * std::sin(yaw),
                   target.y + distance * std::sin(pitch),
                   target.z + distance * std::cos(pitch) * std::cos(yaw)};

    camera.lookAt(eye, target);
    camera.projection = Camera::Projection::Perspective;
    camera.fovY = radians(45.0f);
    camera.aspect = aspect;
}

namespace {

// The base plane, for the case the model has nothing in it yet. A ray that
// misses every voxel still has to land somewhere a first voxel can go.
bool hitFloorPlane(Vec3 origin, Vec3 direction, IVec3 dims, IVec3& cell) {
    if (direction.y > -1e-6f) return false;   // parallel, or looking up from below

    const float t = -origin.y / direction.y;
    if (t <= 0.0f) return false;

    const Vec3 at = origin + direction * t;
    const int x = int(std::floor(at.x));
    const int z = int(std::floor(at.z));
    if (x < 0 || z < 0 || x >= dims.x || z >= dims.z) return false;

    cell = {x, 0, z};
    return true;
}

} // namespace

void SculptView::update(Studio& studio, const Window& window, Orbit& orbit,
                        const Camera& camera, int width, int height, bool pointerOverUi) {
    // Orbit on the right button. The left one belongs to the tools, and the
    // middle one is not something `Window` reports -- which is the honest
    // reason rather than a taste in bindings.
    if (window.mouseRightDown()) {
        const Vec2 delta = window.mouseDelta();
        orbit.yawDegrees -= delta.x * 0.35f;
        orbit.pitchDegrees = std::min(88.0f, std::max(-88.0f, orbit.pitchDegrees + delta.y * 0.35f));
    }

    const float wheel = window.wheelDelta();
    if (wheel != 0.0f) {
        // Multiplicative, so a notch moves the same *fraction* of the way in
        // whether the camera is close or far. Additive zoom crawls when far
        // and slams into the model when near.
        orbit.distance = std::min(12.0f, std::max(0.35f, orbit.distance * std::pow(0.88f, wheel)));
    }

    if (window.keyDown(key::Shift) && window.keyDown('W')) orbit.height += 0.02f;
    if (window.keyDown(key::Shift) && window.keyDown('S')) orbit.height -= 0.02f;

    // ------------------------------------------------------------- pointing
    hover = Hover{};
    if (pointerOverUi || width <= 0 || height <= 0) {
        painting = erasing = false;
        return;
    }

    const Vec2 cursor = window.mousePosition();
    const float u = cursor.x / float(width);
    const float v = cursor.y / float(height);
    if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
        painting = erasing = false;
        return;
    }

    const Ray ray = camera.generateRay(u, v);
    const Mat4 toLocal = inverseAffine(modelToWorld(studio.sculpt.dims()));

    // Both are transformed by the same matrix, which is what keeps the ray
    // parameter meaning the same thing on both sides of it -- the reason
    // `prop/` insists the scale be uniform.
    const Vec3 origin = transformPoint(toLocal, ray.origin);
    const Vec3 direction = transformDir(toLocal, ray.direction);

    const edit::Pick pick = studio.sculpt.pick(origin, direction);
    if (pick.hit) {
        hover.valid = true;
        hover.place = pick.adjacent;
        hover.erase = pick.voxel;
    } else {
        IVec3 cell{};
        if (hitFloorPlane(origin, direction, studio.sculpt.dims(), cell)) {
            hover.valid = true;
            hover.onFloor = true;
            hover.place = cell;
            hover.erase = cell;
        }
    }

    // ---------------------------------------------------------------- tools
    const bool erase = window.keyDown(key::Shift);

    if (window.mouseLeftDown()) {
        if (!painting && !erasing) {
            studio.sculpt.beginStroke(erase ? "erase" : "place");
            painting = !erase;
            erasing = erase;
        }
        if (hover.valid) {
            if (erasing) {
                if (!hover.onFloor) studio.sculpt.erase(hover.erase);
            } else {
                studio.sculpt.place(hover.place, studio.voxelMaterial());
            }
        }
    } else if (painting || erasing) {
        studio.sculpt.endStroke();
        painting = erasing = false;
    }
}

void SculptView::draw(const Studio& studio, Overlay& overlay) const {
    const IVec3 dims = studio.sculpt.dims();

    char line[192];
    std::snprintf(line, sizeof(line), "MODEL  %dx%dx%d   %zu voxels   %zu materials", dims.x,
                  dims.y, dims.z, studio.sculpt.model().solidCount(),
                  studio.sculpt.model().materialCount());
    overlay.textShadowed(16.0f, 44.0f, line, 2.0f, rgb8(226, 230, 236));

    if (hover.valid) {
        std::snprintf(line, sizeof(line), hover.onFloor ? "floor  %d, %d, %d" : "at  %d, %d, %d",
                      hover.erase.x, hover.erase.y, hover.erase.z);
    } else {
        std::snprintf(line, sizeof(line), "%s", "pointing at nothing");
    }
    overlay.textShadowed(16.0f, 68.0f, line, 2.0f, rgb8(150, 158, 170));

    const char* help =
        "LMB place   Shift+LMB erase   RMB orbit   wheel zoom   M mirror   T trim";
    overlay.textShadowed(16.0f, helpLineY(float(overlay.height())), help, 2.0f,
                         rgb8(132, 140, 152));

    if (studio.sculpt.mirrorX) {
        overlay.textShadowed(float(overlay.width()) - 132.0f, 44.0f, "MIRROR X", 2.0f,
                             rgb8(255, 170, 40));
    }
}

} // namespace forge
