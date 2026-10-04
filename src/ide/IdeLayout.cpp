#include "IdeLayout.hpp"
#include "runner/ProfilerOverlay.hpp"

#include "AiClientsPage.hpp"
#include "IdeAssets.hpp"
#include "IdeLayoutInternal.hpp"
#include "McpSetup.hpp"
#include "ScratchResources.hpp"

#include "EditorFont.hpp"

namespace ide {

IdeLayout::IdeLayout(double windowWidth, double windowHeight, const std::filesystem::path& config)
    : preferences_(config.empty() ? std::filesystem::path() : config / "preferences.json"),
      themes_(config.empty() ? std::filesystem::path() : config / "themes"),
      clip_(std::make_unique<Clip>()) {
    if (!config.empty()) {
        layout_file_ = config / "layout.json";
        default_layout_file_ = config / "default-layout.json";
    }
    runner_.prepare();
    // Before any widget reads a color.
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    if (!preferences_.load_error().empty()) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error,
                              "Preferences: " + preferences_.load_error());
    }
    IdeTheme theme;
    std::string theme_error;
    if (!themes_.load(preferences_.theme(), theme, theme_error)) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error,
                              "Theme: " + theme_error + ". Drawing with Light instead.");
        theme = themes_.shipped("light");
    }
    set_editor_font_choice(preferences_.editor_font());
    set_current_theme(std::move(theme));

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
    AddItem(*file, "Export Game…", "Export.png", 0, 0)->setOnAction([this](jadefx::ActionEvent&) { export_game(); });
    file->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    AddItem(*file, "Preferences\u2026", nullptr, jadefx::Key::Comma, jadefx::Key::ModControl)
        ->setOnAction([this](jadefx::ActionEvent&) { open_preferences(); });
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
    auto gap = jadefx::make<jadefx::Pane>();
    gap->setStyle("width: 100%;");
    gap->setMouseTransparent(true);
    ribbon->getChildren().add(gap);
    // The Scene Views' floor grid, at the right end: lit while on.
    auto grid = jadefx::make<RibbonButton>("", "Grid.png", [this] { set_grid(!grid_on_); });
    grid->setElementId("grid-toggle");
    jadefx::Tooltip::install(grid.get(), jadefx::make<jadefx::Tooltip>("Show the floor grid and the world's axes"));
    grid_button_ = grid.get();
    ribbon->getChildren().add(std::move(grid));
    grid_on_ = preferences_.scene_grid();
    show_grid();

    AddItem(*edit, "Find in Scripts", "Search.png", jadefx::Key::F, jadefx::Key::ModControl | jadefx::Key::ModShift)
        ->setOnAction([this](jadefx::ActionEvent&) { open_search(false, scene_); });
    AddItem(*edit, "Replace in Scripts", nullptr, jadefx::Key::H, jadefx::Key::ModControl | jadefx::Key::ModShift)
        ->setOnAction([this](jadefx::ActionEvent&) { open_search(true, scene_); });

    auto view = jadefx::make<jadefx::Menu>("View");
    AddItem(*view, "Zoom In", nullptr, jadefx::Key::Equal, jadefx::Key::ModControl)
        ->setOnAction([this](jadefx::ActionEvent&) { set_zoom(jadefx::Stage::getZoom() + 0.1); });
    AddItem(*view, "Zoom Out", nullptr, jadefx::Key::Minus, jadefx::Key::ModControl)
        ->setOnAction([this](jadefx::ActionEvent&) { set_zoom(jadefx::Stage::getZoom() - 0.1); });
    AddItem(*view, "Actual Size", nullptr, jadefx::Key::Digit0, jadefx::Key::ModControl)
        ->setOnAction([this](jadefx::ActionEvent&) { set_zoom(1.0); });
    view->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    // The frame profiler, over the Scene View last clicked.
    AddItem(*view, "Profiler", nullptr, jadefx::Key::F6, jadefx::Key::ModControl)
        ->setOnAction([](jadefx::ActionEvent&) { runner::ProfilerUi::get().toggleShown(); });
    AddItem(*view, "Pause Profiler", nullptr, jadefx::Key::P, jadefx::Key::ModControl)
        ->setOnAction([](jadefx::ActionEvent&) { runner::ProfilerUi::get().togglePaused(); });
    view->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    AddItem(*view, "Maybe :)", "Smile.png", 0, 0);

    // Filled once the windows it lists are docked, below.
    auto window = jadefx::make<jadefx::Menu>("Window");

    auto menuBar = jadefx::make<jadefx::MenuBar>();
    menuBar->getMenus().add(file);
    menuBar->getMenus().add(edit);
    menuBar->getMenus().add(view);
    menuBar->getMenus().add(window);
    menuBar->setPrefWidthRatio(1);

    auto top = jadefx::make<jadefx::VBox>();
    top->setPrefWidthRatio(1);
    top->getChildren().add(menuBar);
    top->getChildren().add(ribbon);

    engine_core::DataModel& game = runner_.simulation().datamodel();
    ExplorerHost host;
    host.run = [this](engine_core::InstanceAction action, engine_core::InstanceId id) { run_action(action, id); };
    host.run_many = [this](engine_core::InstanceAction action, const std::vector<engine_core::InstanceId>& ids) {
        if (action == engine_core::InstanceAction::Delete) {
            delete_instances(ids);
        } else if (action == engine_core::InstanceAction::Cut) {
            cut(ids);
        } else if (action == engine_core::InstanceAction::Copy) {
            copy(ids);
        } else if (action == engine_core::InstanceAction::Duplicate) {
            duplicate(ids);
        } else if (action == engine_core::InstanceAction::Paste) {
            paste(ids);
        }
    };
    host.enabled = [this](engine_core::InstanceAction action) { return action_enabled(action); };
    host.notice = [this](std::string text) { show_toast(std::move(text)); };
    host.rename = [this](engine_core::InstanceId id, std::string name) { rename(id, std::move(name)); };
    host.move = [this](const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId parent) {
        move(ids, parent);
    };
    host.insert = [this](std::string class_name, engine_core::InstanceId parent, std::shared_ptr<InsertResult> result) {
        runner_.simulation().on_simulation(
            [class_name = std::move(class_name), asked = parent, result](engine_core::DataModel& world) {
                std::string error;
                engine_core::InstanceId made = 0;
                {
                    ScopedRecording step(world, "Insert " + class_name);
                    made = insert_instance(world, class_name, asked, error);
                }
                if (result) {
                    result->id.store(made, std::memory_order_relaxed);
                    result->error = std::move(error);
                    result->done.store(true, std::memory_order_release);
                }
            });
    };

    // The Assets pane, made later, runs the same actions.
    explorer_host_ = host;

    auto gameExplorer = jadefx::make<IdeExplorer>(game, "Game Explorer", host);
    gameExplorer->setIconFile("Explorer.png");
    explorers_.push_back(gameExplorer);

    auto sceneExplorer = jadefx::make<IdeExplorer>(game, "Current Scene", host);
    sceneExplorer->setIconFile("Scenes.png");
    explorers_.push_back(sceneExplorer);

    auto console = jadefx::make<IdeConsole>(runner_.simulation());
    console->bindUndo(&undo_router_.widget_stack(kCommandUndo));
    console_ = console;
    // Clicking a line a script printed opens that script at the line.
    console->log().setOnOpenScript([this](std::uint32_t script, int line) {
        this->edit(script);
        if (std::shared_ptr<IdeScriptEditor> editor = open_editor(script)) {
            editor->showLine(line);
        }
    });

    // Its writes go to the simulation thread, like a rename.
    properties_ = std::make_unique<PropertiesPanel>();
    properties_->set_runner([this](std::function<void(engine_core::DataModel&)> write) {
        runner_.simulation().on_simulation(std::move(write));
    });
    properties_->bind(game, game.selection(), game.history());

    // Docked before the threads start. Its first paint is what lets the
    // uncapped render thread leave its wait.
    auto game_view = jadefx::make<runner::GameView>(runner_);
    frame_view_ = game_view.get();
    scene_view_ = std::move(game_view);
    accept_prefab_drops(*scene_view_);

    auto status = jadefx::make<jadefx::HBox>();
    status->getClassList().add("ide-status");
    status->setSpacing(2);
    status->setAlignment(jadefx::Pos::CenterLeft);
    status->setMinSize(0, kStatusHeight);
    status->setPrefHeight(kStatusHeight);
    // At the left end, whether a test runs, and whether there is work to save.
    auto play = jadefx::make<StatusChip>();
    play->setElementId("play-state");
    play_state_text_ = play->add_label("Editing.png", "play-state-text", "Editing");
    play_chip_ = play.get();
    status->getChildren().add(std::move(play));
    auto saved = jadefx::make<StatusChip>([this] { save_project(); });
    saved->setElementId("save-state");
    save_state_text_ = saved->add_label("Save.png", "save-state-text", "Saved");
    save_tip_ = jadefx::make<jadefx::Tooltip>("");
    jadefx::Tooltip::install(saved.get(), save_tip_);
    save_chip_ = saved.get();
    status->getChildren().add(std::move(saved));
    // The rest sit at the right end.
    auto status_gap = jadefx::make<jadefx::Pane>();
    status_gap->setStyle("width: 100%;");
    status_gap->setMouseTransparent(true);
    status->getChildren().add(std::move(status_gap));
    // The caret in the code editor that has the keyboard. Hidden otherwise.
    auto cursor = jadefx::make<StatusChip>();
    cursor->setElementId("cursor-position");
    cursor->setVisible(false);
    cursor_text_ = cursor->add_label(nullptr, "cursor-position-text", "");
    cursor_chip_ = cursor.get();
    status->getChildren().add(std::move(cursor));
    // How long the Scene View takes between paints. Opens the frame rate limit.
    auto frame = jadefx::make<StatusChip>([this] { open_preferences("Performance"); });
    frame->setElementId("frame-time");
    frame_text_ = frame->add_label("FrameTime.png", "frame-time-text", "-- ms");
    frame_tip_ = jadefx::make<jadefx::Tooltip>("The Scene View is not drawing");
    jadefx::Tooltip::install(frame.get(), frame_tip_);
    status->getChildren().add(std::move(frame));
    // The studio's zoom. A click goes back to actual size.
    auto zoom = jadefx::make<StatusChip>([this] { set_zoom(1.0); });
    zoom->setElementId("zoom-level");
    zoom_text_ = zoom->add_label("Zoom.png", "zoom-level-text", "100%");
    jadefx::Tooltip::install(zoom.get(), jadefx::make<jadefx::Tooltip>("Zoom. Click for actual size"));
    status->getChildren().add(std::move(zoom));
    // The MCP server, and the AI client using it. Opens Preferences at AI.
    auto ai = jadefx::make<StatusChip>([this] { open_preferences("AI"); });
    ai->setElementId("ai-client");
    ai_text_ = ai->add_label("Robot.png", "ai-client-text", "AI off");
    ai_tip_ = jadefx::make<jadefx::Tooltip>("");
    jadefx::Tooltip::install(ai.get(), ai_tip_);
    ai_chip_ = ai.get();
    status->getChildren().add(std::move(ai));
    // The place's script errors and warnings, as Problems counts them. Opens Problems.
    auto problems = jadefx::make<StatusChip>([this] { show_problems(); });
    problems->setElementId("problems-count");
    problem_errors_text_ = problems->add_label("Error.png", "problems-errors-text");
    problem_warnings_text_ = problems->add_label("Exclamation.png", "problems-warnings-text");
    problem_tip_ = jadefx::make<jadefx::Tooltip>("No problems");
    jadefx::Tooltip::install(problems.get(), problem_tip_);
    status->getChildren().add(std::move(problems));
    // Conflicts with the disk, shown only when there are any. Opens Conflicts.
    auto conflicts = jadefx::make<StatusChip>([this] { show_conflicts(); });
    conflicts->setElementId("conflicts-count");
    conflicts->setVisible(false);
    conflict_count_text_ = conflicts->add_label("DiskConflict.png", "conflicts-count-text");
    conflict_tip_ = jadefx::make<jadefx::Tooltip>("");
    jadefx::Tooltip::install(conflicts.get(), conflict_tip_);
    conflict_count_ = conflicts.get();
    status->getChildren().add(std::move(conflicts));

    root_ = jadefx::make<jadefx::BorderPane>();
    root_->setPrefWidthRatio(1);
    root_->setPrefHeightRatio(1);
    root_->getClassList().add("ide-root");
    // Image and model files dropped anywhere a pane does not take them are imported.
    accept_file_drops(*root_);
    root_->setTop(top);
    root_->setBottom(status);

    // The Window menu's pages, kept while their tabs are closed. When the dock
    // one closed from is gone, it opens again where the default layout has it.
    auto keep = [this](std::shared_ptr<IdePane> pane, std::function<IdeDock*()> home) -> WindowEntry& {
        auto entry = std::make_unique<WindowEntry>();
        entry->name = pane->name();
        entry->icon = pane->iconFile();
        entry->pane = std::move(pane);
        entry->home = std::move(home);
        windows_.push_back(std::move(entry));
        return *windows_.back();
    };
    // Made the first time it opens, and closed until then.
    auto keep_closed = [this](std::string name, std::string icon,
                              std::function<std::shared_ptr<IdePane>()> make) -> WindowEntry& {
        auto entry = std::make_unique<WindowEntry>();
        entry->name = std::move(name);
        entry->icon = std::move(icon);
        entry->make = std::move(make);
        entry->home = [this] { return side_home(); };
        entry->starts_closed = true;
        windows_.push_back(std::move(entry));
        return *windows_.back();
    };
    keep(gameExplorer, [this] { return dock_beside(nullptr, DropSide::Left, kSideWidth); });
    keep(sceneExplorer, [this] { return dock_beside(nullptr, DropSide::Right, kSideWidth); });
    keep(properties_->dock_widget(), [this, above = sceneExplorer.get()] {
        if (IdeDock* dock = dockContaining(above)) {
            return dock_beside(dock, DropSide::Bottom, dock->getHeight() * 0.5);
        }
        return dock_beside(nullptr, DropSide::Right, kSideWidth);
    });
    keep(console, [this] {
        IdeDock* above = sceneDock_ != nullptr && sceneDock_->getParent() != nullptr ? sceneDock_ : nullptr;
        return dock_beside(above, DropSide::Bottom, kConsoleHeight);
    });
    search_window_ = &keep_closed("Search", "Search.png", [this] { return make_search(); });
    search_window_->open = [this] { open_search(false, scene_); };
    conflicts_window_ = &keep_closed("Conflicts", "DiskConflict.png", [this] { return make_conflicts(); });
    // Made now, not lazily like Search and Conflicts: so flushFrame can tick
    // it from the first frame, and the list and the title count from startup
    // whether or not Problems is ever opened. Still closed until the Window
    // menu, its checkbox, or a saved layout opens it; window_page then hands
    // out this same pane, since it has no make to remake it.
    problems_window_ = &keep(make_problems(), [this] { return beside_console(); });
    problems_window_->starts_closed = true;
    assets_window_ = &keep_closed("Assets", "AssetFolder.png", [this] { return make_assets(); });
    // In with the console, as a project browser docks under the scene.
    assets_window_->home = [this] { return beside_console(); };

    if (!restore_layout() && !apply_builtin_layout()) {
        default_layout(windowWidth, windowHeight,
                       [](IdeDock& dock, const std::shared_ptr<IdePane>& page) { dock.dock(page); });
    }
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        watch_close(*entry);
    }
    fill_window_menu(*window);
}

