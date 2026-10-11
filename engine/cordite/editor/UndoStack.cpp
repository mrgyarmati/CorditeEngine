#include "cordite/editor/UndoStack.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <utility>

namespace cordite {

UndoStack::UndoStack(size_t capacity) : m_capacity(std::max<size_t>(capacity, 1)) {}

void UndoStack::push(UndoEntry entry) {
    CORDITE_ASSERT(entry.undo != nullptr && entry.redo != nullptr, "an undo entry needs both directions");
    if (entry.undo == nullptr || entry.redo == nullptr) {
        return;
    }

    // Whatever had been undone can no longer be redone.
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(m_position), m_entries.end());

    if (m_entries.size() == m_capacity) {
        // Forgetting the oldest edit makes the state it led to the earliest one reachable.
        m_baseStateId = m_entries.front().stateId;
        m_entries.erase(m_entries.begin());
    }

    m_entries.push_back({std::move(entry), m_nextStateId++});
    m_position = m_entries.size();
}

Status UndoStack::undo() {
    if (!canUndo()) {
        return makeError(ErrorCode::InvalidState, "there is nothing to undo");
    }

    // Moved first: the edit may run code that looks at the history.
    --m_position;
    if (const Status status = m_entries[m_position].entry.undo(); !status) {
        const Error error = status.error().withContext(fmt::format("undo '{}'", m_entries[m_position].entry.label));
        clear();
        return tl::unexpected(error);
    }
    return {};
}

Status UndoStack::redo() {
    if (!canRedo()) {
        return makeError(ErrorCode::InvalidState, "there is nothing to redo");
    }

    ++m_position;
    if (const Status status = m_entries[m_position - 1].entry.redo(); !status) {
        const Error error = status.error().withContext(fmt::format("redo '{}'", m_entries[m_position - 1].entry.label));
        clear();
        return tl::unexpected(error);
    }
    return {};
}

std::string_view UndoStack::undoLabel() const {
    return canUndo() ? std::string_view(m_entries[m_position - 1].entry.label) : std::string_view();
}

std::string_view UndoStack::redoLabel() const {
    return canRedo() ? std::string_view(m_entries[m_position].entry.label) : std::string_view();
}

uint64_t UndoStack::stateId() const {
    return m_position == 0 ? m_baseStateId : m_entries[m_position - 1].stateId;
}

void UndoStack::clear() {
    m_entries.clear();
    m_position = 0;
    m_baseStateId = m_nextStateId++;
}

} // namespace cordite
