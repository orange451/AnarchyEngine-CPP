#pragma once

#include "DataModel.hpp"

namespace engine_core {

// The root of a place, which scripts see as game. A DataModel that is not an
// Instance, since a script cannot make one, and the only class with
// GetService. Constructing a Game makes a new, empty world named Game.
class Game : public DataModel {
public:
    Game();

    // Defined in Game.cpp so that file, and its Lua registration, stays linked.
    const char* class_name() const override;
};

}  // namespace engine_core
