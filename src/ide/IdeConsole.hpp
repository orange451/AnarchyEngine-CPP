#pragma once

#include "IdePane.hpp"

#include <cstdint>
#include <memory>

namespace engine_core {
class Engine;
}  // namespace engine_core

namespace ide {

// Log of Lua print lines and errors, with a command line under it.
// The command line runs against the live data model, stopped or running.
// The log clears when a play session starts.
class IdeConsole : public IdePane {
public:
    explicit IdeConsole(engine_core::Engine& engine);

protected:
    void layoutChildren() override;

private:
    void pull();
    void submitCommand();

    engine_core::Engine& engine_;
    std::shared_ptr<jadefx::StyleClassedTextArea> log_;
    std::shared_ptr<jadefx::TextField> command_;
    std::uint64_t epoch_ = 0;
    bool pulling_ = false;
};

}  // namespace ide
