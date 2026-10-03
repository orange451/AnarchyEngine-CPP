#pragma once

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"

#include <optional>
#include <string>
#include <utility>

namespace ide {

// One IDE command's undo step. Opens a recording, and commits it when the
// command returns, by any path. When another recording is already open, or
// history is off, it opens nothing and the writes go where they would have.
class ScopedRecording {
public:
    ScopedRecording(engine_core::DataModel& world, std::string name) : history_(world.history()) {
        id_ = history_.try_begin_recording(std::move(name));
    }

    ~ScopedRecording() {
        if (id_) {
            history_.finish_recording(*id_, engine_core::FinishRecordingOperation::Commit);
        }
    }

    ScopedRecording(const ScopedRecording&) = delete;
    ScopedRecording& operator=(const ScopedRecording&) = delete;

private:
    engine_core::ChangeHistoryService& history_;
    std::optional<std::string> id_;
};

}  // namespace ide
