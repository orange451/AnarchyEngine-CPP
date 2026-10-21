#pragma once

#include "SourceText.hpp"

namespace engine_core {

// What Script and ModuleScript share: Luau source, in a .luau file, opened in
// the script editor and checked by script analysis. Its Lua class, LuaSource,
// has Source. Script adds Enabled and runs on its own; ModuleScript runs only
// through require.
class LuaSource : public SourceText {
public:
    LuaSource(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : SourceText(tag, state, id) {}

    const char* source_extension(int = 0) const override { return "luau"; }

protected:
    // Script analysis checks it again.
    void on_source_changed() override;
    void on_release() override;
    void on_reuse() override;
};

}  // namespace engine_core
