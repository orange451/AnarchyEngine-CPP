#include "IdeLayout.hpp"

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
#include "ScriptRuntime.hpp"
#include "IdeDock.hpp"
#include "IdeExplorer.hpp"
#include "IdeScriptEditor.hpp"
#include "PropertiesPanel.hpp"
#include "LuaSource.hpp"
#include "McpServer.hpp"
#include "McpTools.hpp"
#include "TestTriangle.hpp"
#include "../runner/GameView.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <functional>
#include <vector>

namespace ide {
namespace {

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
T* Owning(jadefx::Node* node) {
    for (jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (auto* hit = dynamic_cast<T*>(cursor)) {
            return hit;
        }
    }
    return nullptr;
}

bool InTextWidget(jadefx::Node* node) {
    for (jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (dynamic_cast<jadefx::StyledTextArea*>(cursor) != nullptr || dynamic_cast<jadefx::TextField*>(cursor) != nullptr) {
            return true;
        }
    }
    return false;
}

KeyChord ChordOf(const jadefx::KeyEvent& event) {
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

void CloseGesture(engine_core::DataModel& world) { world.history().end_gesture(); }

constexpr const char* kStylesheet = R"CSS(
scene {
    background-color: #d0d0d0;
    font-family: "Open Sans";
    font-size: 13px;
    color: #202124;
}
.ide-status {
    background-color: #eceff1;
}
.ide-ribbon {
    background-color: #f5f6f7;
    border-width: 0 0 1px 0;
    border-color: #c8c8c8;
    padding: 3px 6px;
}
.ide-ribbon-button {
    padding: 0 8px;
    border-radius: 4px;
}
.ide-ribbon-button:hover {
    background-color: #e1e5ea;
}
.ide-ribbon-button:active {
    background-color: #cfd6de;
}
.ide-ribbon-button:disabled {
    background-color: transparent;
    opacity: 0.4;
}
.ide-viewport {
    background-color: #1e1e1e;
}
.ide-fps {
    color: #f2f2f2;
    padding: 6px 8px;
    background-color: rgba(0, 0, 0, 0.45);
}
textfield {
    background-color: #ffffff;
    border-width: 1px 0 0 0;
    border-color: #c8c8c8;
    padding: 6px 8px;
}
styleclassedtextarea {
    background-color: #ffffff;
    padding: 6px 8px;
}
codearea {
    background-color: #ffffff;
    color: #1f2328;
    font-family: "Editor Mono";
    font-size: 14px;
    padding: 8px;
}
split-pane:horizontal > .split-pane-divider,
split-pane:vertical > .split-pane-divider {
    padding: 0 2px;
    background-color: #b0b0b0;
}
)CSS";

void AttachIcon(jadefx::MenuItem& item, const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return;
    }
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(filename)) {
        item.setGraphic(std::move(icon));
    }
}

jadefx::MenuItem* AddItem(jadefx::Menu& menu, const char* label, const char* icon, int key, int mods) {
    auto item = jadefx::make<jadefx::MenuItem>(label);
    AttachIcon(*item, icon);
    if (key != 0) {
        item->setAccelerator(key, mods);
    }
    jadefx::MenuItem* raw = item.get();
    menu.getItems().add(std::move(item));
    return raw;
}

