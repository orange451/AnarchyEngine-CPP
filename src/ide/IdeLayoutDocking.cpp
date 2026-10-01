// IdeLayout: docks, floating windows, tab drags, the Window menu, and layout.json.

#include "IdeLayout.hpp"

#include "IdeLayoutInternal.hpp"

namespace ide {

namespace {

// A layout file capture_layout wrote. False, with why set, when it cannot be
// read or is not version 1.
bool ReadLayoutFile(const std::filesystem::path& file, engine_core::JsonValue& saved, std::string& why) {
    std::string text;
    if (!read_file(file, text, why)) {
        return false;
    }
    if (!engine_core::parse_json(text, saved, why)) {
        why = utf8_path(file) + ": " + why;
        return false;
    }
    const engine_core::JsonValue* version = saved.find("version");
    if (version == nullptr || version->as_number() != 1) {
        why = utf8_path(file) + " is not a layout this studio reads";
        return false;
    }
    return true;
}

// The tab in one of docks that shows page, or null.
std::shared_ptr<jadefx::Tab> TabShowing(const std::vector<std::shared_ptr<IdeDock>>& docks, const jadefx::Node* page) {
    for (const std::shared_ptr<IdeDock>& dock : docks) {
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            if (tab && tab->getContent() == page) {
                return tab;
            }
        }
    }
    return nullptr;
}

}  // namespace

void IdeLayout::default_layout(double windowWidth, double windowHeight,
                               const std::function<void(IdeDock&, const std::shared_ptr<IdePane>&)>& place) {
    // The game explorer on the left, the scene view over the console, and the
    // scene explorer over Properties on the right, as in Roblox Studio.
    auto west = jadefx::make<IdeDock>();
    adoptDock(west);
    if (const std::shared_ptr<IdeExplorer> explorer = explorers_[0].lock()) {
        place(*west, explorer);
    }

    auto center = jadefx::make<IdeDock>();
    adoptDock(center);
    place(*center, scene_view_);
    sceneDock_ = center.get();

    auto south = jadefx::make<IdeDock>();
    adoptDock(south);
    if (const std::shared_ptr<IdeConsole> console = console_.lock()) {
        place(*south, console);
    }

    auto east = jadefx::make<IdeDock>();
    adoptDock(east);
    if (const std::shared_ptr<IdeExplorer> explorer = explorers_[1].lock()) {
        place(*east, explorer);
    }

    auto propertiesDock = jadefx::make<IdeDock>();
    adoptDock(propertiesDock);
    place(*propertiesDock, properties_->dock_widget());

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
    root_->setCenter(horizontal);
}

void IdeLayout::reset_layout() {
    // Read again each time, so a default another studio saved is the one used.
    if (!default_layout_file_.empty()) {
        default_layout_ = engine_core::JsonValue();
        std::error_code missing;
        std::string why;
        if (std::filesystem::exists(default_layout_file_, missing) &&
            !ReadLayoutFile(default_layout_file_, default_layout_, why)) {
            default_layout_ = engine_core::JsonValue();
            runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                         "Default layout: " + why +
                                                             ". Resetting to the built-in layout.");
        }
    }
    if (!default_layout_.is_null()) {
        if (apply_layout(default_layout_)) {
            return;
        }
        const std::string file =
            default_layout_file_.empty() ? std::string("The saved default") : utf8_path(default_layout_file_);
        runner_.simulation().scripts().append_output(
            engine_core::ScriptRuntime::OutputKind::Error,
            "Default layout: " + file + " docks nothing in the main window. Resetting to the built-in layout.");
    }
    reset_builtin_layout();
}

bool IdeLayout::apply_builtin_layout() {
    const std::filesystem::path file = find_resource("layouts/default-layout.json");
    std::string why;
    engine_core::JsonValue saved;
    if (file.empty()) {
        why = "resources/layouts/default-layout.json is missing";
    } else if (ReadLayoutFile(file, saved, why)) {
        if (apply_layout(saved)) {
            return true;
        }
        why = utf8_path(file) + " docks nothing in the main window";
    }
    runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                 "Built-in layout: " + why + ". Using the studio's own.");
    return false;
}

