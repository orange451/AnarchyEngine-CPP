// IdeLayout: cut, paste, rename, delete, Search, and the open script editors.

#include "IdeLayout.hpp"

#include "IdeLayoutInternal.hpp"

#include "FileBytes.hpp"

namespace ide {

void IdeLayout::sync_fold_file() {
    const std::filesystem::path wanted =
        project_ ? project_->root() / ".studio" / "folds.json" : std::filesystem::path();
    if (wanted == fold_file_) {
        return;
    }
    if (!fold_file_.empty()) {
        script_folds_.clear();
    }
    fold_file_ = wanted;
    if (fold_file_.empty()) {
        return;
    }
    std::string text;
    std::string error;
    engine_core::JsonValue root;
    if (!engine_core::read_file(fold_file_, text, error) || !engine_core::parse_json(text, root, error) ||
        !root.is_object()) {
        return;
    }
    for (const engine_core::JsonValue::Member& member : root.members()) {
        std::vector<int> lines;
        for (const engine_core::JsonValue& line : member.second.items()) {
            if (line.is_number() && line.as_number() >= 0) {
                lines.push_back(static_cast<int>(line.as_number()));
            }
        }
        if (script_folds_.find(member.first) == script_folds_.end()) {
            script_folds_[member.first] = std::move(lines);
        }
    }
}

std::vector<int> IdeLayout::recall_folds(const std::string& guid) {
    sync_fold_file();
    const auto found = script_folds_.find(guid);
    return found == script_folds_.end() ? std::vector<int>{} : found->second;
}

void IdeLayout::remember_folds(const std::string& guid, const std::vector<int>& lines) {
    if (guid.empty()) {
        return;
    }
    sync_fold_file();
    const auto found = script_folds_.find(guid);
    if (found != script_folds_.end() && found->second == lines) {
        return;
    }
    if (lines.empty()) {
        if (found == script_folds_.end()) {
            return;
        }
        script_folds_.erase(found);
    } else {
        script_folds_[guid] = lines;
    }
    if (fold_file_.empty()) {
        return;
    }
    engine_core::JsonValue root = engine_core::JsonValue::object();
    for (const auto& entry : script_folds_) {
        std::vector<engine_core::JsonValue> items;
        for (int line : entry.second) {
            items.push_back(engine_core::JsonValue::number(line));
        }
        root.set(entry.first, engine_core::JsonValue::array(std::move(items)));
    }
    std::string error;
    engine_core::write_file(fold_file_, engine_core::write_json(root), error);
}

void IdeLayout::run_action(engine_core::InstanceAction action, std::uint32_t id) {
    switch (action) {
    case engine_core::InstanceAction::Cut:
        cut({id});
        break;
    case engine_core::InstanceAction::Copy:
        copy({id});
        break;
    case engine_core::InstanceAction::Paste:
        paste(id);
        break;
    case engine_core::InstanceAction::Duplicate:
        duplicate({id});
        break;
    case engine_core::InstanceAction::Edit:
        edit(id);
        break;
    case engine_core::InstanceAction::Delete:
        delete_instances({id});
        break;
    case engine_core::InstanceAction::Rename:
        // The explorer renames in its own row.
        break;
    }
}

void IdeLayout::delete_instances(std::vector<std::uint32_t> ids) {
    ids.erase(std::remove(ids.begin(), ids.end(), 0u), ids.end());
    if (ids.empty()) {
        return;
    }
    runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_), ids = std::move(ids)](
                                           engine_core::DataModel& world) {
        bool any = false;
        for (std::uint32_t id : ids) {
            // A selected child is already gone with its selected parent.
            if (!world.alive(id)) {
                continue;
            }
            if (std::optional<std::string> error = world.destroy_error(id)) {
                toast_later(this, alive, std::move(*error));
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

const std::shared_ptr<IdeSearch>& IdeLayout::search_pane() {
    window_page(*search_window_);
    return search_;
}

std::shared_ptr<IdePane> IdeLayout::make_search() {
    SearchHost host;
    host.editor_text = [this](std::uint32_t id) -> std::optional<std::string> {
        const std::shared_ptr<IdeScriptEditor> editor = open_editor(id);
        if (!editor || !editor->isLoaded()) {
            return std::nullopt;
        }
        return editor->text();
    };
    host.replace_in_editor = [this](std::uint32_t id, const SearchQuery& query, const std::string& replacement,
                                    int line) {
        const std::shared_ptr<IdeScriptEditor> editor = open_editor(id);
        if (!editor || !editor->isLoaded()) {
            return -1;
        }
        const int count = editor->replaceMatches(query, replacement, line);
        // Now, not when the editor's tab next lays out, so Source and the place have it.
        editor->flush();
        return count;
    };
    host.open = [this](std::uint32_t id, int line, int column, int column_end) {
        edit(id);
        if (const std::shared_ptr<IdeScriptEditor> editor = open_editor(id)) {
            editor->showRange(line, column, column_end);
        }
    };
    search_ = jadefx::make<IdeSearch>(runner_.simulation(), std::move(host));
    return search_;
}

IdeDock* IdeLayout::side_home() {
    // Beside the left explorer, as VS Code keeps search in its side bar.
    for (const std::weak_ptr<IdeExplorer>& weak : explorers_) {
        if (const std::shared_ptr<IdeExplorer> explorer = weak.lock()) {
            if (IdeDock* home = dockContaining(explorer.get())) {
                return home;
            }
        }
    }
    return editorHome();
}

void IdeLayout::open_search(bool replace, jadefx::Scene* scene) {
    // A selection on one line in the focused editor is what to find.
    std::string seed;
    if (scene != nullptr) {
        if (IdeScriptEditor* editor = Owning<IdeScriptEditor>(scene->focusedNode())) {
            if (!editor->findOwns(scene->focusedNode())) {
                seed = editor->selectedText();
                if (seed.find('\n') != std::string::npos) {
                    seed.clear();
                }
            }
        }
    }
    search_pane();
    if (dockContaining(search_.get()) != nullptr) {
        reveal_window(search_.get());
    } else {
        // Find in Scripts opens the pane with replace hidden.
        if (!replace) {
            search_->setReplaceShown(false);
        }
        show_window(*search_window_);
        if (dockContaining(search_.get()) == nullptr) {
            return;
        }
    }
    if (!seed.empty()) {
        search_->setFindText(seed);
    }
    if (replace && !search_->findInput().text().empty()) {
        search_->focusReplace();
    } else {
        if (replace) {
            search_->setReplaceShown(true);
        }
        search_->focusFind();
    }
}

bool IdeLayout::action_enabled(engine_core::InstanceAction action) const {
    if (action == engine_core::InstanceAction::Paste) {
        return clip_ && (clip_->held || clip_->copies);
    }
    return true;
}

void IdeLayout::copy(const std::vector<std::uint32_t>& ids) {
    if (!clip_) {
        return;
    }
    engine_core::DataModel& game = runner_.simulation().datamodel();
    std::vector<CopiedNode> copies;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Copy"));
            return;
        }
        copies = copy_set(game, ids);
    }
    if (copies.empty()) {
        return;
    }
    if (clip_->held) {
        std::vector<engine_core::InstanceId> dropped = clip_->ids;
        runner_.simulation().on_simulation([dropped](engine_core::DataModel& world) {
            bool any = false;
            for (engine_core::InstanceId id : dropped) {
                if (world.alive(id) && world.parent(id) == engine_core::DataModel::kNoParent) {
                    if (!any) {
                        world.history().set_pending_gesture("Copy");
                        any = true;
                    }
                    world.destroy_tree(id);
                }
            }
            if (any) {
                CloseGesture(world);
            }
        });
    }
    clip_->held = false;
    clip_->ids.clear();
    const std::size_t count = copies.size();
    clip_->copies = std::make_shared<const std::vector<CopiedNode>>(std::move(copies));
    show_toast(count == 1 ? "Copied 1 instance" : "Copied " + std::to_string(count) + " instances");
}

void IdeLayout::duplicate(const std::vector<std::uint32_t>& ids) {
    engine_core::DataModel& game = runner_.simulation().datamodel();
    std::vector<std::pair<engine_core::InstanceId, std::vector<CopiedNode>>> groups;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Duplicate"));
            return;
        }
        for (engine_core::InstanceId id : cut_set(game, ids)) {
            std::vector<CopiedNode> one = copy_set(game, {id});
            if (!one.empty()) {
                groups.emplace_back(game.parent(id), std::move(one));
            }
        }
    }
    if (groups.empty()) {
        return;
    }
    runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_),
                                        groups = std::move(groups)](engine_core::DataModel& world) {
        world.history().set_pending_gesture("Duplicate");
        std::vector<engine_core::InstanceId> made;
        std::string refused;
        for (const auto& group : groups) {
            paste_copies(world, group.second, group.first, &made, &refused);
        }
        CloseGesture(world);
        if (!made.empty()) {
            world.selection().set(made);
        }
        if (!refused.empty()) {
            toast_later(this, alive, std::move(refused));
        }
    });
}