double Fraction(double part, double whole, double limit) {
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

void StretchRoot(jadefx::Node& node) {
    node.setPrefWidthRatio(1);
    node.setPrefHeightRatio(1);
}

DropSide SideOf(DockZone zone) {
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

Box ClampBox(Box box, double sceneW, double sceneH) {
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

constexpr const char* kMergeBorder = "border-width: 2px; border-style: solid; border-color: #1a73e8;";
constexpr const char* kSplitBorder = "border-width: 2px; border-style: solid; border-color: #188038;";
constexpr const char* kFloatBorder = "border-width: 2px; border-style: solid; border-color: #e37400;";
constexpr const char* kCaretBorder = "border-width: 0;";

const jadefx::Color kMergeFill = jadefx::Color::rgba(0.102f, 0.451f, 0.910f, 0.38f);
const jadefx::Color kSplitFill = jadefx::Color::rgba(0.204f, 0.659f, 0.325f, 0.40f);
const jadefx::Color kFloatFill = jadefx::Color::rgba(0.984f, 0.737f, 0.016f, 0.46f);
const jadefx::Color kCaretFill = jadefx::Color::rgba(0.102f, 0.451f, 0.910f, 0.95f);

constexpr int kMcpPort = 7777;
constexpr std::chrono::seconds kUiWait(5);

// Runs fn on the UI thread and waits for it. Called from an MCP server thread.
// Throws when the frame loop does not get to it, or with what fn threw.
void OnUiThread(std::function<void()> fn) {
    struct Wait {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        std::string error;
    };
    auto wait = std::make_shared<Wait>();
    jadefx::runLater([wait, fn = std::move(fn)] {
        std::string error;
        try {
            fn();
        } catch (const std::exception& ex) {
            error = ex.what();
        }
        {
            std::lock_guard<std::mutex> guard(wait->mu);
            wait->error = std::move(error);
            wait->done = true;
        }
        wait->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(wait->mu);
    if (!wait->cv.wait_for(lock, kUiWait, [&] { return wait->done; })) {
        throw std::runtime_error("The studio did not respond within 5 seconds.");
    }
    if (!wait->error.empty()) {
        throw std::runtime_error(wait->error);
    }
}

bool parent_ok(const engine_core::DataModel& game, engine_core::InstanceId parent) {
    return parent == 0 || (parent != engine_core::DataModel::kNoParent && game.alive(parent));
}

bool would_cycle(const engine_core::DataModel& game, engine_core::InstanceId node, engine_core::InstanceId parent) {
    if (node == 0 || parent == node) {
        return true;
    }
    engine_core::InstanceId cursor = parent;
    for (int guard = 0; cursor != 0 && cursor != engine_core::DataModel::kNoParent && guard < 100000; ++guard) {
        if (cursor == node) {
            return true;
        }
        cursor = game.parent(cursor);
    }
    return false;
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

// testing: a play session is active. stepping: that session is executing.
// Edit mode enables Test. A running test enables Pause and Stop. A paused
// test enables Resume and Stop.
void ShowSession(jadefx::Node& test, jadefx::Node& pause, jadefx::Node& resume, jadefx::Node& stop, bool testing,
                 bool stepping) {
    test.setDisable(testing);
    pause.setDisable(!(testing && stepping));
    resume.setDisable(!(testing && !stepping));
    stop.setDisable(!testing);
}

}  // namespace

// What the last Cut took, in tree order. They stay alive and unparented until
// Paste, or until the next Cut deletes them.
struct IdeLayout::Clip {
    std::vector<engine_core::InstanceId> ids;
    bool held = false;
};

IdeLayout::IdeLayout(double windowWidth, double windowHeight) : clip_(std::make_unique<Clip>()) {
    runner_.prepare();
    // Only open scripts, and the modules they require, are checked. Nothing
    // else in the studio reads diagnostics.
    runner_.simulation().analysis().set_scope(engine_core::AnalysisScope::Open);

    auto file = jadefx::make<jadefx::Menu>("File");
    AddItem(*file, "New", "New.png", jadefx::Key::N, jadefx::Key::ModControl)->setOnAction([this](jadefx::ActionEvent&) {
        confirm_discard("Save changes before starting a new place?", [this] { new_place(); });
    });
    AddItem(*file, "Open", "Folder.png", jadefx::Key::O, jadefx::Key::ModControl)
        ->setOnAction([this](jadefx::ActionEvent&) { open_project(); });
    AddItem(*file, "Save", "Save.png", jadefx::Key::S, jadefx::Key::ModControl)
        ->setOnAction([this](jadefx::ActionEvent&) { save_project(); });
    AddItem(*file, "Save As", "SaveAs.png", jadefx::Key::S, jadefx::Key::ModControl | jadefx::Key::ModShift)
        ->setOnAction([this](jadefx::ActionEvent&) { save_project_as(); });
    file->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    // The same path as the window's close button: unsaved work is offered a save first.
    AddItem(*file, "Quit", nullptr, jadefx::Key::Q, jadefx::Key::ModControl)->setOnAction([this](jadefx::ActionEvent&) {
        if (mainStage_ != nullptr && mainStage_->closeRequested()) {
            mainStage_->close();
        }
    });

    auto edit = jadefx::make<jadefx::Menu>("Edit");

    auto ribbon = jadefx::make<jadefx::HBox>();
    ribbon->getClassList().add("ide-ribbon");
    ribbon->setSpacing(2);
    ribbon->setAlignment(jadefx::Pos::CenterLeft);
    ribbon->setPrefWidthRatio(1);
    ribbon->setMinSize(0, kRibbonHeight);
    ribbon->setPrefHeight(kRibbonHeight);
    auto test = jadefx::make<RibbonButton>("Test", "Play.png", [this] { start_test(); });
    auto pause = jadefx::make<RibbonButton>("Pause", "Pause.png", [this] { pause_test(); });
    auto resume = jadefx::make<RibbonButton>("Resume", "Resume.png", [this] { resume_test(); });
    auto stop = jadefx::make<RibbonButton>("Stop", "Stop.png", [this] { stop_test(); });
    session_buttons_[0] = test.get();
    session_buttons_[1] = pause.get();
    session_buttons_[2] = resume.get();
    session_buttons_[3] = stop.get();
    ShowSession(*test, *pause, *resume, *stop, false, false);
    ribbon->getChildren().add(std::move(test));
    ribbon->getChildren().add(std::move(pause));
    ribbon->getChildren().add(std::move(resume));
    ribbon->getChildren().add(std::move(stop));


    auto insert = jadefx::make<jadefx::MenuItem>("Insert Triangle");
    AttachIcon(*insert, "Mesh.png");
    insert->setOnAction([this](jadefx::ActionEvent&) {
        runner_.simulation().on_simulation([](engine_core::DataModel& game) {
            int existing = 0;
            for (engine_core::InstanceId id = game.first_child(game.id()); id != 0; id = game.next_sibling(id)) {
                if (dynamic_cast<engine_core::TestTriangle*>(game.instance(id)) != nullptr) {
                    ++existing;
                }
            }
            engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
            game.set_parent(triangle.id(), game.id());
            // Spread repeats around the view so they do not stack on one point.
            const float angle = static_cast<float>(existing) * 0.9f;
            constexpr float kRadius = 0.42f;
            triangle.set_position(std::cos(angle) * kRadius, std::sin(angle) * kRadius, 0.15f);
            CloseGesture(game);
        });
    });
    edit->getItems().add(std::move(insert));

    auto view = jadefx::make<jadefx::Menu>("View");
    AddItem(*view, "Maybe :)", "Smile.png", 0, 0);

    auto menuBar = jadefx::make<jadefx::MenuBar>();
    menuBar->getMenus().add(file);
    menuBar->getMenus().add(edit);
    menuBar->getMenus().add(view);
    menuBar->setPrefWidthRatio(1);

    auto top = jadefx::make<jadefx::VBox>();
    top->setPrefWidthRatio(1);
    top->getChildren().add(menuBar);
    top->getChildren().add(ribbon);

    engine_core::DataModel& game = runner_.simulation().datamodel();
    ExplorerHost host;
    host.run = [this](std::string_view action, engine_core::InstanceId id) { run_action(action, id); };
    host.run_many = [this](std::string_view action, const std::vector<engine_core::InstanceId>& ids) {
        if (action == "Delete") {
            delete_instances(ids);
        } else if (action == "Cut") {
            cut(ids);
        }
    };
    host.enabled = [this](std::string_view action) { return action_enabled(action); };
    host.rename = [this](engine_core::InstanceId id, std::string name) { rename(id, std::move(name)); };
    host.move = [this](const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId parent) {
        move(ids, parent);
    };
    host.insert = [this](std::string class_name, engine_core::InstanceId parent, std::shared_ptr<InsertResult> result) {
        runner_.simulation().on_simulation(
            [class_name = std::move(class_name), parent, result](engine_core::DataModel& world) {
                engine_core::InstanceId made = 0;
                if (parent_ok(world, parent)) {
                    if (engine_core::DataModel* created = engine_core::lua_create_instance(world, class_name.c_str())) {
                        world.set_parent(created->id(), parent);
                        made = created->id();
                        CloseGesture(world);
                        // An edit while stopped is part of the place. One made during
                        // play is dropped when Stop restores that place.
                        if (!world.simulation_running()) {
                            world.capture_place();
                        }
                    }
                }
                if (result) {
                    result->id.store(made, std::memory_order_relaxed);
                    result->done.store(true, std::memory_order_release);
                }
            });
    };

    auto west = jadefx::make<IdeDock>();
    adoptDock(west);
    // IdeTreeTest is the sample tree page. The Java shell left that dock commented out.
    auto gameExplorer = jadefx::make<IdeExplorer>(game, "Game Explorer", host);
    gameExplorer->setIconFile("Explorer.png");
    west->dock(gameExplorer);
    explorers_.push_back(gameExplorer);

    auto center = jadefx::make<IdeDock>();
    adoptDock(center);
    sceneDock_ = center.get();

    auto south = jadefx::make<IdeDock>();
    adoptDock(south);
    auto console = jadefx::make<IdeConsole>(runner_.simulation());
    console->bindUndo(&undo_router_.widget_stack(kCommandUndo));
    console_ = console;
    south->dock(console);

    auto east = jadefx::make<IdeDock>();
    adoptDock(east);
    auto sceneExplorer = jadefx::make<IdeExplorer>(game, "Current Scene", host);
    sceneExplorer->setIconFile("Scenes.png");
    east->dock(sceneExplorer);
    explorers_.push_back(sceneExplorer);

    // Properties sits under the right-hand explorer, as in Roblox Studio.
    // Its writes go to the simulation thread, like a rename.
    properties_ = std::make_unique<PropertiesPanel>();
    properties_->set_runner([this](std::function<void(engine_core::DataModel&)> write) {
        runner_.simulation().on_simulation(std::move(write));
    });
    properties_->bind(game, game.selection(), game.history());
    auto propertiesDock = jadefx::make<IdeDock>();
    adoptDock(propertiesDock);
    propertiesDock->dock(properties_->dock_widget());

    auto eastColumn = jadefx::make<jadefx::SplitPane>();
    eastColumn->setOrientation(jadefx::Orientation::Vertical);
    eastColumn->getItems().add(east);
    eastColumn->getItems().add(propertiesDock);
    eastColumn->setDividerPositions({0.5});

    auto vertical = jadefx::make<jadefx::SplitPane>();
    vertical->setOrientation(jadefx::Orientation::Vertical);
    vertical->getItems().add(center);
    vertical->getItems().add(south);
    const double contentHeight = windowHeight - kMenuHeight - kRibbonHeight - kStatusHeight;
    vertical->setDividerPositions({1.0 - Fraction(kConsoleHeight, contentHeight, 0.6)});
    jadefx::SplitPane::setResizableWithParent(*south, false);

    auto horizontal = jadefx::make<jadefx::SplitPane>();
    horizontal->getItems().add(west);
    horizontal->getItems().add(vertical);
    horizontal->getItems().add(eastColumn);
    const double side = Fraction(kSideWidth, windowWidth, 0.35);
    horizontal->setDividerPositions({side, 1.0 - side});
    jadefx::SplitPane::setResizableWithParent(*west, false);
    jadefx::SplitPane::setResizableWithParent(*eastColumn, false);
    workArea_ = horizontal;

    auto status = jadefx::make<jadefx::Pane>();
    status->getClassList().add("ide-status");
    status->setMinSize(0, kStatusHeight);
    status->setPrefHeight(kStatusHeight);
    status->setBackground(jadefx::Color::rgb8(236, 239, 241));

    root_ = jadefx::make<jadefx::BorderPane>();
    root_->setPrefWidthRatio(1);
    root_->setPrefHeightRatio(1);
    root_->setBackground(jadefx::Color::rgb8(208, 208, 208));
    root_->setTop(top);
    root_->setCenter(horizontal);
    root_->setBottom(status);
}

engine_core::Engine& IdeLayout::simulation() { return runner_.simulation(); }

void IdeLayout::start() {
    // Dock the view before the threads start. Its first paint is what lets the
    // uncapped render thread leave its wait.
    sceneDock_->dock(jadefx::make<runner::GameView>(runner_));
    runner_.start();
    // Whatever the app built before start is the starting point, not an edit.
    mark_saved();
}

void IdeLayout::start_mcp() {
    const char* enabled = std::getenv("ANARCHY_MCP");
    if (enabled != nullptr && std::string(enabled) == "0") {
        return;
    }
    int port = kMcpPort;
    if (const char* text = std::getenv("ANARCHY_MCP_PORT")) {
        const int asked = std::atoi(text);
        if (asked > 0 && asked < 65536) {
            port = asked;
        }
    }
    // Each hook runs on the UI thread, where the ribbon's own handlers run.
    std::weak_ptr<int> alive = alive_;
    auto on_ui = [alive](std::function<void()> fn) {
        OnUiThread([alive, fn = std::move(fn)] {
            if (alive.expired()) {
                throw std::runtime_error("The studio is closing.");
            }
            fn();
        });
    };
    McpStudio studio;
    studio.start_test = [this, on_ui] { on_ui([this] { start_test(); }); };
    studio.pause_test = [this, on_ui] { on_ui([this] { pause_test(); }); };
    studio.resume_test = [this, on_ui] { on_ui([this] { resume_test(); }); };
    studio.stop_test = [this, on_ui] { on_ui([this] { stop_test(); }); };
    studio.session = [this, on_ui] {
        // Shared, since a task that runs after a timed-out wait still writes it.
        auto state = std::make_shared<std::string>();
        on_ui([this, state] { *state = !testing_ ? "stopped" : (stepping_ ? "running" : "paused"); });
        return *state;
    };
    studio.flush_scripts = [this, on_ui] { on_ui([this] { flush_editors(); }); };
    studio.refresh_scripts = [this, on_ui] { on_ui([this] { reapply_editors(); }); };

    auto server = std::make_unique<McpServer>();
    if (const char* token = std::getenv("ANARCHY_MCP_TOKEN")) {
        server->set_token(token);
    }
    add_engine_tools(*server, runner_.simulation(), std::move(studio));
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    std::string error;
    if (!server->start(port, error)) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error, "MCP server: " + error);
        return;
    }
    scripts.append_output(engine_core::ScriptRuntime::OutputKind::Print,
                          "MCP server listening on http://127.0.0.1:" + std::to_string(port) + "/mcp");
    mcp_ = std::move(server);
}

void IdeLayout::mount(jadefx::Scene& scene) {
    scene_ = &scene;
    scene.setPadding(jadefx::Insets{});
    scene.setStylesheet(kStylesheet);
    scene.setRoot(root_);
    scene.addKeyHook([this](jadefx::KeyEvent& event) {
        // F5 tests, or resumes a paused test. Shift+F5 stops. Both as in Roblox Studio.
        if (event.pressed && !event.repeat && !event.consumed && event.key == kKeyF5 && !event.control &&
            !event.alt && !event.meta) {
            if (event.shift && testing_) {
                event.consume();
                stop_test();
                return;
            }
            if (!event.shift && !testing_) {
                event.consume();
                start_test();
                return;
            }
            if (!event.shift && !stepping_) {
                event.consume();
                resume_test();
                return;
            }
        }
        if (scene_ != nullptr) {
            routeUndo(event, *scene_);
            routeDelete(event, *scene_);
            routeReveal(event, *scene_);
        }
    });
}

void IdeLayout::attachFrame(jadefx::Stage& stage) {
    mainStage_ = &stage;
    resizeWindow_ = [&stage](int width, int height) { stage.setSize(width, height); };
    stage.setFrameTail([this]() { flushFrame(); });
    // The close button, Alt+F4, and Cmd+Q ask about unsaved work first.
    stage.setOnCloseRequest([this]() {
        if (prompt_open_ || dialog_open_) {
            return false;
        }
        if (!has_unsaved_changes()) {
            return true;
        }
        confirm_discard("Save changes before closing?", [this] {
            if (mainStage_ != nullptr) {
                mainStage_->close();
            }
        });
        return false;
    });
    update_title();
}

void IdeLayout::adoptDock(const std::shared_ptr<IdeDock>& dock) {
    if (!dock) {
        return;
    }
    docks_.push_back(dock);
    std::weak_ptr<IdeDock> weak = dock;
    dock->setOnTabDrag([this, weak](const jadefx::TabDrag& drag) {
        if (const std::shared_ptr<IdeDock> live = weak.lock()) {
            onTabDrag(*live, drag);
        }
    });
    dock->setOnEmpty([this, weak]() {
        if (const std::shared_ptr<IdeDock> live = weak.lock()) {
            pendingEmpty_.push_back(live);
        }
    });
    dock->setOnFit([this]() { fitPending_ = true; });
}

std::shared_ptr<jadefx::Node> IdeLayout::shareNode(jadefx::Node* node) const {
    if (node == nullptr) {
        return nullptr;
    }
    if (node == root_.get()) {
        return root_;
    }
    if (node == workArea_.get()) {
        return workArea_;
    }
    for (const std::shared_ptr<IdeDock>& dock : docks_) {
        if (dock.get() == node) {
            return dock;
        }
    }
    for (jadefx::Node* parent = node->getParent(); parent != nullptr; parent = parent->getParent()) {
        auto* split = dynamic_cast<jadefx::SplitPane*>(parent);
        if (split == nullptr) {
            continue;
        }
        for (const std::shared_ptr<jadefx::Node>& item : split->getItems().items()) {
            if (item.get() == node) {
                return item;
            }
        }
    }
    return nullptr;
}

void IdeLayout::forgetDock(const std::shared_ptr<IdeDock>& dock) {
    if (!dock) {
        return;
    }
    if (sceneDock_ == dock.get()) {
        sceneDock_ = nullptr;
    }
    dockWindow_.erase(dock.get());
    docks_.erase(std::remove(docks_.begin(), docks_.end(), dock), docks_.end());
}

void IdeLayout::noteReplaced(jadefx::Node& owner, const std::shared_ptr<jadefx::Node>&,
                             const std::shared_ptr<jadefx::Node>& replacement) {
    if (&owner == root_.get()) {
        workArea_ = replacement;
    }
    if (replacement != nullptr && dynamic_cast<jadefx::Scene*>(&owner) != nullptr) {
        StretchRoot(*replacement);
    }
}

void IdeLayout::rebindUtilities() {
    dockWindow_.clear();
    for (const Floating& item : floating_) {
        if (!item.window || !item.window->isOpen()) {
            continue;
        }
        jadefx::Scene* scene = &item.window->stage().getScene();
        for (const std::shared_ptr<IdeDock>& dock : docks_) {
            if (dock && dock->getScene() == scene) {
                dockWindow_[dock.get()] = item.window.get();
            }
        }
    }
}

jadefx::UtilityWindow* IdeLayout::utilityOf(const IdeDock* dock) const {
    if (dock == nullptr) {
        return nullptr;
    }
    const auto found = dockWindow_.find(const_cast<IdeDock*>(dock));
    if (found == dockWindow_.end()) {
        return nullptr;
    }
    return found->second;
}

bool IdeLayout::utilityHasDock(const jadefx::UtilityWindow* window) const {
    if (window == nullptr) {
        return false;
    }
    for (const auto& entry : dockWindow_) {
        if (entry.second == window) {
            return true;
        }
    }
    return false;
}

void IdeLayout::forgetWindow(jadefx::UtilityWindow* window) {
    if (window == nullptr) {
        return;
    }
    std::vector<std::shared_ptr<IdeDock>> drop;
    for (const std::shared_ptr<IdeDock>& dock : docks_) {
        if (dock && utilityOf(dock.get()) == window) {
            drop.push_back(dock);
        }
    }
    for (const std::shared_ptr<IdeDock>& dock : drop) {
        forgetDock(dock);
    }
    floating_.erase(std::remove_if(floating_.begin(), floating_.end(),
                                   [&](const Floating& item) { return item.window.get() == window; }),
                    floating_.end());
}

void IdeLayout::removeDock(const std::shared_ptr<IdeDock>& dock) {
    if (!dock || std::find(docks_.begin(), docks_.end(), dock) == docks_.end()) {
        return;
    }
    jadefx::UtilityWindow* utility = utilityOf(dock.get());
    std::shared_ptr<jadefx::UtilityWindow> keep;
    for (const Floating& item : floating_) {
        if (item.window.get() == utility) {
            keep = item.window;
        }
    }
    jadefx::Node* parent = dock->getParent();
    while (parent != nullptr && dynamic_cast<jadefx::SplitPane*>(parent) == nullptr &&
           dynamic_cast<jadefx::BorderPane*>(parent) == nullptr && dynamic_cast<jadefx::Scene*>(parent) == nullptr) {
        parent = parent->getParent();
    }
    const std::shared_ptr<jadefx::Node> parentHeld = shareNode(parent);
    if (auto* split = dynamic_cast<jadefx::SplitPane*>(parent)) {
        split->getItems().removeIf([&](const std::shared_ptr<jadefx::Node>& item) { return item == dock; });
    } else if (auto* border = dynamic_cast<jadefx::BorderPane*>(parent)) {
        if (border->getCenter() == dock.get()) {
            border->setCenter(nullptr);
            if (border == root_.get()) {
                workArea_.reset();
            }
        }
    } else if (auto* scene = dynamic_cast<jadefx::Scene*>(parent)) {
        if (scene->getRoot() == dock.get()) {
            scene->setRoot(nullptr);
        }
    }
    forgetDock(dock);
    if (parentHeld) {
        liftDegenerateSplits(parentHeld, root_.get(), [this](jadefx::Node* node) { return shareNode(node); },
                             [this](jadefx::Node& owner, const std::shared_ptr<jadefx::Node>& previous,
                                    const std::shared_ptr<jadefx::Node>& replacement) {
                                 noteReplaced(owner, previous, replacement);
                             });
    }
    rebindUtilities();
    if (keep && keep->isOpen() && !utilityHasDock(keep.get())) {
        keep->close();
    }
}

IdeDock* IdeLayout::dockForPane(const jadefx::TabPane* pane) const {
    if (pane == nullptr) {
        return nullptr;
    }
    for (const std::shared_ptr<IdeDock>& dock : docks_) {
        if (dock && dock->tabs() == pane) {
            return dock.get();
        }
    }
    return nullptr;
}

IdeDock* IdeLayout::dockContaining(const IdePane* pane) const {
    if (pane == nullptr) {
        return nullptr;
    }
    for (const std::shared_ptr<IdeDock>& dock : docks_) {
        if (!dock || dock->tabs() == nullptr) {
            continue;
        }
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            if (tab && tab->getContent() == pane) {
                return dock.get();
            }
        }
    }
    return nullptr;
}

IdeDock* IdeLayout::editorHome() {
    for (const std::shared_ptr<IdeDock>& dock : docks_) {
        if (!dock || dock->getParent() == nullptr || dock->tabs() == nullptr) {
            continue;
        }
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            auto* page = tab ? dynamic_cast<IdePane*>(tab->getContent()) : nullptr;
            if (page != nullptr && !page->closable()) {
                return dock.get();
            }
        }
    }
    if (sceneDock_ != nullptr && sceneDock_->getParent() != nullptr) {
        return sceneDock_;
    }
    for (const std::shared_ptr<IdeDock>& dock : docks_) {
        if (!dock || dock->getParent() == nullptr) {
            continue;
        }
        if (utilityOf(dock.get()) == nullptr) {
            return dock.get();
        }
    }
    if (auto* existing = dynamic_cast<IdeDock*>(workArea_.get())) {
        if (existing->getParent() != nullptr) {
            return existing;
        }
    }
    auto dock = jadefx::make<IdeDock>();
    adoptDock(dock);
    if (root_->getCenter() == nullptr) {
        workArea_ = dock;
        root_->setCenter(dock);
    } else if (auto* split = dynamic_cast<jadefx::SplitPane*>(workArea_.get())) {
        split->getItems().add(dock);
    } else {
        workArea_ = dock;
        root_->setCenter(dock);
    }
    sceneDock_ = dock.get();
    return sceneDock_;
}

void IdeLayout::floatTab(const std::shared_ptr<jadefx::Tab>& tab, double screenX, double screenY) {
    if (!tab) {
        return;
    }
    const jadefx::Node* content = tab->getContent();
    const double contentW = content != nullptr ? content->getMinWidth() : 0;
    const double contentH = content != nullptr ? content->getMinHeight() : 0;
    const int width = std::max(420, static_cast<int>(std::ceil(contentW + 16)));
    const int height = std::max(280, static_cast<int>(std::ceil(contentH + 48)));
    std::shared_ptr<jadefx::UtilityWindow> window =
        jadefx::UtilityWindow::open(tab->getText(), width, height, screenX - 36, screenY - 12);
    if (!window) {
        return;
    }
    auto dock = jadefx::make<IdeDock>();
    adoptDock(dock);
    StretchRoot(*dock);
    auto scene = jadefx::make<jadefx::Scene>(dock, static_cast<double>(width), static_cast<double>(height));
    scene->setStylesheet(kStylesheet);
    jadefx::Scene* utilityScene = scene.get();
    scene->addKeyHook([this, utilityScene](jadefx::KeyEvent& event) {
        routeUndo(event, *utilityScene);
        routeDelete(event, *utilityScene);
        routeReveal(event, *utilityScene);
    });
    window->stage().setScene(std::move(scene));
    dock->take(tab);
    window->setCanClose([this, raw = window.get()]() {
        std::vector<IdeDock*> docks;
        for (const std::shared_ptr<IdeDock>& dock : docks_) {
            if (dock && utilityOf(dock.get()) == raw) {
                docks.push_back(dock.get());
            }
        }
        if (docks.empty()) {
            return true;
        }
        for (IdeDock* dock : docks) {
            if (dock->tabs() == nullptr) {
                continue;
            }
            for (const std::shared_ptr<jadefx::Tab>& item : dock->tabs()->getTabs().items()) {
                if (item && !item->isClosable()) {
                    return false;
                }
            }
        }
        for (IdeDock* dock : docks) {
            if (dock->tabs() == nullptr) {
                continue;
            }
            const std::vector<std::shared_ptr<jadefx::Tab>> copy = dock->tabs()->getTabs().items();
            for (const std::shared_ptr<jadefx::Tab>& item : copy) {
                if (item) {
                    dock->tabs()->close(item);
                }
            }
        }
        for (IdeDock* dock : docks) {
            if (!dock->empty()) {
                return false;
            }
        }
        return true;
    });
    window->setOnClosed([this, raw = window.get()]() { forgetWindow(raw); });
    Floating created;
    created.window = window;
    created.title = tab->getText();
    floating_.push_back(std::move(created));
    rebindUtilities();
}

struct DragPoint {
    jadefx::Stage* stage = nullptr;
    double x = 0;
    double y = 0;
    double screenX = 80;
    double screenY = 80;
    bool overWindow = false;
};

DragPoint LocateDrag(IdeDock& from, const jadefx::TabDrag& drag, jadefx::Stage* main,
                     const std::vector<jadefx::Stage*>& utilities) {
    DragPoint point;
    point.x = drag.x;
    point.y = drag.y;
    jadefx::Stage* source = nullptr;
    if (main != nullptr && from.getScene() == &main->getScene()) {
        source = main;
    } else {
        for (jadefx::Stage* stage : utilities) {
            if (stage != nullptr && from.getScene() == &stage->getScene()) {
                source = stage;
                break;
            }
        }
    }
    if (source != nullptr) {
        double mappedX = 0;
        double mappedY = 0;
        if (jadefx::stageToScreen(*source, drag.x, drag.y, mappedX, mappedY)) {
            point.screenX = mappedX;
            point.screenY = mappedY;
        }
    }
    jadefx::Stage* hit = nullptr;
    double localX = 0;
    double localY = 0;
    if (jadefx::windowUnderScreen(point.screenX, point.screenY, hit, localX, localY) && hit != nullptr) {
        point.stage = hit;
        point.x = localX;
        point.y = localY;
        point.overWindow = true;
    } else if (source != nullptr) {
        point.stage = source;
    }
    return point;
}

enum class DragKind { Stay, Undock, MoveTab, SplitPane, SplitRoot, Restore };

struct DragChoice {
    DragKind kind = DragKind::Undock;
    DockZone zone = DockZone::Outside;
    IdeDock* dock = nullptr;
    std::size_t index = 0;
    Box mark;
    bool caret = false;
};

IdeDock* SmallestDockAt(const std::vector<std::shared_ptr<IdeDock>>& docks, jadefx::Scene* scene, double x, double y) {
    IdeDock* best = nullptr;
    double bestArea = 0;
    for (const std::shared_ptr<IdeDock>& dock : docks) {
        if (!dock || dock->getScene() != scene || dock->getWidth() < 1.0 || dock->getHeight() < 1.0) {
            continue;
        }
        const double left = dock->getAbsoluteX();
        const double top = dock->getAbsoluteY();
        if (x < left || y < top || x >= left + dock->getWidth() || y >= top + dock->getHeight()) {
            continue;
        }
        const double area = dock->getWidth() * dock->getHeight();
        if (best == nullptr || area < bestArea) {
            best = dock.get();
            bestArea = area;
        }
    }
    return best;
}

DragChoice ChooseDrop(IdeDock& from, const DragPoint& point, const std::vector<std::shared_ptr<IdeDock>>& docks,
                      const std::shared_ptr<jadefx::Node>& workArea, bool mainWindow) {
    DragChoice choice;
    if (!point.overWindow || point.stage == nullptr) {
        return choice;
    }
    jadefx::Scene& scene = point.stage->getScene();
    IdeDock* dock = SmallestDockAt(docks, &scene, point.x, point.y);
    if (dock != nullptr && dock->tabs() != nullptr) {
        const jadefx::TabHeaderGap gap = dock->tabs()->headerGap(point.x, point.y);
        if (gap.valid) {
            if (dock == &from) {
                choice.kind = DragKind::Stay;
                return choice;
            }
            choice.kind = DragKind::MoveTab;
            choice.dock = dock;
            choice.index = gap.index;
            choice.caret = true;
            choice.mark = {gap.x, gap.y, gap.width, gap.height};
            return choice;
        }
    }
    if (mainWindow && (!workArea || workArea->getScene() != &scene)) {
        choice.kind = DragKind::Restore;
        choice.mark = {0, 0, scene.getWidth(), scene.getHeight()};
        return choice;
    }
    if (mainWindow && workArea && workArea->getScene() == &scene && workArea->getWidth() > 1.0 &&
        workArea->getHeight() > 1.0) {
        Box work;
        work.x = workArea->getAbsoluteX();
        work.y = workArea->getAbsoluteY();
        work.width = workArea->getWidth();
        work.height = workArea->getHeight();
        const double margin = std::clamp(std::min(work.width, work.height) * 0.12, 64.0, 140.0);
        const DockZone edge = screenEdge(work, margin, point.x, point.y);
        int shown = 0;
        for (const std::shared_ptr<IdeDock>& item : docks) {
            if (item && item->getScene() == &scene && item->tabs() != nullptr && !item->tabs()->getTabs().empty()) {
                ++shown;
            }
        }
        const bool sole = shown <= 1 && from.getScene() == &scene && from.tabs() != nullptr &&
                          from.tabs()->getTabs().size() <= 1;
        if (edge != DockZone::Outside && !sole) {
            const bool horizontal = edge == DockZone::Left || edge == DockZone::Right;
            const double span = horizontal ? work.width : work.height;
            const double desired = horizontal ? kSideWidth : kConsoleHeight;
            double fraction = span > 1.0 ? desired / span : 0.5;
            if (fraction < 0.12) {
                fraction = 0.12;
            }
            if (fraction > 0.5) {
                fraction = 0.5;
            }
            choice.kind = DragKind::SplitRoot;
            choice.zone = edge;
            choice.mark = edgePreview(work, edge, fraction * span);
            return choice;
        }
    }
    if (dock == nullptr) {
        return choice;
    }
    Box bounds;
    bounds.x = dock->getAbsoluteX();
    bounds.y = dock->getAbsoluteY();
    bounds.width = dock->getWidth();
    bounds.height = dock->getHeight();
    const double header = dock->tabs() != nullptr ? dock->tabs()->headerExtent() : 0;
    const DockZone zone = dockZone(bounds, header, point.x, point.y);
    const bool alone = dock->tabs() == nullptr || dock->tabs()->getTabs().size() <= 1;
    if (dock == &from && (zone == DockZone::Center || zone == DockZone::Header || zone == DockZone::Outside ||
                          (alone && zone != DockZone::Outside))) {
        choice.kind = DragKind::Stay;
        return choice;
    }
    if (zone == DockZone::Center || zone == DockZone::Header) {
        choice.kind = DragKind::MoveTab;
        choice.dock = dock;
        choice.index = dock->tabs() != nullptr ? dock->tabs()->getTabs().size() : 0;
        choice.mark = dockPreview(bounds, header, DockZone::Center);
        return choice;
    }
    if (zone == DockZone::Left || zone == DockZone::Right || zone == DockZone::Top || zone == DockZone::Bottom) {
        choice.kind = DragKind::SplitPane;
        choice.zone = zone;
        choice.dock = dock;
        choice.mark = dockPreview(bounds, header, zone);
        return choice;
    }
    return choice;
}

void IdeLayout::showDropMark(jadefx::Scene& scene, double x, double y, double width, double height, const char* border,
                             jadefx::Color fill) {
    if (width < 2.0 || height < 2.0) {
        hideDropMark();
        return;
    }
    if (!dropMark_) {
        dropMark_ = jadefx::make<jadefx::Pane>();
        dropMark_->setMouseTransparent(true);
    }
    dropMark_->setBackground(fill);
    dropMark_->setStyle(border != nullptr ? border : kCaretBorder);
    const Box box = ClampBox(Box{x, y, width, height}, scene.getWidth(), scene.getHeight());
    if (dropMarkScene_ != &scene) {
        hideDropMark();
        jadefx::PopupOptions options;
        options.autoHide = false;
        scene.showPopup(dropMark_, box.x, box.y, box.width, box.height, options);
        dropMarkScene_ = &scene;
        return;
    }
    scene.movePopup(dropMark_.get(), box.x, box.y, box.width, box.height);
}

void IdeLayout::hideDropMark() {
    if (dropMarkScene_ != nullptr && dropMark_) {
        dropMarkScene_->hidePopup(dropMark_.get());
    }
    dropMarkScene_ = nullptr;
}

std::vector<jadefx::Stage*> IdeLayout::utilityStages() const {
    std::vector<jadefx::Stage*> stages;
    for (const Floating& item : floating_) {
        if (item.window && item.window->isOpen()) {
            stages.push_back(&item.window->stage());
        }
    }
    return stages;
}

void IdeLayout::previewDrag(IdeDock& from, const jadefx::TabDrag& drag) {
    const DragPoint point = LocateDrag(from, drag, mainStage_, utilityStages());
    const bool mainWindow = point.stage != nullptr && mainStage_ != nullptr && point.stage == mainStage_;
    const DragChoice choice = ChooseDrop(from, point, docks_, workArea_, mainWindow);
    if (choice.kind == DragKind::Stay || point.stage == nullptr) {
        hideDropMark();
        return;
    }
    Box mark = choice.mark;
    const char* border = kFloatBorder;
    jadefx::Color fill = kFloatFill;
    if (choice.kind == DragKind::Undock) {
        const jadefx::Node* content = drag.tab ? drag.tab->getContent() : nullptr;
        const double contentW = content != nullptr ? content->getMinWidth() : 0;
        const double contentH = content != nullptr ? content->getMinHeight() : 0;
        mark.width = std::clamp(contentW + 16.0, 220.0, 360.0);
        mark.height = std::clamp(contentH + 48.0, 140.0, 240.0);
        mark.x = point.x - 36.0;
        mark.y = point.y - 12.0;
    } else if (choice.kind == DragKind::MoveTab || choice.kind == DragKind::Restore) {
        border = choice.caret ? kCaretBorder : kMergeBorder;
        fill = choice.caret ? kCaretFill : kMergeFill;
    } else {
        border = kSplitBorder;
        fill = kSplitFill;
    }
    showDropMark(point.stage->getScene(), mark.x, mark.y, mark.width, mark.height, border, fill);
}

void IdeLayout::applyDrag(IdeDock& from, const jadefx::TabDrag& drag) {
    const DragPoint point = LocateDrag(from, drag, mainStage_, utilityStages());
    const bool mainWindow = point.stage != nullptr && mainStage_ != nullptr && point.stage == mainStage_;
    const DragChoice choice = ChooseDrop(from, point, docks_, workArea_, mainWindow);
    auto share = [this](jadefx::Node* node) { return shareNode(node); };
    auto replaced = [this](jadefx::Node& owner, const std::shared_ptr<jadefx::Node>& previous,
                           const std::shared_ptr<jadefx::Node>& replacement) { noteReplaced(owner, previous, replacement); };
    if (choice.kind == DragKind::Stay) {
        return;
    }
    if (choice.kind == DragKind::Restore && root_) {
        auto fresh = jadefx::make<IdeDock>();
        adoptDock(fresh);
        workArea_ = fresh;
        root_->setCenter(fresh);
        fresh->take(drag.tab);
        rebindUtilities();
        return;
    }
    if (choice.kind == DragKind::MoveTab && choice.dock != nullptr && choice.dock->tabs() != nullptr) {
        jadefx::TabPane& pane = *choice.dock->tabs();
        const std::size_t index = std::min(choice.index, pane.getTabs().size());
        pane.getTabs().insert(index, drag.tab);
        pane.select(drag.tab);
        rebindUtilities();
        return;
    }
    if (choice.kind == DragKind::SplitPane && choice.dock != nullptr) {
        auto fresh = jadefx::make<IdeDock>();
        adoptDock(fresh);
        if (splitBeside(*choice.dock, fresh, SideOf(choice.zone), share, replaced)) {
            fresh->take(drag.tab);
            rebindUtilities();
            return;
        }
        forgetDock(fresh);
    }
    if (choice.kind == DragKind::SplitRoot && workArea_) {
        auto fresh = jadefx::make<IdeDock>();
        adoptDock(fresh);
        const bool horizontal = choice.zone == DockZone::Left || choice.zone == DockZone::Right;
        const double span = horizontal ? workArea_->getWidth() : workArea_->getHeight();
        const double desired = horizontal ? kSideWidth : kConsoleHeight;
        double fraction = span > 1.0 ? desired / span : 0.5;
        if (splitEdge(*workArea_, fresh, SideOf(choice.zone), fraction, share, replaced)) {
            fresh->take(drag.tab);
            rebindUtilities();
            return;
        }
        forgetDock(fresh);
    }
    floatTab(drag.tab, point.screenX, point.screenY);
}

void IdeLayout::onTabDrag(IdeDock& from, const jadefx::TabDrag& drag) {
    if (!drag.tab) {
        return;
    }
    if (!drag.released) {
        if (drag.outside) {
            previewDrag(from, drag);
        } else {
            hideDropMark();
        }
        return;
    }
    hideDropMark();
    if (drag.outside) {
        applyDrag(from, drag);
    }
}

void GrowToFit(const jadefx::Node* area, jadefx::Scene* scene, const std::function<void(int, int)>& resize, int& lastW,
               int& lastH, int& seenW, int& seenH) {
    if (area == nullptr || scene == nullptr || !resize || area->getWidth() < 1.0 || area->getHeight() < 1.0) {
        return;
    }
    const Extent need = minimumExtent(area);
    const double extraW = need.width - area->getWidth();
    const double extraH = need.height - area->getHeight();
    if (extraW <= 1.0 && extraH <= 1.0) {
        return;
    }
    const int sceneW = static_cast<int>(std::lround(scene->getWidth()));
    const int sceneH = static_cast<int>(std::lround(scene->getHeight()));
    const int targetW = sceneW + static_cast<int>(std::ceil(std::max(0.0, extraW)));
    const int targetH = sceneH + static_cast<int>(std::ceil(std::max(0.0, extraH)));
    if (targetW == lastW && targetH == lastH && sceneW == seenW && sceneH == seenH) {
        return;
    }
    lastW = targetW;
    lastH = targetH;
    seenW = sceneW;
    seenH = sceneH;
    resize(targetW, targetH);
}

void IdeLayout::flushFrame() {
    noteScriptFocus();
    refresh_modified();
    const std::vector<std::shared_ptr<IdeDock>> pending = std::move(pendingEmpty_);
    pendingEmpty_.clear();
    for (const std::shared_ptr<IdeDock>& dock : pending) {
        if (dock && dock->empty()) {
            removeDock(dock);
        }
    }
    const bool fit = fitPending_;
    fitPending_ = false;
    if (fit) {
        GrowToFit(workArea_.get(), scene_, resizeWindow_, lastRequestedW_, lastRequestedH_, lastSceneW_, lastSceneH_);
    }
    for (Floating& item : floating_) {
        if (!item.window || !item.window->isOpen()) {
            continue;
        }
        jadefx::Scene& utilityScene = item.window->stage().getScene();
        if (fit) {
            GrowToFit(utilityScene.getRoot(), &utilityScene,
                      [&item](int width, int height) { item.window->stage().setSize(width, height); },
                      item.lastRequestedW, item.lastRequestedH, item.lastSceneW, item.lastSceneH);
        }
        std::string title;
        jadefx::Node* focus = utilityScene.focusedNode();
        for (const std::shared_ptr<IdeDock>& dock : docks_) {
            if (!dock || utilityOf(dock.get()) != item.window.get() || dock->tabs() == nullptr) {
                continue;
            }
            const jadefx::Tab* selected = dock->tabs()->getSelectedTab();
            if (selected == nullptr || selected->getText().empty()) {
                continue;
            }
            if (title.empty()) {
                title = selected->getText();
            }
            if (focus != nullptr && dock->isAncestorOf(focus)) {
                title = selected->getText();
                break;
            }
        }
        if (!title.empty() && title != item.title) {
            item.title = title;
            item.window->setTitle(title);
        }
    }
}

IdeLayout::~IdeLayout() {
    // Before anything its tools reach is torn down.
    alive_.reset();
    mcp_.reset();
    // Window teardown calls the close hook. Drop it first so that hook does not
    // touch docks that are already being destroyed.
    for (Floating& item : floating_) {
        if (!item.window) {
            continue;
        }
        item.window->setOnClosed(nullptr);
        item.window->setCanClose(nullptr);
    }
}

void IdeLayout::run_action(std::string_view action, std::uint32_t id) {
    if (action == "Cut") {
        cut({id});
    } else if (action == "Paste") {
        paste(id);
    } else if (action == "Edit") {
        edit(id);
    } else if (action == "Delete") {
        delete_instances({id});
    }
}

void IdeLayout::delete_instances(std::vector<std::uint32_t> ids) {
    ids.erase(std::remove(ids.begin(), ids.end(), 0u), ids.end());
    if (ids.empty()) {
        return;
    }
    runner_.simulation().on_simulation([ids = std::move(ids)](engine_core::DataModel& world) {
        bool any = false;
        for (std::uint32_t id : ids) {
            // A selected child is already gone with its selected parent.
            if (!world.alive(id)) {
                continue;
            }
            if (!any) {
                world.history().set_pending_gesture("Delete");
                any = true;
            }
            world.destroy_tree(id);
        }
        if (any) {
            CloseGesture(world);
        }
    });
}

void IdeLayout::routeDelete(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    if (!event.pressed || event.consumed || event.shortcut() || event.alt) {
        return;
    }
    // The Mac keyboard's Delete key is Backspace.
#if defined(__APPLE__)
    const bool key = event.key == jadefx::Key::Delete || event.key == jadefx::Key::Backspace;
#else
    const bool key = event.key == jadefx::Key::Delete;
#endif
    jadefx::Node* focused = scene.focusedNode();
    if (!key || InTextWidget(focused)) {
        return;
    }
    if (IdeExplorer* explorer = Owning<IdeExplorer>(focused)) {
        if (explorer->run_on_selection("Delete")) {
            event.consume();
        }
    }
}

void IdeLayout::routeReveal(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    if (!event.pressed || event.repeat || event.consumed || event.key != jadefx::Key::F || event.shift ||
        event.alt || event.control || event.meta || InTextWidget(scene.focusedNode())) {
        return;
    }
    bool any = false;
    for (const std::weak_ptr<IdeExplorer>& weak : explorers_) {
        if (const std::shared_ptr<IdeExplorer> explorer = weak.lock()) {
            any = explorer->reveal_selection() || any;
        }
    }
    if (any) {
        event.consume();
    }
}

bool IdeLayout::action_enabled(std::string_view action) const {
    if (action == "Paste") {
        return clip_ && clip_->held;
    }
    return true;
}

void IdeLayout::cut(const std::vector<std::uint32_t>& ids) {
    if (!clip_) {
        return;
    }
    engine_core::DataModel& game = runner_.simulation().datamodel();
    std::vector<engine_core::InstanceId> taken;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns()) {
            return;
        }
        taken = cut_set(game, ids);
    }
    if (taken.empty()) {
        return;
    }
    // A second Cut replaces the clipboard. What it held is already out of the
    // place, so it is deleted, not put back. Undo of this Cut brings it back.
    std::vector<engine_core::InstanceId> dropped;
    if (clip_->held) {
        for (engine_core::InstanceId id : clip_->ids) {
            if (std::find(taken.begin(), taken.end(), id) == taken.end()) {
                dropped.push_back(id);
            }
        }
    }
    clip_->ids = taken;
    clip_->held = true;
    // The cut instances leave the tree, so they leave the selection. Paste
    // selects them again.
    game.selection().set({});
    runner_.simulation().on_simulation([taken, dropped](engine_core::DataModel& world) {
        world.history().set_pending_gesture("Cut");
        for (engine_core::InstanceId id : dropped) {
            if (world.alive(id) && world.parent(id) == engine_core::DataModel::kNoParent) {
                world.destroy_tree(id);
            }
        }
        for (engine_core::InstanceId id : taken) {
            if (world.alive(id)) {
                world.set_parent(id, engine_core::DataModel::kNoParent);
            }
        }
        CloseGesture(world);
    });
}

