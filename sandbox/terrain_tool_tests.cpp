// The studio's terrain tools, a built-in plugin: its cards, its pane, and
// Add's box. Grow, Smooth, and Paint pick the Terrain with a ray, which needs
// its colliders; the sandbox has none, so those are checked live in the studio.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Gui.hpp"
#include "PluginUi.hpp"
#include "SceneService.hpp"
#include "SelectionService.hpp"
#include "Terrain.hpp"
#include "ide/PluginLoader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

ide::PluginFile terrain_tool_file() {
    ide::PluginFile file;
    std::string error;
    REQUIRE(ide::read_plugin_file(std::filesystem::path(ANARCHY_SOURCE_DIR) / "resources/plugins/TerrainTool.luau",
                                  file, error));
    return file;
}

// The terrain tools loaded, a Terrain in Workspace, and a camera 10 units over
// the origin looking straight down with a 90 degree view, 200 x 200 points:
// the view's middle is the origin, and 50 points off it are 5 units.
struct TerrainToolRig {
    ScriptRig rig;
    ide::PluginLoader loader;
    InstanceId terrain = 0;

    explicit TerrainToolRig(bool withTerrain = true) {
        const InstanceId workspace = rig.game.scene_service("Workspace");
        engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
        rig.game.set_parent(camera.id(), workspace);
        camera.set_field_of_view(90);
        camera.set_viewport_size(engine_core::Vec2{200, 200});
        camera.set_transform(engine_core::matrix4_look_at(
            engine_core::Vec3{0.f, 10.f, 0.f}, engine_core::Vec3{0.f, 0.f, 0.f}, engine_core::Vec3{0.f, 0.f, -1.f}));
        dynamic_cast<engine_core::Workspace*>(rig.game.instance(workspace))->set_current_camera(camera.id());
        if (withTerrain) {
            engine_core::Terrain& made = rig.game.create<engine_core::Terrain>();
            rig.game.set_parent(made.id(), workspace);
            terrain = made.id();
            rig.game.selection().set({terrain});
        }
        rig.game.history().reset_waypoints();
        REQUIRE(loader.load(rig.game, rig.runtime, {terrain_tool_file()}) == 1);
        rig.frames(1);
    }
    std::vector<std::string> cards() {
        std::vector<std::string> out;
        for (const engine_core::PluginToolbarState& bar : rig.runtime.plugin_ui().toolbars()) {
            if (bar.name == "Terrain") {
                for (const engine_core::PluginButtonState& button : bar.buttons) {
                    out.push_back(button.key);
                }
            }
        }
        return out;
    }
    void click(const char* key) {
        bool clicked = false;
        for (const engine_core::PluginToolbarState& bar : rig.runtime.plugin_ui().toolbars()) {
            for (const engine_core::PluginButtonState& button : bar.buttons) {
                if (bar.name == "Terrain" && button.key == key) {
                    clicked = rig.runtime.plugin_ui().click(button.id);
                }
            }
        }
        REQUIRE(clicked);
        rig.frames(1);
    }
    // The lit card, or "" when none is.
    std::string lit() {
        for (const engine_core::PluginToolbarState& bar : rig.runtime.plugin_ui().toolbars()) {
            for (const engine_core::PluginButtonState& button : bar.buttons) {
                if (bar.name == "Terrain" && button.active) {
                    return button.key;
                }
            }
        }
        return "";
    }
    // The text of each Label and Button the pane shows: it and every GUI above it Visible.
    std::vector<std::string> shown() {
        std::vector<std::string> out;
        for (InstanceId id = 1; id < 100000; ++id) {
            const auto* item = dynamic_cast<const engine_core::GuiValues*>(rig.game.instance(id));
            if ((dynamic_cast<const engine_core::Label*>(item) == nullptr &&
                 dynamic_cast<const engine_core::Button*>(item) == nullptr) ||
                rig.game.parent(id) == 0) {
                continue;
            }
            bool visible = true;
            for (InstanceId at = id; at != 0; at = rig.game.parent(at)) {
                const auto* gui = dynamic_cast<const engine_core::GuiValues*>(rig.game.instance(at));
                if (gui == nullptr || dynamic_cast<const engine_core::DockWidget*>(gui) != nullptr) {
                    break;
                }
                visible = visible && gui->flag(engine_core::GuiProperty::Visible);
            }
            if (visible) {
                out.push_back(item->text(engine_core::GuiProperty::Text));
            }
        }
        return out;
    }
    bool showing(const std::string& text) {
        const std::vector<std::string> texts = shown();
        return std::find(texts.begin(), texts.end(), text) != texts.end();
    }
    // Presses the pane's Button with this text, as a click on it does.
    void press(const std::string& text) {
        for (InstanceId id = 1; id < 100000; ++id) {
            const auto* button = dynamic_cast<const engine_core::Button*>(rig.game.instance(id));
            if (button != nullptr && button->text(engine_core::GuiProperty::Text) == text) {
                rig.game.fire_event(id, engine_core::kGuiAction);
                rig.frames(1);
                return;
            }
        }
        FAIL("no Button " << text);
    }
    // Moves the Slider beside the pane's label that starts with row, such as "Grid", as a drag does.
    void slide(const std::string& row, double value) {
        for (InstanceId id = 1; id < 100000; ++id) {
            const auto* label = dynamic_cast<const engine_core::Label*>(rig.game.instance(id));
            if (label == nullptr || label->text(engine_core::GuiProperty::Text).rfind(row, 0) != 0) {
                continue;
            }
            for (InstanceId child : rig.game.get_children(rig.game.parent(id))) {
                if (auto* slider = dynamic_cast<engine_core::Slider*>(rig.game.instance(child))) {
                    engine_core::LuaSlot slot;
                    slot.kind = engine_core::LuaSlot::Kind::Number;
                    slot.number = value;
                    REQUIRE_FALSE(slider->set_value(engine_core::GuiProperty::Value, slot));
                    rig.frames(2);
                    return;
                }
            }
        }
        FAIL("no Slider beside " << row);
    }
    bool pane_open() {
        for (InstanceId id = 1; id < 100000; ++id) {
            if (const auto* widget = dynamic_cast<const engine_core::DockWidget*>(rig.game.instance(id))) {
                return widget->enabled();
            }
        }
        FAIL("no DockWidget");
        return false;
    }
    void mouse(engine_core::PluginMouseEvent::Kind kind, float x, float y, bool shift = false) {
        engine_core::PluginMouseEvent event;
        event.kind = kind;
        event.x = x;
        event.y = y;
        event.shift = shift;
        // As GameView casts it: from the camera, through the point, 50 points off the middle per 5 units.
        const float dx = (x - 100.f) / 100.f, dz = (y - 100.f) / 100.f;
        const float length = std::sqrt(dx * dx + 1.f + dz * dz);
        event.origin = engine_core::Vec3{0.f, 10.f, 0.f};
        event.direction = engine_core::Vec3{dx / length, -1.f / length, dz / length};
        rig.runtime.plugin_mouse_event(event);
        rig.frames(1);
    }
    // Whether the cell nearest the world point is solid.
    bool solid(float x, float y, float z) {
        rig.runtime.run_chunk("local t = workspace:FindFirstChild('Terrain')\n"
                              "local c = t:WorldToCell(Vector3.new(" +
                              std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) +
                              "))\n"
                              "print(t:ReadVoxels(c, c).Distances[1][1][1] < 0)\n");
        rig.frames(1);
        const auto out = rig.runtime.drain_output();
        INFO(rig.runtime.last_error());
        REQUIRE_FALSE(out.lines.empty());
        REQUIRE(rig.runtime.last_error().empty());
        return out.lines.back().text == "true\n";
    }
};

}  // namespace

