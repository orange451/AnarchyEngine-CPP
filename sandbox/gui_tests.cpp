// The Gui service, the screen GUI classes, and CSS: their properties, their
// events, and how a project saves them.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SceneService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

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
    for (const char* name : {"ScreenGui", "Pane", "ImagePane", "HBox", "VBox", "Label", "Button", "TextField", "CSS"}) {
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

namespace {

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::GameObject& part_at(engine_core::DataModel& game, float x, float y, float z) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_parent(part.id(), game.scene_service("Workspace"));
    part.set_transform(engine_core::matrix4_translation(x, y, z));
    return part;
}

}  // namespace

TEST_CASE("GUI7 BillboardGui is a GuiBase with Adornee and AlwaysOnTop", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::lua_creatable_known("BillboardGui"));
    REQUIRE(engine_core::project_class_known("BillboardGui"));
    REQUIRE(engine_core::lua_class_inherits("BillboardGui", "GuiBase"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("BillboardGui", "ScreenGui"));
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    REQUIRE(std::string(board.class_name()) == "BillboardGui");
    REQUIRE_FALSE(board.always_on_top());
    REQUIRE(board.adornee().kind == engine_core::LuaSlot::Kind::Nil);
    engine_core::PropertyBag saved;
    board.save_properties(saved);
    REQUIRE(saved.empty());
}

TEST_CASE("GUI8 its anchor is the Adornee, else a PVInstance parent, else the origin", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& parent = part_at(game, 1.f, 2.f, 3.f);
    engine_core::GameObject& other = part_at(game, -4.f, 5.f, -6.f);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE(board.anchor_instance() == 0);
    REQUIRE(board.anchor().x == 0.f);

    game.set_parent(board.id(), parent.id());
    REQUIRE(board.anchor_instance() == parent.id());
    REQUIRE(board.anchor().y == 2.f);
    // The parent link is never written to Adornee.
    REQUIRE(board.adornee().kind == engine_core::LuaSlot::Kind::Nil);

    REQUIRE_FALSE(board.set_adornee(instance_slot(other.id())));
    REQUIRE(board.anchor_instance() == other.id());
    REQUIRE(board.anchor().z == -6.f);

    // Moving the part moves the anchor with it.
    other.set_transform(engine_core::matrix4_translation(7.f, 0.f, 0.f));
    REQUIRE(board.anchor().x == 7.f);

    // Moved out from under its parent, with no Adornee, it stops following.
    REQUIRE_FALSE(board.set_adornee(engine_core::LuaSlot{}));
    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE(board.anchor_instance() == 0);
}

TEST_CASE("GUI9 Adornee refuses a non-PVInstance, saves, undoes, and survives its target's destroy", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = part_at(game, 0.f, 9.f, 0.f);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), game.scene_service("Workspace"));
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(*board.set_adornee(instance_slot(folder.id())) == "Adornee must be a PVInstance");

    begin_step(game, "Set Adornee");
    REQUIRE_FALSE(board.set_adornee(instance_slot(part.id())));
    end_step(game);
    engine_core::PropertyBag saved;
    board.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Adornee") != nullptr);
    game.history().undo();
    REQUIRE(board.adornee_id() == 0);
    game.history().redo();
    REQUIRE(board.adornee_id() == part.id());

    begin_step(game, "Delete");
    game.destroy(part.id());
    end_step(game);
    REQUIRE(board.adornee_id() == 0);
    REQUIRE(board.anchor_instance() == 0);
    game.history().undo();
    REQUIRE(board.adornee_id() != 0);
    REQUIRE(board.anchor().y == 9.f);
}