void IdeLayout::paste(std::uint32_t id) {
    if (!clip_ || !clip_->held) {
        return;
    }
    engine_core::DataModel& game = runner_.simulation().datamodel();
    std::vector<engine_core::InstanceId> children;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns() || !parent_ok(game, id)) {
            return;
        }
        // A target inside something cut refuses the whole paste, so nothing
        // is left behind out of the place.
        for (engine_core::InstanceId child : clip_->ids) {
            if (!game.alive(child)) {
                continue;
            }
            if (would_cycle(game, child, id)) {
                return;
            }
            children.push_back(child);
        }
    }
    if (children.empty()) {
        return;
    }
    clip_->held = false;
    clip_->ids.clear();
    // The pasted instances become the selection, as they were when cut.
    game.selection().set(children);
    runner_.simulation().on_simulation([children, id](engine_core::DataModel& world) {
        if (!parent_ok(world, id)) {
            return;
        }
        world.history().set_pending_gesture("Paste");
        // Each goes last, so the pasted instances keep the order they were cut in.
        for (engine_core::InstanceId child : children) {
            if (world.alive(child) && !would_cycle(world, child, id)) {
                world.set_parent(child, id);
            }
        }
        CloseGesture(world);
    });
}

void IdeLayout::move(std::vector<std::uint32_t> ids, std::uint32_t parent) {
    if (ids.empty()) {
        return;
    }
    runner_.simulation().on_simulation([ids = std::move(ids), parent](engine_core::DataModel& world) {
        world.history().set_pending_gesture("Move");
        move_set(world, ids, parent);
        CloseGesture(world);
    });
}

