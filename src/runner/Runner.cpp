#include "Runner.hpp"

#include "../engine/LuaEngine.hpp"
#include "../engine_core/Engine.hpp"

#include <stdexcept>
#include <utility>

namespace runner {

Runner::Runner() = default;

Runner::~Runner() = default;

void Runner::prepare() {
    if (lua_ != nullptr) {
        throw std::logic_error("runner already started");
    }
    auto lua = std::make_unique<engine::LuaEngine>();
    lua->start();
    auto simulation = std::make_unique<engine_core::Engine>();
    // The IDE is open for a long time. Pace both loops so they do not spin a core.
    simulation->set_pace_hz(60.0);
    lua_ = std::move(lua);
    simulation_ = std::move(simulation);
}

void Runner::start() {
    if (lua_ == nullptr) {
        prepare();
    }
    if (threadsStarted_) {
        throw std::logic_error("runner already started");
    }
    try {
        simulation_->start();
    } catch (...) {
        lua_->stop();
        lua_.reset();
        simulation_.reset();
        throw;
    }
    threadsStarted_ = true;
}

void Runner::stop() {
    simulation_.reset();
    lua_.reset();
    threadsStarted_ = false;
}

engine::LuaEngine& Runner::lua() {
    if (lua_ == nullptr) {
        throw std::logic_error("runner is not started");
    }
    return *lua_;
}

engine_core::Engine& Runner::simulation() {
    if (simulation_ == nullptr) {
        throw std::logic_error("runner is not started");
    }
    return *simulation_;
}

}  // namespace runner
