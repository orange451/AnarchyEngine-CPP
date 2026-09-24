#include "Runner.hpp"

#include "../engine/LuaEngine.hpp"

#include <stdexcept>
#include <utility>

namespace runner {

Runner::Runner() = default;

Runner::~Runner() = default;

void Runner::start() {
    if (lua_ != nullptr) {
        throw std::logic_error("runner already started");
    }
    auto engine = std::make_unique<engine::LuaEngine>();
    engine->start();
    lua_ = std::move(engine);
}

void Runner::stop() {
    lua_.reset();
}

engine::LuaEngine& Runner::lua() {
    if (lua_ == nullptr) {
        throw std::logic_error("runner is not started");
    }
    return *lua_;
}

}  // namespace runner