void IdeLayout::cut(const std::vector<std::uint32_t>& ids) {
    if (!clip_) {
        return;
    }
    engine_core::DataModel& game = runner_.simulation().datamodel();
    std::vector<engine_core::InstanceId> taken;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Cut"));
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
    clip_->copies.reset();
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
    if (clip_ && !clip_->held && clip_->copies) {
        engine_core::DataModel& game = runner_.simulation().datamodel();
        {
            engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
            if (!lock.owns()) {
                show_toast(busy_message("Paste"));
                return;
            }
            id = insert_target(game, id);
            if (!parent_ok(game, id)) {
                return;
            }
        }
        runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_), copies = clip_->copies,
                                            id](engine_core::DataModel& world) {
            if (!parent_ok(world, id)) {
                return;
            }
            world.history().set_pending_gesture("Paste");
            std::vector<engine_core::InstanceId> made;
            std::string refused;
            paste_copies(world, *copies, id, &made, &refused);
            CloseGesture(world);
            if (!made.empty()) {
                world.selection().set(made);
            }
            if (!refused.empty()) {
                toast_later(this, alive, std::move(refused));
            }
        });
        return;
    }
    if (!clip_ || !clip_->held) {
        return;
    }
    engine_core::DataModel& game = runner_.simulation().datamodel();
    std::vector<engine_core::InstanceId> children;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Paste"));
            return;
        }
        id = insert_target(game, id);
        if (!parent_ok(game, id)) {
            return;
        }
        // A target inside something cut refuses the whole paste, so nothing
        // is left behind out of the place.
        for (engine_core::InstanceId child : clip_->ids) {
            if (!game.alive(child)) {
                continue;
            }
            if (std::optional<std::string> error = game.parent_error(child, id)) {
                show_toast(std::move(*error));
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
            if (world.alive(child) && !world.parent_error(child, id)) {
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
    runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_), ids = std::move(ids),
                                        parent](engine_core::DataModel& world) {
        world.history().set_pending_gesture("Move");
        std::string refused;
        const bool moved = move_set(world, ids, parent, &refused);
        CloseGesture(world);
        if (!moved && !refused.empty()) {
            toast_later(this, alive, std::move(refused));
        }
    });
}

