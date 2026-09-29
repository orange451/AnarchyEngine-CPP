#pragma once

#include <chrono>

namespace engine_core {

class DataModel;

// RAII write (or read) guard. Read uses the same exclusive mutex: a reader
// must not observe a half-updated instance.
//
// Re-entrant only on the thread that already holds it, which is legal for
// SimulationThread. RenderThread takes Write only for the Prepare window.
class DataModelLock {
public:
    enum Kind { Write, Read };

    // Blocks. The thread that already holds this world's mutex, such as
    // SimulationThread, bumps the world's depth and does not lock again.
    DataModelLock(DataModel& game, Kind kind);

    // Render Prepare. On timeout, owns() is false and the destructor is a no-op.
    // timeout is the whole Prepare budget (2ms), not a per-instance wait.
    DataModelLock(DataModel& game, Kind kind, std::chrono::milliseconds timeout);

    ~DataModelLock();

    DataModelLock(const DataModelLock&) = delete;
    DataModelLock& operator=(const DataModelLock&) = delete;

    bool owns() const { return owns_; }

private:
    DataModel* game_ = nullptr;
    bool owns_ = false;
};

}  // namespace engine_core
