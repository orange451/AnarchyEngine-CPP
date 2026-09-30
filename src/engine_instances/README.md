# engine_instances

The instance classes: `GameObject`, `Folder`, `TestTriangle`, `Script`, `ModuleScript`, and the six asset classes. Each inherits `DataModel` from engine_core and registers its Lua class, with any properties, in its own `.cpp`.

The asset classes live in `AssetInstances.{hpp,cpp}`, each only under its own category in `Assets`. `Texture`, `Mesh`, and `Sound` are a `FileAsset`, with a `Path`. `Material` and `Model` are a `ReferenceAsset`, whose saved properties are references held by GUID (`InstanceRef`, from engine_core): `Material` has `DiffuseTexture`, `NormalTexture`, `RoughnessTexture`, and `MetalnessTexture`, each a `Texture?`; `Model` has `Mesh` and `Material`. `Prefab` holds `Model`s as its only children.

`Script` and `ModuleScript` are separate classes that share `LuaSource`, which holds `Source` and the editor's Edit action. `Script` adds `Enabled` and the start generation the runtime checks; `ModuleScript` adds only its starter source.

A registration runs only if its object file is linked, and that happens only when something uses the class. The `Instance.new` factories in `ScriptRuntime` keep the classes a script can make linked.
