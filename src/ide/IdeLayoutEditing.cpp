// IdeLayout: cut, paste, rename, delete, Search, and the open script editors.

#include "IdeLayout.hpp"

#include "IdeLayoutInternal.hpp"

namespace ide {

void IdeLayout::run_action(engine_core::InstanceAction action, std::uint32_t id) {
    switch (action) {
    case engine_core::InstanceAction::Cut:
        cut({id});
        break;
    case engine_core::InstanceAction::Paste:
        paste(id);
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
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Paste"));
            return;
        }
        if (!parent_ok(game, id)) {
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
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            show_toast(busy_message("Edit"));
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
    // Those ids belong to the place that is going away, and so do their undo stacks.
    for (const std::shared_ptr<IdeScriptEditor>& editor : editors) {
        editor->bindUndo(nullptr);
    }
    undo_router_.forget_scripts();
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

}  // namespace ide
