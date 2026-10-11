#pragma once

/// @file
/// The editor panel that shows the log and takes typed commands.
///
/// Everything the engine logs appears here, colored by severity and filterable. The line
/// at the bottom runs any command of the editor's command registry, which makes the
/// console the place to try out what a script or an agent would send:
///
///     entity.create {"name": "Lamp", "parent": "Room"}
///
/// @code
/// ConsolePanel console;
/// std::vector<std::string> submitted;
/// console.draw(submitted);         // once per frame; `submitted` receives entered lines
/// @endcode

#include "cordite/core/Base.h"
#include "cordite/core/Log.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cordite {

class ConsolePanel {
public:
    /// Starts collecting log messages, beginning with those already in the log's history.
    ConsolePanel();
    ~ConsolePanel();
    CORDITE_NON_MOVABLE(ConsolePanel);

    /// Draws the panel as its own window. Lines the user entered are appended to
    /// `submittedLines`; running them is up to the caller.
    void draw(std::vector<std::string>& submittedLines);

    /// Number of messages currently kept.
    [[nodiscard]] size_t messageCount() const { return m_messages.size(); }

private:
    void collectIncoming();

    /// Where log messages wait, on whichever thread they were logged, until the
    /// interface thread picks them up.
    struct Inbox {
        std::mutex mutex;
        std::vector<LogMessage> messages;
    };

    Log::ListenerId m_listener = 0;
    std::shared_ptr<Inbox> m_inbox;
    std::vector<LogMessage> m_messages;
    LogLevel m_minimumLevel = LogLevel::Info;
    bool m_followNewest = true;
    /// Set when messages arrived, to scroll down after they have been laid out.
    bool m_scrollToNewest = false;
    std::string m_input;
};

} // namespace cordite
