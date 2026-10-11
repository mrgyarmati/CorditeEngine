#pragma once

/// @file
/// The history of reversible edits behind Undo and Redo.
///
/// Every change the editor makes to a scene is pushed here as a pair of functions: one
/// that takes the change back and one that makes it again.
///
/// @code
/// UndoStack history;
/// history.push({.label = "Rename entity",
///               .undo = [&] { return rename(id, oldName); },
///               .redo = [&] { return rename(id, newName); }});
///
/// CORDITE_TRY(history.undo());     // the entity has its old name again
/// CORDITE_TRY(history.redo());
/// @endcode
///
/// The edit has already been made when it is pushed; push() does not call `redo`.

#include "cordite/core/Result.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

struct UndoEntry {
    /// What the edit did, as shown in the Edit menu: "Delete 3 entities".
    std::string label;
    /// Each returns an error when the edit can no longer be applied, which means the
    /// scene was changed behind the history's back.
    std::function<Status()> undo;
    std::function<Status()> redo;
};

class UndoStack {
public:
    /// Edits kept by default; older ones are forgotten.
    static constexpr size_t kDefaultCapacity = 256;

    explicit UndoStack(size_t capacity = kDefaultCapacity);

    /// Records an edit that has just been made. Everything that had been undone is
    /// discarded: the history does not branch.
    void push(UndoEntry entry);

    /// Takes back the most recent edit. Fails with InvalidState when there is none. If
    /// the edit itself fails, its error is returned and the whole history is cleared,
    /// because it no longer describes the scene.
    [[nodiscard]] Status undo();

    /// Makes the most recently undone edit again, with the same rules as undo().
    [[nodiscard]] Status redo();

    [[nodiscard]] bool canUndo() const { return m_position > 0; }

    [[nodiscard]] bool canRedo() const { return m_position < m_entries.size(); }

    /// The labels of the edits undo() and redo() would act on; empty when there is none.
    [[nodiscard]] std::string_view undoLabel() const;
    [[nodiscard]] std::string_view redoLabel() const;

    /// Number of edits that can be undone and redone.
    [[nodiscard]] size_t undoCount() const { return m_position; }

    [[nodiscard]] size_t redoCount() const { return m_entries.size() - m_position; }

    /// Identifies the state the history is in: it changes with every push, undo and redo,
    /// and comes back to an earlier value when undoing or redoing returns to that state.
    /// Comparing it with the value remembered at the last save tells whether there are
    /// unsaved changes.
    [[nodiscard]] uint64_t stateId() const;

    /// Forgets everything. stateId() takes a value it never had before.
    void clear();

private:
    struct Record {
        UndoEntry entry;
        /// The state reached by making this edit.
        uint64_t stateId = 0;
    };

    size_t m_capacity;
    std::vector<Record> m_entries;
    /// Entries before this index are done; entries from it on are undone.
    size_t m_position = 0;
    /// The state before the first entry.
    uint64_t m_baseStateId = 0;
    uint64_t m_nextStateId = 1;
};

} // namespace cordite
