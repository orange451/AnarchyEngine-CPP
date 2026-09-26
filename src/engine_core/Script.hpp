#pragma once

#include "DataModel.hpp"

#include <cstdint>
#include <string>

namespace engine_core {

class Script;

// Notices from Script. The runtime enqueues or kills. It does not resume here.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual void on_script_parent(Script& script, InstanceId previous, InstanceId next) = 0;
    virtual void on_script_enabled(Script& script, bool enabled) = 0;
    virtual void on_script_destroyed(Script& script) = 0;
};

// Source and Enabled. Script auto-starts. ModuleScript does not.
class LuaSource : public DataModel {
public:
    LuaSource(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    void set_source(std::string source);
    void set_enabled(bool enabled);

    // Edit, then the actions every instance has. Edit is the double-click.
    void context_actions(std::vector<ContextAction>& out) const override;

    // Enabled when false. Source is never a property: it lives in the .luau file.
    void save_properties(PropertyBag& out) const override;
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

    const std::string& source() const { return source_; }
    bool enabled() const { return enabled_; }
    std::uint32_t start_generation() const { return start_generation_; }
    // Bumps the generation connections must match. Returns the new value.
    std::uint32_t bump_start_generation();

protected:
    void on_release() override;
    void on_reuse() override;
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

    void reset_source_fields();

    std::string source_;
    bool enabled_ = true;
    std::uint32_t start_generation_ = 0;
};

// A DataModel instance. It may live anywhere in the tree. The Luau global
// `script` is the userdata for this instance. It does not run inside set_parent,
// set_enabled, or set_source; the runtime resumes it at the end of a drain
// or on the next Heartbeat.
class Script : public LuaSource {
public:
    Script(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : LuaSource(tag, state, id) {}

    const char* class_name() const override { return "Script"; }

protected:
    void on_release() override;
    void on_parent_changed(InstanceId previous, InstanceId next) override;
};

// Same fields as Script. Never auto-started. Runs only through require.
// A new module starts with an empty table that it returns.
class ModuleScript : public LuaSource {
public:
    ModuleScript(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const char* class_name() const override { return "ModuleScript"; }

protected:
    void on_reuse() override;
};

}  // namespace engine_core