void IdeLayout::rename(std::uint32_t id, std::string name) {
    if (name.empty()) {
        return;
    }
    if (std::shared_ptr<IdeScriptEditor> editor = open_editor(id)) {
        editor->setTitleText(name);
    }
    runner_.simulation().on_simulation([id, name = std::move(name)](engine_core::DataModel& world) {
        if (id != 0 && !world.alive(id)) {
            return;
        }
        world.history().set_pending_gesture("Rename");
        world.set_name(id, name);
        CloseGesture(world);
    });
}

void IdeLayout::edit(std::uint32_t id) {
    IdeDock* home = editorHome();
    if (home == nullptr) {
        return;
    }
    engine_core::DataModel& game = runner_.simulation().datamodel();
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns()) {
            return;
        }
        if (dynamic_cast<const engine_core::LuaSource*>(game.instance(id)) == nullptr) {
            return;
        }
    }
    kept_sources_.erase(id);
    if (std::shared_ptr<IdeScriptEditor> existing = open_editor(id)) {
        if (IdeDock* dock = dockContaining(existing.get())) {
            home = dock;
        }
        home->select(existing.get());
        existing->focus();
        return;
    }
    auto editor = jadefx::make<IdeScriptEditor>(runner_.simulation(), id);
    editor->bindUndo(&undo_router_.script_stack(id));
    std::shared_ptr<jadefx::Tab> tab = home->dock(editor);
    if (tab) {
        std::weak_ptr<jadefx::Tab> weak = tab;
        editor->setOnTitle([weak](const std::string& title) {
            if (std::shared_ptr<jadefx::Tab> live = weak.lock()) {
                live->setText(title);
            }
        });
        tab->setOnClosed([this, id, editor] {
            if (editor && editor->isLoaded()) {
                kept_sources_[id] = editor->text();
            }
        });
    }
    open_scripts_[id] = editor;
}

