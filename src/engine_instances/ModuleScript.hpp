#pragma once

#include "LuaSource.hpp"

namespace engine_core {

// A LuaSource that never starts on its own and has no Enabled. It runs only
// through require. A new module starts with an empty table that it returns.
class ModuleScript : public LuaSource {
public:
    ModuleScript(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const char* class_name() const override { return "ModuleScript"; }

protected:
    void on_reuse() override;
};

}  // namespace engine_core
