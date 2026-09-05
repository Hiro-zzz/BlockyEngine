#pragma once
// A list of sprites being arranged: the third document.
//
// A sprite is a quad standing in the world -- a particle, a glyph, a mote in
// a shaft of light. Scenes place them by hand today, in code, and that code
// is a column of numbers nobody can picture. This is the thing that lets you
// put them where they look right and then writes those numbers out.
//
// ---------------------------------------------------- undo by snapshot, here
//
// The other two documents keep undo as differences over cells, because a
// stroke touches a handful of cells out of thousands and a snapshot of a
// 64-cube is half a megabyte. Neither half of that argument survives the trip
// here: a sprite list has no cells to index, and an edit adds or removes a
// whole object rather than nudging part of one. Tens of sprites at a hundred
// bytes each is a snapshot that costs nothing to take.
//
// So this is not an inconsistency to be tidied away later. It is the same
// question -- what is the smallest honest unit of change -- answered by two
// documents that are shaped differently.
#include "engine/core/math.hpp"
#include "engine/sprite/sprite.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace blocky {
namespace edit {

class SpriteDoc {
public:
    // A snapshot cap for the same reason the cell history has one: a session
    // that only grows is a leak that waits for a long afternoon.
    explicit SpriteDoc(size_t maxSteps = 128) : maxSteps_(maxSteps) {}

    const std::vector<Sprite>& sprites() const { return sprites_; }
    size_t size() const { return sprites_.size(); }
    bool   empty() const { return sprites_.empty(); }

    // Every mutator takes its own snapshot, so a caller cannot forget to. The
    // cell documents leave that to the caller because a drag is many calls
    // and one step; nothing here is dragged.
    void add(const Sprite& sprite);
    bool removeAt(size_t index);
    void clear();

    // The sprite whose centre lies nearest the ray, or -1. A quad is thin and
    // hard to hit edge-on, so this is a proximity test rather than an
    // intersection: what the pointer means is "that one", not "that surface".
    // The tolerance is each sprite's own half-size, so a big one is easy to
    // click and a mote still needs aiming at.
    int pick(Vec3 origin, Vec3 direction) const;

    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    const std::string& undoName() const;
    const std::string& redoName() const;

    bool undo();
    bool redo();

    void resetHistory();

private:
    struct Step {
        std::string name;
        std::vector<Sprite> before;
    };

    void snapshot(std::string name);

    std::vector<Sprite> sprites_;
    std::vector<Step> done_;
    std::vector<Step> undone_;
    size_t maxSteps_;
};

} // namespace edit
} // namespace blocky