std::shared_ptr<IdeScriptEditor> IdeLayout::open_editor(std::uint32_t id) const {
    const auto found = open_scripts_.find(id);
    if (found == open_scripts_.end()) {
        return nullptr;
    }
    return found->second.lock();
}

void IdeLayout::flush_editors() {
    for (const auto& entry : open_scripts_) {
        if (std::shared_ptr<IdeScriptEditor> editor = entry.second.lock()) {
            editor->flush();
        }
    }
}

void IdeLayout::reapply_editors() {
    for (const auto& entry : open_scripts_) {
        if (std::shared_ptr<IdeScriptEditor> editor = entry.second.lock()) {
            editor->reapply();
        }
    }
}

void IdeLayout::restore_closed_edits() {
    std::unordered_map<std::uint32_t, std::string> pending;
    for (auto it = kept_sources_.begin(); it != kept_sources_.end();) {
        if (open_editor(it->first)) {
            it = kept_sources_.erase(it);
        } else {
            ++it;
        }
    }
    pending.swap(kept_sources_);
    if (pending.empty()) {
        return;
    }
    runner_.simulation().on_simulation([pending](engine_core::DataModel& game) {
        const bool edit = !game.simulation_running();
        std::optional<std::string> recording;
        if (edit) {
            recording = game.history().try_begin_recording("Edit Script");
        }
        bool changed = false;
        for (const auto& entry : pending) {
            auto* source = dynamic_cast<engine_core::LuaSource*>(game.instance(entry.first));
            if (source == nullptr || source->source() == entry.second) {
                continue;
            }
            source->set_source(entry.second);
            changed = true;
        }
        if (recording) {
            game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
        }
        if (changed && edit) {
            game.capture_place();
        }
    });
}

