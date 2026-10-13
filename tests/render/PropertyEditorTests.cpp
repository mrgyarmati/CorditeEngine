#include "cordite/editor/PropertyEditor.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/renderer/RenderTarget.h"
#include "cordite/ui/UiSystem.h"

#include "render/RenderTestSupport.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::unwrap;

namespace {

constexpr UVec2 kSize(400, 300);
constexpr float kFrameSeconds = 1.0f / 60.0f;

/// Something with a property that is JSON of no fixed shape, as a script's settings are.
struct Scripted {
    Json settings = Json::object();
};

/// A panel that shows one property of one object, and a field after it to move on to.
/// It stands in for the inspector: the value is fetched anew every frame, and a change
/// is stored - or, when `refuse` says so, turned down, as an edit can be.
class Panel {
public:
    explicit Panel(RenderContext& context)
        : m_context(context),
          m_ui(unwrap(UiSystem::create(context.device(), *context.shaders))),
          m_target(unwrap(RenderTarget::create(context.device(), {.size = kSize}))),
          m_type(TypeBuilder<Scripted>("Scripted").property("settings", &Scripted::settings).build()) {}

    Scripted object;
    /// How often the editor said that the value was changed.
    int changes = 0;
    bool refuse = false;

    /// One frame. `focus` puts the keyboard into the JSON text (Field::Json) or into the
    /// field after it (Field::Next), as a click there would.
    enum class Field {
        None,
        Json,
        Next
    };

    void frame(Field focus = Field::None) {
        m_ui->beginFrame(kSize, kFrameSeconds);
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(380.0f, 280.0f), ImGuiCond_Always);
        ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        if (ImGui::BeginTable("properties", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            const PropertyInfo& property = m_type.properties()[0];
            Json value = property.get(&object);
            if (focus == Field::Json) {
                ImGui::SetKeyboardFocusHere();
            }
            if (m_editor.draw(property, value)) {
                ++changes;
                if (!refuse) {
                    const Status stored = property.set(&object, value);
                    CHECK_MESSAGE(stored.has_value(), test::errorOf(stored));
                }
            }
            ImGui::EndTable();
        }
        if (focus == Field::Next) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("next", &m_next);
        ImGui::End();
        recordAndExecute(m_context, [&](nvrhi::ICommandList* commandList) {
            m_target.clear(commandList);
            m_ui->render(commandList, m_target.framebuffer());
        });
        // Many frames are drawn here, one right after the other; each is finished before
        // the next, so that what it used is free again.
        m_context.graphics->waitIdle();
        m_context.graphics->collectGarbage();
    }

    /// Replaces the text of the JSON field by `text`, as someone would who clicks into
    /// it, selects everything and types - and then clicks into the next field.
    void type(std::string_view text) {
        ImGuiIO& io = ImGui::GetIO();
        // The field has the keyboard a frame after it was asked for, and its keys - the
        // one that selects everything among them - a frame after that. Nobody types faster.
        frame(Field::Json);
        frame();
        frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
        io.AddInputCharactersUTF8(std::string(text).c_str());
        frame();
        frame(Field::Next);
        frame();
        frame();
    }

    [[nodiscard]] std::string settings() const { return dumpJson(object.settings, -1); }

private:
    RenderContext& m_context;
    std::unique_ptr<UiSystem> m_ui;
    RenderTarget m_target;
    TypeInfo m_type;
    PropertyEditor m_editor;
    std::string m_next;
};

} // namespace

TEST_SUITE("PropertyEditor") {
    TEST_CASE("JSON of no fixed shape is edited as text, and is the value once the text is an object") {
        RenderContext context;
        Panel panel(context);
        panel.object.settings = unwrap(parseJson(R"({"speed": 2})"));

        // Shown, and left alone: nothing is changed by looking.
        panel.frame();
        panel.frame();
        CHECK(panel.changes == 0);

        // Typed, and left: the text is the value from then on.
        panel.type(R"({"speed": 3.5, "opens": ["north", "east"]})");
        CHECK(panel.settings() == R"({"speed":3.5,"opens":["north","east"]})");
        CHECK(panel.changes == 1);

        // Typing the value that is there already changes nothing.
        panel.type(R"(  { "speed" : 3.5, "opens" : [ "north", "east" ] }  )");
        CHECK(panel.settings() == R"({"speed":3.5,"opens":["north","east"]})");
        CHECK(panel.changes == 1);

        // Text that is no JSON, or no object, or more than a property holds, is not the
        // value: what is there stays.
        for (const std::string_view wrong : {R"({"speed": )", "[1, 2, 3]", "42", "", R"({"speed": 1, "speed": 2})"}) {
            const std::string text(wrong);
            CAPTURE(text);
            panel.type(wrong);
            CHECK(panel.settings() == R"({"speed":3.5,"opens":["north","east"]})");
            CHECK(panel.changes == 1);
        }
        std::string tooDeep = R"({"a": )";
        for (uint32_t level = 0; level < kMaxJsonPropertyNesting; ++level) {
            tooDeep += "[";
        }
        tooDeep += std::string(kMaxJsonPropertyNesting, ']') + "}";
        panel.type(tooDeep);
        CHECK(panel.settings() == R"({"speed":3.5,"opens":["north","east"]})");
        CHECK(panel.changes == 1);

        // After text that was wrong, text that is right is taken as before.
        panel.type("{}");
        CHECK(panel.settings() == "{}");
        CHECK(panel.changes == 2);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the text of JSON follows the value when something else changes it") {
        RenderContext context;
        Panel panel(context);
        panel.object.settings = unwrap(parseJson(R"({"speed": 2})"));
        panel.frame();

        // An edit that is turned down leaves the value as it was, and the next thing
        // typed starts from that value again.
        panel.refuse = true;
        panel.type(R"({"speed": 9})");
        CHECK(panel.changes == 1);
        CHECK(panel.settings() == R"({"speed":2})");
        panel.refuse = false;

        // Undo, a command, a script: the value is another, and so is what is shown -
        // which typing the same text again then changes back.
        panel.object.settings = unwrap(parseJson(R"({"speed": 5})"));
        panel.frame();
        panel.frame();
        CHECK(panel.changes == 1);
        panel.type(R"({"speed": 9})");
        CHECK(panel.settings() == R"({"speed":9})");
        CHECK(panel.changes == 2);
        checkNoValidationIssues(*context.graphics);
    }
}
