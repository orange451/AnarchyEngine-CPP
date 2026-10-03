// The Gui service, the screen GUI classes, and CSS: their properties, their
// events, and how a project saves them.

#include "support.hpp"

#include "ChangeHistoryService.hpp"
#include "Enum.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SceneService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <string>

namespace {

using engine_core::GuiProperty;

void require_globals(ScriptRig& rig, std::initializer_list<const char*> names) {
    INFO(rig.runtime.last_error());
    for (const char* name : names) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

engine_core::InstanceId gui_service(engine_core::DataModel& game) { return game.scene_service("Gui"); }

engine_core::LuaSlot number(double value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

engine_core::LuaSlot vec2(float x, float y) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Vec2;
    slot.vec = engine_core::Vec3{x, y, 0.f};
    return slot;
}

}  // namespace

TEST_CASE("GUI1 game has a Gui service, after Scripts", "[gui]") {
    engine_core::Game game;
    const engine_core::InstanceId gui = gui_service(game);
    REQUIRE(gui != 0);
    REQUIRE(game.guid(gui) == "gui");
    REQUIRE(game.name(gui) == "Gui");
    REQUIRE(game.instance(gui)->is_scene_service());
    REQUIRE(game.get_children(0)[4] == gui);
    REQUIRE(engine_core::lua_service_known("Gui"));
}

TEST_CASE("GUI2 the classes, their bases, and their defaults", "[gui]") {
    SimRole role;
    engine_core::Game game;
    for (const char* name : {"ScreenGui", "Pane", "HBox", "VBox", "Label", "Button", "TextField", "CSS"}) {
        INFO(name);
        REQUIRE(engine_core::lua_creatable_known(name));
        REQUIRE(engine_core::project_class_known(name));
    }
    REQUIRE_FALSE(engine_core::lua_creatable_known("GuiBase"));
    REQUIRE_FALSE(engine_core::lua_creatable_known("GuiBasePane"));
    REQUIRE(engine_core::lua_class_inherits("Pane", "GuiBasePane"));
    REQUIRE(engine_core::lua_class_inherits("HBox", "GuiBase"));
    REQUIRE(engine_core::lua_class_inherits("Label", "GuiBase"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("Label", "GuiBasePane"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("CSS", "GuiBase"));

    engine_core::Pane& pane = game.create<engine_core::Pane>();
    REQUIRE(pane.vec2(GuiProperty::Size).x == 100.f);
    REQUIRE(pane.vec2(GuiProperty::Size).y == 100.f);
    REQUIRE(pane.flag(GuiProperty::Visible));
    REQUIRE(pane.color(GuiProperty::BackgroundColor).g == 1.f);
    engine_core::Label& label = game.create<engine_core::Label>();
    REQUIRE(label.text(GuiProperty::Text) == "Label");
    REQUIRE(label.number(GuiProperty::FontSize) == 16);
    REQUIRE(label.vec2(GuiProperty::Size).x == 0.f);
    REQUIRE(game.create<engine_core::Button>().text(GuiProperty::Text) == "Button");
    REQUIRE(game.create<engine_core::TextField>().text(GuiProperty::Prompt) == "Prompt");
    REQUIRE(game.create<engine_core::Css>().source() == "/* CSS Document */");

    // Defaults are not saved.
    engine_core::PropertyBag saved;
    pane.save_properties(saved);
    REQUIRE(saved.empty());
}

TEST_CASE("GUI3 writes are checked and clamped, undo, and come back at Stop", "[gui]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Pane& pane = game.create<engine_core::Pane>();
    game.set_parent(pane.id(), gui_service(game));

    REQUIRE_FALSE(pane.set_value(GuiProperty::BackgroundTransparency, number(3)));
    REQUIRE(pane.number(GuiProperty::BackgroundTransparency) == 1);
    REQUIRE(*pane.set_value(GuiProperty::BackgroundTransparency, number(std::nan(""))) ==
            "BackgroundTransparency must be a finite number");
    REQUIRE(*pane.set_value(GuiProperty::Size, number(4)) == "Size must be a finite Vector2");
    REQUIRE_FALSE(pane.set_value(GuiProperty::Size, vec2(-5.f, 40.f)));
    REQUIRE(pane.vec2(GuiProperty::Size).x == 0.f);
    REQUIRE(pane.vec2(GuiProperty::Size).y == 40.f);

    const std::uint64_t before = pane.revision();
    begin_step(game, "Set Size");
    REQUIRE_FALSE(pane.set_value(GuiProperty::Size, vec2(20.f, 30.f)));
    end_step(game);
    REQUIRE(pane.revision() != before);
    game.history().undo();
    REQUIRE(pane.vec2(GuiProperty::Size).y == 40.f);
    game.history().redo();
    REQUIRE(pane.vec2(GuiProperty::Size).x == 20.f);

    engine_core::PropertyBag saved;
    pane.save_properties(saved);
    const engine_core::JsonValue* size = engine_core::bag_find(saved, "Size");
    REQUIRE(size != nullptr);
    REQUIRE(engine_core::write_json(*size) == "[20, 30]\n");

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(pane.set_text(GuiProperty::ClassList, "big red"));
    game.stop_simulation();
    REQUIRE(pane.text(GuiProperty::ClassList).empty());
    REQUIRE(pane.vec2(GuiProperty::Size).x == 20.f);
}

TEST_CASE("GUI4 scripts make GUIs and set them", "[gui]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        _G.service = game:GetService("Gui") == game.Gui and screen:IsA("GuiBase")
        local box = Instance.new("VBox", screen)
        _G.box = box:IsA("GuiBasePane") and box.Spacing == 0 and box.Size == Vector2.new(0, 0)
            and box.Alignment == Enum.GuiAlignment.TopLeft
        box.Size = Vector2.new(200, 50)
        box.Alignment = "Center"
        box.BackgroundColor = Color3.new(0, 0, 1)
        box.Spacing = 4
        _G.set = box.Size == Vector2.new(200, 50) and box.Alignment == Enum.GuiAlignment.Center
            and box.BackgroundColor == Color3.new(0, 0, 1) and box.Spacing == 4
        _G.typed = not pcall(function() box.Size = Vector3.new(1, 2, 3) end)
            and not pcall(function() box.Alignment = "Sideways" end)
        local label = Instance.new("Label", box)
        label.Text = "Score: 0"
        label.FontSize = 1000
        _G.label = label.Text == "Score: 0" and label.FontSize == 512 and label.TextColor == Color3.new(0, 0, 0)
        local sheet = Instance.new("CSS", screen)
        sheet.Source = "label { color: red; }"
        _G.css = sheet.Source == "label { color: red; }" and not sheet:IsA("GuiBase")
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"service", "box", "set", "typed", "label", "css"});
}

TEST_CASE("GUI5 a GuiBase's events reach scripts, and a Button's Action", "[gui]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        local button = Instance.new("Button", screen)
        button.Name = "Play"
        _G.clicks = 0
        _G.actions = 0
        button.MouseClicked:Connect(function(...)
            _G.clicks += 1
            _G.no_args = select("#", ...) == 0
        end)
        button.Action:Connect(function() _G.actions += 1 end)
        task.spawn(function()
            button.MouseEntered:Wait()
            _G.entered = true
        end)
        _G.label_has_no_action = not pcall(function() return Instance.new("Label").Action end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    const engine_core::InstanceId screen = rig.game.find_first_child(gui_service(rig.game), "ScreenGui");
    const engine_core::InstanceId button = rig.game.find_first_child(screen, "Play");
    REQUIRE(button != 0);
    rig.game.fire_event(button, engine_core::kGuiMouseClicked);
    rig.game.fire_event(button, engine_core::kGuiMouseClicked);
    rig.game.fire_event(button, engine_core::kGuiAction);
    rig.game.fire_event(button, engine_core::kGuiMouseEntered);
    // An event nothing listens to, and one the class does not have, do nothing.
    rig.game.fire_event(button, engine_core::kGuiMouseExited);
    rig.game.fire_event(button, "Nonsense");
    rig.frames(1, 0.05);
    double clicks = 0;
    double actions = 0;
    REQUIRE(rig.runtime.global_number("clicks", clicks));
    REQUIRE(rig.runtime.global_number("actions", actions));
    REQUIRE(clicks == 2);
    REQUIRE(actions == 1);
    require_globals(rig, {"no_args", "entered", "label_has_no_action"});

    // Destroying the Button ends its connections with it.
    rig.game.destroy(button);
    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_number("clicks", clicks));
    REQUIRE(clicks == 2);
}

TEST_CASE("GUI6 a project saves and loads GUIs and CSS", "[gui][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
        game.set_parent(screen.id(), gui_service(game));
        engine_core::HBox& row = game.create<engine_core::HBox>();
        game.set_name(row.id(), "Row");
        game.set_parent(row.id(), screen.id());
        REQUIRE_FALSE(row.set_value(GuiProperty::Size, vec2(300.f, 24.f)));
        REQUIRE_FALSE(row.set_text(GuiProperty::ClassList, "toolbar"));
        engine_core::Css& sheet = game.create<engine_core::Css>();
        game.set_parent(sheet.id(), screen.id());
        REQUIRE_FALSE(sheet.set_text(GuiProperty::Source, ".toolbar {\n  spacing: 8px;\n}\n"));
        project.save();
        REQUIRE_FALSE(project.unsaved());
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    REQUIRE_FALSE(loaded.unsaved());
    const engine_core::InstanceId screen = game.find_first_child(gui_service(game), "ScreenGui");
    REQUIRE(screen != 0);
    auto* row = dynamic_cast<engine_core::HBox*>(game.instance(game.find_first_child(screen, "Row")));
    REQUIRE(row != nullptr);
    REQUIRE(row->vec2(GuiProperty::Size).x == 300.f);
    REQUIRE(row->text(GuiProperty::ClassList) == "toolbar");
    auto* sheet = dynamic_cast<engine_core::Css*>(game.instance(game.find_first_child(screen, "CSS")));
    REQUIRE(sheet != nullptr);
    REQUIRE(sheet->source() == ".toolbar {\n  spacing: 8px;\n}\n");
}