void IdeLayout::reset_builtin_layout() {
    if (apply_builtin_layout()) {
        return;
    }
    const std::vector<std::shared_ptr<IdeDock>> old_docks = docks_;
    std::vector<std::shared_ptr<jadefx::UtilityWindow>> old_windows;
    for (const Floating& item : floating_) {
        if (item.window) {
            old_windows.push_back(item.window);
        }
    }
    // A copy, since moving a tab takes it out of the list it was found in.
    auto tab_of = [&old_docks](const jadefx::Node* page) { return TabShowing(old_docks, page); };
    auto kept = [this](const jadefx::Node* page) {
        for (const std::unique_ptr<WindowEntry>& entry : windows_) {
            if (entry->pane && entry->pane.get() == page) {
                return true;
            }
        }
        return page == scene_view_.get();
    };
    // Search and Conflicts are closed in the default layout. Each keeps its page for next time.
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        if (!entry->starts_closed || !entry->pane) {
            continue;
        }
        if (const std::shared_ptr<jadefx::Tab> tab = tab_of(entry->pane.get()); tab && tab->getTabPane() != nullptr) {
            tab->getTabPane()->close(tab);
        }
    }
    // Script editors, extra scene views, and terminals stay open, beside the scene view.
    std::vector<std::shared_ptr<jadefx::Tab>> others;
    for (const std::shared_ptr<IdeDock>& dock : old_docks) {
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            if (tab && !kept(tab->getContent())) {
                others.push_back(tab);
            }
        }
    }
    const double width = scene_ != nullptr ? scene_->getWidth() : root_->getWidth();
    const double height = scene_ != nullptr ? scene_->getHeight() : root_->getHeight();
    // A page that is open moves with its tab. A closed one opens.
    default_layout(width, height, [this, &tab_of](IdeDock& dock, const std::shared_ptr<IdePane>& page) {
        if (const std::shared_ptr<jadefx::Tab> tab = tab_of(page.get())) {
            dock.take(tab);
            return;
        }
        dock.dock(page);
        for (const std::unique_ptr<WindowEntry>& entry : windows_) {
            if (entry->pane == page) {
                watch_close(*entry);
            }
        }
    });
    for (const std::shared_ptr<jadefx::Tab>& tab : others) {
        sceneDock_->take(tab);
    }
    sceneDock_->select(scene_view_.get());
    for (const std::shared_ptr<IdeDock>& dock : old_docks) {
        forgetDock(dock);
    }
    for (const std::shared_ptr<jadefx::UtilityWindow>& window : old_windows) {
        if (window->isOpen()) {
            window->close();
        }
    }
    rebindUtilities();
}

