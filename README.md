# Anarchy Engine

Anarchy Engine is a game engine and studio for building a place and playing it. The place is a tree of instances. Gameplay is Luau that reads and changes that tree. Test runs the simulation. Stop puts the place back to what you authored.

You use it to make a small game in one window: arrange instances, write the scripts that move them, run the place, and edit again from the restored world. The desktop studio is the way in. The engine underneath is what keeps the live place, the scripts, and the picture on screen in step with each other.

## The place

Every object in the world is an instance. It has a name, a parent, and children, and siblings may share a name. The root of that tree is the place a script sees as `game`.

`GameObject` is the instance that occupies space. It carries a transform, a color, a size, and a velocity. `Script` and `ModuleScript` hold Luau source. Any other instance is a node in the tree, with a name, a parent, and signals.

Creating an instance, parenting it, or writing a property is how the place grows. Those writes are what explorers, scripts, and the play session all share.

## Scripts

A script runs against the live place. It gets `game` for the root and `script` for itself. From there it creates instances with `Instance.new`, finds children, reads and writes properties, and connects to signals such as `Changed`. `task.wait`, `task.spawn`, `task.defer`, and `task.delay` schedule work on the simulation clock. A `ModuleScript` runs when another script calls `require`.

```lua
local tri = game:FindFirstChild("Tri0")
assert(tri)
local home = tri.Position

while true do
    task.wait(0.5)
    tri.Position = home + Vector3.new(0.45, 0, 0)
end
```

A script starts when it is parented, enabled, and the place is playing. It runs on the simulation, on the same clock as Heartbeat. The code is sandboxed to the libraries the engine opens: the Luau base libraries, `task`, and the instance API.

## Play

The simulation starts paused, so the place holds still while you edit. Test captures that place, starts every eligible script, and steps the world at 60 Hz. A frame can take several physics steps, then Heartbeat, which wakes `task.wait` and steps the instances in the tree.

Stop ends the session. Scripts are aborted, instances created during play are gone, and property changes revert to the captured place. Text you still have open in a script editor is written back onto those restored scripts, so the edit survives the stop.

## The studio

The window is the studio. Explorers on either side list the place by instance name. The scene view draws each triangle in the place at that instance's position, and it keeps painting while the simulation is paused. The script editor opens a script from its row, highlights Luau, and completes names and members as you type. The console shows `print` output and script errors, and its command line runs Luau against the same place while play is stopped.

Cut, paste, and rename work on any instance. Rename edits the name in place on its row: Enter keeps it, and Escape or a click elsewhere drops it. Clicking a row again after a pause also renames it. Double-click a script to edit it. Edit > Test and Edit > Stop are the play session. The widgets and the window come from [JadeFX](https://github.com/orange451/JadeFX_CPP).
