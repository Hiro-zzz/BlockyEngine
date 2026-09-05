#pragma once
// Undo, for documents made of cells.
//
// Everything the editor edits is a grid of cells holding one small value: a
// canvas cell is RGBA8, a voxel is a palette index, and both fit in a
// uint32. So the history does not need to know which of the two it is
// holding. It stores where, what the cell was, and what it became, and the
// document puts the value back.
//
// ------------------------------------------------------- why not snapshots
//
// A snapshot per action is the shorter code and the wrong trade. A stroke
// touches a handful of cells; a 64-cube model is half a megabyte. Fifty
// snapshots of a document nobody has finished is not a history, it is a leak
// with a keyboard shortcut. Diffs cost what the edit cost.
//
// ------------------------------------------------------------ what it caps
//
// Both the number of strokes and the total cells they hold, because either
// one alone can be defeated. A thousand pencil dots is a lot of strokes and
// no memory; one box fill over a 64-cube is one stroke and a megabyte.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace blocky {
namespace edit {

// Where a cell is, what it held, and what it holds now. The index is the
// document's own linear index -- the history never computes one.
struct CellEdit {
    uint32_t index = 0;
    uint32_t before = 0;
    uint32_t after = 0;
};

// One undoable action, named so the interface can say what Ctrl+Z will do.
struct Stroke {
    std::string name;
    std::vector<CellEdit> cells;
};

class History {
public:
    // Defaults: deep enough that nobody reaches the end by accident, small
    // enough that the worst case is tens of megabytes rather than however
    // long the session ran.
    explicit History(size_t maxStrokes = 128, size_t maxCells = 4u * 1024u * 1024u)
        : maxStrokes_(maxStrokes), maxCells_(maxCells) {}

    // Applies one cell of a stroke being undone or redone.
    using Apply = std::function<void(uint32_t index, uint32_t value)>;

    // ------------------------------------------------------------ recording
    //
    // A document calls `begin` before it touches anything, `record` for each
    // cell it changes, and `commit` when the action is over. A stroke that
    // changed nothing is dropped by `commit` rather than becoming an undo
    // step that does nothing visible -- pressing Ctrl+Z and watching the
    // picture not change is indistinguishable from a broken undo.
    void begin(std::string name);
    void record(uint32_t index, uint32_t before, uint32_t after);
    bool commit();
    void abandon();
    bool recording() const { return recording_; }

    // True while `record` would be dropped on the floor. A document may
    // assert on this: recording without begin is a missing call, not a
    // no-op, and it silently costs the user their undo.
    bool armed() const { return recording_; }

    // ------------------------------------------------------------- stepping
    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }

    // Names of the steps the two shortcuts would take, or empty.
    const std::string& undoName() const;
    const std::string& redoName() const;

    bool undo(const Apply& apply);
    bool redo(const Apply& apply);

    void clear();

    size_t depth() const { return done_.size(); }
    size_t cellsHeld() const { return cells_; }

private:
    void trim();

    std::deque<Stroke> done_;
    std::vector<Stroke> undone_;
    Stroke pending_;
    bool recording_ = false;
    size_t cells_ = 0;

    size_t maxStrokes_;
    size_t maxCells_;
};

} // namespace edit
} // namespace blocky
