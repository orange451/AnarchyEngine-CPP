#include "ChangeHistoryService.hpp"

#include "DataModel.hpp"
#include "PropertyReflection.hpp"

#include <algorithm>
#include <cstring>

namespace engine_core {
namespace {

std::string default_gesture(const Mutation& mutation) {
    switch (mutation.kind) {
    case MutationKind::CreateInstance:
        if (mutation.record.class_name.empty()) {
            return "Create";
        }
        return "Create " + mutation.record.class_name;
    case MutationKind::DestroyInstance:
        return "Delete";
    case MutationKind::SetParent:
        return "Set Parent";
    case MutationKind::SetProperty:
        break;
    }
    switch (mutation.after.prop) {
    case HistoryProp::Transform:
        return "Move";
    case HistoryProp::Name:
        return "Rename";
    case HistoryProp::Source:
        return "Edit Source";
    case HistoryProp::Enabled:
        return "Set Enabled";
    case HistoryProp::Simulated:
        return "Set Simulated";
    case HistoryProp::VisualOnly:
        return "Set Visual";
    case HistoryProp::Reflected:
        return std::string("Set ") + lua_property_name(mutation.after.property);
    }
    return "Edit";
}

// Roughly what a record keeps alive: its strings, its bag, and its subtree.
std::size_t record_bytes(const AuthoredRecord& record) {
    std::size_t bytes = sizeof(AuthoredRecord) + record.class_name.size() + record.name.size() + record.guid.size() +
                        record.source.size() + record.extra.size() + record.extras.size() * sizeof(JsonValue::Member);
    for (const AuthoredRecord& child : record.children) {
        bytes += record_bytes(child);
    }
    return bytes;
}

std::size_t mutation_bytes(const Mutation& mutation) {
    return sizeof(Mutation) - sizeof(AuthoredRecord) + mutation.before.text.size() + mutation.after.text.size() +
           mutation.before.slot.text.size() + mutation.after.slot.text.size() +
           record_bytes(mutation.record);
}

bool same_value(const PropertyValue& a, const PropertyValue& b) {
    if (a.prop != b.prop || a.property != b.property) {
        return false;
    }
    switch (a.prop) {
    case HistoryProp::Transform:
        return same_matrix4(a.transform, b.transform);
    case HistoryProp::Simulated:
    case HistoryProp::VisualOnly:
    case HistoryProp::Enabled:
        return a.flag == b.flag;
    case HistoryProp::Name:
    case HistoryProp::Source:
        return a.text == b.text;
    case HistoryProp::Reflected:
        return same_slot(a.slot, b.slot);
    }
    return false;
}

bool blocks_coalesce(const Mutation& existing, InstanceId id) {
    if (existing.id != id) {
        return false;
    }
    return existing.kind == MutationKind::CreateInstance || existing.kind == MutationKind::DestroyInstance;
}

}  // namespace

ChangeHistoryService::ChangeHistoryService(DataModel& game) : game_(&game) {
    static_assert(kHistoryNoParent == DataModel::kNoParent, "history parent sentinel drifted");
}

bool ChangeHistoryService::playing() const { return game_ != nullptr && game_->simulation_running(); }

std::vector<ChangeHistoryService::Waypoint>& ChangeHistoryService::undo_stack() {
    return playing() ? session_undo_ : edit_undo_;
}

std::vector<ChangeHistoryService::Waypoint>& ChangeHistoryService::redo_stack() {
    return playing() ? session_redo_ : edit_redo_;
}

const std::vector<ChangeHistoryService::Waypoint>& ChangeHistoryService::undo_stack() const {
    return playing() ? session_undo_ : edit_undo_;
}

const std::vector<ChangeHistoryService::Waypoint>& ChangeHistoryService::redo_stack() const {
    return playing() ? session_redo_ : edit_redo_;
}

bool ChangeHistoryService::wants_mutation() const {
    if (!enabled_ || applying_ != 0 || game_ == nullptr) {
        return false;
    }
    if (playing() && !recording_) {
        return false;
    }
    return true;
}

void ChangeHistoryService::set_enabled(bool enabled) { enabled_ = enabled; }

void ChangeHistoryService::set_pending_gesture(std::string name) { pending_ = std::move(name); }

std::optional<std::string> ChangeHistoryService::try_begin_recording(std::string name, std::string display_name) {
    if (!enabled_ || recording_ || applying_ != 0) {
        return std::nullopt;
    }
    if (display_name.empty()) {
        display_name = name;
    }
    Recording recording;
    recording.id = std::to_string(next_id_++);
    recording.name = std::move(name);
    recording.display_name = std::move(display_name);
    recording.implicit = false;
    const std::string started_name = recording.name;
    const std::string started_display = recording.display_name;
    const std::string id = recording.id;
    recording_ = std::move(recording);
    on_recording_started.emit(started_name, started_display);
    return id;
}

void ChangeHistoryService::open_implicit(const Mutation& first) {
    Recording recording;
    recording.id = std::to_string(next_id_++);
    if (!pending_.empty()) {
        recording.name = pending_;
        recording.display_name = pending_;
        pending_.clear();
    } else {
        recording.name = default_gesture(first);
        recording.display_name = recording.name;
    }
    recording.implicit = true;
    recording_ = std::move(recording);
}

void ChangeHistoryService::push_or_coalesce(Mutation mutation) {
    if (!recording_) {
        return;
    }
    std::vector<Mutation>& list = recording_->mutations;
    if (mutation.kind == MutationKind::SetProperty) {
        for (std::size_t index = list.size(); index > 0; --index) {
            Mutation& existing = list[index - 1];
            if (blocks_coalesce(existing, mutation.id)) {
                break;
            }
            if (existing.kind == MutationKind::SetProperty && existing.id == mutation.id &&
                existing.before.prop == mutation.before.prop && existing.before.property == mutation.before.property) {
                existing.after = std::move(mutation.after);
                if (same_value(existing.before, existing.after)) {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index - 1));
                }
                return;
            }
        }
    }
    list.push_back(std::move(mutation));
}

