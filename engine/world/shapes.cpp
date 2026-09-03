#include "engine/world/shapes.hpp"

namespace blocky {
namespace shape {
namespace {

// Bounding box of a shape, expanded by one block so nothing is clipped by a
// rounding error at the edge.
IVec3 floorBound(Vec3 v) { return floorToInt(v - Vec3{1.0f}); }
IVec3 ceilBound(Vec3 v)  { return floorToInt(v + Vec3{1.0f}); }

// An orthonormal frame whose +Z is `axis`, for the shapes defined around one.
struct AxisFrame {
    Vec3 axis, tangent, bitangent;

    explicit AxisFrame(Vec3 direction) {
        axis = normalize(direction);
        if (lengthSq(axis) < 0.5f) axis = Vec3{0.0f, 1.0f, 0.0f};
        orthonormalBasis(axis, tangent, bitangent);
    }

    // Distance along the axis, and perpendicular distance from it.
    void decompose(Vec3 offset, float& along, float& radial) const {
        along = dot(offset, axis);
        Vec3 perpendicular = offset - axis * along;
        radial = length(perpendicular);
    }
};

} // namespace

void ellipsoid(World& world, Vec3 centre, Vec3 radii, BlockId id, bool hollow) {
    Vec3 safe = maxv(radii, Vec3{1e-3f});
    IVec3 lo = floorBound(centre - safe);
    IVec3 hi = ceilBound(centre + safe);

    fillWhere(world, lo, hi, id, [&](Vec3 p) {
        Vec3 d = (p - centre) / safe;
        float outer = lengthSq(d);
        if (outer > 1.0f) return false;
        if (!hollow) return true;

        // One block of wall: keep the point only if stepping inwards by a
        // block would leave the ellipsoid.
        Vec3 inner = (p - centre) / maxv(safe - Vec3{1.0f}, Vec3{1e-3f});
        return lengthSq(inner) > 1.0f;
    });
}

void cylinder(World& world, Vec3 base, Vec3 axis, float radius, float height, BlockId id) {
    AxisFrame frame(axis);
    Vec3 tip = base + frame.axis * height;

    Vec3 extent{radius + 1.0f, radius + 1.0f, radius + 1.0f};
    IVec3 lo = floorBound(minv(base, tip) - extent);
    IVec3 hi = ceilBound(maxv(base, tip) + extent);

    fillWhere(world, lo, hi, id, [&](Vec3 p) {
        float along = 0.0f, radial = 0.0f;
        frame.decompose(p - base, along, radial);
        return along >= 0.0f && along <= height && radial <= radius;
    });
}

void cone(World& world, Vec3 base, Vec3 axis, float baseRadius, float topRadius, float height,
          BlockId id) {
    AxisFrame frame(axis);
    Vec3 tip = base + frame.axis * height;

    float widest = std::max(baseRadius, topRadius) + 1.0f;
    Vec3 extent{widest, widest, widest};
    IVec3 lo = floorBound(minv(base, tip) - extent);
    IVec3 hi = ceilBound(maxv(base, tip) + extent);

    fillWhere(world, lo, hi, id, [&](Vec3 p) {
        float along = 0.0f, radial = 0.0f;
        frame.decompose(p - base, along, radial);
        if (along < 0.0f || along > height) return false;

        float t = height > 0.0f ? along / height : 0.0f;
        return radial <= lerp(baseRadius, topRadius, t);
    });
}

void line(World& world, Vec3 from, Vec3 to, float radius, BlockId id) {
    Vec3 delta = to - from;
    float span = length(delta);
    if (span < 1e-4f) {
        ellipsoid(world, from, Vec3{radius}, id);
        return;
    }
    Vec3 direction = delta / span;

    Vec3 extent{radius + 1.0f, radius + 1.0f, radius + 1.0f};
    IVec3 lo = floorBound(minv(from, to) - extent);
    IVec3 hi = ceilBound(maxv(from, to) + extent);

    fillWhere(world, lo, hi, id, [&](Vec3 p) {
        // Distance to the segment, clamped at both ends so the line has caps.
        float t = saturate(dot(p - from, direction) / span);
        Vec3 closest = from + direction * (t * span);
        return lengthSq(p - closest) <= radius * radius;
    });
}

void torus(World& world, Vec3 centre, Vec3 axis, float majorRadius, float minorRadius, BlockId id) {
    AxisFrame frame(axis);

    float reach = majorRadius + minorRadius + 1.0f;
    IVec3 lo = floorBound(centre - Vec3{reach});
    IVec3 hi = ceilBound(centre + Vec3{reach});

    fillWhere(world, lo, hi, id, [&](Vec3 p) {
        float along = 0.0f, radial = 0.0f;
        frame.decompose(p - centre, along, radial);
        // Distance to the ring of radius majorRadius lying in the plane.
        float dr = radial - majorRadius;
        return dr * dr + along * along <= minorRadius * minorRadius;
    });
}

void disc(World& world, Vec3 centre, Vec3 axis, float radius, BlockId id) {
    AxisFrame frame(axis);

    float reach = radius + 1.0f;
    IVec3 lo = floorBound(centre - Vec3{reach});
    IVec3 hi = ceilBound(centre + Vec3{reach});

    fillWhere(world, lo, hi, id, [&](Vec3 p) {
        float along = 0.0f, radial = 0.0f;
        frame.decompose(p - centre, along, radial);
        return std::fabs(along) <= 0.5f && radial <= radius;
    });
}

void replace(World& world, IVec3 minCorner, IVec3 maxCorner, BlockId from, BlockId to) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);

