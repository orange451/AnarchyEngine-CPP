# engine_services

`Game` is the root of a place, the object scripts see as `game`. It is the only class that makes a new world, and `GetService` is on it alone.

A new `Game` makes the four scene services as its children, in order: `Workspace`, `Lighting`, `Storage`, and `Scripts` (`SceneService.hpp`, `Lighting.hpp`). They are in the tree like any instance, each with a fixed GUID, and they live as long as the world: a project load or File > New empties them and puts their properties back to defaults rather than making new ones, so their ids never change. `GetService` returns them as instances. `Lighting`'s properties are saved registry properties (see engine_core's README), so the class holds only the values and their checks.

- `RunService`: the simulation phase signals, fired with each step's dt. Scripts reach it through `GetService`.
- `SelectionService`: what the studio has selected. Scripts reach it as `GetService("Selection")`, and the studio through `DataModel::selection()`.
- `UserInputService`: keys and the mouse over the scene view, as Roblox's `UserInputService` gives them. It works in edit mode too, not only while the place is playing, which is how a plugin such as the studio's built-in SceneCamera uses it. Scripts reach it as `GetService("UserInputService")`, and the studio posts to it through `DataModel::input()`. `MouseBehavior` (`Enum.MouseBehavior`: `Default`, `LockCenter`, `LockCurrentPosition`) locks the pointer in whichever scene view has focus, and `GetMouseDelta` then returns its motion since the last read; `Workspace.CurrentCamera` is the Camera that view last used, which a plugin like that one flies.
- `ChangeHistoryService`: edit undo, through `DataModel::history()`. Scripts do not see it.

`Game`, `RunService`, `Selection`, and `UserInputService` each declare their own Lua class. The members that need the script VM, such as `GetService`, `Selection:Get`, `Selection:Set`, and `UserInputService:IsKeyDown`, are added by `ScriptRuntime` in engine_core.

`GameService`, a `Service` the Game Explorer does not show, is the base of `Assets` and its five categories: `Materials`, `Prefabs`, `Meshes`, `Textures`, and `Audio` (`GameService.hpp`). A new `Game` makes them the same way it makes the scene services, under `Assets`, in that order, each with its own fixed GUID.

The scene view posts input from the UI thread into a queue with its own lock. The service keeps posts while a script runtime is attached, not only while the place is playing: the play step dispatches the queue at PreAnimation, and in edit mode the tool step dispatches it before Heartbeat, so the drain after either one runs `InputBegan`, `InputChanged`, and `InputEnded` before anything else in that step, and `IsKeyDown` agrees with them from then on. Mouse movement between two steps arrives as one `InputChanged` whose `Delta` is the sum.