bool IdeLayout::apply_layout(const engine_core::JsonValue& saved) {
    const engine_core::JsonValue* main = saved.find("main");
    if (main == nullptr) {
        return false;
    }
    const std::vector<std::shared_ptr<IdeDock>> old_docks = docks_;
    std::vector<std::shared_ptr<jadefx::UtilityWindow>> old_windows;
    for (const Floating& item : floating_) {
        if (item.window) {
            old_windows.push_back(item.window);
        }
    }
    // As layout_host, except that a page already open moves with its tab.
    auto placed = std::make_shared<std::unordered_set<const IdePane*>>();
    LayoutHost host = layout_host();
    host.dock_page = [this, &old_docks, placed](IdeDock& dock, const std::string& name) {
        const std::shared_ptr<IdePane> page = page_named(name);
        if (!page || placed->count(page.get()) != 0) {
            return false;
        }
        placed->insert(page.get());
        if (const std::shared_ptr<jadefx::Tab> tab = TabShowing(old_docks, page.get())) {
            dock.take(tab);
            return true;
        }
        dock.dock(page);
        for (const std::unique_ptr<WindowEntry>& entry : windows_) {
            if (entry->pane == page) {
                watch_close(*entry);
            }
        }
        return true;
    };
    std::shared_ptr<jadefx::Node> tree = load_layout_node(*main, host);
    if (!tree) {
        return false;
    }
    adopt_tree(tree);
    workArea_ = tree;
    root_->setCenter(tree);
    // From here the old docks are out of the window. Their tabs are found through old_docks.
    for (const std::shared_ptr<IdeDock>& dock : old_docks) {
        forgetDock(dock);
    }
    // The first scene view stays in the main window.
    sceneDock_ = dockContaining(scene_view_.get());
    if (sceneDock_ == nullptr) {
        if (IdeDock* home = editorHome()) {
            if (const std::shared_ptr<jadefx::Tab> tab = TabShowing(old_docks, scene_view_.get())) {
                home->take(tab);
            } else {
                home->dock(scene_view_);
            }
            placed->insert(scene_view_.get());
            sceneDock_ = home;
        }
    }
    std::vector<std::string> named;
    layout_tab_names(*main, named);
    const engine_core::JsonValue* floating = saved.find("floating");
    if (floating != nullptr && floating->is_array()) {
        for (const engine_core::JsonValue& window : floating->items()) {
            if (const engine_core::JsonValue* root = window.find("root")) {
                layout_tab_names(*root, named);
            }
        }
        open_saved_floating(*floating, host);
    }
    if (const engine_core::JsonValue* closed = saved.find("closed"); closed != nullptr && closed->is_array()) {
        for (const engine_core::JsonValue& name : closed->items()) {
            named.push_back(name.as_string());
        }
    }
    // A window the layout has closed closes. One it does not name, such as one
    // added since it was saved, goes where the built-in layout has it.
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        if (!entry->pane || placed->count(entry->pane.get()) != 0) {
            continue;
        }
        if (const std::shared_ptr<jadefx::Tab> tab = TabShowing(old_docks, entry->pane.get());
            tab && tab->getTabPane() != nullptr) {
            tab->getTabPane()->close(tab);
        }
        if (!entry->starts_closed && std::find(named.begin(), named.end(), entry->name) == named.end()) {
            show_window(*entry);
        }
    }
    // Script editors, extra scene views, and terminals stay open, beside the scene view,
    // which keeps the tab the layout selected there.
    if (sceneDock_ != nullptr) {
        jadefx::Tab* selected = sceneDock_->tabs()->getSelectedTab();
        std::vector<std::shared_ptr<jadefx::Tab>> others;
        for (const std::shared_ptr<IdeDock>& dock : old_docks) {
            for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                if (tab) {
                    others.push_back(tab);
                }
            }
        }
        for (const std::shared_ptr<jadefx::Tab>& tab : others) {
            sceneDock_->take(tab);
        }
        if (selected != nullptr) {
            sceneDock_->tabs()->select(selected);
        }
    }
    for (const std::shared_ptr<jadefx::UtilityWindow>& window : old_windows) {
        if (window->isOpen()) {
            window->close();
        }
    }
    rebindUtilities();
    return true;
}

void IdeLayout::save_default_layout() {
    engine_core::JsonValue saved = capture_layout();
    // A reset keeps the main window where it is.
    saved.erase("window");
    if (!default_layout_file_.empty()) {
        std::string error;
        if (!write_file(default_layout_file_, engine_core::write_json(saved), error)) {
            runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                         "Default layout: " + error);
            return;
        }
    }
    default_layout_ = std::move(saved);
    if (restore_builtin_item_ != nullptr) {
        restore_builtin_item_->setDisable(false);
    }
    show_toast("Saved this layout as the default");
}

void IdeLayout::restore_builtin_layout() {
    default_layout_ = engine_core::JsonValue();
    if (!default_layout_file_.empty()) {
        std::error_code failure;
        std::filesystem::remove(default_layout_file_, failure);
        if (failure) {
            runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                         "Default layout: cannot remove " +
                                                             utf8_path(default_layout_file_) + ": " +
                                                             failure.message());
        }
    }
    if (restore_builtin_item_ != nullptr) {
        restore_builtin_item_->setDisable(!has_default_layout());
    }
    reset_builtin_layout();
}