    for (int y = lo.y; y <= hi.y; ++y) {
        for (int z = lo.z; z <= hi.z; ++z) {
            for (int x = lo.x; x <= hi.x; ++x) {
                IVec3 p{x, y, z};
                if (world.get(p) == from) world.set(p, to);
            }
        }
    }
}

void erode(World& world, IVec3 minCorner, IVec3 maxCorner, int minNeighbours, int passes) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);

    for (int pass = 0; pass < passes; ++pass) {
        // Decide against a snapshot, then apply: eroding in place would let
        // one removal cascade through the rest of the pass.
        std::vector<IVec3> doomed;

        for (int y = lo.y; y <= hi.y; ++y) {
            for (int z = lo.z; z <= hi.z; ++z) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    IVec3 p{x, y, z};
                    if (world.get(p) == block::Air) continue;

                    int neighbours = 0;
                    neighbours += world.get(p + IVec3{ 1, 0, 0}) != block::Air;
                    neighbours += world.get(p + IVec3{-1, 0, 0}) != block::Air;
                    neighbours += world.get(p + IVec3{ 0, 1, 0}) != block::Air;
                    neighbours += world.get(p + IVec3{ 0,-1, 0}) != block::Air;
                    neighbours += world.get(p + IVec3{ 0, 0, 1}) != block::Air;
                    neighbours += world.get(p + IVec3{ 0, 0,-1}) != block::Air;

                    if (neighbours < minNeighbours) doomed.push_back(p);
                }
            }
        }
        for (IVec3 p : doomed) world.set(p, block::Air);
        if (doomed.empty()) break;
    }
}

Clipboard copy(const World& world, IVec3 minCorner, IVec3 maxCorner) {
    IVec3 lo = minv(minCorner, maxCorner);
    IVec3 hi = maxv(minCorner, maxCorner);

    Clipboard clip;
    clip.size = hi - lo + IVec3{1, 1, 1};
    clip.blocks.assign(size_t(clip.size.x) * size_t(clip.size.y) * size_t(clip.size.z), block::Air);

    for (int y = 0; y < clip.size.y; ++y) {
        for (int z = 0; z < clip.size.z; ++z) {
            for (int x = 0; x < clip.size.x; ++x) {
                clip.set(x, y, z, world.get(lo + IVec3{x, y, z}));
            }
        }
    }
    return clip;
}

void paste(World& world, const Clipboard& clip, IVec3 at, bool skipAir) {
    for (int y = 0; y < clip.size.y; ++y) {
        for (int z = 0; z < clip.size.z; ++z) {
            for (int x = 0; x < clip.size.x; ++x) {
                BlockId id = clip.at(x, y, z);
                if (skipAir && id == block::Air) continue;
                world.set(at + IVec3{x, y, z}, id);
            }
        }
    }
}

Clipboard mirrored(const Clipboard& clip, int axis) {
    Clipboard out;
    out.size = clip.size;
    out.blocks.assign(clip.blocks.size(), block::Air);

    for (int y = 0; y < clip.size.y; ++y) {
        for (int z = 0; z < clip.size.z; ++z) {
            for (int x = 0; x < clip.size.x; ++x) {
                int sx = axis == 0 ? clip.size.x - 1 - x : x;
                int sy = axis == 1 ? clip.size.y - 1 - y : y;
                int sz = axis == 2 ? clip.size.z - 1 - z : z;
                out.set(x, y, z, clip.at(sx, sy, sz));
            }
        }
    }
    return out;
}

Clipboard rotatedY(const Clipboard& clip, int quarterTurns) {
    int turns = ((quarterTurns % 4) + 4) % 4;
    if (turns == 0) return clip;

    Clipboard out;
    // A quarter or three-quarter turn swaps the footprint.
    out.size = (turns % 2 == 1) ? IVec3{clip.size.z, clip.size.y, clip.size.x} : clip.size;
    out.blocks.assign(size_t(out.size.x) * size_t(out.size.y) * size_t(out.size.z), block::Air);

    for (int y = 0; y < clip.size.y; ++y) {
        for (int z = 0; z < clip.size.z; ++z) {
            for (int x = 0; x < clip.size.x; ++x) {
                int nx = x, nz = z;
                for (int t = 0; t < turns; ++t) {
                    // One clockwise step seen from above.
                    int px = nx, pz = nz;
                    int width = (t % 2 == 0) ? clip.size.z : clip.size.x;
                    nx = width - 1 - pz;
                    nz = px;
                }
                out.set(nx, y, nz, clip.at(x, y, z));
            }
        }
    }
    return out;
}

SurfacePoint findSurface(const World& world, int x, int z, int topY, int bottomY) {
    SurfacePoint result;
    for (int y = topY; y >= bottomY; --y) {
        BlockId id = world.get({x, y, z});
        if (id == block::Air) continue;
        result.block = {x, y, z};
        result.id = id;
        result.found = true;
        return result;
    }
    return result;
}

int surfaceRoughness(const World& world, int x, int z, int topY, int radius) {
    int lowest = 1 << 30, highest = -(1 << 30);

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            SurfacePoint point = findSurface(world, x + dx, z + dz, topY);
            if (!point.found) return 1 << 20;  // a hole counts as maximally rough
            lowest = std::min(lowest, point.block.y);
            highest = std::max(highest, point.block.y);
        }
    }
    return highest - lowest;
}

} // namespace shape
} // namespace blocky
