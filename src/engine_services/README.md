# engine_services

`Game` is the root of a place, the object scripts see as `game`. It is the only class that makes a new world, and `GetService` is on it alone.

- `RunService`: the simulation phase signals, fired with each step's dt. Scripts reach it through `GetService`.
- `SelectionService`: what the studio has selected. Scripts reach it as `GetService("Selection")`, and the studio through `DataModel::selection()`.
- `InputService`: keys and the mouse over the scene view, as Roblox's `UserInputService` gives them. Scripts reach it as `GetService("InputService")`, and the studio posts to it through `DataModel::input()`.
- `ChangeHistoryService`: edit undo, through `DataModel::history()`. Scripts do not see it.

`Game`, `RunService`, `Selection`, and `InputService` each declare their own Lua class. The members that need the script VM, such as `GetService`, `Selection:Get`, `Selection:Set`, and `InputService:IsKeyDown`, are added by `ScriptRuntime` in engine_core.

The scene view posts input from the UI thread into a queue with its own lock. The service keeps posts only while the place is playing. Each simulation step dispatches the queue at PreAnimation, so the drain after it runs `InputBegan`, `InputChanged`, and `InputEnded` before anything else in that step, and `IsKeyDown` agrees with them from then on. Mouse movement between two steps arrives as one `InputChanged` whose `Delta` is the sum.