TEST_CASE("GUI10 a BillboardGui is drawn in Workspace or Core, and not inside another GUI", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    REQUIRE_FALSE(board.drawn());
    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE(board.drawn());
    engine_core::GameObject& part = part_at(game, 0.f, 0.f, 0.f);
    game.set_parent(board.id(), part.id());
    REQUIRE(board.drawn());
    // What is in Core stays there, so a separate instance checks that case.
    engine_core::BillboardGui& in_core = game.create<engine_core::BillboardGui>();
    game.set_parent(in_core.id(), game.core());
    REQUIRE(in_core.drawn());
    game.set_parent(board.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(board.drawn());
    game.set_parent(board.id(), game.scene_service("Gui"));
    REQUIRE_FALSE(board.drawn());

    engine_core::BillboardGui& outer = game.create<engine_core::BillboardGui>();
    game.set_parent(outer.id(), game.scene_service("Workspace"));
    game.set_parent(board.id(), outer.id());
    REQUIRE_FALSE(board.drawn());
    engine_core::Pane& pane = game.create<engine_core::Pane>();
    game.set_parent(pane.id(), game.scene_service("Workspace"));
    game.set_parent(board.id(), pane.id());
    REQUIRE_FALSE(board.drawn());
}

TEST_CASE("GUI11 scripts set Adornee and AlwaysOnTop, and both fire Changed", "[gui][billboard]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local part = Instance.new("GameObject", workspace)
        local board = Instance.new("BillboardGui", part)
        _G.defaults = board.AlwaysOnTop == false and board.Adornee == nil and board:IsA("GuiBase")
        local changed = {}
        board.Changed:Connect(function(name) changed[name] = true end)
        board.AlwaysOnTop = true
        board.Adornee = part
        _G.set = board.AlwaysOnTop == true and board.Adornee == part
        _G.refused = not pcall(function() board.Adornee = workspace end)
            and not pcall(function() board.AlwaysOnTop = 3 end)
        task.wait()
        task.wait()
        _G.changed = changed.AlwaysOnTop == true and changed.Adornee == true
        _G.label = pcall(function() Instance.new("Label", board) end)
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    require_globals(rig, {"defaults", "set", "refused", "changed", "label"});
}

TEST_CASE("GUI12 AlwaysOnTop saves, loads, undoes, and comes back at Stop", "[gui][billboard][project]") {
    engine_core::LuaSlot yes;
    yes.kind = engine_core::LuaSlot::Kind::Bool;
    yes.flag = true;
    SECTION("a project saves and loads it") {
        SimRole role;
        TempDir dir;
        {
            engine_core::Project project = engine_core::Project::create(dir.path);
            engine_core::DataModel& game = project.datamodel();
            engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
            game.set_name(board.id(), "Board");
            game.set_parent(board.id(), game.scene_service("Workspace"));
            REQUIRE_FALSE(board.set_value(GuiProperty::AlwaysOnTop, yes));
            project.save();
        }
        engine_core::Game game;
        engine_core::Project loaded = engine_core::Project::load(dir.path, game);
        auto* board = dynamic_cast<engine_core::BillboardGui*>(
            game.instance(game.find_first_child(game.scene_service("Workspace"), "Board")));
        REQUIRE(board != nullptr);
        REQUIRE(board->always_on_top());
    }
    SECTION("undo and redo set it back and forth") {
        SimRole role;
        engine_core::Game game;
        engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
        game.set_parent(board.id(), game.scene_service("Workspace"));
        begin_step(game, "Set AlwaysOnTop");
        REQUIRE_FALSE(board.set_value(GuiProperty::AlwaysOnTop, yes));
        end_step(game);
        game.history().undo();
        REQUIRE_FALSE(board.always_on_top());
        game.history().redo();
        REQUIRE(board.always_on_top());
    }
    SECTION("Stop puts back what it was before a script changed it") {
        ScriptRig rig;
        engine_core::BillboardGui& board = rig.game.create<engine_core::BillboardGui>();
        rig.game.set_name(board.id(), "Board");
        rig.game.set_parent(board.id(), rig.game.scene_service("Workspace"));
        REQUIRE_FALSE(board.set_value(GuiProperty::AlwaysOnTop, yes));
        add_script(rig.game, "Flip", R"(
            workspace.Board.AlwaysOnTop = false
            _G.flipped = workspace.Board.AlwaysOnTop == false
        )");
        rig.game.start_simulation();
        rig.frames(2);
        require_globals(rig, {"flipped"});
        REQUIRE_FALSE(board.always_on_top());
        rig.game.stop_simulation();
        REQUIRE(board.always_on_top());
    }
}

namespace {

// first is ahead of second among their parent's children, so it is saved and loaded first.
bool comes_before(engine_core::DataModel& game, engine_core::InstanceId first, engine_core::InstanceId second) {
    const std::vector<engine_core::InstanceId> children = game.get_children(game.parent(first));
    const auto at_first = std::find(children.begin(), children.end(), first);
    const auto at_second = std::find(children.begin(), children.end(), second);
    return at_first != children.end() && at_second != children.end() && at_first < at_second;
}

}  // namespace

TEST_CASE("GUI13 a loaded Adornee finds a target saved after its BillboardGui", "[gui][billboard][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        // The BillboardGui is made and parented first, so it is saved and
        // loaded before the part its Adornee names.
        engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
        game.set_name(board.id(), "Board");
        game.set_parent(board.id(), game.scene_service("Workspace"));
        engine_core::GameObject& part = part_at(game, 3.f, 4.f, 5.f);
        game.set_name(part.id(), "Target");
        REQUIRE(comes_before(game, board.id(), part.id()));
        REQUIRE_FALSE(board.set_adornee(instance_slot(part.id())));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    const engine_core::InstanceId board_id = game.find_first_child(workspace, "Board");
    const engine_core::InstanceId part_id = game.find_first_child(workspace, "Target");
    REQUIRE(comes_before(game, board_id, part_id));
    auto* board = dynamic_cast<engine_core::BillboardGui*>(game.instance(board_id));
    auto* part = dynamic_cast<engine_core::GameObject*>(game.instance(part_id));
    REQUIRE(board != nullptr);
    REQUIRE(part != nullptr);
    REQUIRE(board->adornee_id() == part_id);
    REQUIRE(board->anchor_instance() == part_id);
    REQUIRE(board->anchor().x == 3.f);
    REQUIRE(board->anchor().z == 5.f);
    part->set_transform(engine_core::matrix4_translation(-1.f, 0.f, 0.f));
    REQUIRE(board->anchor().x == -1.f);
}

TEST_CASE("GUI14 Stop puts back the Adornee a script changed during play", "[gui][billboard]") {
    ScriptRig rig;
    engine_core::GameObject& first = part_at(rig.game, 1.f, 0.f, 0.f);
    rig.game.set_name(first.id(), "First");
    engine_core::GameObject& second = part_at(rig.game, 2.f, 0.f, 0.f);
    rig.game.set_name(second.id(), "Second");
    engine_core::BillboardGui& board = rig.game.create<engine_core::BillboardGui>();
    rig.game.set_name(board.id(), "Board");
    rig.game.set_parent(board.id(), rig.game.scene_service("Workspace"));
    REQUIRE_FALSE(board.set_adornee(instance_slot(first.id())));
    add_script(rig.game, "Move", R"(
        workspace.Board.Adornee = workspace.Second
        _G.moved = workspace.Board.Adornee == workspace.Second
    )");
    rig.game.start_simulation();
    rig.frames(2);
    require_globals(rig, {"moved"});
    REQUIRE(board.adornee_id() == second.id());
    REQUIRE(board.anchor().x == 2.f);
    rig.game.stop_simulation();
    REQUIRE(board.adornee_id() == first.id());
    REQUIRE(board.anchor().x == 1.f);
}

TEST_CASE("GUI15 ImagePane is a Pane with an Image and an ImageTransparency", "[gui][image]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::lua_class_inherits("ImagePane", "GuiBasePane"));
    engine_core::ImagePane& pane = game.create<engine_core::ImagePane>();
    game.set_parent(pane.id(), gui_service(game));
    REQUIRE(pane.vec2(GuiProperty::Size).x == 100.f);
    REQUIRE(pane.vec2(GuiProperty::Size).y == 100.f);
    REQUIRE(pane.number(GuiProperty::ImageTransparency) == 0);
    REQUIRE(pane.image().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(pane.image_texture() == nullptr);
    engine_core::PropertyBag defaults;
    pane.save_properties(defaults);
    REQUIRE(defaults.empty());

    REQUIRE_FALSE(pane.set_value(GuiProperty::ImageTransparency, number(-2)));
    REQUIRE(pane.number(GuiProperty::ImageTransparency) == 0);
    REQUIRE_FALSE(pane.set_value(GuiProperty::ImageTransparency, number(0.25)));
    REQUIRE(pane.number(GuiProperty::ImageTransparency) == 0.25);

    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(*pane.set_image(instance_slot(folder.id())) == "Image must be a Texture");
    engine_core::Texture& texture = game.create<engine_core::Texture>();
    game.set_parent(texture.id(), game.service("Textures"));
    REQUIRE_FALSE(texture.set_path("ui/logo.png"));

    const std::uint64_t before = pane.revision();
    begin_step(game, "Set Image");
    REQUIRE_FALSE(pane.set_image(instance_slot(texture.id())));
    end_step(game);
    REQUIRE(pane.revision() != before);
    REQUIRE(pane.image_texture() == &texture);
    engine_core::PropertyBag saved;
    pane.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Image") != nullptr);
    REQUIRE(engine_core::bag_find(saved, "ImageTransparency") != nullptr);
    game.history().undo();
    REQUIRE(pane.image_texture() == nullptr);
    game.history().redo();
    REQUIRE(pane.image_texture() == &texture);
    REQUIRE_FALSE(pane.set_image(engine_core::LuaSlot{}));
    REQUIRE(pane.image_texture() == nullptr);
}

TEST_CASE("GUI16 a project saves and loads an ImagePane's Image", "[gui][image][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
        game.set_parent(screen.id(), gui_service(game));
        engine_core::ImagePane& pane = game.create<engine_core::ImagePane>();
        game.set_name(pane.id(), "Logo");
        game.set_parent(pane.id(), screen.id());
        engine_core::Texture& texture = game.create<engine_core::Texture>();
        game.set_name(texture.id(), "Logo");
        game.set_parent(texture.id(), game.service("Textures"));
        REQUIRE_FALSE(texture.set_path("ui/logo.png"));
        REQUIRE_FALSE(pane.set_image(instance_slot(texture.id())));
        REQUIRE_FALSE(pane.set_value(GuiProperty::ImageTransparency, number(0.5)));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const engine_core::InstanceId screen = game.find_first_child(gui_service(game), "ScreenGui");
    auto* pane = dynamic_cast<engine_core::ImagePane*>(game.instance(game.find_first_child(screen, "Logo")));
    REQUIRE(pane != nullptr);
    REQUIRE(pane->number(GuiProperty::ImageTransparency) == 0.5);
    const engine_core::Texture* texture = pane->image_texture();
    REQUIRE(texture != nullptr);
    REQUIRE(texture->path() == "ui/logo.png");
}

TEST_CASE("GUI17 scripts set an ImagePane's Image and ImageTransparency", "[gui][image]") {
    ScriptRig rig;
    engine_core::Texture& logo = rig.game.create<engine_core::Texture>();
    rig.game.set_name(logo.id(), "Logo");
    rig.game.set_parent(logo.id(), rig.game.service("Textures"));
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        local pane = Instance.new("ImagePane", screen)
        _G.defaults = pane.Image == nil and pane.ImageTransparency == 0 and pane.Size == Vector2.new(100, 100)
            and pane:IsA("GuiBasePane")
        pane.Image = game.Assets.Textures.Logo
        pane.ImageTransparency = 3
        _G.set = pane.Image == game.Assets.Textures.Logo and pane.ImageTransparency == 1
        _G.typed = not pcall(function() pane.Image = screen end)
        pane.Image = nil
        _G.cleared = pane.Image == nil
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"defaults", "set", "typed", "cleared"});
}

