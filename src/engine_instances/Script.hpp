#pragma once

#include "LuaSource.hpp"
#include "ScriptHost.hpp"

#include <cstdint>
#include <string>

namespace engine_core {

// A LuaSource that runs on its own. It may live anywhere in the tree. The Luau
// global `script` is the userdata for this instance. It does not run inside
// set_parent, set_enabled, or set_source; the runtime resumes it at the end of
// a drain or on the next Heartbeat. Enabled false stops it and keeps it from
// starting.
class Script : public LuaSource {
public:
    Script(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : LuaSource(tag, state, id) {}

    const char* class_name() const override { return "Script"; }

    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }

    std::uint32_t start_generation() const { return start_generation_; }
    // Bumps the generation connections must match. Returns the new value.
    std::uint32_t bump_start_generation();

    // Enabled when false.
    void save_properties(PropertyBag& out) const override;
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    void on_release() override;
    void on_reuse() override;
    void on_parent_changed(InstanceId previous, InstanceId next) override;
    // Enabled as one byte, then the source.
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    bool enabled_ = true;
    std::uint32_t start_generation_ = 0;
};

}  // namespace engine_core
