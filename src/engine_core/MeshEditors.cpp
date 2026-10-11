#include "MeshEditors.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"

#include <algorithm>

namespace engine_core {

std::shared_ptr<MeshEditor> MeshEditors::open(DataModel& game, Mesh& mesh, std::string& error) {
    for (Entry& entry : entries_) {
        if (entry.editor->owner.mesh == mesh.id() && usable(game, *entry.editor) && !end_if_stale(game, entry)) {
            return entry.editor;
        }
    }
    anarchy::amesh::Data data;
    if (std::optional<std::string> failed = mesh.read_geometry(data)) {
        error = *failed;
        return nullptr;
    }
    const bool playing = game.simulation_running();
    if (!playing) {
        if (std::optional<std::string> failed = mesh.prepare_file()) {
            error = *failed;
            return nullptr;
        }
    }
    Entry entry;
    entry.editor = std::make_shared<MeshEditor>(data);
    entry.editor->owner = MeshEditor::Owner{mesh.id(), game.world_generation(), playing};
    entry.built = entry.editor->edits();
    entry.seen = entry.built;
    entry.path = mesh.path();
    entry.stamp = playing ? std::string() : mesh.file_stamp();
    entries_.push_back(std::move(entry));
    return entries_.back().editor;
}

bool MeshEditors::usable(const DataModel& game, const MeshEditor& editor) const {
    const MeshEditor::Owner& owner = editor.owner;
    return !editor.ended() && owner.generation == game.world_generation() &&
           owner.playing == game.simulation_running() && game.alive(owner.mesh) &&
           dynamic_cast<const Mesh*>(game.instance(owner.mesh)) != nullptr;
}

Mesh* MeshEditors::mesh_of(DataModel& game, const Entry& entry) const {
    const InstanceId id = entry.editor->owner.mesh;
    return game.alive(id) ? dynamic_cast<Mesh*>(game.instance(id)) : nullptr;
}

bool MeshEditors::end_if_stale(DataModel& game, Entry& entry) {
    Mesh* mesh = mesh_of(game, entry);
    if (mesh == nullptr) {
        return false;
    }
    const bool moved = mesh->path() != entry.path;
    const bool rewritten = !entry.editor->owner.playing && mesh->file_stamp() != entry.stamp;
    if (!moved && !rewritten) {
        return false;
    }
    mesh->drop_live_geometry();
    entry.editor->end();
    return true;
}

void MeshEditors::commit_one(DataModel& game, Entry& entry) {
    if (entry.editor->edits() == entry.built) {
        return;
    }
    Mesh* mesh = mesh_of(game, entry);
    if (mesh == nullptr) {
        return;
    }
    mesh->set_live_geometry(std::make_shared<const anarchy::amesh::Data>(entry.editor->build()));
    entry.built = entry.editor->edits();
    if (!entry.editor->owner.playing) {
        entry.unwritten = true;
        entry.drop_at.reset();
    }
}

void MeshEditors::write_one(DataModel& game, Entry& entry, Clock::time_point now) {
    entry.unwritten = false;
    Mesh* mesh = mesh_of(game, entry);
    const Mesh::SessionGeometry live = mesh != nullptr ? mesh->session_geometry() : Mesh::SessionGeometry{};
    if (live.data == nullptr) {
        return;
    }
    if (std::optional<std::string> error = mesh->write_geometry(*live.data)) {
        // Kept drawing from the live copy, since the file does not have it.
        game.warn("Mesh " + mesh->path() + " could not be written: " + *error);
        return;
    }
    entry.stamp = mesh->file_stamp();
    entry.drop_at = now + kDropAfterWrite;
}

void MeshEditors::commit(DataModel& game) {
    for (Entry& entry : entries_) {
        if (usable(game, *entry.editor)) {
            commit_one(game, entry);
        }
    }
}

void MeshEditors::step(DataModel& game, Clock::time_point now) {
    for (Entry& entry : entries_) {
        MeshEditor& editor = *entry.editor;
        if (!usable(game, editor)) {
            editor.end();
            continue;
        }
        // A new Path (an undo, or a script's) or, stopped, another writer.
        if (end_if_stale(game, entry)) {
            continue;
        }
        Mesh* mesh = mesh_of(game, entry);
        const bool playing = editor.owner.playing;
        if (editor.edits() != entry.seen) {
            entry.seen = editor.edits();
            entry.changed = now;
        }
        commit_one(game, entry);
        // Only this registry holds it: no script can edit it again.
        const bool let_go = entry.editor.use_count() == 1;
        if (playing) {
            if (let_go) {
                // Its live copy stays until Stop.
                editor.end();
            }
            continue;
        }
        if (entry.unwritten && (let_go || now - entry.changed >= kIdleWrite)) {
            write_one(game, entry, now);
        }
        if (entry.drop_at && now >= *entry.drop_at) {
            mesh->drop_live_geometry();
            entry.drop_at.reset();
        }
        if (let_go && !entry.unwritten && !entry.drop_at) {
            editor.end();
        }
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const Entry& entry) { return entry.editor->ended(); }),
                   entries_.end());
}

void MeshEditors::write_all(DataModel& game, bool end) {
    for (Entry& entry : entries_) {
        if (entry.editor->owner.playing || !usable(game, *entry.editor) || end_if_stale(game, entry)) {
            continue;
        }
        commit_one(game, entry);
        if (entry.unwritten) {
            write_one(game, entry, Clock::now());
        }
        if (end) {
            if (Mesh* mesh = mesh_of(game, entry)) {
                mesh->drop_live_geometry();
            }
            entry.editor->end();
        }
    }
    if (end) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [](const Entry& entry) { return entry.editor->ended(); }),
                       entries_.end());
    }
}

}  // namespace engine_core