void IdeLayout::rename(std::uint32_t id, std::string name) {
    if (name.empty()) {
        return;
    }
    if (std::shared_ptr<IdeScriptEditor> editor = open_editor(id)) {
        editor->setTitleText(name);
    }
    runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_), id,
                                        name = std::move(name)](engine_core::DataModel& world) {
        if (id != 0 && !world.alive(id)) {
            return;
        }
        if (std::optional<std::string> error = world.rename_error(id, name)) {
            toast_later(this, alive, std::move(*error));
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
    bool is_prefab = false;
    std::string guid;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Edit"));
            return;
        }
        const engine_core::DataModel* object = game.instance(id);
        if (dynamic_cast<const engine_core::Prefab*>(object) != nullptr) {
            is_prefab = true;
        } else if (dynamic_cast<const engine_core::LuaSource*>(object) == nullptr) {
            return;
        }
        guid = game.guid(id);
    }
    if (is_prefab) {
        edit_prefab(id, *home);
        return;
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
    editor->setFoldMemory([this, guid] { return recall_folds(guid); },
                          [this, guid](const std::vector<int>& lines) { remember_folds(guid, lines); });
    std::shared_ptr<jadefx::Tab> tab = home->dock(editor);
    if (tab) {
        tab->setOnClosed([this, id, editor] {
            // While stopped, closing flushed the text into the place, and Stop has
            // nothing to restore. A copy kept then would be written back over any
            // change made later, such as Replace All or a file loaded from disk.
            if (in_test() && editor && editor->isLoaded()) {
                kept_sources_[id] = editor->text();
            }
            // A reopened editor starts its undo over, so the closed one's history
            // is only memory.
            if (editor) {
                editor->bindUndo(nullptr);
            }
            undo_router_.forget_script(id);
        });
    }
    open_scripts_[id] = editor;
}

