# Core Service Design

2026-10-02 · Second of three: event arguments (`2026-10-02-event-arguments-design.md`), then this, then the Dragger (`2026-10-02-dragger-brainstorm.md`). It does not depend on event arguments. The Dragger depends on both.

## Goal

A service, `Core`, where the studio's own tools live. Its contents render as Workspace's do, have no physics, and are never saved or recorded in undo history. They stay unchanged through New, Open, Play, and Stop. Its Scripts run in the plugin VM all the time. Game scripts cannot see it. The built-in plugins move into it from the unparented Scripts `PluginLoader` makes today.

## Decisions

| Question | Decision |
| --- | --- |
| Where it sits | A child of `game`, made last by `Game`, GUID `core`. Not in `kServices`, which is the place's services, saved and read by the project. |
| Persistence | Unchanged by New, Open, Play, and Stop: ids, properties, and children all stay. |
| Saved | Never. Never counts as an unsaved change. |
| Undo | Nothing under Core is ever recorded. |
| Physics | None. |
| Rendering | As Workspace. |
| Its Scripts | Run in the plugin VM, in edit mode and in play, from when they enter Core until they leave, are destroyed, or are disabled. |
| Who sees it | Plugins, the command line, and the MCP tools. Game scripts do not. |
| Explorer | Hidden. |
| Crossing into or out of Core | Refused, both ways, except an instance with no parent going in. `Parent = nil` on an instance in Core is refused: Destroy is the only way out. |
| Built-in plugins | Put into Core once at startup, never reloaded. |

## Architecture

### 1. The service (`engine_services/SceneService`, `engine_core/Containment`)

1. `class Core : public Service` with `hidden_in_explorer()` true, in SceneService.{hpp,cpp}. It is not a SceneService: `scene_service()` never returns it, and `ScriptRuntime::runs_here` never runs a play script under it.
2. It is not in `kServices`. That table is the place's services, which the project reader adopts, makes when missing, and resets (Project.cpp `adopt_services`, `class_registry`, `clear_world`). Core is outside the place, so none of that may touch it. `Containment.hpp` names it `kCoreClass`. `placement_error("Game", "Core")` allows it, and `DataModel::parent_error` places it under game once, as it places a service. `Game`'s constructor makes it last, with GUID `core`. `DataModel::core()` finds it. What Core may hold matches Workspace: `placement_error` has no rule for it.
3. Containment refuses reparenting across Core's boundary: an instance in Core may move only to another parent in Core, and an instance outside it may move in only from no parent. An instance that was in the place earlier and was then set to no parent may go in. Undo skips it from then on (section 4.3), so a waypoint naming it cannot pull it out of Core. The refusal's message says Core's contents stay in Core. Setting `Parent = nil` on an instance in Core is refused the same way.

### 2. Scope tag (`engine_core/Ecs`, `DataModel`)

1. `ecs::InCore` joins `InWorkspace` and `InLighting`. `apply_scope` (DataModel.cpp) computes it: true for Core's descendants. `clear_hierarchy` clears it with the others.
2. `bool DataModel::in_core(InstanceId id) const;` reads it. `bool DataModel::core_holds(InstanceId id) const;` is true for Core and everything under it, the test every rule below uses.

### 3. Persistence

1. **New and Open.** `clear_world` (Project.cpp) collects ids to destroy. It skips Core's descendants, and its reset loop skips Core. Project's `Rebuild` and the reader must not touch Core's subtree either. Check every other place a rebuild walks the tree for the same.
2. **Play and Stop.**
   - `capture_place_unlocked` (DataModelPlace.cpp) leaves Core's descendants out of `instances`, and leaves them out of Core's own `children` list.
   - `restore_place_unlocked` does not retire a live slot that is in Core, and does not restore one.
   - `clear_hierarchy` plus relinking must leave Core's subtree linked as it is: either relink Core's live children after the place's links are restored, or make `clear_hierarchy` skip that subtree.
   - The checks that the snapshot is complete must not count Core's live children as lost.
   - Slots: an instance captured at Test and destroyed during play would put its slot on the free list, where a new Core instance could take it, and Stop would then retire that Core instance to restore the captured record. While the simulation runs, `destroy` keeps a captured instance's slot off the free list. `rebuild_free_list` at Stop returns it.
   - So a change made in Core during play, including a new instance, is still there after Stop.
   - `stop_simulation` still drops every queued event, Core's included, and `disconnect_all` leaves plugin connections, which are kept connections, as it does today.
3. **Saving.** `authored_tree` skips Core and its descendants, so `Project::unsaved`, which plans files from it, never sees them.

### 4. Undo (`engine_services/ChangeHistoryService`, `engine_core/DataModelHistory`)

