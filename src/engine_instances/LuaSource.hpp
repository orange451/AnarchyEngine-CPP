#pragma once

#include "DataModel.hpp"

#include <string>
#include <vector>

namespace engine_core {

// What Script and ModuleScript share: Luau source, opened in the script editor
// and checked by script analysis. Its Lua class, LuaSource, has Source. Script
// adds Enabled and runs on its own; ModuleScript runs only through require.
class LuaSource : public DataModel {
public:
    LuaSource(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    void set_source(std::string source);
    const std::string& source() const { return source_; }
    // Moves each time the source changes, by set_source or a Stop restoring
    // it. A reader that keeps a copy checks this to know the copy is current.
    std::uint64_t source_version() const { return source_version_; }

    // Edit, then the actions every instance has. Edit is the double-click.
    void context_actions(std::vector<ContextAction>& out) const override;

    // Source is never a property: it lives in the .luau file.
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    void on_release() override;
    void on_reuse() override;
    // The source's length, then its bytes.
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

    std::string source_;
    std::uint64_t source_version_ = 1;
};

}  // namespace engine_core
