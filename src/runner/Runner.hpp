#pragma once

#include <memory>

namespace engine {
class LuaEngine;
}

namespace runner {

// Play session. The IDE owns one and starts it with the shell.
// Starting the runner starts the Lua engine that will drive the data model.
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

private:
    std::unique_ptr<engine::LuaEngine> lua_;
};

}  // namespace runner
