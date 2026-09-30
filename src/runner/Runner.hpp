#pragma once

#include <memory>

namespace engine_core {
class Engine;
class LuaEngine;
}

namespace runner {

class SceneFeed;

// Play session. The IDE owns one and starts it with the shell.
// prepare() builds the Lua VM and the simulation object. start() launches the
// simulation and render threads. Jobs bound between those two calls are in
// place before either loop runs. SimulationThread is the only thread that
// runs gameplay on the DataModel.
class Runner {
public:
    Runner();
    ~Runner();

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    void prepare();
    void start();
    void stop();
    bool running() const { return lua_ != nullptr; }

    engine_core::LuaEngine& lua();
    engine_core::Engine& simulation();
    // What the Scene Views draw: the snapshots the simulation's render thread publishes.
    SceneFeed& feed() { return *feed_; }

private:
    // Declared first, so it outlives the engine whose render thread writes it.
    std::unique_ptr<SceneFeed> feed_;
    std::unique_ptr<engine_core::LuaEngine> lua_;
    std::unique_ptr<engine_core::Engine> simulation_;
    bool threadsStarted_ = false;
};

}  // namespace runner