TEST_CASE("TB1 the terrain tools are four cards, Add Grow Smooth Paint, none lit at first", "[TB1]") {
    TerrainToolRig tools;
    INFO(tools.rig.runtime.last_error());
    REQUIRE(tools.cards() == std::vector<std::string>{"Add", "Grow", "Smooth", "Paint"});
    REQUIRE(tools.lit().empty());
    REQUIRE_FALSE(tools.pane_open());
}

TEST_CASE("TB2 a card turns its tool on with the pane; another switches; the lit one turns them off", "[TB2]") {
    TerrainToolRig tools;
    tools.click("Grow");
    REQUIRE(tools.lit() == "Grow");
    REQUIRE(tools.pane_open());
    REQUIRE(tools.rig.runtime.plugin_ui().active() != 0);
    tools.click("Smooth");
    REQUIRE(tools.lit() == "Smooth");
    tools.click("Smooth");
    REQUIRE(tools.lit().empty());
    REQUIRE_FALSE(tools.pane_open());
    REQUIRE(tools.rig.runtime.plugin_ui().active() == 0);
}

TEST_CASE("TB3 Add draws as the Brushes tool does: a footprint on the grid, then the height, then a click fills it",
          "[TB3]") {
    TerrainToolRig tools;
    tools.click("Add");
    REQUIRE_FALSE(tools.solid(0, 2, 0));
    using Kind = engine_core::PluginMouseEvent::Kind;
    // The grid from 1 unit to 4, its third notch.
    tools.slide("Grid", 3);
    // From (-5, 0, -5) to (5, 0, 5) on the ground, which the 4 unit grid makes -4 to 4.
    tools.mouse(Kind::Move, 50, 50);
    tools.mouse(Kind::Button1Down, 50, 50);
    tools.mouse(Kind::Move, 150, 150);
    tools.mouse(Kind::Button1Up, 150, 150);
    INFO(tools.rig.runtime.last_error());
    // Let go: nothing filled yet, only the height to set.
    REQUIRE_FALSE(tools.solid(0, 2, 0));
    // Looking straight down, the height is one grid step.
    tools.mouse(Kind::Move, 150, 150);
    tools.mouse(Kind::Button1Down, 150, 150);
    tools.mouse(Kind::Button1Up, 150, 150);
    REQUIRE(tools.solid(0, 2, 0));
    REQUIRE(tools.solid(3, 3, 3));
    REQUIRE_FALSE(tools.solid(0, 6, 0));
    REQUIRE_FALSE(tools.solid(5, 2, 0));
    REQUIRE(tools.rig.game.history().can_undo().second == "Sculpt Terrain");
    tools.rig.game.history().undo();
    tools.rig.frames(1);
    REQUIRE_FALSE(tools.solid(0, 2, 0));
}