void ChangeHistoryService::forget_core() {
    if (!recording_) {
        return;
    }
    std::vector<Mutation>& list = recording_->mutations;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [this](const Mutation& mutation) { return game_->core_holds(mutation.id); }),
               list.end());
    if (recording_->implicit && list.empty()) {
        const std::string id = recording_->id;
        finish_recording(id, FinishRecordingOperation::Cancel);
    }
}

void ChangeHistoryService::note(Mutation mutation) {
    if (!enabled_ || applying_ != 0 || game_ == nullptr) {
        return;
    }
    // Core is the studio's, not the place's: what happens there is never an
    // undo step, and what entered it leaves the open recording.
    if (game_->core_holds(mutation.id)) {
        forget_core();
        return;
    }
    if (playing() && !recording_) {
        return;
    }
    bool opened = false;
    if (!recording_) {
        if (playing()) {
            return;
        }
        open_implicit(mutation);
        opened = true;
    }
    if (!recording_) {
        return;
    }
    const std::string started_name = recording_->name;
    const std::string started_display = recording_->display_name;
    push_or_coalesce(std::move(mutation));
    if (opened) {
        on_recording_started.emit(started_name, started_display);
    }
}

void ChangeHistoryService::apply_waypoint(Waypoint& waypoint, bool inverse) {
    if (game_ == nullptr) {
        return;
    }
    struct Guard {
        int& depth;
        explicit Guard(int& depth) : depth(depth) { ++depth; }
        ~Guard() { --depth; }
    } guard(applying_);
    if (inverse) {
        for (std::size_t index = waypoint.mutations.size(); index > 0; --index) {
            const Mutation& mutation = waypoint.mutations[index - 1];
            // A step recorded before its instance went into Core no longer reaches it.
            if (!game_->core_holds(mutation.id)) {
                game_->apply_history(mutation, true);
            }
        }
    } else {
        for (const Mutation& mutation : waypoint.mutations) {
            if (!game_->core_holds(mutation.id)) {
                game_->apply_history(mutation, false);
            }
        }
    }
}

void ChangeHistoryService::finish_recording(std::string id, FinishRecordingOperation op) {
    if (!recording_ || recording_->id != id) {
        return;
    }
    Recording recording = std::move(*recording_);
    recording_.reset();
    if (op == FinishRecordingOperation::Cancel) {
        Waypoint inverse;
        inverse.mutations = std::move(recording.mutations);
        apply_waypoint(inverse, true);
    } else if (!recording.mutations.empty()) {
        Waypoint waypoint;
        waypoint.name = recording.name;
        waypoint.display_name = recording.display_name;
        waypoint.mutations = std::move(recording.mutations);
        waypoint.bytes = sizeof(Waypoint) + waypoint.name.size() + waypoint.display_name.size();
        for (const Mutation& mutation : waypoint.mutations) {
            waypoint.bytes += mutation_bytes(mutation);
        }
        std::vector<Waypoint>& undo = undo_stack();
        undo.push_back(std::move(waypoint));
        redo_stack().clear();
        trim(undo);
    }
    on_recording_finished.emit(recording.name, recording.display_name, recording.id, op);
}

