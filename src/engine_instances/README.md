# engine_instances

The instance classes: `GameObject`, `Folder`, `TestTriangle`, `Script`, and `ModuleScript`. Each inherits `DataModel` from engine_core and registers its Lua class, with any properties, in its own `.cpp`.

`Script` and `ModuleScript` are separate classes that share `LuaSource`, which holds `Source` and the editor's Edit action. `Script` adds `Enabled` and the start generation the runtime checks; `ModuleScript` adds only its starter source.

A registration runs only if its object file is linked, and that happens only when something uses the class. The `Instance.new` factories in `ScriptRuntime` keep the classes a script can make linked.