TEST_CASE("TB6 Shift while setting Add's height makes the box a cube", "[TB6]") {
    TerrainToolRig tools;
    tools.click("Add");
    tools.slide("Grid", 3);
    using Kind = engine_core::PluginMouseEvent::Kind;
    tools.mouse(Kind::Move, 50, 50);
    tools.mouse(Kind::Button1Down, 50, 50);
    tools.mouse(Kind::Move, 150, 150);
    tools.mouse(Kind::Button1Up, 150, 150);
    tools.mouse(Kind::Move, 150, 150, true);
    tools.mouse(Kind::Button1Down, 150, 150, true);
    tools.mouse(Kind::Button1Up, 150, 150, true);
    INFO(tools.rig.runtime.last_error());
    REQUIRE(tools.solid(0, 6, 0));
    REQUIRE_FALSE(tools.solid(0, 10, 0));
}

TEST_CASE("TB7 with no Terrain selected the pane hides its controls and offers Insert terrain", "[TB7]") {
    TerrainToolRig tools(false);
    tools.click("Grow");
    INFO(tools.rig.runtime.last_error());
    REQUIRE(tools.showing("No terrain selected"));
    REQUIRE(tools.showing("Insert terrain"));
    REQUIRE_FALSE(tools.showing("Size 8 units"));
    // Insert makes one in Workspace, selects it, and is one undo step.
    tools.press("Insert terrain");
    const InstanceId workspace = tools.rig.game.scene_service("Workspace");
    const InstanceId made = tools.rig.game.find_first_child(workspace, "Terrain");
    REQUIRE(made != 0);
    REQUIRE(tools.rig.game.selection().get() == std::vector<InstanceId>{made});
    REQUIRE(tools.showing("Size 8 units"));
    REQUIRE_FALSE(tools.showing("No terrain selected"));
    REQUIRE(tools.rig.game.history().can_undo().second == "Insert Terrain");
    tools.rig.game.history().undo();
    tools.rig.frames(1);
    REQUIRE(tools.rig.game.find_first_child(workspace, "Terrain") == 0);
    REQUIRE(tools.showing("No terrain selected"));
}

TEST_CASE("TB8 the tools work only on the selected Terrain; Add with none selected fills nothing", "[TB8]") {
    TerrainToolRig tools;
    tools.rig.game.selection().set({});
    tools.rig.frames(1);
    tools.click("Add");
    using Kind = engine_core::PluginMouseEvent::Kind;
    tools.mouse(Kind::Move, 50, 50);
    tools.mouse(Kind::Button1Down, 50, 50);
    tools.mouse(Kind::Move, 150, 150);
    tools.mouse(Kind::Button1Up, 150, 150);
    tools.mouse(Kind::Button1Down, 150, 150);
    tools.mouse(Kind::Button1Up, 150, 150);
    INFO(tools.rig.runtime.last_error());
    REQUIRE_FALSE(tools.solid(0, 2, 0));
    REQUIRE(tools.showing("No terrain selected"));
}

TEST_CASE("TB4 Play turns the terrain tools off and closes the pane", "[TB4]") {
    TerrainToolRig tools;
    tools.click("Paint");
    REQUIRE(tools.lit() == "Paint");
    tools.rig.game.capture_place();
    tools.rig.game.start_simulation();
    tools.rig.frames(1);
    REQUIRE(tools.lit().empty());
    REQUIRE(tools.rig.runtime.plugin_ui().active() == 0);
    tools.rig.game.stop_simulation();
    tools.rig.frames(1);
    REQUIRE(tools.lit().empty());
}

TEST_CASE("TB5 the pane steps the size and names the Terrain", "[TB5]") {
    TerrainToolRig tools;
    tools.click("Grow");
    std::vector<std::string> texts;
    for (InstanceId id = 1; id < 100000; ++id) {
        if (const auto* label = dynamic_cast<const engine_core::Label*>(tools.rig.game.instance(id))) {
            texts.push_back(label->text(engine_core::GuiProperty::Text));
        }
    }
    INFO(tools.rig.runtime.last_error());
    REQUIRE(std::find(texts.begin(), texts.end(), "Terrain: Terrain") != texts.end());
    REQUIRE(std::find(texts.begin(), texts.end(), "Size 8 units") != texts.end());
    REQUIRE(std::find(texts.begin(), texts.end(), "Strength 50%") != texts.end());
}

TEST_CASE("TB9 the pane's sliders set the size, the strength, and the grid", "[TB9]") {
    TerrainToolRig tools;
    tools.click("Grow");
    REQUIRE(tools.showing("Size 8 units"));
    tools.slide("Size", 16.4);
    REQUIRE(tools.showing("Size 16 units"));
    tools.slide("Strength", 0.73);
    REQUIRE(tools.showing("Strength 75%"));
    tools.slide("Grid", 5);
    REQUIRE(tools.showing("Grid 16 units"));
}