void IdeLayout::noteScriptFocus() {
    std::uint32_t focused = 0;
    auto consider = [&](jadefx::Scene* scene) {
        if (scene == nullptr || focused != 0) {
            return;
        }
        if (IdeScriptEditor* editor = Owning<IdeScriptEditor>(scene->focusedNode())) {
            focused = editor->instanceId();
        }
    };
    consider(scene_);
    for (Floating& item : floating_) {
        if (item.window && item.window->isOpen()) {
            consider(&item.window->stage().getScene());
        }
    }
    if (last_script_focus_ != 0 && last_script_focus_ != focused) {
        if (std::shared_ptr<IdeScriptEditor> editor = open_editor(last_script_focus_)) {
            editor->flush();
        }
    }
    last_script_focus_ = focused;
}

void IdeLayout::routeUndo(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    if (!event.pressed) {
        return;
    }
    const KeyChord chord = ChordOf(event);
    if (!is_undo(chord) && !is_redo(chord)) {
        return;
    }
    jadefx::Node* focused = scene.focusedNode();
    if (IdeScriptEditor* editor = Owning<IdeScriptEditor>(focused)) {
        Focus target;
        target.kind = FocusKind::ScriptEditor;
        target.script = editor->instanceId();
        undo_router_.set_focus(target);
        undo_router_.handle(chord, nullptr);
        editor->applyUndoText();
        event.consume();
        return;
    }
    if (const std::shared_ptr<IdeConsole> console = console_.lock()) {
        if (console->commandFocused(focused)) {
            Focus target;
            target.kind = FocusKind::OtherTextField;
            target.widget = kCommandUndo;
            undo_router_.set_focus(target);
            undo_router_.handle(chord, nullptr);
            console->applyUndoText();
            event.consume();
            return;
        }
    }
    // A Properties field undoes its own typing first. A half-typed value is not
    // a document, so once that is spent the chord goes to the place.
    const bool properties = properties_ && focused != nullptr && properties_->owns(focused);
    if (properties && properties_->field_undo(is_redo(chord))) {
        event.consume();
        return;
    }
    // A text widget we do not own still keeps the chord. Place undo does not run.
    if (!properties && InTextWidget(focused)) {
        return;
    }
    Focus target;
    if (properties) {
        target.kind = FocusKind::Properties;
    } else if (Owning<IdeExplorer>(focused) != nullptr) {
        target.kind = FocusKind::Explorer;
    } else if (Owning<runner::GameView>(focused) != nullptr) {
        target.kind = FocusKind::Viewport;
    } else {
        target.kind = FocusKind::None;
    }
    undo_router_.set_focus(target);
    if (undo_router_.classify(chord) != UndoRoute::Place) {
        return;
    }
    const bool redo = is_redo(chord);
    event.consume();
    runner_.simulation().on_simulation([redo](engine_core::DataModel& game) {
        if (redo) {
            game.history().redo();
        } else {
            game.history().undo();
        }
    });
}

