#include "engine/edit/sprite_doc.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace blocky {
namespace edit {

namespace {
const std::string kNoStep;
}

void SpriteDoc::snapshot(std::string name) {
    Step step;
    step.name = std::move(name);
    step.before = sprites_;

    done_.push_back(std::move(step));
    undone_.clear();   // a new action makes the redo branch unreachable

    if (done_.size() > maxSteps_) done_.erase(done_.begin());
}

void SpriteDoc::add(const Sprite& sprite) {
    snapshot("place sprite");
    sprites_.push_back(sprite);
}

bool SpriteDoc::removeAt(size_t index) {
    if (index >= sprites_.size()) return false;

    snapshot("remove sprite");
    sprites_.erase(sprites_.begin() + std::ptrdiff_t(index));
    return true;
}

void SpriteDoc::clear() {
    if (sprites_.empty()) return;

    snapshot("clear sprites");
    sprites_.clear();
}

int SpriteDoc::pick(Vec3 origin, Vec3 direction) const {
    const float lengthSquared = dot(direction, direction);
    if (lengthSquared < 1e-12f) return -1;

    int best = -1;
    float bestAlong = 0.0f;

    for (size_t i = 0; i < sprites_.size(); ++i) {
        const Sprite& sprite = sprites_[i];

        // Closest approach of the ray to the sprite's centre.
        const Vec3 toCentre = sprite.position - origin;
        const float along = dot(toCentre, direction) / lengthSquared;
        if (along <= 0.0f) continue;   // behind the eye

        const Vec3 nearest = origin + direction * along;
        const float missed = length(sprite.position - nearest);

        // Its own half-size is the tolerance, so the thing you can see is the
        // thing you can hit.
        const float tolerance = std::max(sprite.size.x, sprite.size.y) * 0.5f;
        if (missed > tolerance) continue;

        // Nearest along the ray wins, not nearest to it: two sprites in a line
        // should hand you the front one.
        if (best < 0 || along < bestAlong) {
            best = int(i);
            bestAlong = along;
        }
    }
    return best;
}

const std::string& SpriteDoc::undoName() const {
    return done_.empty() ? kNoStep : done_.back().name;
}

const std::string& SpriteDoc::redoName() const {
    return undone_.empty() ? kNoStep : undone_.back().name;
}

bool SpriteDoc::undo() {
    if (done_.empty()) return false;

    Step step = std::move(done_.back());
    done_.pop_back();

    // The step going the other way holds what is here now, which is what makes
    // redo work without a second kind of record.
    Step forward;
    forward.name = step.name;
    forward.before = std::move(sprites_);

    sprites_ = std::move(step.before);
    undone_.push_back(std::move(forward));
    return true;
}

bool SpriteDoc::redo() {
    if (undone_.empty()) return false;

    Step step = std::move(undone_.back());
    undone_.pop_back();

    Step backward;
    backward.name = step.name;
    backward.before = std::move(sprites_);

    sprites_ = std::move(step.before);
    done_.push_back(std::move(backward));
    return true;
}

void SpriteDoc::resetHistory() {
    done_.clear();
    undone_.clear();
}

} // namespace edit
} // namespace blocky