bool IdeLayout::has_default_layout() const {
    if (default_layout_file_.empty()) {
        return !default_layout_.is_null();
    }
    std::error_code missing;
    return std::filesystem::exists(default_layout_file_, missing);
}

void IdeLayout::fill_window_menu(jadefx::Menu& menu) {
    auto add = [&menu](const std::string& label, const std::string& icon, std::function<bool()> open) {
        auto item = jadefx::make<jadefx::MenuItem>(label);
        item->setGraphic(jadefx::make<WindowGraphic>(icon, std::move(open)));
        jadefx::MenuItem* raw = item.get();
        menu.getItems().add(std::move(item));
        return raw;
    };
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        WindowEntry* kept = entry.get();
        add(kept->name, kept->icon, [this, kept] { return dockContaining(kept->pane.get()) != nullptr; })
            ->setOnAction([this, kept](jadefx::ActionEvent&) {
                toggle_window(kept->pane.get(), [this, kept] {
                    if (kept->open) {
                        kept->open();
                    } else {
                        show_window(*kept);
                    }
                });
            });
    }
    menu.getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    add("New Scene View", "Camera.png", nullptr)->setOnAction([this](jadefx::ActionEvent&) { new_scene_view(); });
    add("New Terminal", "Console.png", nullptr)->setOnAction([this](jadefx::ActionEvent&) { new_terminal(); });
    menu.getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    add("Save Layout as Default", std::string(), nullptr)->setOnAction([this](jadefx::ActionEvent&) {
        save_default_layout();
    });
    add("Reset to Default Layout", std::string(), nullptr)->setOnAction([this](jadefx::ActionEvent&) {
        reset_layout();
    });
    restore_builtin_item_ = add("Restore Built-in Default", std::string(), nullptr);
    restore_builtin_item_->setOnAction([this](jadefx::ActionEvent&) { restore_builtin_layout(); });
    restore_builtin_item_->setDisable(!has_default_layout());
}

void IdeLayout::toggle_window(IdePane* pane, const std::function<void()>& open) {
    IdeDock* dock = dockContaining(pane);
    if (dock == nullptr) {
        open();
        return;
    }
    const std::vector<std::shared_ptr<jadefx::Tab>> tabs = dock->tabs()->getTabs().items();
    for (const std::shared_ptr<jadefx::Tab>& tab : tabs) {
        if (!tab || tab->getContent() != pane) {
            continue;
        }
        if (tab->isSelected()) {
            dock->tabs()->close(tab);
        } else {
            reveal_window(pane);
        }
        return;
    }
}

void IdeLayout::show_window(WindowEntry& entry) {
    IdeDock* target = nullptr;
    // Still in a window, with the tabs that were beside this one.
    if (const std::shared_ptr<IdeDock> last = entry.last.lock()) {
        if (last->getParent() != nullptr && std::find(docks_.begin(), docks_.end(), last) != docks_.end()) {
            target = last.get();
        }
    }
    if (target == nullptr && entry.home) {
        target = entry.home();
    }
    if (target == nullptr) {
        return;
    }
    target->dock(window_page(entry));
    watch_close(entry);
}

void IdeLayout::open_window(WindowEntry& entry) {
    window_page(entry);
    reveal_window(entry.pane.get(), [this, &entry] { show_window(entry); });
}

const std::shared_ptr<IdePane>& IdeLayout::window_page(WindowEntry& entry) {
    if (entry.make && entry.pane && dockContaining(entry.pane.get()) == nullptr &&
        entry.pane->getParent() != nullptr) {
        // Still held by a tab that is on its way out. Start a new page.
        entry.pane.reset();
    }
    if (!entry.pane && entry.make) {
        entry.pane = entry.make();
    }
    return entry.pane;
}

