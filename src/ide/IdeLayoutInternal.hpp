#pragma once

// Internal to the IdeLayout*.cpp files: the constants, small helpers, and
// nested types they share. Nothing else includes this.

#include "IdeLayout.hpp"
#include "Environment.hpp"
#include "SelectionService.hpp"
#include "ChangeHistoryService.hpp"
#include "CutSet.hpp"
#include "DataModelLock.hpp"
#include "DockArrange.hpp"
#include "Engine.hpp"
#include "IdeConsole.hpp"
#include "IdeIcons.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "ScriptAnalysis.hpp"
#include "SavedLayout.hpp"
#include "ScriptRuntime.hpp"
#include "IdeDock.hpp"
#include "IdeConflicts.hpp"
#include "IdeExplorer.hpp"
#include "IdePrefabEditor.hpp"
#include "IdeScriptEditor.hpp"
#include "IdeSearch.hpp"
#include "IdeTerminal.hpp"
#include "IdeTheme.hpp"
#include "PreferencesPanel.hpp"
#include "PropertiesPanel.hpp"
#include "AssetInstances.hpp"
#include "LuaSource.hpp"
#include "IdeResources.hpp"
#include "McpServer.hpp"
#include "McpTools.hpp"
#include "StudioRegistry.hpp"
#include "UiCalls.hpp"
#include "runner/GameView.hpp"
#include "runner/ViewCapture.hpp"
#include <algorithm>
#include <cstdio>
#include <random>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace ide {
namespace layout_detail {

// MenuBar's title row is 28 points. The ribbon under it is 32. The status
// strip below is 24, matching the Java toolbar.
constexpr double kMenuHeight = 28;
constexpr double kRibbonHeight = 32;
constexpr double kStatusHeight = 24;
constexpr double kSideWidth = 240;
constexpr double kConsoleHeight = 150;

// JadeFX key codes match GLFW. This is GLFW_KEY_F5.
constexpr int kKeyF5 = 294;
constexpr std::uint64_t kCommandUndo = 1;

template <typename T>
inline T* Owning(jadefx::Node* node) {
    for (jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (auto* hit = dynamic_cast<T*>(cursor)) {
            return hit;
        }
    }
    return nullptr;
}

inline std::string Counted(std::size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// "Loaded 3 changes from disk: Part, Door, and 1 more".
inline std::string LoadedText(const std::vector<std::string>& names) {
    if (names.size() == 1) {
        return "Loaded a change from disk: " + names[0];
    }
    if (names.size() == 2) {
        return "Loaded 2 changes from disk: " + names[0] + " and " + names[1];
    }
    return "Loaded " + std::to_string(names.size()) + " changes from disk: " + names[0] + ", " + names[1] + ", and " +
           std::to_string(names.size() - 2) + " more";
}

// One row of the save gate: "Part · Color", or what happened to the whole instance.
inline std::string ConflictLine(const engine_core::SaveConflict& row) {
    std::string what = row.key;
    if (what.empty()) {
        switch (row.kind) {
        case engine_core::SaveConflict::Kind::DeletedOutside:
            what = "deleted on disk";
            break;
        case engine_core::SaveConflict::Kind::MovedOutside:
            what = "moved on disk";
            break;
        case engine_core::SaveConflict::Kind::AddedOutside:
            what = "added on disk";
            break;
        case engine_core::SaveConflict::Kind::EditedOutside:
            what = row.disk == "can't be read" ? "can't be read" : "changed on disk";
            break;
        }
    }
    return (row.name.empty() ? row.path : row.name) + " \u00b7 " + what;
}

inline bool InTextWidget(jadefx::Node* node) {
    for (jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (dynamic_cast<jadefx::StyledTextArea*>(cursor) != nullptr || dynamic_cast<jadefx::TextField*>(cursor) != nullptr) {
            return true;
        }
    }
    return false;
}

inline KeyChord ChordOf(const jadefx::KeyEvent& event) {
    KeyChord chord;
    chord.primary = event.shortcut();
    chord.shift = event.shift;
    chord.alt = event.alt;
#if defined(__APPLE__)
    chord.apple = true;
#else
    chord.apple = false;
#endif
    if (event.key == jadefx::Key::Z) {
        chord.key = ChordKey::Z;
    } else if (event.key == jadefx::Key::Y) {
        chord.key = ChordKey::Y;
    }
    return chord;
}

inline void CloseGesture(engine_core::DataModel& world) { world.history().end_gesture(); }

// Every color is a variable of the current theme, which set_current_theme puts in
// JadeFX's user-agent stylesheet. See resources/themes/light.css.
constexpr const char* kStylesheet = R"CSS(
scene {
    background-color: var(--ide-window-color);
    font-family: "Open Sans";
    font-size: 13px;
    color: var(--ide-text-color);
}
.ide-root {
    background-color: var(--ide-window-color);
}
.ide-status {
    background-color: var(--ide-status-bar-color);
}
.ide-ribbon {
    background-color: var(--ide-ribbon-color);
    border-width: 0 0 1px 0;
    border-color: var(--ide-ribbon-border-color);
    padding: 3px 6px;
}
.ide-ribbon-button {
    padding: 0 8px;
    border-radius: 6px;
    transition: background-color 0.12s, opacity 0.12s;
}
.ide-ribbon-button:hover {
    background-color: var(--ide-ribbon-hover-color);
}
.ide-ribbon-button:active {
    background-color: var(--ide-ribbon-pressed-color);
}
.ide-ribbon-button.on {
    background-color: var(--ide-ribbon-pressed-color);
}
.ide-ribbon-button:disabled {
    background-color: transparent;
    opacity: 0.4;
}
.ide-viewport {
    background-color: var(--ide-viewport-color);
}
.ide-fps {
    color: var(--ide-fps-text-color);
    padding: 6px 8px;
    background-color: var(--ide-fps-color);
}
textfield {
    background-color: var(--ide-field-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--ide-field-border-color);
    border-radius: 4px;
    padding: 6px 8px;
    transition: border-color 0.12s;
}
styleclassedtextarea {
    background-color: var(--ide-panel-color);
    padding: 6px 8px;
}
codearea {
    background-color: var(--ide-editor-color);
    color: var(--ide-editor-text-color);
    font-family: "Editor Mono";
    font-size: 14px;
    padding: 8px;
}
split-pane:horizontal > .split-pane-divider,
split-pane:vertical > .split-pane-divider {
    padding: 0 2px;
    background-color: var(--divider-color);
}
.toast {
    border-radius: 6px;
    box-shadow: 0px 4px 14px 0px var(--ide-popup-shadow-color);
}
)CSS";

inline void AttachIcon(jadefx::MenuItem& item, const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return;
    }
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(filename)) {
        item.setGraphic(std::move(icon));
    }
}