bool ChangeHistoryService::is_recording_in_progress(std::optional<std::string> id) const {
    if (!recording_) {
        return false;
    }
    if (!id || id->empty()) {
        return true;
    }
    return recording_->id == *id;
}

void ChangeHistoryService::end_gesture() {
    if (recording_ && recording_->implicit && !pending_.empty()) {
        recording_->name = pending_;
        recording_->display_name = pending_;
    }
    pending_.clear();
    if (!recording_ || !recording_->implicit) {
        return;
    }
    finish_recording(recording_->id, FinishRecordingOperation::Commit);
}

void ChangeHistoryService::set_waypoint(std::string name) {
    if (!recording_) {
        pending_.clear();
        return;
    }
    recording_->name = name;
    recording_->display_name = std::move(name);
    pending_.clear();
    finish_recording(recording_->id, FinishRecordingOperation::Commit);
}

void ChangeHistoryService::reset_waypoints() {
    if (recording_) {
        const Recording recording = std::move(*recording_);
        recording_.reset();
        on_recording_finished.emit(recording.name, recording.display_name, recording.id, FinishRecordingOperation::Cancel);
    }
    pending_.clear();
    edit_undo_.clear();
    edit_redo_.clear();
    session_undo_.clear();
    session_redo_.clear();
}

void ChangeHistoryService::trim(std::vector<Waypoint>& stack) {
    std::size_t bytes = 0;
    for (const Waypoint& waypoint : stack) {
        bytes += waypoint.bytes;
    }
    std::size_t drop = 0;
    while (stack.size() - drop > 1 && (stack.size() - drop > max_waypoints_ || bytes > max_bytes_)) {
        bytes -= stack[drop].bytes;
        ++drop;
    }
    stack.erase(stack.begin(), stack.begin() + static_cast<std::ptrdiff_t>(drop));
}

void ChangeHistoryService::set_limits(std::size_t waypoints, std::size_t bytes) {
    max_waypoints_ = waypoints;
    max_bytes_ = bytes;
}

void ChangeHistoryService::undo() { step(true); }

void ChangeHistoryService::redo() { step(false); }

void ChangeHistoryService::step(bool undoing) {
    if (recording_ || applying_ != 0) {
        return;
    }
    std::vector<Waypoint>& from = undoing ? undo_stack() : redo_stack();
    if (from.empty()) {
        return;
    }
    Waypoint waypoint = std::move(from.back());
    from.pop_back();
    const std::string name = waypoint.display_name;
    apply_waypoint(waypoint, undoing);
    std::vector<Waypoint>& to = undoing ? redo_stack() : undo_stack();
    to.push_back(std::move(waypoint));
    (undoing ? on_undo : on_redo).emit(name);
}

std::pair<bool, std::string> ChangeHistoryService::can_undo() const {
    if (recording_ || applying_ != 0) {
        return {false, {}};
    }
    const std::vector<Waypoint>& undo = undo_stack();
    if (undo.empty()) {
        return {false, {}};
    }
    return {true, undo.back().display_name};
}

std::pair<bool, std::string> ChangeHistoryService::can_redo() const {
    if (recording_ || applying_ != 0) {
        return {false, {}};
    }
    const std::vector<Waypoint>& redo = redo_stack();
    if (redo.empty()) {
        return {false, {}};
    }
    return {true, redo.back().display_name};
}

void ChangeHistoryService::seal_edit_recording() {
    if (!recording_) {
        return;
    }
    finish_recording(recording_->id, FinishRecordingOperation::Commit);
}

void ChangeHistoryService::drop_session() {
    if (recording_) {
        const Recording recording = std::move(*recording_);
        recording_.reset();
        on_recording_finished.emit(recording.name, recording.display_name, recording.id,
                                   FinishRecordingOperation::Cancel);
    }
    session_undo_.clear();
    session_redo_.clear();
}

}  // namespace engine_core