TEST_CASE("GUI18 Slider holds Value inside Min and Max, on its Step", "[gui][slider]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local slider = Instance.new("Slider")
        _G.defaults = slider:IsA("GuiBase") and slider.Min == 0 and slider.Max == 1
            and slider.Value == 0 and slider.Step == 0
        slider.Max = 10
        slider.Value = 15
        _G.clamped = slider.Value == 10
        slider.Value = -3
        _G.low = slider.Value == 0
        slider.Step = 2
        slider.Value = 4.9
        _G.stepped = slider.Value == 4
        slider.Min = 1
        _G.fromMin = slider.Value == 5
        slider.Value = 9.6
        _G.top = slider.Value == 9
        slider.Max = 3
        _G.shrunk = slider.Value == 3
        slider.Min = 7
        _G.raised = slider.Max == 7 and slider.Value == 7
        local changed = false
        slider.Changed:Connect(function(name) if name == "Value" then changed = true end end)
        slider.Min = 0
        slider.Max = 10
        slider.Step = 0
        slider.Value = 2.5
        _G.free = slider.Value == 2.5
        task.wait()
        task.wait()
        _G.changed = changed
        _G.refused = not pcall(function() slider.Value = 0 / 0 end)
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    require_globals(rig, {"defaults", "clamped", "low", "stepped", "fromMin", "top", "shrunk", "raised", "free",
                          "changed", "refused"});
}
