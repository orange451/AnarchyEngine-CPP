#include "ChangeHistoryService.hpp"

#include "DataModel.hpp"

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
    case HistoryProp::Position:
        return "Move";
    case HistoryProp::Color:
        return "Set Color";
    case HistoryProp::Size:
        return "Resize";
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
    }
    return "Edit";
}

bool same_transform(const Transform& a, const Transform& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

bool same_value(const PropertyValue& a, const PropertyValue& b) {
    if (a.prop != b.prop) {
        return false;
    }
    switch (a.prop) {
    case HistoryProp::Transform:
        return same_transform(a.transform, b.transform);
    case HistoryProp::Color:
        return a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b && a.color.a == b.color.a;
    case HistoryProp::Size:
        return a.size[0] == b.size[0] && a.size[1] == b.size[1] && a.size[2] == b.size[2];
    case HistoryProp::Simulated:
    case HistoryProp::VisualOnly:
    case HistoryProp::Enabled:
        return a.flag == b.flag;
    case HistoryProp::Name:
    case HistoryProp::Source:
        return a.text == b.text;
    case HistoryProp::Position:
        return a.vector.x == b.vector.x && a.vector.y == b.vector.y && a.vector.z == b.vector.z;
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
                existing.before.prop == mutation.before.prop) {
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

void ChangeHistoryService::note(Mutation mutation) {
    if (!enabled_ || applying_ != 0 || game_ == nullptr) {
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
            game_->apply_history(waypoint.mutations[index - 1], true);
        }
    } else {
        for (const Mutation& mutation : waypoint.mutations) {
            game_->apply_history(mutation, false);
        }
    }
}

void ChangeHistoryService::finish_recording(const std::string& id, FinishRecordingOperation op) {
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
        undo_stack().push_back(std::move(waypoint));
        redo_stack().clear();
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

void ChangeHistoryService::undo() {
    if (recording_ || applying_ != 0) {
        return;
    }
    std::vector<Waypoint>& undo = undo_stack();
    if (undo.empty()) {
        return;
    }
    Waypoint waypoint = std::move(undo.back());
    undo.pop_back();
    const std::string name = waypoint.display_name;
    apply_waypoint(waypoint, true);
    redo_stack().push_back(std::move(waypoint));
    on_undo.emit(name);
}

void ChangeHistoryService::redo() {
    if (recording_ || applying_ != 0) {
        return;
    }
    std::vector<Waypoint>& redo = redo_stack();
    if (redo.empty()) {
        return;
    }
    Waypoint waypoint = std::move(redo.back());
    redo.pop_back();
    const std::string name = waypoint.display_name;
    apply_waypoint(waypoint, false);
    undo_stack().push_back(std::move(waypoint));
    on_redo.emit(name);
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
