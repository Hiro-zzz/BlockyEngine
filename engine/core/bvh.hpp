#pragma once
// A bounding volume hierarchy over axis-aligned boxes, deliberately ignorant
// of what it contains.
//
// The voxel world needs no such thing -- its chunk grid already is an
// acceleration structure, and the two-level DDA rides on it. Loose geometry
// is the opposite case: sprites scattered through open air sit on no grid at
// all, and a linear scan over ten thousand of them costs far more than the
// ray it is answering.
//
// It stores only boxes and indices. What primitive an index refers to, and
// how a ray meets it, is entirely the caller's business -- which is why the
// same tree can serve sprites now and entity boxes later.
#include "engine/core/math.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace blocky {

struct Aabb {
    static constexpr float kBig = std::numeric_limits<float>::infinity();

    Vec3 lo{kBig, kBig, kBig};
    Vec3 hi{-kBig, -kBig, -kBig};

    void expand(Vec3 p) { lo = minv(lo, p); hi = maxv(hi, p); }
    void expand(const Aabb& b) { lo = minv(lo, b.lo); hi = maxv(hi, b.hi); }

    bool valid() const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }
    Vec3 centroid() const { return (lo + hi) * 0.5f; }
    Vec3 extent() const { return valid() ? hi - lo : Vec3{0.0f}; }

    float surfaceArea() const {
        if (!valid()) return 0.0f;
        Vec3 d = hi - lo;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

// Componentwise reciprocal, for feeding traverse(). An infinite component is
// deliberate and correct: it puts the slab bound for a ray running parallel to
// that axis at +/-infinity, which is the right answer.
//
// The warning is silenced rather than avoided. A caller with an axis-aligned
// constant direction -- which the tests very much want to use -- lets the
// compiler see the zero and complain, and the alternative would be to make
// those rays artificially crooked to keep it quiet.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4723)  // potential divide by zero
#endif
inline Vec3 inverseDirection(Vec3 direction) {
    return {1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z};
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

class Bvh {
public:
    // Builds over `boxes`, indexed as the caller indexes its primitives.
    void build(const std::vector<Aabb>& boxes);
    void clear();

    bool   empty() const { return nodes_.empty(); }
    size_t nodeCount() const { return nodes_.size(); }
    size_t primitiveCount() const { return order_.size(); }
    int    maxDepth() const { return maxDepth_; }

    // Visits candidate primitives roughly front-to-back.
    //
    // `test(primitiveIndex, tMax)` takes tMax by reference: it returns true
    // when it accepts a hit nearer than tMax, having tightened tMax itself.
    // That is the same bookkeeping a linear scan does by hand, just handed to
    // the tree so whole subtrees behind the current best can be skipped.
    //
    // With AnyHit the walk stops at the first acceptance, which is all a
    // shadow ray ever needed.
    template <bool AnyHit = false, class Fn>
    bool traverse(Vec3 origin, Vec3 invDirection, float maxDistance, Fn&& test) const;

    struct Node {
        Vec3 lo, hi;
        uint32_t start = 0;  // leaf: first entry in order_
        uint32_t count = 0;  // 0 marks an interior node
        uint32_t right = 0;  // interior: index of the right child; left is this + 1
        uint32_t axis = 0;   // interior: split axis, so the walk can order children
    };

    // The built tree, so it can be handed to a shader instead of walked here.
    // The traversal above is already an explicit stack rather than recursion,
    // which is what makes it portable at all -- GLSL has no recursion, and a
    // tree rebuilt on the other side would be a second chance to disagree
    // about which primitives a node owns.
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<uint32_t>& order() const { return order_; }

private:
    // Bins per axis for the surface-area heuristic. Twelve is the usual
    // sweet spot: enough to find the good split, cheap enough to not matter.
    static constexpr int      kBins = 12;
    static constexpr uint32_t kLeafSize = 4;
    static constexpr int      kMaxDepth = 48;   // traversal stack is sized for this

    void buildNode(uint32_t nodeIndex, uint32_t start, uint32_t count,
                   const std::vector<Aabb>& boxes, int depth);

    // Slab test against a node, narrowed to the interval already accepted.
    static bool slabHit(const Node& node, Vec3 origin, Vec3 invDirection, float tMax) {
        float t0 = 0.0f, t1 = tMax;
        for (int a = 0; a < 3; ++a) {
            // An infinite inverse is deliberate and correct for a ray running
            // parallel to the slab; it lands the bound at +/-inf, which is
            // exactly the answer.
            float near = (node.lo[a] - origin[a]) * invDirection[a];
            float far  = (node.hi[a] - origin[a]) * invDirection[a];
            if (invDirection[a] < 0.0f) { float tmp = near; near = far; far = tmp; }
            if (near > t0) t0 = near;
            if (far < t1) t1 = far;
            if (t0 > t1) return false;
        }
        return true;
    }

    std::vector<Node> nodes_;
    std::vector<uint32_t> order_;  // primitive indices, permuted into leaf runs
    int maxDepth_ = 0;
};

template <bool AnyHit, class Fn>
bool Bvh::traverse(Vec3 origin, Vec3 invDirection, float maxDistance, Fn&& test) const {
    if (nodes_.empty()) return false;

    uint32_t stack[kMaxDepth + 8];
    int top = 0;
    stack[top++] = 0;

    float tMax = maxDistance;
    bool found = false;

    while (top > 0) {
        const uint32_t nodeIndex = stack[--top];
        const Node& node = nodes_[nodeIndex];
        if (!slabHit(node, origin, invDirection, tMax)) continue;

        if (node.count > 0) {
            const uint32_t end = node.start + node.count;
            for (uint32_t i = node.start; i < end; ++i) {
                if (!test(order_[i], tMax)) continue;
                found = true;
                if constexpr (AnyHit) return true;
            }
            continue;
        }

        // Push the far child first so the near one is popped and tested
        // first: hits found there tighten tMax before the far side is walked.
        const uint32_t leftChild = nodeIndex + 1u;
        const uint32_t rightChild = node.right;

        if (invDirection[node.axis] < 0.0f) {
            stack[top++] = leftChild;
            stack[top++] = rightChild;
        } else {
            stack[top++] = rightChild;
            stack[top++] = leftChild;
        }
    }
    return found;
}

} // namespace blocky