engine_core::Engine& IdeLayout::simulation() { return runner_.simulation(); }

void IdeLayout::start() {
    runner_.start();
    // The app made an untitled place, which keeps its resources in scratch.
    if (!project_) {
        begin_scratch();
    }
    update_title();
    // The initial place exists by now: the app makes it right after
    // constructing this layout, before calling start. Loading here, rather
    // than in the constructor, keeps the plugins' Scripts out of that place's
    // clearing.
    load_plugins();
}

void IdeLayout::load_plugins() {
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    std::vector<std::string> errors;
    const std::vector<PluginFile> files = read_builtin_plugins(errors);
    for (const std::string& error : errors) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error, "Plugin: " + error);
    }
    run_now([&](engine_core::DataModel& game) { plugins_.load(game, scripts, files); });
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
            if (event.shift && in_test()) {
                event.consume();
                stop_test();
                return;
            }
            if (!event.shift && !in_test()) {
                event.consume();
                start_test();
                return;
            }
            if (!event.shift && play_ != PlayState::Running) {
                event.consume();
                resume_test();
                return;
            }
        }
        if (scene_ != nullptr) {
            routeKeys(event, *scene_);
        }
    });
    for (auto& [text, seconds] : pending_toasts_) {
        show_toast(std::move(text), seconds);
    }
    pending_toasts_.clear();
}