inline jadefx::MenuItem* AddItem(jadefx::Menu& menu, const char* label, const char* icon, int key, int mods) {
    auto item = jadefx::make<jadefx::MenuItem>(label);
    AttachIcon(*item, icon);
    if (key != 0) {
        item->setAccelerator(key, mods);
    }
    jadefx::MenuItem* raw = item.get();
    menu.getItems().add(std::move(item));
    return raw;
}

inline double Fraction(double part, double whole, double limit) {
    if (whole <= 1.0) {
        return limit;
    }
    double fraction = part / whole;
    if (fraction < 0.05) {
        fraction = 0.05;
    }
    if (fraction > limit) {
        fraction = limit;
    }
    return fraction;
}

// The stage hands over each key its scene did not consume.
inline void LeaveFieldsOnEscape(jadefx::Stage& stage) {
    jadefx::Stage* raw = &stage;
    stage.setOnKey([raw](int key, bool pressed) { leave_field_on_escape(raw->getScene(), key, pressed); });
}

inline void StretchRoot(jadefx::Node& node) {
    node.setPrefWidthRatio(1);
    node.setPrefHeightRatio(1);
}

inline DropSide SideOf(DockZone zone) {
    switch (zone) {
        case DockZone::Left:
            return DropSide::Left;
        case DockZone::Right:
            return DropSide::Right;
        case DockZone::Top:
            return DropSide::Top;
        case DockZone::Bottom:
            return DropSide::Bottom;
        case DockZone::Header:
        case DockZone::Center:
        case DockZone::Outside:
            return DropSide::Left;
    }
    return DropSide::Left;
}

