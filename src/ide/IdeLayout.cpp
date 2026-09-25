#include "IdeLayout.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "IdeConsole.hpp"
#include "LuaApi.hpp"
#include "IdeDock.hpp"
#include "IdeExplorer.hpp"
#include "IdeScriptEditor.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"
#include "../runner/GameView.hpp"

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

void AddItem(jadefx::Menu& menu, const char* label, int key, int mods) {
    auto item = jadefx::make<jadefx::MenuItem>(label);
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
void ShowOne(jadefx::MenuItem& show, jadefx::MenuItem& hide) {
    if (jadefx::Menu* menu = show.getParentMenu()) {
        menu->hide();
    }
    hide.setVisible(false);
    show.setVisible(true);
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
    AddItem(*file, "New", jadefx::Key::N, jadefx::Key::ModControl);
    AddItem(*file, "Open", jadefx::Key::O, jadefx::Key::ModControl);
    AddItem(*file, "Save", jadefx::Key::S, jadefx::Key::ModControl);
    AddItem(*file, "Save As", jadefx::Key::S, jadefx::Key::ModControl | jadefx::Key::ModShift);

    auto edit = jadefx::make<jadefx::Menu>("Edit");
    auto test = jadefx::make<jadefx::MenuItem>("Test");
    auto stop = jadefx::make<jadefx::MenuItem>("Stop");
    jadefx::MenuItem* testItem = test.get();
    jadefx::MenuItem* stopItem = stop.get();
    test->setAccelerator(kKeyF5, 0);
    stop->setAccelerator(kKeyF5, 0);
    stop->setVisible(false);
    test->setOnAction([this, testItem, stopItem](jadefx::ActionEvent&) {
        engine_core::Engine& engine = runner_.simulation();
        // The open editors write Source, and while stopped that becomes the
        // place, before Test captures or resumes.
        flush_editors();
        // Opens the script VM and enqueues every eligible Script. The place is
        // captured the first time. Heartbeats after resume run task.wait.
        engine.on_simulation([](engine_core::DataModel& model) {
            if (!model.simulation_running()) {
                model.start_simulation();
            }
        });
        engine.resume();
        ShowOne(*stopItem, *testItem);
    });
    stop->setOnAction([this, testItem, stopItem](jadefx::ActionEvent&) {
        engine_core::Engine& engine = runner_.simulation();
        // Pause first so stop_simulation runs on this thread once the sim
        // step has released the write lock. That aborts scripts and restores
        // the place before another Heartbeat can run.
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
        ShowOne(*testItem, *stopItem);
    });

    auto insert = jadefx::make<jadefx::MenuItem>("Insert Triangle");
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
    edit->getItems().add(std::move(stop));

    auto view = jadefx::make<jadefx::Menu>("View");
    AddItem(*view, "Maybe :)", 0, 0);

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
    west->setMinSize(160, 80);
    // IdeTreeTest is the sample tree page. The Java shell left that dock commented out.
    west->dock(jadefx::make<IdeExplorer>(model, "Game Explorer", host));

    auto center = jadefx::make<IdeDock>();
    center->setMinSize(64, 64);
    sceneDock_ = center.get();

    auto south = jadefx::make<IdeDock>();
    south->setMinSize(80, 96);
    south->dock(jadefx::make<IdeConsole>(runner_.simulation()));

    auto east = jadefx::make<IdeDock>();
    east->setMinSize(160, 80);
    east->dock(jadefx::make<IdeExplorer>(model, "Current Scene", host));

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

IdeLayout::~IdeLayout() = default;

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
    if (sceneDock_ == nullptr) {
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
        sceneDock_->select(existing.get());
        existing->focus();
        return;
    }
    auto editor = jadefx::make<IdeScriptEditor>(runner_.simulation(), id);
    std::shared_ptr<jadefx::Tab> tab = sceneDock_->dock(editor);
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
