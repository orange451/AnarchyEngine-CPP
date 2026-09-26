# engine_services

`Game` is the root of a place, the object scripts see as `game`. It is the only class that makes a new world, and `GetService` is on it alone.

- `RunService`: the simulation phase signals, fired with each step's dt. Scripts reach it through `GetService`.
- `SelectionService`: what the studio has selected. Scripts reach it as `GetService("Selection")`, and the studio through `DataModel::selection()`.
- `ChangeHistoryService`: edit undo, through `DataModel::history()`. Scripts do not see it.

`Game`, `RunService`, and `Selection` each declare their own Lua class. The members that need the script VM, such as `GetService`, `Selection:Get`, and `Selection:Set`, are added by `ScriptRuntime` in engine_core.