inline Box ClampBox(Box box, double sceneW, double sceneH) {
    if (sceneW > 1.0 && box.width > sceneW) {
        box.width = sceneW;
    }
    if (sceneH > 1.0 && box.height > sceneH) {
        box.height = sceneH;
    }
    if (box.x < 0.0) {
        box.x = 0.0;
    }
    if (box.y < 0.0) {
        box.y = 0.0;
    }
    if (sceneW > 1.0 && box.x + box.width > sceneW) {
        box.x = std::max(0.0, sceneW - box.width);
    }
    if (sceneH > 1.0 && box.y + box.height > sceneH) {
        box.y = std::max(0.0, sceneH - box.height);
    }
    return box;
}

// The drop mark over a dock while a tab is dragged: its outline and fill for each kind of drop.
constexpr const char* kMergeMark = "border-width: 2px; border-style: solid; border-color: var(--ide-dock-merge-color); "
                                   "background-color: var(--ide-dock-merge-fill-color);";
constexpr const char* kSplitMark = "border-width: 2px; border-style: solid; border-color: var(--ide-dock-split-color); "
                                   "background-color: var(--ide-dock-split-fill-color);";
constexpr const char* kFloatMark = "border-width: 2px; border-style: solid; border-color: var(--ide-dock-float-color); "
                                   "background-color: var(--ide-dock-float-fill-color);";
constexpr const char* kCaretMark = "border-width: 0; background-color: var(--ide-dock-caret-color);";

constexpr int kMcpPort = 7777;
constexpr std::chrono::seconds kUiWait(5);
// An action the person asked for waits longer than a repaint, then says why it did nothing.
constexpr std::chrono::milliseconds kActionWait(250);

// 128 random bits as hex: this session's MCP token when none is set.
inline std::string session_token() {
    std::random_device device;
    std::string out;
    char word[9];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(word, sizeof(word), "%08x", static_cast<unsigned>(device()));
        out += word;
    }
    return out;
}

inline std::string busy_message(const char* action) {
    return std::string("The place is busy, so ") + action + " did nothing. Try again.";
}
// How long screenshot waits for the Scene View to paint.
constexpr std::chrono::seconds kCaptureWait(3);

inline bool parent_ok(const engine_core::DataModel& game, engine_core::InstanceId parent) {
    return parent == 0 || (parent != engine_core::DataModel::kNoParent && game.alive(parent));
}

// Where a paste at parent lands. game's rows are hidden except the scene
// services, so one at the top of the tree goes into Workspace.
inline engine_core::InstanceId insert_target(const engine_core::DataModel& game, engine_core::InstanceId parent) {
    return parent == 0 ? game.scene_service("Workspace") : parent;
}

// An icon and a label on the ribbon. A left click runs action. A disabled
// button is dimmed and takes no clicks.
class RibbonButton : public jadefx::HBox {
public:
    RibbonButton(const char* label, const char* icon, std::function<void()> action) : action_(std::move(action)) {
        getClassList().add("ide-ribbon-button");
        setSpacing(5);
        setAlignment(jadefx::Pos::CenterLeft);
        setCursor(jadefx::Cursor::Pointer);
        if (std::shared_ptr<jadefx::ImageView> view = icon_graphic(icon)) {
            getChildren().add(std::move(view));
        }
        auto text = jadefx::make<jadefx::Label>(label);
        text->setMouseTransparent(true);
        getChildren().add(std::move(text));
        setOnMouseClicked([this](const jadefx::MouseEvent& event) {
            if (event.button == 0 && action_) {
                action_();
            }
        });
    }

private:
    std::function<void()> action_;
};

// A number in a layout.json object, or fallback when it is missing or not a finite number.
inline double NumberOr(const engine_core::JsonValue& object, const char* key, double fallback) {
    const engine_core::JsonValue* value = object.find(key);
    return value != nullptr && value->is_number() && std::isfinite(value->as_number()) ? value->as_number() : fallback;
}

