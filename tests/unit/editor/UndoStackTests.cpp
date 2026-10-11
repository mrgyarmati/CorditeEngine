#include "cordite/editor/UndoStack.h"

#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;

namespace {

/// An entry that sets `value` to `after` when redone and to `before` when undone.
UndoEntry setValue(int& value, int before, int after, std::string label = "Set value") {
    return {.label = std::move(label),
            .undo = [&value, before]() -> Status {
                value = before;
                return {};
            },
            .redo = [&value, after]() -> Status {
                value = after;
                return {};
            }};
}

} // namespace

TEST_SUITE("UndoStack") {
    TEST_CASE("a new history has nothing to undo or redo") {
        UndoStack history;
        CHECK_FALSE(history.canUndo());
        CHECK_FALSE(history.canRedo());
        CHECK(history.undoLabel().empty());
        CHECK(history.redoLabel().empty());
        CHECK(history.undoCount() == 0);
        CHECK(history.redoCount() == 0);

        const Status undone = history.undo();
        REQUIRE_FALSE(undone.has_value());
        CHECK(undone.error().code == ErrorCode::InvalidState);
        CHECK_FALSE(history.redo().has_value());
    }

    TEST_CASE("edits are undone and redone in order") {
        UndoStack history;
        int value = 0;

        value = 1;
        history.push(setValue(value, 0, 1, "First"));
        value = 2;
        history.push(setValue(value, 1, 2, "Second"));
        CHECK(history.undoCount() == 2);
        CHECK(history.undoLabel() == "Second");

        REQUIRE(history.undo().has_value());
        CHECK(value == 1);
        CHECK(history.undoLabel() == "First");
        CHECK(history.redoLabel() == "Second");

        REQUIRE(history.undo().has_value());
        CHECK(value == 0);
        CHECK_FALSE(history.canUndo());
        CHECK(history.redoCount() == 2);

        REQUIRE(history.redo().has_value());
        CHECK(value == 1);
        REQUIRE(history.redo().has_value());
        CHECK(value == 2);
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("pushing does not run the edit") {
        UndoStack history;
        int value = 5;
        history.push(setValue(value, 0, 99));
        CHECK(value == 5);
    }

    TEST_CASE("a new edit discards what had been undone") {
        UndoStack history;
        int value = 0;
        history.push(setValue(value, 0, 1, "First"));
        history.push(setValue(value, 1, 2, "Second"));
        REQUIRE(history.undo().has_value());

        history.push(setValue(value, 1, 7, "Third"));
        CHECK_FALSE(history.canRedo());
        CHECK(history.undoCount() == 2);
        CHECK(history.undoLabel() == "Third");

        REQUIRE(history.undo().has_value());
        REQUIRE(history.undo().has_value());
        CHECK(value == 0);
    }

    TEST_CASE("the state id identifies where the history stands") {
        UndoStack history;
        int value = 0;
        const uint64_t initial = history.stateId();

        history.push(setValue(value, 0, 1));
        const uint64_t afterFirst = history.stateId();
        history.push(setValue(value, 1, 2));
        const uint64_t afterSecond = history.stateId();
        CHECK(afterFirst != initial);
        CHECK(afterSecond != afterFirst);

        // Undoing returns to the states that were passed through, and redoing likewise.
        REQUIRE(history.undo().has_value());
        CHECK(history.stateId() == afterFirst);
        REQUIRE(history.undo().has_value());
        CHECK(history.stateId() == initial);
        REQUIRE(history.redo().has_value());
        CHECK(history.stateId() == afterFirst);

        // A different edit leads to a state that never existed before.
        history.push(setValue(value, 1, 5));
        CHECK(history.stateId() != afterSecond);
        CHECK(history.stateId() != afterFirst);
    }

    TEST_CASE("clear forgets everything and starts from a fresh state") {
        UndoStack history;
        int value = 0;
        const uint64_t initial = history.stateId();
        history.push(setValue(value, 0, 1));
        const uint64_t afterFirst = history.stateId();

        history.clear();
        CHECK_FALSE(history.canUndo());
        CHECK_FALSE(history.canRedo());
        CHECK(history.stateId() != initial);
        CHECK(history.stateId() != afterFirst);
    }

    TEST_CASE("the oldest edits are forgotten beyond the capacity") {
        UndoStack history(3);
        int value = 0;
        for (int step = 1; step <= 5; ++step) {
            value = step;
            history.push(setValue(value, step - 1, step, "Step " + std::to_string(step)));
        }
        CHECK(history.undoCount() == 3);

        // Only the last three can be undone: the value stops at what step 2 left.
        REQUIRE(history.undo().has_value());
        REQUIRE(history.undo().has_value());
        REQUIRE(history.undo().has_value());
        CHECK(value == 2);
        CHECK_FALSE(history.undo().has_value());

        // The earliest reachable state is distinct from the states after it.
        const uint64_t earliest = history.stateId();
        REQUIRE(history.redo().has_value());
        CHECK(history.stateId() != earliest);
        REQUIRE(history.undo().has_value());
        CHECK(history.stateId() == earliest);
    }

    TEST_CASE("a capacity of zero still keeps one edit") {
        UndoStack history(0);
        int value = 0;
        history.push(setValue(value, 0, 1));
        history.push(setValue(value, 1, 2));
        CHECK(history.undoCount() == 1);
    }

    TEST_CASE("an edit that fails clears the history and reports what failed") {
        UndoStack history;
        int value = 0;
        history.push(setValue(value, 0, 1, "Good"));
        history.push({.label = "Broken",
                      .undo = []() -> Status { return makeError(ErrorCode::InvalidState, "the entity is gone"); },
                      .redo = []() -> Status { return {}; }});

        const Status status = history.undo();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::InvalidState);
        CHECK(status.error().message == "undo 'Broken': the entity is gone");
        // The history no longer describes the document, so none of it is kept.
        CHECK_FALSE(history.canUndo());
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("a failing redo is handled the same way") {
        UndoStack history;
        history.push({.label = "Fragile",
                      .undo = []() -> Status { return {}; },
                      .redo = []() -> Status { return makeError(ErrorCode::NotFound, "missing"); }});
        REQUIRE(history.undo().has_value());

        const Status status = history.redo();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message == "redo 'Fragile': missing");
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("an edit may inspect the history while it runs") {
        UndoStack history;
        std::vector<size_t> seen;
        history.push({.label = "Observer",
                      .undo = [&]() -> Status {
                          seen.push_back(history.undoCount());
                          return {};
                      },
                      .redo = [&]() -> Status {
                          seen.push_back(history.undoCount());
                          return {};
                      }});

        REQUIRE(history.undo().has_value());
        REQUIRE(history.redo().has_value());
        // The history already reflects the outcome when the edit is called.
        CHECK(seen == std::vector<size_t>{0, 1});
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("an entry without both directions is a programmer error") {
        UndoStack history;
        CHECK_THROWS_AS(history.push({.label = "Half", .undo = []() -> Status { return {}; }, .redo = nullptr}),
                        test::AssertionFailure);
        CHECK_FALSE(history.canUndo());
    }
#endif
}