void IdeLayout::watch_close(WindowEntry& entry) {
    IdeDock* dock = dockContaining(entry.pane.get());
    if (dock == nullptr) {
        return;
    }
    for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
        if (!tab || tab->getContent() != entry.pane.get()) {
            continue;
        }
        // The tab keeps this through drags to other docks and windows.
        std::weak_ptr<jadefx::Tab> weak = tab;
        WindowEntry* kept = &entry;
        tab->setOnCloseRequest([this, weak, kept](jadefx::TabCloseRequest&) {
            const std::shared_ptr<jadefx::Tab> live = weak.lock();
            const IdeDock* from = live ? dockForPane(live->getTabPane()) : nullptr;
            for (const std::shared_ptr<IdeDock>& candidate : docks_) {
                if (candidate.get() == from) {
                    kept->last = candidate;
                }
            }
        });
        return;
    }
}

IdeDock* IdeLayout::dock_beside(jadefx::Node* target, DropSide side, double depth) {
    auto fresh = jadefx::make<IdeDock>();
    adoptDock(fresh);
    if (target == nullptr && root_->getCenter() == nullptr) {
        // Nothing is docked in the main window. The new dock fills it.
        workArea_ = fresh;
        root_->setCenter(fresh);
        return fresh.get();
    }
    if (target == nullptr) {
        target = workArea_.get();
    }
    if (target == nullptr) {
        forgetDock(fresh);
        return nullptr;
    }
    const bool across = side == DropSide::Left || side == DropSide::Right;
    const double span = across ? target->getWidth() : target->getHeight();
    const double fraction = span > 1.0 ? clampFraction(depth / span) : 0.25;
    auto share = [this](jadefx::Node* node) { return shareNode(node); };
    auto replaced = [this](jadefx::Node& owner, const std::shared_ptr<jadefx::Node>& previous,
                           const std::shared_ptr<jadefx::Node>& replacement) { noteReplaced(owner, previous, replacement); };
    if (!splitEdge(*target, fresh, side, fraction, share, replaced)) {
        forgetDock(fresh);
        return nullptr;
    }
    rebindUtilities();
    return fresh.get();
}

void IdeLayout::new_scene_view() {
    IdeDock* home = editorHome();
    if (home == nullptr) {
        return;
    }
    ++scene_views_;
    auto view = jadefx::make<runner::GameView>(runner_, "Scene View " + std::to_string(scene_views_), true);
    accept_prefab_drops(*view);
    home->dock(view);
}

void IdeLayout::new_terminal() {
    if (IdeDock* home = beside_console()) {
        home->dock(make_terminal());
    }
}

IdeDock* IdeLayout::beside_console() {
    if (const std::shared_ptr<IdeConsole> log = console_.lock()) {
        if (IdeDock* dock = dockContaining(log.get())) {
            return dock;
        }
    }
    IdeDock* above = sceneDock_ != nullptr && sceneDock_->getParent() != nullptr ? sceneDock_ : nullptr;
    return dock_beside(above, DropSide::Bottom, kConsoleHeight);
}

LayoutHost IdeLayout::layout_host() {
    // The pages one load has docked, so a name the file repeats docks once.
    auto placed = std::make_shared<std::unordered_set<const IdePane*>>();
    LayoutHost host;
    // The one-of-a-kind pages and the first scene view. Script editors, extra
    // views, and terminals are not kept.
    host.name_of = [this](const IdePane& page) -> std::string {
        if (&page == scene_view_.get()) {
            return page.name();
        }
        for (const std::unique_ptr<WindowEntry>& entry : windows_) {
            if (entry->pane.get() == &page) {
                return entry->name;
            }
        }
        return {};
    };
    host.dock_page = [this, placed](IdeDock& dock, const std::string& name) {
        const std::shared_ptr<IdePane> page = page_named(name);
        if (!page || placed->count(page.get()) != 0 || dockContaining(page.get()) != nullptr) {
            return false;
        }
        placed->insert(page.get());
        dock.dock(page);
        return true;
    };
    return host;
}

std::shared_ptr<IdePane> IdeLayout::page_named(const std::string& name) {
    std::shared_ptr<IdePane> page;
    if (name == scene_view_->name()) {
        page = scene_view_;
    }
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        if (entry->name == name) {
            page = window_page(*entry);
        }
    }
    return page;
}

