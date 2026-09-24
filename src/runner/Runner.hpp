#pragma once

#include <memory>

namespace engine {
class LuaEngine;
}

namespace engine_core {
class Engine;
}

namespace runner {

// Play session. The IDE owns one and starts it with the shell.
// Starting the runner starts the Lua VM and the simulation/render threads.
// SimulationThread is the only thread that runs gameplay on the DataModel.
class Runner {
public:
    Runner();
    ~Runner();

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    void start();
    void stop();
    bool running() const { return lua_ != nullptr; }

    engine::LuaEngine& lua();
    engine_core::Engine& simulation();

private:
    std::unique_ptr<engine::LuaEngine> lua_;
    std::unique_ptr<engine_core::Engine> simulation_;
};

}  // namespace runner
