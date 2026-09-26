#pragma once

#include "PropertyBag.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine_core {

class DataModel;

// Parent used when an instance is not in the tree. Matches DataModel::kNoParent.
inline constexpr InstanceId kHistoryNoParent = 0xffffffffu;

enum class FinishRecordingOperation { Commit, Cancel };

// Property identity for a place waypoint. Linear velocity is not here:
// physics state is not edit history.
enum class HistoryProp : std::uint8_t {
    Transform = 0,
    Color,
    Size,
    Simulated,
    VisualOnly,
    Name,
    Source,
    Enabled,
    Position
};

enum class MutationKind : std::uint8_t { SetProperty, SetParent, CreateInstance, DestroyInstance };

struct PropertyValue {
    HistoryProp prop = HistoryProp::Name;
    Transform transform = transform_identity();
    ColorRgb color{};
    float size[3] = {1.f, 1.f, 1.f};
    bool flag = false;
    std::string text;
    Vec3 vector{};
};

// One instance's authored fields. Children are the subtree at capture time.
// Velocity, the visual snapshot, and Lua thread state are not stored.
struct AuthoredRecord {
    InstanceId id = 0;
    const void* type_key = nullptr;
    std::string class_name;
    std::string name;
    // Undo of a destroy brings back the same GUID, so the same file on disk.
    std::string guid;
    PropertyBag extras;
    InstanceId parent = kHistoryNoParent;
    int sibling_index = -1;
    bool simulated = false;
    bool visual_only = false;
    bool spatial = false;
    Transform transform = transform_identity();
    ColorRgb color{};
    float size[3] = {1.f, 1.f, 1.f};
    bool has_source = false;
    std::string source;
    bool enabled = true;
    // Subclass place bytes for types other than GameObject and LuaSource
    // (TestTriangle pose). Restored through read_place.
    std::vector<std::byte> extra;
    std::vector<AuthoredRecord> children;
};

struct Mutation {
    MutationKind kind = MutationKind::SetProperty;
    InstanceId id = 0;
    PropertyValue before{};
    PropertyValue after{};
    InstanceId old_parent = kHistoryNoParent;
    InstanceId new_parent = kHistoryNoParent;
    int old_sibling_index = -1;
    AuthoredRecord record;
};

// Listeners run after the history operation that fired them, not inside a setter.
template <typename... Args>
class HistorySignal {
public:
    using Handler = std::function<void(Args...)>;

    std::uint64_t connect(Handler handler) {
        const std::uint64_t id = next_++;
        handlers_.push_back({id, std::move(handler)});
        return id;
    }

    void disconnect(std::uint64_t id) {
        for (auto it = handlers_.begin(); it != handlers_.end(); ++it) {
            if (it->first == id) {
                handlers_.erase(it);
                return;
            }
        }
    }

    void emit(Args... args) {
        const std::vector<std::pair<std::uint64_t, Handler>> copy = handlers_;
        for (const auto& entry : copy) {
            if (entry.second) {
                entry.second(args...);
            }
        }
    }

private:
    std::vector<std::pair<std::uint64_t, Handler>> handlers_;
    std::uint64_t next_ = 1;
};

// Edit undo for DataModel mutations. A second stack holds waypoints committed
// during play and is dropped on stop. Text keystrokes do not come here.
class ChangeHistoryService {
public:
    explicit ChangeHistoryService(DataModel& game);

    void rebind(DataModel& game) { game_ = &game; }

    // Null when a recording is already open or history is disabled.
    std::optional<std::string> try_begin_recording(std::string name, std::string display_name = {});
    void finish_recording(const std::string& id, FinishRecordingOperation op);
    // Empty id asks whether any recording is open.
    bool is_recording_in_progress(std::optional<std::string> id = std::nullopt) const;

    // Commits the open recording under this name. Empty when nothing is open.
    void set_waypoint(std::string name);
    // Clears the undo and redo stacks and drops an open recording without reverting.
    void reset_waypoints();

    void undo();
    void redo();
    std::pair<bool, std::string> can_undo() const;
    std::pair<bool, std::string> can_redo() const;

    // Off: mutators do not enter history. An open recording can still be finished.
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }
    // True while undo, redo, or a cancel is writing the place back.
    bool applying_undo_redo() const { return applying_ != 0; }

    // Name used the next time edit mode opens an implicit recording.
    void set_pending_gesture(std::string name);
    // Commits an implicit recording. An explicit recording stays open.
    void end_gesture();

    // True when a mutator should capture. Play writes are included only
    // inside an open recording. Undo application is not.
    bool wants_mutation() const;

    void note(Mutation mutation);

    // Commit an open edit recording before simulation_running becomes true.
    void seal_edit_recording();
    // Drop play waypoints and an open play recording. Does not touch the edit stack.
    void drop_session();

    HistorySignal<const std::string&> on_undo;
    HistorySignal<const std::string&> on_redo;
    HistorySignal<const std::string&, const std::string&> on_recording_started;
    HistorySignal<const std::string&, const std::string&, const std::string&, FinishRecordingOperation>
        on_recording_finished;

private:
    struct Recording {
        std::string id;
        std::string name;
        std::string display_name;
        bool implicit = false;
        std::vector<Mutation> mutations;
    };

    struct Waypoint {
        std::string name;
        std::string display_name;
        std::vector<Mutation> mutations;
    };

    void open_implicit(const Mutation& first);
    void push_or_coalesce(Mutation mutation);
    void apply_waypoint(Waypoint& waypoint, bool inverse);
    std::vector<Waypoint>& undo_stack();
    std::vector<Waypoint>& redo_stack();
    const std::vector<Waypoint>& undo_stack() const;
    const std::vector<Waypoint>& redo_stack() const;
    bool playing() const;

    DataModel* game_ = nullptr;
    bool enabled_ = true;
    int applying_ = 0;
    std::uint64_t next_id_ = 1;
    std::string pending_;
    std::optional<Recording> recording_;
    std::vector<Waypoint> edit_undo_;
    std::vector<Waypoint> edit_redo_;
    std::vector<Waypoint> session_undo_;
    std::vector<Waypoint> session_redo_;
};

}  // namespace engine_core