void IdeLayout::attachFrame(jadefx::Stage& stage) {
    mainStage_ = &stage;
    // 120 unless Preferences > Performance says otherwise. JadeFX's own default is 60.
    stage.setMaxFrameRate(Preferences::stage_frame_rate(preferences_.frame_rate()));
    jadefx::Stage::setZoom(preferences_.zoom());
    resizeWindow_ =[&stage](int width, int height) { stage.setSize(width, height); };
    stage.setFrameTail([this]() { flushFrame(); });
    LeaveFieldsOnEscape(stage);
    // The close button, Alt+F4, and Cmd+Q ask about unsaved work first.
    stage.setOnCloseRequest([this]() {
        // Kept whether or not the close goes ahead.
        save_layout();
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
    restore_window(stage);
    restore_floating();
}

namespace {

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
}  // namespace

void IdeLayout::flushFrame() {
    ++frames_;
    noteScriptFocus();
    // Coming back to the window checks the disk. During a test the check only
    // notes that changes wait; after it, or once an edit ends, it runs.
    const bool focused = scene_ != nullptr && scene_->isWindowFocused();
    if (focused && !was_focused_) {
        if (in_test()) {
            check_disk();
        } else {
            check_pending_ = true;
        }
    }
    was_focused_ = focused;
    if (check_pending_ && !in_test() && !editing_field()) {
        check_disk();
    }
    refresh_modified();
    // A tab that is not showing, or a closed page kept for reopening, is not
    // laid out, so Problems would stop counting. Its tick keeps the list and
    // the title current and leaves the rows until it shows again. It does
    // nothing a layout this frame already did.
    if (problems_window_ != nullptr && problems_window_->pane) {
        if (auto* problems = dynamic_cast<IdeProblems*>(problems_window_->pane.get())) {
            problems->tick(scene_ != nullptr ? scene_->timeSeconds() : 0);
            show_problem_count(problems->list().total);
        }
    }
    show_cursor_position();
    show_frame_time();
    show_zoom();
    show_ai_client();
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
    stop_mcp();
    alive_.reset();
    // Window teardown calls the close hook. Drop it first so that hook does not
    // touch docks that are already being destroyed.
    for (Floating& item : floating_) {
        if (!item.window) {
            continue;
        }
        item.window->setOnClosed(nullptr);
        item.window->setCanClose(nullptr);
    }
    if (preferences_window_) {
        preferences_window_->setOnClosed(nullptr);
        preferences_window_->setCanClose(nullptr);
    }
    // Closing asked to save first; what is still in scratch was not kept.
    remove_scratch_resources(scratch_resources_);
}

void IdeLayout::routeKeys(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    routeUndo(event, scene);
    routeDelete(event, scene);
    routeClipboard(event, scene);
    routeReveal(event, scene);
    routeSearch(event, scene);
    routeZoom(event);
}

void IdeLayout::routeZoom(jadefx::KeyEvent& event) {
    constexpr int kKeypad0 = 320;
    constexpr int kKeypadSubtract = 333;
    constexpr int kKeypadAdd = 334;
    if (!event.pressed || event.consumed || !event.shortcut() || event.alt) {
        return;
    }
    if (event.key == jadefx::Key::Equal || event.key == kKeypadAdd) {
        set_zoom(jadefx::Stage::getZoom() + 0.1);
    } else if (event.key == jadefx::Key::Minus || event.key == kKeypadSubtract) {
        set_zoom(jadefx::Stage::getZoom() - 0.1);
    } else if ((event.key == jadefx::Key::Digit0 || event.key == kKeypad0) && !event.shift) {
        set_zoom(1.0);
    } else {
        return;
    }
    event.consume();
}

void IdeLayout::set_zoom(double zoom) {
    const double kept = std::clamp(std::round(zoom * 10.0) / 10.0, Preferences::kMinZoom, Preferences::kMaxZoom);
    jadefx::Stage::setZoom(kept);
    preferences_.set_zoom(kept);
    std::string failure;
    preferences_.save(failure);
    show_toast("Zoom " + std::to_string(static_cast<int>(std::lround(kept * 100.0))) + "%");
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
        if (explorer->run_on_selection(engine_core::InstanceAction::Delete)) {
            event.consume();
        }
    }
}

void IdeLayout::routeClipboard(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    if (!event.pressed || event.repeat || event.consumed || !event.shortcut() || event.alt) {
        return;
    }
    const int key = event.key;
    if (key != jadefx::Key::X && key != jadefx::Key::C && key != jadefx::Key::V && key != jadefx::Key::D) {
        return;
    }
    // Shift only changes V: Paste goes beside the selection, Shift+Paste into it.
    if (event.shift && key != jadefx::Key::V) {
        return;
    }
    jadefx::Node* focused = scene.focusedNode();
    if (InTextWidget(focused) || Owning<IdeExplorer>(focused) == nullptr) {
        return;
    }
    const std::vector<engine_core::InstanceId> selected = runner_.simulation().datamodel().selection().get();
    if (key == jadefx::Key::V) {
        paste(selected, !event.shift);
    } else if (selected.empty()) {
        return;
    } else if (key == jadefx::Key::X) {
        cut(selected);
    } else if (key == jadefx::Key::C) {
        copy(selected);
    } else {
        duplicate(selected);
    }
    event.consume();
}

void IdeLayout::routeReveal(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    if (!event.pressed || event.repeat || event.consumed || event.key != jadefx::Key::F || event.shift ||
        event.alt || event.control || event.meta || InTextWidget(scene.focusedNode())) {
        return;
    }
    bool any = false;
    for (const std::weak_ptr<IdeExplorer>& weak : explorers_) {
        // A closed explorer is kept for the Window menu, and has nothing to show.
        const std::shared_ptr<IdeExplorer> explorer = weak.lock();
        if (explorer && dockContaining(explorer.get()) != nullptr) {
            any = explorer->reveal_selection() || any;
        }
    }
    if (any) {
        event.consume();
    }
}

void IdeLayout::routeSearch(jadefx::KeyEvent& event, jadefx::Scene& scene) {
    if (event.consumed) {
        return;
    }
    const FindChord chord = find_chord(event);
    if (chord == FindChord::FindInScripts || chord == FindChord::ReplaceInScripts) {
        event.consume();
        open_search(chord == FindChord::ReplaceInScripts, &scene);
    }
}

bool IdeLayout::editing_field() const {
    if (scene_ == nullptr) {
        return false;
    }
    jadefx::Node* focused = scene_->focusedNode();
    // PropertiesPanel is not a node, so it answers for its own fields.
    return InTextWidget(focused) &&
           (Owning<IdeExplorer>(focused) != nullptr || Owning<IdeAssets>(focused) != nullptr ||
            (properties_ != nullptr && properties_->owns(focused)));
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
    // The find bar's fields are text fields of their own, not the script.
    IdeScriptEditor* editor = Owning<IdeScriptEditor>(focused);
    if (editor != nullptr && !editor->findOwns(focused)) {
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

void IdeLayout::show_session(PlayState state) {
    play_ = state;
    runner_.setTesting(in_test());
    // Changes on disk wait for Stop, and so does Apply.
    if (conflicts_pane_) {
        conflicts_pane_->setApplyEnabled(!in_test());
    }
    if (session_buttons_[0] != nullptr) {
        ShowSession(*session_buttons_[0], *session_buttons_[1], *session_buttons_[2], *session_buttons_[3], in_test(),
                    play_ == PlayState::Running);
    }
    // A test shows the game as it plays, without the editor's grid.
    show_grid();
    show_play_state();
}

void IdeLayout::set_grid(bool on) {
    grid_on_ = on;
    preferences_.set_scene_grid(on);
    std::string error;
    preferences_.save(error);
    show_grid();
}

void IdeLayout::show_grid() {
    runner_.setSceneGrid(grid_on_ && !in_test());
    if (grid_button_ != nullptr) {
        SetStyleClass(*grid_button_, "on", grid_on_);
    }
}

void IdeLayout::start_test() {
    engine_core::Engine& engine = runner_.simulation();
    // A sound auditioned in Properties is not heard over the game.
    properties_->stop_sound();
    // Open editors write Source before the place is frozen.
    flush_editors();
    // Edit mode is the authored place. Freeze that tree before play so
    // Stop restores it, including a folder removed while stopped.
    // start_simulation alone keeps the previous snapshot.
    engine.on_simulation([](engine_core::DataModel& game) {
        if (!game.simulation_running()) {
            // The place Stop brings back. Edits while stopped do not snapshot the
            // place each time; this one capture, as play starts, holds them all.
            game.capture_place();
            game.start_simulation();
        }
    });
    engine.resume();
    show_session(PlayState::Running);
    // An open console brings its tab forward for the test's output. A closed one
    // stays closed, and a floating one is not raised: that would take the
    // keyboard from the game.
    const std::shared_ptr<IdeConsole> console = console_.lock();
    if (IdeDock* dock = dockContaining(console.get())) {
        dock->select(console.get());
    }
}

void IdeLayout::pause_test() {
    // The session stays active: scripts and the play tree remain, and
    // steps wait until Resume. Stop still restores the authored place.
    runner_.simulation().pause();
    show_session(PlayState::Paused);
}

void IdeLayout::resume_test() {
    runner_.simulation().resume();
    show_session(PlayState::Running);
}

void IdeLayout::stop_test() {
    engine_core::Engine& engine = runner_.simulation();
    // Nor one started during the test left playing after it.
    properties_->stop_sound();
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
    // during play, write their buffers back; the next Test captures them.
    reapply_editors();
    restore_closed_edits();
    show_session(PlayState::Stopped);
    // Changes on disk that waited for the test load on the next frame.
    check_pending_ = true;
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

std::filesystem::path IdeLayout::place_resources() const {
    return project_ ? project_->resources_root() : scratch_resources_;
}

void IdeLayout::begin_scratch() {
    end_scratch();
    scratch_resources_ = new_scratch_resources();
    run_now([this](engine_core::DataModel& game) { game.set_resources_root(scratch_resources_); });
}

void IdeLayout::end_scratch() {
    remove_scratch_resources(scratch_resources_);
    scratch_resources_.clear();
}

void IdeLayout::update_title() {
    // Open, New, and Save As all come here, so the registry follows the project.
    publish_studio();
    if (mainStage_ == nullptr) {
        return;
    }
    title_modified_ = has_unsaved_changes();
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

void IdeLayout::toast_later(IdeLayout* layout, std::weak_ptr<int> alive, std::string text) {
    jadefx::runLater([layout, alive = std::move(alive), text = std::move(text)]() mutable {
        if (!alive.expired()) {
            layout->show_toast(std::move(text));
        }
    });
}

void IdeLayout::show_toast(std::string text, double seconds) {
    if (scene_ == nullptr) {
        pending_toasts_.emplace_back(std::move(text), seconds);
        return;
    }
    jadefx::Toast::show(*root_, std::move(text), seconds, jadefx::Pos::BottomRight);
}

void IdeLayout::open_preferences(const std::string& page) {
    if (preferences_window_ && preferences_window_->isOpen()) {
        if (!page.empty() && preferences_panel_) {
            preferences_panel_->show_page(page);
            preferences_window_->toFront();
        }
        return;
    }
    constexpr int kWidth = 700;
    constexpr int kHeight = 640;
    // Centered across the main window, a little below its top.
    double x = 120;
    double y = 120;
    double screenX = 0;
    double screenY = 0;
    if (mainStage_ != nullptr && scene_ != nullptr && jadefx::stageToScreen(*mainStage_, 0, 0, screenX, screenY)) {
        x = screenX + std::max(0.0, (scene_->getWidth() - kWidth) * 0.5);
        y = screenY + 60;
    }
    std::shared_ptr<jadefx::UtilityWindow> window = jadefx::UtilityWindow::open("Preferences", kWidth, kHeight, x, y);
    if (!window) {
        return;
    }
    auto panel = jadefx::make<PreferencesPanel>(themes_, preferences_);
    panel->set_on_frame_rate([this](int fps) {
        if (mainStage_ != nullptr) {
            mainStage_->setMaxFrameRate(Preferences::stage_frame_rate(fps));
        }
    });
    AiClientsPage::Server server;
    server.setting = [this] { return decide_mcp(preferences_.mcp_enabled(), engine_core::environment_variable); };
    server.set_enabled = [this](bool on) {
        preferences_.set_mcp_enabled(on);
        std::string error;
        if (!preferences_.save(error)) {
            runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                         "Could not save your preferences: " + error);
        }
        apply_mcp_setting();
    };
    server.status = [this] { return mcp_status(); };
    AiClientsPage::ClaudeCode claude;
    claude.cli = find_claude_cli();
    claude.bridge = find_bridge(executable_directory());
    panel->add_page("AI", jadefx::make<AiClientsPage>(std::move(server), std::move(claude)));
    auto scene = jadefx::make<jadefx::Scene>(panel, static_cast<double>(kWidth), static_cast<double>(kHeight));
    window->stage().setScene(std::move(scene));
    LeaveFieldsOnEscape(window->stage());
    // Unsaved colors ask first. The answer closes the window on a later frame, off the alert's own event.
    std::weak_ptr<jadefx::UtilityWindow> weak = window;
    window->setCanClose([this, weak]() {
        if (!preferences_panel_) {
            return true;
        }
        return preferences_panel_->request_close([weak] {
            jadefx::runLater([weak] {
                if (std::shared_ptr<jadefx::UtilityWindow> open = weak.lock()) {
                    open->close();
                }
            });
        });
    });
    window->setOnClosed([this]() {
        preferences_panel_.reset();
        preferences_window_.reset();
    });
    if (!page.empty()) {
        panel->show_page(page);
    }
    preferences_window_ = std::move(window);
    preferences_panel_ = std::move(panel);
}

}  // namespace ide