1. Each mutator's capture (`record_created`, `record_destroyed`, the parent and property mutations) skips an instance that is in Core.
2. Creating an instance records `CreateInstance` at once, while it still has no parent (`record_created`). When an instance enters Core, the open recording drops every mutation about it and its descendants. A recording left with nothing in it commits nothing. Check that an empty implicit recording leaves no waypoint.
3. `apply_waypoint` skips a mutation whose instance is now in Core. That covers a recording that committed before the instance went into Core.

### 5. Visibility to scripts (`engine_core/ScriptRuntime`, `ScriptBindings`)

In the play VM:
- `game:GetService("Core")` raises "Core is not available to game scripts".
- `game.Core`, `game:FindFirstChild("Core")`, and `game:FindFirstChildOfClass("Core")` give nil.
- `game:GetChildren()` leaves Core out, and `game:WaitForChild("Core")` never finds it. (There is no `GetDescendants` or `FindFirstChildOfClass` yet.)

The plugin and console VMs see Core like any service. The MCP tools go through the console's or the studio's view and see it too.

### 6. Scripts in Core (`engine_core/ScriptRuntime`, `ScriptHost`)

1. When a Script's `InCore` turns true and it is Enabled, `ScriptRuntime` registers it with `register_plugin(script)`. When `InCore` turns false, it is destroyed, or `Enabled` turns false, it unregisters it. `Enabled` turning true while in Core registers it again.
2. The hooks are the script host's `on_moved`, `on_script_enabled`, and `on_script_destroyed`, which run in edit mode too for this. They queue the Script, and `start_core_scripts()` registers or unregisters what is queued. `step_tools` calls it first, and `tools_open()` is true while something is queued, so the engine's edit loop steps even before the plugin VM opens. Starting on the next step, not inside the setter, keeps a plugin that parents a Script into Core from running another chunk inside its own call.
3. A ModuleScript in Core runs only through `require`, as everywhere.
4. Script ids in Core stay valid through Stop, New, and Open (section 3), so the plugin VM's registrations do too.

### 7. Rendering and physics

1. `SnapshotPump` (SnapshotPump.cpp:76, the prefab check at :201, and the light walk at :251) treats `in_core` as it treats `in_workspace`. GameObjects reach the snapshot through `render_query`, which needs `InWorkspace`. A second query, `core_render_query`, needs `InCore`, and `for_each_rendered` walks both. `body_query` keeps `InWorkspace` alone, so Core has no physics.
2. `PhysicsWorld` walks only Workspace (PhysicsWorld.cpp:383), so Core has no physics with no change. Add a test so that stays true.
3. Sounds in Core follow `AudioWorld`'s rule, under `game`, unchanged.

### 8. Built-in plugins move into Core (`ide/PluginLoader`, `ide/IdeLayout`)

1. `PluginLoader::load` creates each plugin's Script, sets `Name` and `Source`, parents it to Core, and calls `start_core_scripts()` so they run before it returns. It no longer calls `register_plugin` or turns history off. It keeps `loaded_` and destroys what it made last time, as the scene camera tests load twice.
2. `IdeLayout` calls it once at startup. The calls after New and Open (IdeLayoutProject.cpp:654 and :714) go.
3. `SceneCamera.luau` is unchanged.

## Tests

Sandbox Catch2: `sandbox/core_tests.cpp`, and the plugin cases in `plugin_tests.cpp`.

- CO1 New and Open leave Core's instances with the same ids, properties, and children.
- CO2 A change in Core during play, including a new instance, is still there after Stop. The place outside Core is restored as before.
- CO3 Saving writes nothing from Core, and changing Core never makes `unsaved()` true.
- CO4 Creating, changing, moving within Core, and destroying record no history. `Instance.new` followed by `Parent = Core` leaves no undo step. Undoing a step recorded before an instance went into Core does not touch it.
- CO5 Moving from the place into Core, or from Core into the place, is refused, and so is `Parent = nil` in Core. An instance with no parent may go in, including one moved out of the place to no parent earlier.
- CO6 In the play VM, GetService, indexing, FindFirstChild, and GetChildren do not reach Core. In the plugin and console VMs they do.
- CO7 A Script put into Core runs. Moving it within Core keeps it running, and destroying or disabling it stops it. Enabling it starts it again. It keeps running through Play and Stop, and through New.
- CO8 A Prefab-showing GameObject in Core is a snapshot row. A PhysicsObject in Core never simulates.
- CO9 The Explorer does not list Core.
- CO10 The built-in plugins are in Core after startup, are still there after New and Open, and SceneCamera flies. Existing `plugin_tests` and `scene_camera_tests` pass.