// A window whose point 0, 0 is at x, y shows the top of itself on some display,
// so it can be dragged. With no displays to ask about, any place is taken.
inline bool OnScreen(const std::vector<jadefx::ScreenArea>& areas, double x, double y) {
    if (areas.empty()) {
        return true;
    }
    for (const jadefx::ScreenArea& area : areas) {
        if (x + 40 >= area.x && x + 40 < area.x + area.width && y >= area.y && y < area.y + area.height) {
            return true;
        }
    }
    return false;
}

// A Window menu row's graphic: a check while the window is open, then the
// window's icon. The menu lays its rows out each time it opens, and the check
// is worked out then. Check.png is white and takes the menu's text color.
// Without open, the check never shows; the slot keeps the icons in line.
class WindowGraphic : public jadefx::HBox {
public:
    WindowGraphic(const std::string& icon, std::function<bool()> open) : open_(std::move(open)) {
        constexpr double kIcon = 16;
        constexpr double kGap = 4;
        setSpacing(kGap);
        setAlignment(jadefx::Pos::CenterLeft);
        setMouseTransparent(true);
        auto slot = jadefx::make<jadefx::StackPane>();
        slot->setMouseTransparent(true);
        slot->setMinSize(kIcon, kIcon);
        slot->setPrefSize(kIcon, kIcon);
        slot->setMaxSize(kIcon, kIcon);
        if (std::shared_ptr<jadefx::ImageView> check = icon_graphic("Check.png")) {
            check->getClassList().add("ide-window-check");
            check->setStyle("image-color: currentColor;");
            check->setVisible(false);
            check_ = check.get();
            slot->getChildren().add(std::move(check));
        }
        getChildren().add(std::move(slot));
        if (std::shared_ptr<jadefx::ImageView> view = icon_graphic(icon)) {
            getChildren().add(std::move(view));
        }
        setPrefWidth(kIcon + kGap + kIcon);
    }

protected:
    void layoutChildren() override {
        if (check_ != nullptr) {
            check_->setVisible(open_ && open_());
        }
        HBox::layoutChildren();
    }

private:
    std::function<bool()> open_;
    jadefx::Node* check_ = nullptr;
};

// testing: a play session is active. stepping: that session is executing.
// Edit mode enables Test. A running test enables Pause and Stop. A paused
// test enables Resume and Stop.
inline void ShowSession(jadefx::Node& test, jadefx::Node& pause, jadefx::Node& resume, jadefx::Node& stop, bool testing,
                 bool stepping) {
    test.setDisable(testing);
    pause.setDisable(!(testing && stepping));
    resume.setDisable(!(testing && !stepping));
    stop.setDisable(!testing);
}

}  // namespace layout_detail

using namespace layout_detail;

// What the last Cut took, in tree order. They stay alive and unparented until
// Paste, or until the next Cut deletes them.
struct IdeLayout::Clip {
    std::vector<engine_core::InstanceId> ids;
    bool held = false;
    std::shared_ptr<const std::vector<CopiedNode>> copies;
};

// A page the Window menu opens and closes.
// It is saved in layout.json by name, and listed in the Window menu.
struct IdeLayout::WindowEntry {
    std::string name;
    std::string icon;
    // Null until the page is made, for a window with make.
    std::shared_ptr<IdePane> pane;
    // Makes the page the first time it is asked for, and again when the old one
    // is still held by a tab on its way out. Empty for a page made up front.
    std::function<std::shared_ptr<IdePane>()> make;
    // Makes a dock for the page when the one it last closed from is gone.
    std::function<IdeDock*()> home;
    // What the Window menu does to open it. Empty docks it with show_window.
    std::function<void()> open;
    // Closed in the default layout, and when layout.json does not name it.
    bool starts_closed = false;
    std::weak_ptr<IdeDock> last;
};

// What this studio's registry entry says, shared with the server's threads.
struct IdeLayout::McpIdentity {
    std::mutex mu;
    StudioEntry entry;
    std::filesystem::path dir;
    bool published = false;
};

}  // namespace ide