void IdeLayout::show_session(bool testing, bool stepping) {
    testing_ = testing;
    stepping_ = stepping;
    if (session_buttons_[0] != nullptr) {
        ShowSession(*session_buttons_[0], *session_buttons_[1], *session_buttons_[2], *session_buttons_[3], testing,
                    stepping);
    }
}

void IdeLayout::start_test() {
    engine_core::Engine& engine = runner_.simulation();
    // Open editors write Source before the place is frozen.
    flush_editors();
    // Edit mode is the authored place. Freeze that tree before play so
    // Stop restores it, including a folder removed while stopped.
    // start_simulation alone keeps the previous snapshot.
    engine.on_simulation([](engine_core::DataModel& game) {
        if (!game.simulation_running()) {
            game.capture_place();
            game.start_simulation();
        }
    });
    engine.resume();
    show_session(true, true);
}

void IdeLayout::pause_test() {
    // The session stays active: scripts and the play tree remain, and
    // steps wait until Resume. Stop still restores the authored place.
    runner_.simulation().pause();
    show_session(true, false);
}

void IdeLayout::resume_test() {
    runner_.simulation().resume();
    show_session(true, true);
}

void IdeLayout::stop_test() {
    engine_core::Engine& engine = runner_.simulation();
    // Pause first so stop_simulation runs on this thread once the sim
    // step has released the write lock. That aborts scripts and restores
    // the place before another Heartbeat can run. Already paused is the
    // same restore.
    engine.pause();
    engine.on_simulation([](engine_core::DataModel& game) {
        if (game.simulation_running()) {
            game.stop_simulation();
        }
    });
    // Stop put the authored scripts back. Open editors, and editors closed
    // during play, write their buffers back and capture that place.
    reapply_editors();
    restore_closed_edits();
    show_session(false, false);
}

void IdeLayout::run_now(const std::function<void(engine_core::DataModel&)>& fn) {
    engine_core::Engine& engine = runner_.simulation();
    // A paused engine runs the edit here, under the write lock. A stepping
    // test waits one step for it, then carries on.
    const bool stepping = !engine.paused();
    if (stepping) {
        engine.pause();
    }
    engine.on_simulation(fn);
    if (stepping) {
        engine.resume();
    }
}

std::filesystem::path IdeLayout::dialog_directory() const {
    if (!project_) {
        return {};
    }
    return project_->root().parent_path();
}

