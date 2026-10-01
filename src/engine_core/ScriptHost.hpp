#pragma once

#include "types.hpp"

#include <string>

namespace engine_core {

class Script;

// Notices from Script. The runtime enqueues or kills. It does not resume here.
// The DataModel holds the host, and ScriptRuntime is the one that implements it.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    // id was reparented, so every Script under it, id included, may have gone
    // into or out of Workspace, Scripts, and Gui, the only places a script runs.
    // Runs at the end of set_parent.
    virtual void on_moved(InstanceId id) = 0;
    virtual void on_script_enabled(Script& script, bool enabled) = 0;
    virtual void on_script_destroyed(Script& script) = 0;
    // `child` is now under `parent` with this name, by a reparent or a rename.
    // Runs inside set_parent and set_name. WaitForChild wakes on it.
    virtual void on_child_named(InstanceId parent, InstanceId child, const std::string& name) {
        (void)parent;
        (void)child;
        (void)name;
    }
};

}  // namespace engine_core
