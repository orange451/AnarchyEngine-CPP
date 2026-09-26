# engine_instances

The instance classes: `GameObject`, `Folder`, `TestTriangle`, `Script`, and `ModuleScript`. Each inherits `DataModel` from engine_core and registers its Lua class, with any properties, in its own `.cpp`.

A registration runs only if its object file is linked, and that happens only when something uses the class. The `Instance.new` factories in `ScriptRuntime` keep the classes a script can make linked.
