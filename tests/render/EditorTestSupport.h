#pragma once

/// @file
/// What the tests of the editor are built on: an editor without a window, made the same way
/// for every test, its commands run and refused, and the shortcuts of its interface pressed.
///
/// @code
/// EditorAppDesc desc = headlessDesc(UVec2(640, 360));
/// desc.project = showcaseProject();
/// const std::unique_ptr<EditorApp> editor = createEditor(desc);
///
/// run(*editor, "viewport.lookAt", R"({"position": [0, 2, 8], "target": [0, 2, 0]})");
/// const Error error = refusal(*editor, "viewport.close", R"({"viewport": 1})");
/// pressShortcut(*editor, ImGuiMod_Ctrl | ImGuiKey_D);
/// @endcode

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Result.h"
#include "cordite/editor/EditorApp.h"
#include "cordite/math/Math.h"

#include "render/RenderTestSupport.h"
#include "support/TestSupport.h"

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite::test {

/// The project that uses every feature of the engine.
inline std::filesystem::path showcaseProject() {
    return sourceDirectory() / "samples" / "showcase";
}

/// An editor without a window, `size` pixels large, that checks what it does with the GPU
/// and lets the same time pass in every frame.
inline EditorAppDesc headlessDesc(UVec2 size = UVec2(1280, 720)) {
    EditorAppDesc desc;
    desc.engine.headless = true;
    desc.engine.validation = true;
    desc.engine.renderSize = size;
    desc.engine.fixedTimeStep = 1.0 / 60.0;
    // Tests must not depend on where the engine would look for shaders by default.
    desc.engine.shaderDirectory = shaderDirectory();
    // Each compiled once in a run (sharedCacheDirectory()).
    desc.engine.cacheDirectory = sharedCacheDirectory();
    return desc;
}

inline std::unique_ptr<EditorApp> createEditor(const EditorAppDesc& desc) {
    Result<std::unique_ptr<EditorApp>> editor = EditorApp::create(desc);
    REQUIRE_MESSAGE(editor.has_value(), errorOf(editor));
    return std::move(*editor);
}

inline Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// Runs a command that is expected to succeed.
inline Json run(EditorApp& editor, std::string_view command, std::string_view parameters = "{}") {
    const Result<Json> result = editor.commands().execute(command, json(parameters));
    REQUIRE_MESSAGE(result.has_value(), errorOf(result));
    return *result;
}

/// Runs a command that is expected to fail, and returns why it did.
inline Error refusal(EditorApp& editor, std::string_view command, std::string_view parameters = "{}") {
    const Result<Json> result = editor.commands().execute(command, json(parameters));
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// An editor for a picture of its whole window. The console is one of its panels, and
/// shows what was logged - by this test or by any that ran before it in the same
/// process: the log is emptied first, so that the picture does not depend on which
/// tests those were.
inline std::unique_ptr<EditorApp> createEditorForPicture(const EditorAppDesc& desc) {
    Log::clearHistory();
    return createEditor(desc);
}

/// A JSON string literal holding a path, quoted and escaped.
inline std::string quoted(const std::filesystem::path& path) {
    return Json(pathToUtf8(path)).dump();
}

/// The key a person holds for what Dear ImGui calls Ctrl: Cmd on a Mac, where Dear ImGui
/// swaps the two as they come in (`ImGuiIO::ConfigMacOSXBehaviors`).
inline ImGuiKey controlKey() {
    return ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
}

/// A shortcut of the interface - a key, with Ctrl, Shift or Alt - pressed in one frame and
/// let go of in the next. Without a window the engine tells the interface of its pointer
/// alone, so the keys are given to Dear ImGui itself.
inline void pressShortcut(EditorApp& editor, ImGuiKeyChord shortcut) {
    // Super is the key controlKey() decides on: no test holds it by its name.
    REQUIRE((shortcut & ImGuiMod_Super) == 0);
    std::vector<ImGuiKey> keys;
    if ((shortcut & ImGuiMod_Ctrl) != 0) {
        keys.push_back(controlKey());
    }
    if ((shortcut & ImGuiMod_Shift) != 0) {
        keys.push_back(ImGuiMod_Shift);
    }
    if ((shortcut & ImGuiMod_Alt) != 0) {
        keys.push_back(ImGuiMod_Alt);
    }
    keys.push_back(static_cast<ImGuiKey>(shortcut & ~ImGuiMod_Mask_));
    ImGuiIO& io = ImGui::GetIO();
    for (const ImGuiKey key : keys) {
        io.AddKeyEvent(key, true);
    }
    REQUIRE(editor.runFrames(1).has_value());
    for (const ImGuiKey key : keys) {
        io.AddKeyEvent(key, false);
    }
    REQUIRE(editor.runFrames(1).has_value());
}

} // namespace cordite::test