void IdeLayout::adopt_tree(const std::shared_ptr<jadefx::Node>& node) {
    if (const std::shared_ptr<IdeDock> dock = std::dynamic_pointer_cast<IdeDock>(node)) {
        adoptDock(dock);
        return;
    }
    if (auto* split = dynamic_cast<jadefx::SplitPane*>(node.get())) {
        for (const std::shared_ptr<jadefx::Node>& item : split->getItems().items()) {
            adopt_tree(item);
        }
    }
}

bool IdeLayout::restore_layout() {
    std::error_code missing;
    if (layout_file_.empty() || !std::filesystem::exists(layout_file_, missing)) {
        return false;
    }
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    auto refuse = [&](const std::string& why) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error,
                              "Layout: " + why + ". Starting with the default layout.");
        return false;
    };
    std::string error;
    engine_core::JsonValue saved;
    if (!ReadLayoutFile(layout_file_, saved, error)) {
        return refuse(error);
    }
    const engine_core::JsonValue* main = saved.find("main");
    if (const engine_core::JsonValue* window = saved.find("window"); window != nullptr && window->is_object()) {
        saved_window_ = *window;
    }
    if (main == nullptr) {
        return refuse(utf8_path(layout_file_) + " is not a layout this studio reads");
    }
    std::shared_ptr<jadefx::Node> tree = load_layout_node(*main, layout_host());
    if (!tree) {
        return refuse(utf8_path(layout_file_) + " docks nothing in the main window");
    }
    adopt_tree(tree);
    workArea_ = tree;
    root_->setCenter(tree);
    // The first scene view stays in the main window, even when it was floating.
    sceneDock_ = dockContaining(scene_view_.get());
    if (sceneDock_ == nullptr) {
        if (IdeDock* home = editorHome()) {
            home->dock(scene_view_);
            sceneDock_ = home;
        }
    }
    std::vector<std::string> named;
    layout_tab_names(*main, named);
    const engine_core::JsonValue* floating = saved.find("floating");
    if (floating != nullptr && floating->is_array()) {
        for (const engine_core::JsonValue& window : floating->items()) {
            if (const engine_core::JsonValue* root = window.find("root")) {
                layout_tab_names(*root, named);
            }
        }
        saved_floating_ = *floating;
    }
    if (const engine_core::JsonValue* closed = saved.find("closed"); closed != nullptr && closed->is_array()) {
        for (const engine_core::JsonValue& name : closed->items()) {
            named.push_back(name.as_string());
        }
    }
    // A window the file does not name, such as one added since it was
    // written, opens where the default layout has it.
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        if (!entry->starts_closed && dockContaining(entry->pane.get()) == nullptr &&
            std::find(named.begin(), named.end(), entry->name) == named.end()) {
            show_window(*entry);
        }
    }
    return true;
}

void IdeLayout::restore_floating() {
    const engine_core::JsonValue windows = std::move(saved_floating_);
    saved_floating_ = engine_core::JsonValue();
    open_saved_floating(windows, layout_host());
}

void IdeLayout::open_saved_floating(const engine_core::JsonValue& windows, const LayoutHost& host) {
    const std::vector<jadefx::ScreenArea> areas = jadefx::screenWorkAreas();
    for (const engine_core::JsonValue& window : windows.items()) {
        const engine_core::JsonValue* tree = window.find("root");
        if (tree == nullptr) {
            continue;
        }
        // flushFrame titles it after the tab that shows.
        std::vector<std::string> names;
        layout_tab_names(*tree, names);
        const int width = static_cast<int>(std::lround(std::clamp(NumberOr(window, "width", 420), 200.0, 4000.0)));
        const int height = static_cast<int>(std::lround(std::clamp(NumberOr(window, "height", 280), 140.0, 4000.0)));
        double x = NumberOr(window, "x", 120);
        double y = NumberOr(window, "y", 120);
        // Its display may be gone since.
        if (!OnScreen(areas, x, y)) {
            x = 120;
            y = 120;
        }
        open_floating(names.empty() ? std::string() : names.front(), width, height, x, y,
                      [&] {
                          std::shared_ptr<jadefx::Node> node = load_layout_node(*tree, host);
                          if (node) {
                              adopt_tree(node);
                          }
                          return node;
                      });
    }
}

