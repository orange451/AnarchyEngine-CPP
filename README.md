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

## Projects

A project is a directory, and it is meant to live in git. `project.json` names it. `src/` holds the authored tree, one file per instance, and `resources/` is where textures, meshes, and audio will go. The live place is a working copy: loading reads `src/`, and saving writes back only the files whose bytes changed.

Every instance has a GUID that never changes, and every file is named `<Name>.<guid>`. Two siblings may both be called `Part`; they are two files. Adding a third `Part` adds one file, and renaming one moves only that file. An instance with children becomes a folder holding its own `init.json`. A script is a `.luau` file with a `.meta.json` beside it, so its source never lands inside JSON. Keys the engine does not know are kept as they are.

File > New starts an empty, untitled place. File > Open picks a project folder, File > Save writes to it, and File > Save As names a new folder and writes the whole project there. A place that has never been saved asks for a folder on its first Save. The title shows `*` while the place differs from what is on disk; New, Open, and closing the window offer to save it first. An edit undone back to the saved state does not count, and neither does anything a test does. The dialogs are the system's own: Cocoa on macOS, the Windows file dialog, and zenity or kdialog on Linux. `AnarchyEngine-CPP <folder>` opens a project at launch.

Saving during play writes the place as it was when Test started, never what the session created. Undo history, caches, and editor layout stay out of the tree; `.studio/` is ignored for them.

## The studio

The window is the studio. Explorers on either side list the place by instance name. The scene view draws each triangle in the place at that instance's position, and it keeps painting while the simulation is paused. The script editor opens a script from its row, highlights Luau, and completes names and members as you type. The console shows `print` output and script errors, and its command line runs Luau against the same place while play is stopped.

A click on a row selects that instance. Ctrl (Cmd on macOS) and a click adds or removes a row, and Shift and a click selects every row from the last one clicked. Both explorers show the same selection, and so do scripts: `game:GetService("Selection")` has `Get`, which returns the selected instances, and `Set`, which replaces them and shows in the explorers on the next frame. Delete removes every selected instance as one undo step, and Cut takes them all out of the place; a selected child goes with its selected parent. Paste puts everything cut under the row, in the order it was shown, and selects it. Cut, paste, and rename work on any instance. Rename edits the name in place on its row: Enter keeps it, and Escape or a click elsewhere drops it. Clicking a row again after a pause also renames it. Double-click a script to edit it. Edit > Test and Edit > Stop are the play session. The widgets and the window come from [JadeFX](https://github.com/orange451/JadeFX_CPP).
