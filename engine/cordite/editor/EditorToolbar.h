#pragma once

/// @file
/// The bars along the top and the bottom of the editor, as in the editors people know:
/// under the menu, what is done to the document at the left - save, undo, redo - and
/// playing in the middle; at the bottom, the newest message of the log and the state of
/// the scene.
///
/// @code
/// drawToolbar(documentButtons(context), gizmoButtons(mode, space), playButtons(play.state()), submittedLines);
/// const StatusBarState status{.message = console.newestMessage(LogLevel::Info), ...};
/// if (drawStatusBar(status)) {
///     focusConsole();
/// }
/// @endcode
///
/// A button is described before it is drawn (ToolbarButton), so that what each one does
/// in each state is tested without an interface; a click queues its command like a line
/// of the console, to run before the next frame.

#include "cordite/core/Log.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/editor/Gizmo.h"
#include "cordite/editor/PlayMode.h"
#include "cordite/ui/UiIcons.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cordite {

struct ToolbarButton {
    UiIcon icon;
    /// What the button does, with its shortcut: shown while the pointer rests on it.
    std::string tooltip;
    /// The command line a click queues.
    std::string command;
    bool enabled = true;
    /// What the button stands for is going on - the scene plays - and it is drawn in the
    /// accent.
    bool active = false;
};

/// Save, undo and redo, for the document of `context`: each enabled when it can be done -
/// a scene without a file is saved under a name, from the File menu, and none while it
/// is played - with what would be undone or redone in its tooltip.
[[nodiscard]] std::vector<ToolbarButton> documentButtons(const EditorContext& context);

/// What the gizmo does - translate, rotate or scale, the one in use on - and the axes it
/// uses, the world's or the entity's own, which a click switches.
[[nodiscard]] std::vector<ToolbarButton> gizmoButtons(GizmoMode mode, GizmoSpace space);

/// Play, pause and step. Play starts the scene, and while it plays - or is held - is a
/// Stop button that stops it, drawn in the accent; pause holds a scene that plays and
/// resumes one that is held; step runs one frame of a held scene.
[[nodiscard]] std::vector<ToolbarButton> playButtons(PlayState state);

/// Draws the toolbar along the top of the main viewport, under the menu, where it takes
/// its room from what is docked: `document` and then `tools` at the left, `playing` in the
/// middle. The
/// command of a button that is clicked is appended to `submittedLines`. Call it after the
/// main menu and before the dock space, during a frame.
void drawToolbar(std::span<const ToolbarButton> document, std::span<const ToolbarButton> tools,
                 std::span<const ToolbarButton> playing, std::vector<std::string>& submittedLines);

/// What the status bar says.
struct StatusBarState {
    /// The newest message of the log worth showing, if there is one.
    std::optional<LogMessage> message;
    PlayState play = PlayState::Stopped;
    size_t entityCount = 0;
    size_t selectionCount = 0;
};

/// What the right end of the status bar says: whether the scene plays, how many entities
/// it has, and how many are selected, when any are - "Playing  ·  41 entities  ·  2
/// selected".
[[nodiscard]] std::string statusSummary(const StatusBarState& state);

/// Draws the status bar along the bottom of the main viewport: the message at the left, in
/// the color of its severity, and statusSummary() at the right. Returns true when the
/// message was clicked, which asks for the console. Call it after the main menu and
/// before the dock space, during a frame.
[[nodiscard]] bool drawStatusBar(const StatusBarState& state);

} // namespace cordite