void IdeLayout::edit_prefab(std::uint32_t prefab, IdeDock& home) {
    // An editor whose tab was closed is not reused: a new one docks at home.
    const std::shared_ptr<IdePrefabEditor> existing = open_prefab_editor(prefab);
    if (existing && dockContaining(existing.get()) != nullptr) {
        // Selects its tab, and brings a floating window that holds it to the front.
        reveal_window(existing.get());
        return;
    }
    auto editor = jadefx::make<IdePrefabEditor>(runner_.simulation().datamodel(), prefab, prefab_editor_host());
    home.dock(editor);
    open_prefabs_[prefab] = editor;
}

PrefabEditorHost IdeLayout::prefab_editor_host() {
    PrefabEditorHost host;
    host.add_model = [this](engine_core::InstanceId prefab, engine_core::InstanceId mesh,
                            engine_core::InstanceId material, std::shared_ptr<InsertResult> result) {
        runner_.simulation().on_simulation([prefab, mesh, material, result](engine_core::DataModel& world) {
            world.history().set_pending_gesture("Add Model");
            std::string error;
            const engine_core::InstanceId made = add_model(world, prefab, mesh, material, error);
            CloseGesture(world);
            if (result) {
                result->id.store(made, std::memory_order_relaxed);
                result->error = std::move(error);
                result->done.store(true, std::memory_order_release);
            }
        });
    };
    host.set_part = [this](engine_core::InstanceId model, ModelPart part, engine_core::InstanceId target) {
        runner_.simulation().on_simulation(
            [this, alive = std::weak_ptr<int>(alive_), model, part, target](engine_core::DataModel& world) {
                world.history().set_pending_gesture(std::string(target != 0 ? "Set " : "Clear ") +
                                                    model_part_name(part));
                std::optional<std::string> error = set_model_part(world, model, part, target);
                CloseGesture(world);
                if (error) {
                    toast_later(this, alive, std::move(*error));
                }
            });
    };
    host.rename = [this](engine_core::InstanceId id, std::string name) { rename(id, std::move(name)); };
    host.remove = [this](const std::vector<engine_core::InstanceId>& ids) { delete_instances(ids); };
    host.notice = [this](std::string text) { show_toast(std::move(text)); };
    return host;
}

std::shared_ptr<IdePrefabEditor> IdeLayout::open_prefab_editor(std::uint32_t prefab) const {
    const auto found = open_prefabs_.find(prefab);
    if (found == open_prefabs_.end()) {
        return nullptr;
    }
    return found->second.lock();
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
        for (const auto& entry : pending) {
            auto* source = dynamic_cast<engine_core::LuaSource*>(game.instance(entry.first));
            if (source == nullptr || source->source() == entry.second) {
                continue;
            }
            source->set_source(entry.second);
        }
        if (recording) {
            game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
        }
    });
}

void IdeLayout::close_script_editors() {
    std::vector<std::shared_ptr<IdeScriptEditor>> editors;
    for (const auto& entry : open_scripts_) {
        if (std::shared_ptr<IdeScriptEditor> editor = entry.second.lock()) {
            editors.push_back(std::move(editor));
        }
    }
    std::vector<std::shared_ptr<IdePane>> pages(editors.begin(), editors.end());
    for (const auto& entry : open_prefabs_) {
        if (std::shared_ptr<IdePrefabEditor> editor = entry.second.lock()) {
            pages.push_back(std::move(editor));
        }
    }
    for (const std::shared_ptr<IdePane>& editor : pages) {
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
    // Those ids belong to the place that is going away, and so do their undo stacks.
    for (const std::shared_ptr<IdeScriptEditor>& editor : editors) {
        editor->bindUndo(nullptr);
    }
    undo_router_.forget_scripts();
    open_scripts_.clear();
    open_prefabs_.clear();
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

}  // namespace ide
