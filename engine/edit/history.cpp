#include "engine/edit/history.hpp"

#include <utility>

namespace blocky {
namespace edit {

namespace {
const std::string kNoStep;
}

void History::begin(std::string name) {
    // An unclosed stroke means somebody forgot to commit. Abandoning it is
    // better than folding two actions into one: the second action would then
    // undo the first as well, which reads as undo skipping a step.
    if (recording_) abandon();

    pending_.name = std::move(name);
    pending_.cells.clear();
    recording_ = true;
}

void History::record(uint32_t index, uint32_t before, uint32_t after) {
    if (!recording_) return;

    // A pencil put back the colour that was already there. Storing it would
    // make a stroke that changed nothing look like one that did.
    if (before == after) return;

    pending_.cells.push_back({index, before, after});
}

bool History::commit() {
    if (!recording_) return false;
    recording_ = false;

    if (pending_.cells.empty()) {
        pending_ = Stroke{};
        return false;
    }

    // A new action is what makes the redo branch unreachable. Dropping it
    // here rather than on the next redo keeps the accounting honest: those
    // cells are gone, so they stop counting against the budget.
    for (const Stroke& stroke : undone_) cells_ -= stroke.cells.size();
    undone_.clear();

    cells_ += pending_.cells.size();
    done_.push_back(std::move(pending_));
    pending_ = Stroke{};

    trim();
    return true;
}

void History::abandon() {
    pending_ = Stroke{};
    recording_ = false;
}

const std::string& History::undoName() const {
    return done_.empty() ? kNoStep : done_.back().name;
}

const std::string& History::redoName() const {
    return undone_.empty() ? kNoStep : undone_.back().name;
}

bool History::undo(const Apply& apply) {
    if (done_.empty()) return false;

    Stroke stroke = std::move(done_.back());
    done_.pop_back();

    // Backwards, and this is the whole reason the loop is written by hand.
    //
    // A stroke may name the same cell more than once: a brush dragged back
    // over its own track does exactly that, and so does a box fill over a
    // region the same stroke already painted. The **first** record of a cell
    // is the one holding what it was before the action started. Restoring
    // forwards would leave every repeated cell at its second-to-last value,
    // which looks like undo working everywhere except where the hand moved
    // slowly.
    for (size_t i = stroke.cells.size(); i-- > 0;) {
        apply(stroke.cells[i].index, stroke.cells[i].before);
    }

    undone_.push_back(std::move(stroke));
    return true;
}

bool History::redo(const Apply& apply) {
    if (undone_.empty()) return false;

    Stroke stroke = std::move(undone_.back());
    undone_.pop_back();

    // Forwards, by the same argument upside down: the last record of a cell
    // is what the action left there.
    for (const CellEdit& cell : stroke.cells) apply(cell.index, cell.after);

    done_.push_back(std::move(stroke));
    return true;
}

void History::clear() {
    done_.clear();
    undone_.clear();
    pending_ = Stroke{};
    recording_ = false;
    cells_ = 0;
}

void History::trim() {
    // Always leave the newest stroke, whatever it cost. A box fill bigger
    // than the whole budget would otherwise delete itself on the way in, and
    // the one action a person is most likely to want back is the big one
    // they just made by accident.
    while (done_.size() > 1 && (done_.size() > maxStrokes_ || cells_ > maxCells_)) {
        cells_ -= done_.front().cells.size();
        done_.pop_front();
    }
}

} // namespace edit
} // namespace blocky