void IdeLayout::update_title() {
    if (mainStage_ == nullptr) {
        return;
    }
    title_modified_ = place_modified_ || editors_unflushed();
    const std::string name = project_ ? project_->name() : std::string("Untitled");
    mainStage_->setTitle(name + (title_modified_ ? "*" : "") + " - Anarchy Engine");
}

void IdeLayout::show_error(const std::string& heading, const std::string& detail) {
    runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                 heading + ": " + detail);
    if (scene_ == nullptr) {
        return;
    }
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& alert) {
                                     return !alert || alert->getResult() != nullptr;
                                 }),
                  alerts_.end());
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Error, detail);
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(heading);
    alert->show(*scene_);
    alerts_.push_back(std::move(alert));
}

void IdeLayout::close_script_editors() {
    std::vector<std::shared_ptr<IdeScriptEditor>> editors;
    for (const auto& entry : open_scripts_) {
        if (std::shared_ptr<IdeScriptEditor> editor = entry.second.lock()) {
            editors.push_back(std::move(editor));
        }
    }
    for (const std::shared_ptr<IdeScriptEditor>& editor : editors) {
        IdeDock* dock = dockContaining(editor.get());
        if (dock == nullptr || dock->tabs() == nullptr) {
            continue;
        }
        const std::vector<std::shared_ptr<jadefx::Tab>> tabs = dock->tabs()->getTabs().items();
        for (const std::shared_ptr<jadefx::Tab>& tab : tabs) {
            if (tab && tab->getContent() == editor.get() && !dock->tabs()->close(tab)) {
                dock->tabs()->getTabs().removeIf(
                    [&tab](const std::shared_ptr<jadefx::Tab>& item) { return item == tab; });
            }
        }
    }
    // Those ids belong to the place that is going away.
    open_scripts_.clear();
    kept_sources_.clear();
    last_script_focus_ = 0;
}

bool IdeLayout::editors_unflushed() const {
    for (const auto& entry : open_scripts_) {
        if (std::shared_ptr<IdeScriptEditor> editor = entry.second.lock()) {
            if (editor->hasUnflushedText()) {
                return true;
            }
        }
    }
    return false;
}

void IdeLayout::mark_saved() {
    engine_core::DataModel& game = runner_.simulation().datamodel();
    run_now([this](engine_core::DataModel& world) { saved_fingerprint_ = engine_core::Project::place_fingerprint(world); });
    seen_revision_ = game.authored_revision();
    place_modified_ = false;
    update_title();
}

void IdeLayout::refresh_modified() {
    engine_core::DataModel& game = runner_.simulation().datamodel();
    const std::uint64_t revision = game.authored_revision();
    if (revision != seen_revision_) {
        // Only an authored change moves the revision, so this runs at the pace
        // of edits. Play steps do not move it.
        seen_revision_ = revision;
        std::uint64_t now = 0;
        run_now([&now](engine_core::DataModel& world) { now = engine_core::Project::place_fingerprint(world); });
        place_modified_ = now != saved_fingerprint_;
    }
    if ((place_modified_ || editors_unflushed()) != title_modified_) {
        update_title();
    }
}

bool IdeLayout::has_unsaved_changes() {
    seen_revision_ = ~std::uint64_t{0};
    refresh_modified();
    return place_modified_ || editors_unflushed();
}

void IdeLayout::confirm_discard(const std::string& question, std::function<void()> proceed) {
    if (prompt_open_ || dialog_open_) {
        return;
    }
    if (!has_unsaved_changes() || scene_ == nullptr) {
        proceed();
        return;
    }
    prompt_open_ = true;
    const jadefx::ButtonType save("Save", jadefx::ButtonType::Data::OkDone);
    const jadefx::ButtonType discard("Don't Save", jadefx::ButtonType::Data::Left);
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Warning,
                                                 "Your changes will be lost if you don't save them.",
                                                 std::vector<jadefx::ButtonType>{save, discard, jadefx::ButtonType::Cancel()});
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(question);
    alert->setOnClosed([this, save, discard, proceed = std::move(proceed)](const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice == nullptr) {
            return;
        }
        if (*choice == discard) {
            // Don't Save: the editors' text is dropped with the place.
            proceed();
        } else if (*choice == save) {
            // The editors' text is part of what is saved.
            if (testing_) {
                stop_test();
            }
            save_project(proceed);
        }
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene_);
    // Stable names for the three answers, so a test can find them.
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {
        {&save, "unsaved-save"}, {&discard, "unsaved-discard"}, {&jadefx::ButtonType::Cancel(), "unsaved-cancel"}};
    for (const auto& [type, id] : ids) {
        if (jadefx::Button* button = alert->lookupButton(*type)) {
            button->setElementId(id);
        }
    }
    alerts_.push_back(std::move(alert));
}

void IdeLayout::new_place() {
    if (testing_) {
        stop_test();
    }
    close_script_editors();
    if (clip_) {
        clip_->held = false;
    }
    run_now([](engine_core::DataModel& game) { engine_core::Project::reset_place(game); });
    project_.reset();
    mark_saved();
    runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Print, "New place");
}

void IdeLayout::open_project() {
    confirm_discard("Save changes before opening another project?", [this] {
        if (dialog_open_) {
            return;
        }
        dialog_open_ = true;
        jadefx::FolderDialogOptions options;
        options.title = "Open Project";
        options.directory = dialog_directory().u8string();
        jadefx::showFolderDialog(std::move(options), [this](jadefx::DialogResult result, const std::string& path) {
            dialog_open_ = false;
            if (result == jadefx::DialogResult::Unavailable) {
                show_error("No folder dialog",
                           "This system has no folder picker. On Linux, install zenity or kdialog. You can also "
                           "start the studio with a project folder: AnarchyEngine-CPP <folder>");
                return;
            }
            if (result == jadefx::DialogResult::Chosen) {
                open_project_at(std::filesystem::u8path(path));
            }
        });
    });
}

void IdeLayout::open_project_at(const std::filesystem::path& root) {
    // The load replaces the tree that Stop would restore.
    if (testing_) {
        stop_test();
    }
    std::unique_ptr<engine_core::Project> loaded;
    std::string error;
    run_now([&](engine_core::DataModel& game) {
        try {
            loaded = std::make_unique<engine_core::Project>(engine_core::Project::load(root, game));
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!loaded) {
        show_error("Could not open project", error);
        return;
    }
    // Every instance id changed. Editors and the clipboard pointed at the old ones.
    close_script_editors();
    if (clip_) {
        clip_->held = false;
    }
    project_ = std::move(loaded);
    mark_saved();
    runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Print,
                                                 "Opened " + project_->root().u8string());
}

bool IdeLayout::save_open_project() {
    // Open editors write Source first. During play the save writes the place
    // captured at Test, so play edits stay out of it either way.
    flush_editors();
    std::string error;
    run_now([&](engine_core::DataModel&) {
        try {
            project_->save();
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!error.empty()) {
        show_error("Could not save project", error);
        return false;
    }
    mark_saved();
    const engine_core::Project::SaveReport& report = project_->last_save();
    const std::size_t changed = report.written.size() + report.moved.size() + report.removed.size();
    runner_.simulation().scripts().append_output(
        engine_core::ScriptRuntime::OutputKind::Print,
        "Saved " + project_->root().u8string() +
            (changed == 0 ? std::string(" (no changes)") : " (" + std::to_string(changed) + " files changed)"));
    return true;
}

void IdeLayout::save_project(std::function<void()> then) {
    if (!project_) {
        save_project_as(std::move(then));
        return;
    }
    if (save_open_project() && then) {
        then();
    }
}

void IdeLayout::save_project_as(std::function<void()> then) {
    if (dialog_open_) {
        return;
    }
    dialog_open_ = true;
    jadefx::FolderDialogOptions options;
    options.title = "Save Project As";
    options.save = true;
    options.directory = dialog_directory().u8string();
    options.name = project_ ? project_->name() : std::string("MyPlace");
    jadefx::showFolderDialog(std::move(options), [this, then = std::move(then)](jadefx::DialogResult result,
                                                                              const std::string& path) {
        dialog_open_ = false;
        if (result == jadefx::DialogResult::Unavailable) {
            show_error("No folder dialog", "This system has no folder picker. On Linux, install zenity or kdialog.");
            return;
        }
        if (result == jadefx::DialogResult::Chosen && save_project_to(std::filesystem::u8path(path)) && then) {
            then();
        }
    });
}

bool IdeLayout::save_project_to(const std::filesystem::path& root) {
    flush_editors();
    std::string error;
    run_now([&](engine_core::DataModel& game) {
        try {
            if (project_) {
                project_->save_as(root);
            } else {
                project_ = std::make_unique<engine_core::Project>(engine_core::Project::adopt(root, game));
            }
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!error.empty()) {
        show_error("Could not save project", error);
        return false;
    }
    mark_saved();
    runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Print,
                                                 "Saved " + project_->root().u8string());
    return true;
}

}  // namespace ide
