#include "IdeLayout.hpp"

#include "DataModelLock.hpp"
#include "DockArrange.hpp"
#include "Engine.hpp"
#include "IdeConsole.hpp"
#include "IdeIcons.hpp"
#include "LuaApi.hpp"
#include "IdeDock.hpp"
#include "IdeExplorer.hpp"
#include "IdeScriptEditor.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"
#include "../runner/GameView.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ide {
namespace {

// MenuBar's title row is 28 points. The status strip below is 24, matching the Java toolbar.
constexpr double kMenuHeight = 28;
constexpr double kStatusHeight = 24;
constexpr double kSideWidth = 240;
constexpr double kConsoleHeight = 150;

// JadeFX key codes match GLFW. This is GLFW_KEY_F5.
constexpr int kKeyF5 = 294;

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

void AddItem(jadefx::Menu& menu, const char* label, const char* icon, int key, int mods) {
    auto item = jadefx::make<jadefx::MenuItem>(label);
    AttachIcon(*item, icon);
    if (key != 0) {
        item->setAccelerator(key, mods);
    }
    menu.getItems().add(std::move(item));
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

bool parent_ok(const engine_core::DataModel& model, engine_core::InstanceId parent) {
    return parent == 0 || (parent != engine_core::DataModel::kNoParent && model.alive(parent));
}

bool would_cycle(const engine_core::DataModel& model, engine_core::InstanceId node, engine_core::InstanceId parent) {
    if (node == 0 || parent == node) {
        return true;
    }
    engine_core::InstanceId cursor = parent;
    for (int guard = 0; cursor != 0 && cursor != engine_core::DataModel::kNoParent && guard < 100000; ++guard) {
        if (cursor == node) {
            return true;
        }
        cursor = model.parent(cursor);
    }
    return false;
}

// The click handler runs on the open menu's row. Hiding first keeps that row
// alive: a visibility change on an open menu rebuilds its rows.
// testing: a play session is active. stepping: that session is executing.
// Edit mode shows Test. A running test shows Pause and Stop. A paused test
// shows Resume and Stop.
void ShowSession(jadefx::MenuItem& test, jadefx::MenuItem& pause, jadefx::MenuItem& resume, jadefx::MenuItem& stop,
                 bool testing, bool stepping) {
    if (jadefx::Menu* menu = test.getParentMenu()) {
        menu->hide();
    }
    test.setVisible(!testing);
    pause.setVisible(testing && stepping);
    resume.setVisible(testing && !stepping);
    stop.setVisible(testing);
}

}  // namespace

struct IdeLayout::Clip {
    engine_core::InstanceId id = 0;
    engine_core::InstanceId parent = engine_core::DataModel::kNoParent;
    bool held = false;
};

struct IdeLayout::Prompt {
    std::shared_ptr<jadefx::Node> sheet;
    std::shared_ptr<jadefx::TextField> field;
    std::function<void(std::string)> apply;
};

IdeLayout::IdeLayout(double windowWidth, double windowHeight) : clip_(std::make_unique<Clip>()) {
    runner_.prepare();

    auto file = jadefx::make<jadefx::Menu>("File");
    AddItem(*file, "New", "New.png", jadefx::Key::N, jadefx::Key::ModControl);
    AddItem(*file, "Open", "Folder.png", jadefx::Key::O, jadefx::Key::ModControl);
    AddItem(*file, "Save", "Save.png", jadefx::Key::S, jadefx::Key::ModControl);
    AddItem(*file, "Save As", "SaveAs.png", jadefx::Key::S, jadefx::Key::ModControl | jadefx::Key::ModShift);

    auto edit = jadefx::make<jadefx::Menu>("Edit");
    auto test = jadefx::make<jadefx::MenuItem>("Test");
    auto pause = jadefx::make<jadefx::MenuItem>("Pause");
    auto resume = jadefx::make<jadefx::MenuItem>("Resume");
    auto stop = jadefx::make<jadefx::MenuItem>("Stop");
    AttachIcon(*test, "Play.png");
    AttachIcon(*pause, "Pause.png");
    AttachIcon(*resume, "Resume.png");
    AttachIcon(*stop, "Stop.png");
    jadefx::MenuItem* testItem = test.get();
    jadefx::MenuItem* pauseItem = pause.get();
    jadefx::MenuItem* resumeItem = resume.get();
    jadefx::MenuItem* stopItem = stop.get();
    test->setAccelerator(kKeyF5, 0);
    stop->setAccelerator(kKeyF5, 0);
    pause->setVisible(false);
    resume->setVisible(false);
    stop->setVisible(false);
    test->setOnAction([this, testItem, pauseItem, resumeItem, stopItem](jadefx::ActionEvent&) {
        engine_core::Engine& engine = runner_.simulation();
        // Open editors write Source before the place is frozen.
        flush_editors();
        // Edit mode is the authored place. Freeze that tree before play so
        // Stop restores it, including a folder removed while stopped.
        // start_simulation alone keeps the previous snapshot.
        engine.on_simulation([](engine_core::DataModel& model) {
            if (!model.simulation_running()) {
                model.capture_place();
                model.start_simulation();
            }
        });
        engine.resume();
        ShowSession(*testItem, *pauseItem, *resumeItem, *stopItem, true, true);
    });
    pause->setOnAction([this, testItem, pauseItem, resumeItem, stopItem](jadefx::ActionEvent&) {
        // The session stays active: scripts and the play tree remain, and
        // steps wait until Resume. Stop still restores the authored place.
        runner_.simulation().pause();
        ShowSession(*testItem, *pauseItem, *resumeItem, *stopItem, true, false);
    });
    resume->setOnAction([this, testItem, pauseItem, resumeItem, stopItem](jadefx::ActionEvent&) {
        runner_.simulation().resume();
        ShowSession(*testItem, *pauseItem, *resumeItem, *stopItem, true, true);
    });
    stop->setOnAction([this, testItem, pauseItem, resumeItem, stopItem](jadefx::ActionEvent&) {
        engine_core::Engine& engine = runner_.simulation();
        // Pause first so stop_simulation runs on this thread once the sim
        // step has released the write lock. That aborts scripts and restores
        // the place before another Heartbeat can run. Already paused is the
        // same restore.
        engine.pause();
        engine.on_simulation([](engine_core::DataModel& model) {
            if (model.simulation_running()) {
                model.stop_simulation();
            }
        });
        // Stop put the authored scripts back. Open editors, and editors closed
        // during play, write their buffers back and capture that place.
        reapply_editors();
        restore_closed_edits();
        ShowSession(*testItem, *pauseItem, *resumeItem, *stopItem, false, false);
    });

    auto insert = jadefx::make<jadefx::MenuItem>("Insert Triangle");
    AttachIcon(*insert, "Mesh.png");
    insert->setOnAction([this](jadefx::ActionEvent&) {
        runner_.simulation().on_simulation([](engine_core::DataModel& model) {
            int existing = 0;
            for (engine_core::InstanceId id = model.first_child(model.id()); id != 0; id = model.next_sibling(id)) {
                if (dynamic_cast<engine_core::TestTriangle*>(model.instance(id)) != nullptr) {
                    ++existing;
                }
            }
            engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
            model.set_parent(triangle.id(), model.id());
            // Spread repeats around the view so they do not stack on one point.
            const float angle = static_cast<float>(existing) * 0.9f;
            constexpr float kRadius = 0.42f;
            triangle.set_position(std::cos(angle) * kRadius, std::sin(angle) * kRadius, 0.15f);
        });
    });
    edit->getItems().add(std::move(insert));
    edit->getItems().add(std::move(test));
    edit->getItems().add(std::move(pause));
    edit->getItems().add(std::move(resume));
    edit->getItems().add(std::move(stop));

    auto view = jadefx::make<jadefx::Menu>("View");
    AddItem(*view, "Maybe :)", "Smile.png", 0, 0);

    auto menuBar = jadefx::make<jadefx::MenuBar>();
    menuBar->getMenus().add(file);
    menuBar->getMenus().add(edit);
    menuBar->getMenus().add(view);

    engine_core::DataModel& model = runner_.simulation().datamodel();
    ExplorerHost host;
    host.run = [this](std::string_view action, engine_core::InstanceId id) { run_action(action, id); };
    host.enabled = [this](std::string_view action) { return action_enabled(action); };
    host.insert = [this](std::string class_name, engine_core::InstanceId parent, std::shared_ptr<InsertResult> result) {
        runner_.simulation().on_simulation(
            [class_name = std::move(class_name), parent, result](engine_core::DataModel& world) {
                engine_core::InstanceId made = 0;
                if (parent_ok(world, parent)) {
                    if (engine_core::DataModel* created = engine_core::lua_create_instance(world, class_name.c_str())) {
                        world.set_parent(created->id(), parent);
                        made = created->id();
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
    auto gameExplorer = jadefx::make<IdeExplorer>(model, "Game Explorer", host);
    gameExplorer->setIconFile("Explorer.png");
    west->dock(gameExplorer);

    auto center = jadefx::make<IdeDock>();
    adoptDock(center);
    sceneDock_ = center.get();

    auto south = jadefx::make<IdeDock>();
    adoptDock(south);
    south->dock(jadefx::make<IdeConsole>(runner_.simulation()));

    auto east = jadefx::make<IdeDock>();
    adoptDock(east);
    auto sceneExplorer = jadefx::make<IdeExplorer>(model, "Current Scene", host);
    sceneExplorer->setIconFile("Scenes.png");
    east->dock(sceneExplorer);

    auto vertical = jadefx::make<jadefx::SplitPane>();
    vertical->setOrientation(jadefx::Orientation::Vertical);
    vertical->getItems().add(center);
    vertical->getItems().add(south);
    const double contentHeight = windowHeight - kMenuHeight - kStatusHeight;
    vertical->setDividerPositions({1.0 - Fraction(kConsoleHeight, contentHeight, 0.6)});
    jadefx::SplitPane::setResizableWithParent(*south, false);

    auto horizontal = jadefx::make<jadefx::SplitPane>();
    horizontal->getItems().add(west);
    horizontal->getItems().add(vertical);
    horizontal->getItems().add(east);
    const double side = Fraction(kSideWidth, windowWidth, 0.35);
    horizontal->setDividerPositions({side, 1.0 - side});
    jadefx::SplitPane::setResizableWithParent(*west, false);
    jadefx::SplitPane::setResizableWithParent(*east, false);
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
    root_->setTop(menuBar);
    root_->setCenter(horizontal);
    root_->setBottom(status);
}

engine_core::Engine& IdeLayout::simulation() { return runner_.simulation(); }

void IdeLayout::start() {
    // Dock the view before the threads start. Its first paint is what lets the
    // uncapped render thread leave its wait.
    sceneDock_->dock(jadefx::make<runner::GameView>(runner_));
    runner_.start();
}

void IdeLayout::mount(jadefx::Scene& scene) {
    scene_ = &scene;
    scene.setPadding(jadefx::Insets{});
    scene.setStylesheet(kStylesheet);
    scene.setRoot(root_);
}

void IdeLayout::attachFrame(jadefx::Stage& stage) {
    mainStage_ = &stage;
    resizeWindow_ = [&stage](int width, int height) { stage.setSize(width, height); };
    stage.setFrameTail([this]() { flushFrame(); });
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
    retiring_.reset();
    if (prompt_ && (prompt_->sheet == nullptr || prompt_->sheet->getScene() == nullptr)) {
        prompt_.reset();
    }
    if (action == "Cut") {
        cut(id);
    } else if (action == "Paste") {
        paste(id);
    } else if (action == "Rename") {
        rename(id);
    } else if (action == "Edit") {
        edit(id);
    }
}

bool IdeLayout::action_enabled(std::string_view action) const {
    if (action == "Paste") {
        return clip_ && clip_->held;
    }
    return true;
}

void IdeLayout::cut(std::uint32_t id) {
    if (!clip_ || id == 0) {
        return;
    }
    engine_core::DataModel& model = runner_.simulation().datamodel();
    engine_core::InstanceId old_parent = engine_core::DataModel::kNoParent;
    {
        engine_core::DataModelLock lock(model, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns() || !model.alive(id)) {
            return;
        }
        old_parent = model.parent(id);
    }
    const engine_core::InstanceId put_back = clip_->held && clip_->id != id ? clip_->id : 0;
    const engine_core::InstanceId put_parent = put_back != 0 ? clip_->parent : engine_core::DataModel::kNoParent;
    clip_->id = id;
    clip_->parent = old_parent;
    clip_->held = true;
    runner_.simulation().on_simulation([id, put_back, put_parent](engine_core::DataModel& world) {
        if (put_back != 0 && world.alive(put_back) && world.parent(put_back) == engine_core::DataModel::kNoParent &&
            parent_ok(world, put_parent) && !would_cycle(world, put_back, put_parent)) {
            world.set_parent(put_back, put_parent);
        }
        if (world.alive(id)) {
            world.set_parent(id, engine_core::DataModel::kNoParent);
        }
    });
}

void IdeLayout::paste(std::uint32_t id) {
    if (!clip_ || !clip_->held) {
        return;
    }
    const engine_core::InstanceId child = clip_->id;
    engine_core::DataModel& model = runner_.simulation().datamodel();
    {
        engine_core::DataModelLock lock(model, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns() || !model.alive(child) || !parent_ok(model, id) || would_cycle(model, child, id)) {
            return;
        }
    }
    clip_->held = false;
    runner_.simulation().on_simulation([child, id](engine_core::DataModel& world) {
        if (!world.alive(child) || !parent_ok(world, id) || would_cycle(world, child, id)) {
            return;
        }
        world.set_parent(child, id);
    });
}

void IdeLayout::rename(std::uint32_t id) {
    engine_core::DataModel& model = runner_.simulation().datamodel();
    std::string current;
    {
        engine_core::DataModelLock lock(model, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns()) {
            return;
        }
        if (id != 0 && !model.alive(id)) {
            return;
        }
        current = model.name(id);
    }
    show_rename(std::move(current), [this, id](std::string name) {
        runner_.simulation().on_simulation([id, name](engine_core::DataModel& world) {
            if (id != 0 && !world.alive(id)) {
                return;
            }
            world.set_name(id, name);
        });
        if (std::shared_ptr<IdeScriptEditor> editor = open_editor(id)) {
            editor->setTitleText(name);
        }
    });
}

void IdeLayout::edit(std::uint32_t id) {
    IdeDock* home = editorHome();
    if (home == nullptr) {
        return;
    }
    engine_core::DataModel& model = runner_.simulation().datamodel();
    {
        engine_core::DataModelLock lock(model, engine_core::DataModelLock::Read, std::chrono::milliseconds(5));
        if (!lock.owns()) {
            return;
        }
        if (dynamic_cast<const engine_core::LuaSource*>(model.instance(id)) == nullptr) {
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

void IdeLayout::close_prompt(bool apply) {
    std::shared_ptr<Prompt> prompt = std::move(prompt_);
    if (!prompt) {
        return;
    }
    std::string name = prompt->field ? prompt->field->getText() : std::string();
    std::function<void(std::string)> done = std::move(prompt->apply);
    if (scene_ != nullptr && prompt->sheet != nullptr && prompt->sheet->getScene() == scene_) {
        scene_->hidePopup(prompt->sheet.get());
    }
    retiring_ = std::move(prompt);
    if (apply && done) {
        done(std::move(name));
    }
}

void IdeLayout::show_rename(std::string current, std::function<void(std::string)> apply) {
    if (scene_ == nullptr) {
        return;
    }
    if (prompt_ && prompt_->sheet && prompt_->sheet->getScene() == scene_) {
        scene_->hidePopup(prompt_->sheet.get());
    }
    retiring_ = std::move(prompt_);

    auto sheet = jadefx::make<jadefx::VBox>();
    sheet->setSpacing(8);
    sheet->setPadding(jadefx::Insets::uniform(12));
    sheet->setPrefWidth(300);
    sheet->setBackground(jadefx::Color::white());
    sheet->getClassList().add("rename-prompt");

    auto label = jadefx::make<jadefx::Label>("Rename");
    auto field = jadefx::make<jadefx::TextField>(current);
    field->setStyle("background-color: #ffffff; border-width: 1px; border-color: #c8c8c8; padding: 6px 8px;");
    field->selectAll();
    auto row = jadefx::make<jadefx::HBox>();
    row->setSpacing(8);
    row->setAlignment(jadefx::Pos::CenterRight);
    auto cancel = jadefx::make<jadefx::Button>("Cancel");
    auto ok = jadefx::make<jadefx::Button>("OK");
    ok->setDefaultButton(true);
    cancel->setOnAction([this](jadefx::ActionEvent&) { close_prompt(false); });
    ok->setOnAction([this](jadefx::ActionEvent&) { close_prompt(true); });
    field->setOnAction([this](jadefx::ActionEvent&) { close_prompt(true); });
    row->getChildren().add(cancel);
    row->getChildren().add(ok);
    sheet->getChildren().add(label);
    sheet->getChildren().add(field);
    sheet->getChildren().add(row);

    prompt_ = std::make_shared<Prompt>();
    prompt_->sheet = sheet;
    prompt_->field = field;
    prompt_->apply = std::move(apply);

    jadefx::PopupOptions options;
    options.autoHide = true;
    scene_->showPopup(sheet, 0, 0, -1, -1, options);
    const double width = sheet->getWidth();
    const double height = sheet->getHeight();
    double x = (scene_->getWidth() - width) * 0.5;
    double y = (scene_->getHeight() - height) * 0.5;
    if (x < 8) {
        x = 8;
    }
    if (y < 8) {
        y = 8;
    }
    scene_->movePopup(sheet.get(), x, y, width, height);
    field->requestFocus();
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
    runner_.simulation().on_simulation([pending](engine_core::DataModel& model) {
        bool changed = false;
        for (const auto& entry : pending) {
            auto* source = dynamic_cast<engine_core::LuaSource*>(model.instance(entry.first));
            if (source == nullptr || source->source() == entry.second) {
                continue;
            }
            source->set_source(entry.second);
            changed = true;
        }
        if (changed && !model.simulation_running()) {
            model.capture_place();
        }
    });
}

}  // namespace ide