void IdeLayout::restore_window(jadefx::Stage& stage) {
    const engine_core::JsonValue window = std::move(saved_window_);
    saved_window_ = engine_core::JsonValue();
    if (!window.is_object()) {
        return;
    }
    const std::vector<jadefx::ScreenArea> areas = jadefx::screenWorkAreas();
    double width = NumberOr(window, "width", 0);
    double height = NumberOr(window, "height", 0);
    if (width >= 200 && height >= 150) {
        // No bigger than the largest display now.
        double widest = 0;
        double tallest = 0;
        for (const jadefx::ScreenArea& area : areas) {
            widest = std::max(widest, area.width);
            tallest = std::max(tallest, area.height);
        }
        if (widest > 0) {
            width = std::min(width, widest);
            height = std::min(height, tallest);
        }
        stage.setSize(static_cast<int>(std::lround(width)), static_cast<int>(std::lround(height)));
    }
    const double x = NumberOr(window, "x", std::nan(""));
    const double y = NumberOr(window, "y", std::nan(""));
    // A place on a display that is gone is left to the system.
    if (std::isfinite(x) && std::isfinite(y) && OnScreen(areas, x, y)) {
        jadefx::moveStageTo(stage, x, y);
    }
    if (const engine_core::JsonValue* maximized = window.find("maximized"); maximized != nullptr && maximized->as_bool()) {
        // Once the window is showing.
        jadefx::runLater([&stage] { jadefx::maximizeStage(stage); });
    }
}

void IdeLayout::save_layout() {
    if (layout_file_.empty()) {
        return;
    }
    // Quitting asks every window to close at once, the floating ones first,
    // and each takes its pages with it. What they held before that is what is kept.
    const engine_core::JsonValue saved = quit_frame_ == frames_ ? quit_layout_ : capture_layout();
    std::string error;
    if (!write_file(layout_file_, engine_core::write_json(saved), error)) {
        runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                     "Layout: " + error);
    }
}

