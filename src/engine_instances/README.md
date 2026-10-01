# engine_instances

The instance classes: `GameObject`, `Camera`, `Folder`, `Script`, `ModuleScript`, and the six asset classes. Each inherits `DataModel` from engine_core and registers its Lua class, with any properties, in its own `.cpp`.

The asset classes live in `AssetInstances.{hpp,cpp}`, each only under its own category in `Assets`. `Texture`, `Mesh`, and `Sound` are a `FileAsset`, with a `Path`. `Material` and `Model` are a `ReferenceAsset`, whose saved properties are references held by GUID (`InstanceRef`, from engine_core): `Material` has `DiffuseTexture`, `NormalTexture`, `RoughnessTexture`, and `MetalnessTexture`, each a `Texture?`, plus `Color` (a `Color3`, white) and `Reflectivity` (0.5) and `Transparency` (0), numbers shown on a 0 to 1 slider; `Model` has `Mesh` and `Material`. `Prefab` holds `Model`s as its only children.

`Camera` is a `GameObject` with a `FieldOfView`, in degrees from 1 to 120, a saved registry property. Its Transform places it, looking down its -Z. A new place starts with one in Workspace (`Project::add_default_camera`), and each Scene View follows one Camera in Workspace, chosen from the list at its top right; the render snapshot carries the Camera's FieldOfView on its row.

`PhysicsObject` is a rigid body, a plain `DataModel` with its own Transform, not a GameObject. Its class says `physics_body()`, which tags its entity so `PhysicsWorld` (engine_core, the only code that includes Box3D) finds the ones in Workspace. Script and Properties writes mark what changed in a dirty mask the physics world takes each step; the world's own writes go through `store_simulated` and record nothing. `Shape` is the first enum-typed saved property (`lua_saved_enum`), saved as the item's name, and `Mesh` is shown only for a Hull or a Custom (`lua_shown_when`, which takes a set of items).

`Script` and `ModuleScript` are separate classes that share `LuaSource`, which holds `Source` and the editor's Edit action. `Script` adds `Enabled` and the start generation the runtime checks; `ModuleScript` adds only its starter source.

A registration runs only if its object file is linked, and that happens only when something uses the class. The `Instance.new` factories in `ScriptRuntime` keep the classes a script can make linked.
