#pragma once

// Every live MeshEditor, one per Mesh, and when each one's edits reach what
// the Mesh draws and its file (docs/superpowers/specs/2026-10-11-mesh-editor-design.md,
// section 2). Owned by the DataModel. Gameplay thread only, under the write lock.
//
// An edit only counts. Once a step, after scripts and tools, each changed
// editor builds into its Mesh's live copy, drawn from the next frame.
// Playing, that is all: Stop drops the copy. Stopped, the file is written
// once the editor has had no edit for kIdleWrite, at once when nothing but
// this registry holds it, and before Play and a project save; kDropAfterWrite
// later the live copy is dropped and the Mesh draws its file again.

#include "MeshEditor.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

class DataModel;
class Mesh;

class MeshEditors {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kIdleWrite{1000};
    // Longer than MeshCache's once-a-second look at a file, so the file it
    // draws next is the one just written.
    static constexpr std::chrono::milliseconds kDropAfterWrite{2000};

    // mesh's live editor, or a new one opened from what it draws now. Stopped,
    // the Mesh gets its file first (Mesh::prepare_file). Null, with error set,
    // when the Mesh cannot be edited.
    std::shared_ptr<MeshEditor> open(DataModel& game, Mesh& mesh, std::string& error);
    // False once editor has ended, its Mesh is gone, or a Play or Stop came since it opened.
    bool usable(const DataModel& game, const MeshEditor& editor) const;
    // Builds every changed editor's live copy now, for a read that must see this frame's edits.
    void commit(DataModel& game);
    // Once a step, after scripts and tools; see the top of this file.
    void step(DataModel& game, Clock::time_point now);
    // Writes every stopped editor's edits its file lacks. end (before Play)
    // also drops their live copies and ends them.
    void write_all(DataModel& game, bool end);
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::shared_ptr<MeshEditor> editor;
        // edits() at the last commit, and at the last step.
        std::uint64_t built = 0;
        std::uint64_t seen = 0;
        // When a step last saw edits() move.
        Clock::time_point changed{};
        // Stopped: the live copy holds edits the file does not.
        bool unwritten = false;
        // The Path at open; stopped, also Mesh::file_stamp() at open or the last write.
        std::string path;
        std::string stamp;
        // Stopped: when to drop the live copy after a write.
        std::optional<Clock::time_point> drop_at;
    };

    Mesh* mesh_of(DataModel& game, const Entry& entry) const;
    // Ends a usable editor whose Mesh has a new Path, or (stopped) a file some
    // other writer changed: what it holds no longer applies, so its live copy
    // goes and the Mesh draws its file. True when it ended it.
    bool end_if_stale(DataModel& game, Entry& entry);
    void commit_one(DataModel& game, Entry& entry);
    void write_one(DataModel& game, Entry& entry, Clock::time_point now);

    std::vector<Entry> entries_;
};

}  // namespace engine_core
