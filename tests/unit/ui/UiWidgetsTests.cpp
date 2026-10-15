#include "cordite/ui/UiWidgets.h"

#include "support/TestSupport.h"

#include <imgui_internal.h>

#include <string>

using namespace cordite;

TEST_SUITE("UiWidgets") {
    TEST_CASE("a window shows an icon before its title, and is known by its title alone") {
        const std::string name = windowName(icons::kConsole, "Console");
        CHECK(name == std::string(icons::kConsole.text()) + " Console###Console");

        // What ImGui finds a window by - focusing it, docking it, remembering its place - is
        // what follows "###": the same whatever the icon. And it is the title alone, which
        // is what the window was called before it had an icon: a layout remembered then
        // still places it.
        CHECK(ImHashStr(name.c_str()) == ImHashStr("###Console"));
        CHECK(ImHashStr(windowName(icons::kWarning, "Console").c_str()) == ImHashStr("###Console"));
        CHECK(ImHashStr(name.c_str()) == ImHashStr("Console"));
        CHECK(ImHashStr(name.c_str()) != ImHashStr(windowName(icons::kConsole, "Assets").c_str()));
    }
}
