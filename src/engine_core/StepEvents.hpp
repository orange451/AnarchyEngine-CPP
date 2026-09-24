#pragma once

#include <mutex>

namespace engine_core {

// One queue of simulation-step deltas.
// SimulationThread publishes at the end of a step. The scene view consumes
// on the UI thread. The mutex hold is a few doubles, never a frame.
class StepEvents {
public:
    // Adds this step's delta. If the queue is full, the delta folds into the
    // newest entry so sim time is not dropped.
    void publish(double dt);

    // Sum of every delta published since the last consume. Zero when the
    // simulation has not stepped.
    double consume();

private:
    static constexpr int kCapacity = 64;

    // Guards dts_, head_, and size_.
    std::mutex mu_;
    double dts_[kCapacity] = {};
    int head_ = 0;
    int size_ = 0;
};

}  // namespace engine_core