engine_core::JsonValue IdeLayout::capture_layout() {
    const LayoutHost host = layout_host();
    engine_core::JsonValue saved = engine_core::JsonValue::object();
    saved.set("version", engine_core::JsonValue::number(1));
    if (const jadefx::Node* center = root_->getCenter()) {
        engine_core::JsonValue main = save_layout_node(*center, host);
        if (!main.is_null()) {
            saved.set("main", std::move(main));
        }
    }
    engine_core::JsonValue floating = engine_core::JsonValue::array();
    for (const Floating& item : floating_) {
        if (!item.window || !item.window->isOpen() || item.window->stage().getScene().getRoot() == nullptr) {
            continue;
        }
        jadefx::Stage& stage = item.window->stage();
        engine_core::JsonValue tree = save_layout_node(*stage.getScene().getRoot(), host);
        double x = 0;
        double y = 0;
        if (tree.is_null() || !jadefx::stageToScreen(stage, 0, 0, x, y)) {
            continue;
        }
        engine_core::JsonValue window = engine_core::JsonValue::object();
        window.set("x", engine_core::JsonValue::number(x));
        window.set("y", engine_core::JsonValue::number(y));
        window.set("width", engine_core::JsonValue::number(stage.getWidth()));
        window.set("height", engine_core::JsonValue::number(stage.getHeight()));
        window.set("root", std::move(tree));
        floating.items().push_back(std::move(window));
    }
    saved.set("floating", std::move(floating));
    engine_core::JsonValue closed = engine_core::JsonValue::array();
    for (const std::unique_ptr<WindowEntry>& entry : windows_) {
        if (dockContaining(entry->pane.get()) == nullptr) {
            closed.items().push_back(engine_core::JsonValue::string(entry->name));
        }
    }
    saved.set("closed", std::move(closed));
    double x = 0;
    double y = 0;
    if (mainStage_ != nullptr && jadefx::stageToScreen(*mainStage_, 0, 0, x, y)) {
        engine_core::JsonValue window = engine_core::JsonValue::object();
        window.set("x", engine_core::JsonValue::number(x));
        window.set("y", engine_core::JsonValue::number(y));
        window.set("width", engine_core::JsonValue::number(mainStage_->getWidth()));
        window.set("height", engine_core::JsonValue::number(mainStage_->getHeight()));
        window.set("maximized", engine_core::JsonValue::boolean(jadefx::isStageMaximized(*mainStage_)));
        saved.set("window", std::move(window));
    }
    return saved;
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

void IdeLayout::reveal_window(IdePane* pane, const std::function<void()>& open) {
    IdeDock* dock = dockContaining(pane);
    if (dock == nullptr) {
        if (open) {
            open();
        }
        return;
    }
    dock->select(pane);
    if (jadefx::UtilityWindow* window = utilityOf(dock)) {
        window->toFront();
    }
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
    std::shared_ptr<IdeDock> dock;
    open_floating(tab->getText(), width, height, screenX - 36, screenY - 12, [this, &dock] {
        dock = jadefx::make<IdeDock>();
        adoptDock(dock);
        return dock;
    });
    if (dock) {
        dock->take(tab);
    }
}

jadefx::UtilityWindow* IdeLayout::open_floating(const std::string& title, int width, int height, double screenX,
                                                double screenY,
                                                const std::function<std::shared_ptr<jadefx::Node>()>& fill) {
    std::shared_ptr<jadefx::UtilityWindow> window = jadefx::UtilityWindow::open(title, width, height, screenX, screenY);
    if (!window) {
        return nullptr;
    }
    const std::shared_ptr<jadefx::Node> root = fill ? fill() : nullptr;
    if (!root) {
        window->close();
        return nullptr;
    }
    StretchRoot(*root);
    accept_texture_drops(*root);
    auto scene = jadefx::make<jadefx::Scene>(root, static_cast<double>(width), static_cast<double>(height));
    scene->setStylesheet(kStylesheet);
    jadefx::Scene* utilityScene = scene.get();
    scene->addKeyHook([this, utilityScene](jadefx::KeyEvent& event) { routeKeys(event, *utilityScene); });
    window->stage().setScene(std::move(scene));
    LeaveFieldsOnEscape(window->stage());
    window->setCanClose([this, raw = window.get()]() {
        // Closing its tabs changes the layout. A quit saves it as it was.
        if (!layout_file_.empty()) {
            quit_layout_ = capture_layout();
            quit_frame_ = frames_;
        }
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
    created.title = title;
    floating_.push_back(std::move(created));
    rebindUtilities();
    return window.get();
}

// Free helpers for docking drags and window sizing, local to this file.
namespace {

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
            const double fraction = clampFraction(span > 1.0 ? desired / span : 0.5);
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
}  // namespace

void IdeLayout::showDropMark(jadefx::Scene& scene, double x, double y, double width, double height, const char* style) {
    if (width < 2.0 || height < 2.0) {
        hideDropMark();
        return;
    }
    if (!dropMark_) {
        dropMark_ = jadefx::make<jadefx::Pane>();
        dropMark_->setMouseTransparent(true);
    }
    dropMark_->setStyle(style);
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
    const char* style = kFloatMark;
    if (choice.kind == DragKind::Undock) {
        const jadefx::Node* content = drag.tab ? drag.tab->getContent() : nullptr;
        const double contentW = content != nullptr ? content->getMinWidth() : 0;
        const double contentH = content != nullptr ? content->getMinHeight() : 0;
        mark.width = std::clamp(contentW + 16.0, 220.0, 360.0);
        mark.height = std::clamp(contentH + 48.0, 140.0, 240.0);
        mark.x = point.x - 36.0;
        mark.y = point.y - 12.0;
    } else if (choice.kind == DragKind::MoveTab || choice.kind == DragKind::Restore) {
        style = choice.caret ? kCaretMark : kMergeMark;
    } else {
        style = kSplitMark;
    }
    showDropMark(point.stage->getScene(), mark.x, mark.y, mark.width, mark.height, style);
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

}  // namespace ide
