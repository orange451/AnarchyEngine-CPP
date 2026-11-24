# Tweens, tags, gamepads, and save data

Four small engine additions, approved together on 2026-11-23. Each is independent of the others.

## Why

Every game written for the engine hand-rolls the same four things or goes without: animating a property over time, finding "every coin", reading a controller, and keeping a high score between runs. Each is small to add and helps almost every game.

## Goals, non-goals, anti-goals

- Goal: a script reaches each feature in one or two lines.
- Goal: keep Roblox's names where they work, and improve where Roblox's shape is awkward or exists only because Roblox is a cloud service.
- Non-goal: a named input-action layer ("Jump" is Space or ButtonA). It is a feature of its own that builds on the gamepad work.
- Non-goal: gamepad rumble (GLFW has none), cloud saves, per-player server saves.
- Anti-goal: an API that needs `pcall`, positional option lists, or boilerplate to do the common thing.

## Approaches

1. Mirror Roblox exactly (`TweenInfo.new` with six positional arguments, `Create` then `Play`; `DataStoreService` with `GetAsync`/`SetAsync`). Familiar, but carries Roblox's awkward parts into an engine that has no reason for them.
2. Invent new shapes throughout. Loses the familiarity the rest of the engine's API trades on.
3. **Picked:** Roblox's names and concepts where they are good (`TweenService`, `CollectionService`, `AddTag`, `UserInputService` events, `Enum.EasingStyle`), with better shapes where Roblox is weak: one-call tweens with named options, a tag observer, direct gamepad polls, and a plain synchronous local save store.

## TweenService

`game:GetService("TweenService")`.

- `TweenService:Tween(instance, goals, options?)` starts a tween at once and returns it. `goals` is a table of property name to end value. `options` is a table: `Time` (seconds, 1 by default), `Style` (an `Enum.EasingStyle` item or its name, `Quad` by default), `Direction` (`Enum.EasingDirection`, `Out` by default), `Delay` (0), `Repeat` (extra plays, 0; -1 is forever), `Reverses` (false).
- Tweenable values: number, `Vector2`, `Vector3`, `Color3`, and `Matrix4` (position lerped, rotation slerped). Anything else is refused with an error naming the property.
- The tween: `Pause()`, `Resume()`, `Cancel()` (stays where it is), `Completed` (fires once, with `true` when it ran to the end and `false` when cancelled or replaced), `Wait()` (yields until then, returning the same boolean), and `Playing`.
- `TweenService:Value(from, to, options, callback)` tweens a plain value, calling `callback(value)` each frame; it returns the same kind of tween.
- A new tween on an instance's property replaces the running one on that property; the replaced tween keeps its other properties.
- Tweens step once a frame during play, before Heartbeat, through the normal property setters, so `Changed` fires and Stop restores everything. A tween whose instance is destroyed ends as cancelled. Stop drops every tween.
- Styles: Linear, Sine, Quad, Cubic, Quart, Quint, Exponential, Circular, Back, Bounce, Elastic. Directions: In, Out, InOut.

## Tags

- On every instance: `AddTag(tag)`, `RemoveTag(tag)`, `HasTag(tag)`, `GetTags()` (sorted). A tag is 1 to 100 characters with no control characters.
- Stored as one hidden saved property, `Tags`, a JSON array, the way `Attributes` is one hidden map: save, load, copy, undo, and Stop carry it with no further work. A change fires `Changed` with `"Tags"`.
- `game:GetService("CollectionService")`: `GetTagged(tag)` returns every instance in the place with the tag; `GetInstanceAddedSignal(tag)` and `GetInstanceRemovedSignal(tag)` fire with the instance.
- New: `CollectionService:Observe(tag, callback)`. `callback(instance)` runs for every instance that has the tag now and every one that gets it later. `Observe` returns a connection whose `Disconnect()` stops it. Removal is `GetInstanceRemovedSignal` (changed 2026-11-24: the callback no longer returns a cleanup function).
- A Tags row in Properties when it fits the panel simply; otherwise tags are set from scripts and the command line in this pass.

## Gamepad

- `Enum.UserInputType.Gamepad1` to `Gamepad4`. `Enum.KeyCode` gains `ButtonA`, `ButtonB`, `ButtonX`, `ButtonY`, `ButtonL1`, `ButtonR1`, `ButtonL2`, `ButtonR2`, `ButtonL3`, `ButtonR3`, `ButtonStart`, `ButtonSelect`, `DPadUp`, `DPadDown`, `DPadLeft`, `DPadRight`, `Thumbstick1`, `Thumbstick2`.
- Buttons fire `InputBegan` and `InputEnded` with the pad as `UserInputType`. A stick or trigger that moves fires `InputChanged` with its value in `Position`.
- `UserInputService.GamepadConnected` and `GamepadDisconnected` fire with the pad; `GetConnectedGamepads()` lists them.
- Direct polls, the improvement over Roblox's array search: `GetGamepadAxis(gamepad, keyCode)` returns a `Vector2` for `Thumbstick1`/`Thumbstick2` (up is +Y, dead zone 0.15 applied and rescaled) and `(value, 0)` for `ButtonL2`/`ButtonR2`; `IsGamepadButtonDown(gamepad, keyCode)`.
- The host polls GLFW once a frame (studio scene view while it has focus, and the player) and posts changes through the same queue as keys.
- `PlayerController` follows the left stick and jumps on `ButtonA` when its input path allows it without a redesign.

## SaveService

`game:GetService("SaveService")`.

- `SaveService:Open(name)` returns the store of that name (1 to 50 letters, digits, `_`, `-`); the same store object for the same name.
- `store:Get(key, default?)`, `store:Set(key, value)`, `store:Remove(key)`, `store:Keys()` (sorted), `store:Clear()`. All synchronous; none needs `pcall` in normal use.
- Values: boolean, number, string, a table of these (array or string-keyed, nested, no cycles), and `Vector2`, `Vector3`, `Color3`, `Matrix4`. Anything else is refused with an error naming what it was.
- One JSON file per store: `<user data>/saves/<gameId>/<name>.json`, where user data is the engine's per-user folder. Written to a temporary file and renamed, within a second of a change, and at Stop and quit. A file that cannot be read loads as empty and the console says why; it is not overwritten until the first `Set`.
- A studio Test reads and writes the same file a built game would for that project, so saves can be tested in the editor. An untitled place keeps saves in memory for the session only.
- Runs wherever the script runs: on a server, the server's machine; on a client, the client's.

## Left to the builder

File layout, where each service's state lives, the easing formulas, the on-disk JSON shape for typed values, and the test list. Each feature gets tests in `sandbox/`, a README section, and script-analysis definitions so the editor completes and checks it.
