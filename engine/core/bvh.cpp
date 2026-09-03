#include "engine/core/bvh.hpp"

#include <algorithm>

namespace blocky {

void Bvh::clear() {
    nodes_.clear();
    order_.clear();
    maxDepth_ = 0;
}

void Bvh::build(const std::vector<Aabb>& boxes) {
    clear();
    if (boxes.empty()) return;

    order_.resize(boxes.size());
    for (uint32_t i = 0; i < uint32_t(order_.size()); ++i) order_[i] = i;

    // Splitting off a single primitive at a time is the worst case, and it
    // tops out at 2n-1 nodes. Reserving that much means the vector never
    // moves while the recursion is holding indices into it.
    nodes_.reserve(2 * boxes.size() + 1);
    nodes_.push_back(Node{});
    buildNode(0, 0, uint32_t(boxes.size()), boxes, 0);
}

void Bvh::buildNode(uint32_t nodeIndex, uint32_t start, uint32_t count,
                    const std::vector<Aabb>& boxes, int depth) {
    maxDepth_ = std::max(maxDepth_, depth);

    Aabb nodeBounds, centroidBounds;
    for (uint32_t i = start; i < start + count; ++i) {
        nodeBounds.expand(boxes[order_[i]]);
        centroidBounds.expand(boxes[order_[i]].centroid());
    }

    nodes_[nodeIndex].lo = nodeBounds.lo;
    nodes_[nodeIndex].hi = nodeBounds.hi;

    auto makeLeaf = [&]() {
        Node& node = nodes_[nodeIndex];
        node.start = start;
        node.count = count;
        node.right = 0;
        node.axis = 0;
    };

    if (count <= kLeafSize || depth >= kMaxDepth) { makeLeaf(); return; }

    Vec3 span = centroidBounds.extent();
    int axis = 0;
    if (span.y > span[axis]) axis = 1;
    if (span.z > span[axis]) axis = 2;

    // Every centroid sitting at one point cannot be split by any plane.
    if (span[axis] <= kEps) { makeLeaf(); return; }

    const float binLo = centroidBounds.lo[axis];
    const float binScale = float(kBins) / span[axis];

    auto binOf = [&](uint32_t primitive) {
        int b = int((boxes[primitive].centroid()[axis] - binLo) * binScale);
        return std::min(std::max(b, 0), kBins - 1);
    };

    Aabb binBounds[kBins];
    uint32_t binCount[kBins]{};
    for (uint32_t i = start; i < start + count; ++i) {
        int b = binOf(order_[i]);
        binBounds[b].expand(boxes[order_[i]]);
        ++binCount[b];
    }

    // Sweep from the right so every candidate split knows both of its sides
    // in one pass each way.
    Aabb rightBounds[kBins];
    uint32_t rightCount[kBins]{};
    {
        Aabb accumulated;
        uint32_t running = 0;
        for (int b = kBins - 1; b >= 0; --b) {
            accumulated.expand(binBounds[b]);
            running += binCount[b];
            rightBounds[b] = accumulated;
            rightCount[b] = running;
        }
    }

    // Surface area heuristic: the cost of a split is the chance a ray enters
    // each side, weighted by what it would then have to test. Not splitting
    // at all costs `count`, so a split has to beat that to be worth taking.
    const float area = nodeBounds.surfaceArea();
    const float invArea = area > 0.0f ? 1.0f / area : 0.0f;

    float bestCost = float(count);
    int bestSplit = -1;
    {
        Aabb accumulated;
        uint32_t running = 0;
        for (int b = 0; b < kBins - 1; ++b) {
            accumulated.expand(binBounds[b]);
            running += binCount[b];
            if (running == 0 || rightCount[b + 1] == 0) continue;

            float cost = 0.125f + invArea * (accumulated.surfaceArea() * float(running) +
                                             rightBounds[b + 1].surfaceArea() *
                                                 float(rightCount[b + 1]));
            if (cost < bestCost) { bestCost = cost; bestSplit = b; }
        }
    }

    if (bestSplit < 0) { makeLeaf(); return; }

    auto first = order_.begin() + start;
    auto last = first + count;
    auto middle = std::partition(
        first, last, [&](uint32_t primitive) { return binOf(primitive) <= bestSplit; });

    uint32_t leftCount = uint32_t(middle - first);

    // The sweep and the partition disagreeing means a centroid landed exactly
    // on a bin edge. Fall back to a median split rather than give up and
    // leave a leaf holding everything.
    if (leftCount == 0 || leftCount == count) {
        leftCount = count / 2;
        std::nth_element(first, first + leftCount, last, [&](uint32_t a, uint32_t b) {
            return boxes[a].centroid()[axis] < boxes[b].centroid()[axis];
        });
    }

    nodes_[nodeIndex].count = 0;
    nodes_[nodeIndex].axis = uint32_t(axis);

    // The left child is always the next node allocated, which is what lets
    // traversal find it as `parent + 1` without storing a second index.
    uint32_t leftIndex = uint32_t(nodes_.size());
    nodes_.push_back(Node{});
    buildNode(leftIndex, start, leftCount, boxes, depth + 1);

    uint32_t rightIndex = uint32_t(nodes_.size());
    nodes_.push_back(Node{});
    nodes_[nodeIndex].right = rightIndex;
    buildNode(rightIndex, start + leftCount, count - leftCount, boxes, depth + 1);
}

} // namespace blocky
