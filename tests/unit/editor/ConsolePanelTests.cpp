#include "cordite/editor/ConsolePanel.h"

#include "cordite/core/Log.h"
#include "cordite/core/Result.h"
#include "cordite/editor/EditorFeedback.h"

#include "support/TestSupport.h"

#include <optional>
#include <string>

using namespace cordite;

namespace {

bool sameColor(const ImVec4& first, const ImVec4& second) {
    return first.x == second.x && first.y == second.y && first.z == second.z && first.w == second.w;
}

} // namespace

TEST_SUITE("ConsolePanel") {
    TEST_CASE("messages are shown in the colors of the theme for their severity") {
        for (const UiTheme theme : kUiThemes) {
            CAPTURE(std::string(nameOf(theme)));
            const UiPalette& palette = paletteOf(theme);
            CHECK(sameColor(messageColor(LogLevel::Trace, palette), palette.textDisabled));
            CHECK(sameColor(messageColor(LogLevel::Debug, palette), palette.textDisabled));
            CHECK(sameColor(messageColor(LogLevel::Info, palette), palette.text));
            CHECK(sameColor(messageColor(LogLevel::Warn, palette), palette.warning));
            CHECK(sameColor(messageColor(LogLevel::Error, palette), palette.error));
            CHECK(sameColor(messageColor(LogLevel::Critical, palette), palette.error));
        }
    }

    TEST_CASE("what the user asked for and did not get is a failed action, and nothing else is") {
        const auto message = [](LogLevel level, const char* category) {
            return LogMessage{.level = level, .category = category, .text = "something"};
        };
        // What the interface could not do, and what a line of the console could not.
        CHECK(reportsFailedAction(message(LogLevel::Error, "Editor")));
        CHECK(reportsFailedAction(message(LogLevel::Error, "Console")));
        CHECK(reportsFailedAction(message(LogLevel::Critical, "Editor")));
        // What they did, and what they only warn of.
        CHECK_FALSE(reportsFailedAction(message(LogLevel::Info, "Console")));
        CHECK_FALSE(reportsFailedAction(message(LogLevel::Warn, "Editor")));
        CHECK_FALSE(reportsFailedAction(message(LogLevel::Debug, "Editor")));
        // What the game that runs has to say, however bad.
        CHECK_FALSE(reportsFailedAction(message(LogLevel::Error, "Script")));
        CHECK_FALSE(reportsFailedAction(message(LogLevel::Critical, "Assets")));
        CHECK_FALSE(reportsFailedAction(message(LogLevel::Error, "editor")));
        CHECK(kInterfaceLogCategory == "Editor");
        CHECK(kConsoleLogCategory == "Console");
    }

    TEST_CASE("the newest message that is severe enough is what the status bar says") {
        const test::LogCapture capture(LogLevel::Trace);
        ConsolePanel console;
        CORDITE_LOG_WARN("Editor", "an older warning");
        CORDITE_LOG_INFO("Console", "scene.save: done");
        CORDITE_LOG_DEBUG("Engine", "a detail");

        const std::optional<LogMessage> info = console.newestMessage(LogLevel::Info);
        REQUIRE(info.has_value());
        CHECK(info->text == "scene.save: done");
        CHECK(info->level == LogLevel::Info);
        // The newest of all, and the newest of what is worse than information.
        CHECK(console.newestMessage(LogLevel::Trace)->text == "a detail");
        CHECK(console.newestMessage(LogLevel::Warn)->text == "an older warning");
        // None that bad, none at all.
        CORDITE_LOG_ERROR("Script", "a script has stopped");
        CHECK(console.newestMessage(LogLevel::Critical) == std::nullopt);
        CHECK(console.newestMessage(LogLevel::Error)->text == "a script has stopped");
    }

    TEST_CASE("the console says once that an action has failed, for any number of them") {
        // Lets everything through to whoever listens, and keeps it off the screen.
        const test::LogCapture capture(LogLevel::Trace);
        ConsolePanel console;
        // Whatever the log remembers from before this test.
        static_cast<void>(console.takeFailedAction());
        CHECK_FALSE(console.takeFailedAction());
        const size_t before = console.messageCount();

        // Nothing the user has to see the console for.
        CORDITE_LOG_ERROR("Script", "a script has stopped");
        CORDITE_LOG_WARN("Editor", "only a warning");
        CORDITE_LOG_INFO("Console", "scene.save: done");
        CHECK(reportIfFailed(Status()));
        CHECK_FALSE(console.takeFailedAction());
        CHECK(console.messageCount() == before + 3);

        // An edit that failed, and a line that did: told of once, together.
        const Status failed = makeError(ErrorCode::NotFound, "there is no such entity");
        CHECK_FALSE(reportIfFailed(failed));
        CORDITE_LOG_ERROR("Console", "no.such.command: there is no command of that name");
        CHECK(capture.contains("there is no such entity"));
        CHECK(console.takeFailedAction());
        CHECK_FALSE(console.takeFailedAction());
        CHECK(console.messageCount() == before + 5);

        // And again when the next one comes.
        CORDITE_LOG_INFO("Console", "editor.undo: done");
        CHECK_FALSE(console.takeFailedAction());
        CHECK_FALSE(reportIfFailed(failed));
        CHECK(console.takeFailedAction());
    }
}
