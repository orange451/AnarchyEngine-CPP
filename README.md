# Anarchy Engine

Anarchy Engine is a game engine and studio for building a place and playing it. The place is a tree of instances. Gameplay is Luau that reads and changes that tree. Test runs the simulation. Stop puts the place back to what you authored.

You use it to make a small game in one window: arrange instances, write the scripts that move them, run the place, and edit again from the restored world. The desktop studio is the way in. The engine underneath is what keeps the live place, the scripts, and the picture on screen in step with each other.

## The place

Every object in the world is an instance. It has a name, a parent, and children, and siblings may share a name. The root of that tree is the place a script sees as `game`.

Everything in the tree is a `DataModel`: it has `Name`, `Parent`, `FindFirstChild`, and the rest. What `Instance.new` makes is an `Instance`. `game` is a `Game`, a `DataModel` that is not an `Instance`, since a script cannot make one; `GetService` is on `game` alone. A `Parent` is any `DataModel`, so `game` or any instance.

`GameObject` is the instance that occupies space. It carries a transform, a color, a size, and a velocity. `Script` and `ModuleScript` hold Luau source; both are a `LuaSource`, which has `Source`. Only a `Script` has `Enabled`, since a `ModuleScript` runs only through `require`. Any other instance is a node in the tree, with a name, a parent, and signals.

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

`game:GetService("UserInputService")` is the keyboard and mouse, the way Roblox's `UserInputService` has them. Click the scene view during a test to give it the keyboard. `InputBegan`, `InputChanged`, and `InputEnded` fire with an `InputObject`, whose `KeyCode`, `UserInputType`, `UserInputState`, `Position`, and `Delta` say what happened, and `gameProcessedEvent`. `IsKeyDown`, `IsMouseButtonPressed`, `GetKeysPressed`, `GetMouseButtonsPressed`, and `GetMouseLocation` ask what is held now. Positions are points from the scene view's top-left corner. As in Roblox, `GetMouseLocation` returns a `Vector2`, and an `InputObject`'s `Position` and `Delta` are `Vector3`s whose z is 0, except that a `MouseWheel` input's `Position.Z` is how far the wheel turned. A key still held when the view loses focus gets its `InputEnded` then, and Stop forgets whatever was held.

```lua
local UserInputService = game:GetService("UserInputService")

UserInputService.InputBegan:Connect(function(input, gameProcessedEvent)
    if input.KeyCode == Enum.KeyCode.Space then
        print("jump")
    end
end)

game:GetService("RunService").Heartbeat:Connect(function(dt)
    if UserInputService:IsKeyDown(Enum.KeyCode.W) then
        print("forward", dt)
    end
end)
```

## Play

The simulation starts paused, so the place holds still while you edit. Test captures that place, starts every eligible script, and steps the world at 60 Hz. A frame can take several physics steps, then Heartbeat, which wakes `task.wait` and steps the instances in the tree.

Stop ends the session. Scripts are aborted, instances created during play are gone, and property changes revert to the captured place. Text you still have open in a script editor is written back onto those restored scripts, so the edit survives the stop.

## Projects

A project is a directory, and it is meant to live in git. `project.json` names it. `src/` holds the authored tree, one file per instance, and `resources/` is where textures, meshes, and audio will go. The live place is a working copy: loading reads `src/`, and saving writes back only the files whose bytes changed.

Every instance has a GUID that never changes, and every file is named `<Name>.<guid>`. Two siblings may both be called `Part`; they are two files. Adding a third `Part` adds one file, and renaming one moves only that file. An instance with children becomes a folder holding its own `init.json`. A script is a `.luau` file with a `.meta.json` beside it, so its source never lands inside JSON. Keys the engine does not know are kept as they are.

File > New starts an empty, untitled place. File > Open picks a project folder, File > Save writes to it, and File > Save As names a new folder and writes the whole project there. A place that has never been saved asks for a folder on its first Save. A toast at the bottom right of the window says what each one did, such as `Saved MyPlace (2 files changed)`. The title shows `*` while the place differs from what is on disk; New, Open, and closing the window offer to save it first. An edit undone back to the saved state does not count, and neither does anything a test does. The dialogs are the system's own: Cocoa on macOS, the Windows file dialog, and zenity or kdialog on Linux. `AnarchyEngine-CPP <folder>` opens a project at launch.

Saving during play writes the place as it was when Test started, never what the session created. Undo history, caches, and editor layout stay out of the tree; `.studio/` is ignored for them.

## The studio

The window is the studio. Explorers on either side list the place by instance name. The scene view draws each triangle in the place at that instance's position, and it keeps painting while the simulation is paused. The script editor opens a script from its row, highlights Luau, with datatypes such as `Vector3`, `Instance`, and `Enum` in their own color, and completes names and members as you type. A `.` at the start of a line, outside every function and block, lists the ModuleScripts in the place and the services whose names start with what you type after the dot; choosing one writes the line that gets it, so `.Conf` becomes `local Config = require(game.Workspace.Folder.Config)` and `.UserIn` becomes `local UserInputService = game:GetService("UserInputService")`. The console shows `print` output and script errors. A printed table shows as `{...}`, or `{}` when it is empty. Clicking `{...}` opens it one level at a time, written the way Luau writes a table, as it was when printed; clicking its `{` closes it. A table with a `__tostring` prints what that returns and does not open. Clear Output, from a right-click on the log or Cmd+K (Ctrl+K elsewhere), empties it. Its command line runs Luau against the same place while play is stopped.

Cmd+F (Ctrl+F elsewhere) in a script opens the find bar at the editor's top right, as in VS Code. It starts from the selection, or from the name under the caret, and highlights every match, the current one more strongly, with a count such as `2 of 6`. Aa matches case, ab matches whole words, and .* reads the text as a regular expression; Cmd+Alt+C, W, and R (Alt elsewhere) flip them. Enter and Shift+Enter, the up and down buttons, F3, or Cmd+G step through the matches. The replace field starts hidden. The chevron shows it, and Cmd+Alt+F (Ctrl+H) opens the bar with it: Enter or its first button replaces the current match and moves on, and Cmd+Enter or the second replaces every match as one undo step. A regular expression's replacement can use `$1` and `$&`. Escape closes the bar.

Edit > Find in Scripts, Cmd+Shift+F (Ctrl+Shift+F), docks a Search pane beside the Game Explorer that does the same across every Script and ModuleScript in the place. Each script with a match is listed with its path and how many matches it has, with its matching lines under it; clicking a line opens the script there with the match selected. Its replace field starts hidden too; Replace in Scripts, Cmd+Shift+H, shows it. Replace All asks first, then writes through open editors, where their own undo takes it back, and into every other script as one undo step in the place. While replace is shown, the button on the row under the pointer replaces just that line, or that whole script. The results keep up with edits, including text an editor has not written to its script yet.

A click on a row selects that instance. Ctrl (Cmd on macOS) and a click adds or removes a row, and Shift and a click selects every row from the last one clicked. Both explorers show the same selection, and so do scripts: `game:GetService("Selection")` has `Get`, which returns the selected instances, and `Set`, which replaces them and shows in the explorers on the next frame. Delete removes every selected instance as one undo step, and Cut takes them all out of the place; a selected child goes with its selected parent. Paste puts everything cut under the row, in the order it was shown, and selects it. Dragging a row moves it, and the rest of the selection with it when it is selected: dropped on the middle of a row, it goes inside that instance; dropped on a row's top or bottom edge, it goes beside it. While you drag, a box around the row or a line between rows shows where it will land, and under the last row of a branch the pointer's distance from the left picks how deep the line goes. A move is one undo step, a row never goes inside itself, and Escape cancels. Cut, paste, and rename work on any instance. Rename edits the name in place on its row: Enter keeps it, and Escape or a click elsewhere drops it. Clicking a row again after a pause also renames it. Double-click a script to edit it. Properties lists what every selected instance has in common: Name, Parent, and ClassName first, then the rest by name. A value the selection does not agree on is blank. Typing a value and pressing Enter, or leaving the field, writes it to every selected instance as one undo step; a blank field that was never typed in writes nothing, and Escape drops what was typed. A Vector3 axis writes only that axis. Parent shows the instance's name, with its path on hover: clicking the name takes the next instance clicked in an explorer, clicking it again cancels, x clears it, and a Parent that would put an instance inside itself is refused. Undo in a Properties field first undoes the typing in that field, then the place. The ribbon under the menu bar runs the play session: Test starts it, Pause and Resume hold and continue it, and Stop puts the place back. F5 is Test, or Resume when paused, and Shift+F5 is Stop. The Filter field at the top of each explorer hides rows whose name does not contain what you type, keeping the branches that lead to a match; its × empties it, and Escape leaves the field. Escape leaves every text field that way, keeping its text, such as the console's command line, the Search pane's fields, and those in Preferences. A field with its own use for Escape keeps it: a rename or a Properties value drops what was typed as it leaves, and the find bar closes. Escape in the tree clears the selection, and F opens the branches above the selected instances and scrolls to them. The widgets and the window come from [JadeFX](https://github.com/orange451/JadeFX_CPP).

### Themes and preferences

File > Preferences, Cmd+, (Ctrl+, elsewhere), opens the Preferences window. Its Appearance tab picks the theme the studio draws with: Light, Dark, Dracula, Nord, One Dark, Monokai, Solarized Dark, Solarized Light, Classic Studio (the colors of Roblox Studio's classic dark theme), or one of your own. The choice is kept in `preferences.json` in the studio's config folder, `~/Library/Application Support/AnarchyEngine` on macOS, `%APPDATA%\AnarchyEngine` on Windows, and `$XDG_CONFIG_HOME/anarchy-engine` (else `~/.config/anarchy-engine`) on Linux, and the studio starts in it.

A theme is a CSS file of custom properties on `:root`. `--theme-name` names it, and `--theme-base`, `light` or `dark`, picks the JadeFX look under it. The `--ide-*` variables are the studio's colors, such as `--ide-syntax-keyword-color`; JadeFX's own, such as `--accent-color`, can be set too, and a value may use `var()`. A theme sets only what it changes. The rest comes from the shipped theme it names in `--theme-extends`, such as `nord`, or else the shipped theme of its base. The shipped themes are the files in `resources/themes`; each but Light and Dark starts with its published palette, and every color after it names one of those. Nothing but variables is read.

```css
:root {
    --theme-name: "Solarized Dark";
    --theme-base: dark;
    --ide-editor-color: #002b36;
    --ide-syntax-keyword-color: #859900;
}
```

The Appearance tab lists every color with its variable's name, in groups such as Studio, Script Editor, and Console, each closed until its heading is clicked. Another click closes it again, and a filter narrows the list, opening every group with a match. A changed color shows in the studio at once. Reset gives a color back what the theme under it has, Revert drops every change, and Save writes them. The shipped themes are built in, so changes to one are saved with Save As, as a new theme that extends it. A saved theme keeps only the colors that differ from the theme it extends. Import copies a theme file into the `themes` folder beside `preferences.json` and picks it, and so does dropping a `.css` file on the window. Open Themes Folder shows that folder; a file put there by hand is listed the next time Preferences opens. Delete removes one of your themes.

## Talking to an LLM (MCP)

While the studio is open it runs a [Model Context Protocol](https://modelcontextprotocol.io) server, so an LLM client can read and edit the place. The first studio listens at `http://127.0.0.1:7777/mcp`; one opened while another has 7777 listens on any free port. A toast at the bottom right of the window says where, and if the server could not start, the console says why.

Several studios can be open at once, so clients go through `anarchy-mcp`, a bridge the build puts in `build/`. Register it once, for every folder you work in. With Claude Code:

```
claude mcp add anarchy -s user -- /path/to/AnarchyEngine-CPP/build/anarchy-mcp
```

Each studio names its project and port in a `studios` folder beside `preferences.json`, and the bridge sends each tool call to one of them:

1. the studio `select_studio` picked, by project name, folder, or pid, until it closes;
2. else the one `--project <name|folder|pid>` names, when the bridge was registered with it;
3. else the only studio open;
4. else the studio whose project folder holds the folder the client was started in, so `cd MyGame && claude` talks to the studio that has MyGame open.

Otherwise a call fails and lists the open studios. Once the studio `select_studio` picked closes, calls fail until it picks another; `select_studio` with `""` goes back to the usual order. `list_studios` shows every open studio and which one calls go to, and `get_studio_info` asks a studio which project it has open. The entry of a studio that crashed is dropped the next time the bridge looks.

With only one studio open, a client that speaks MCP over HTTP can also connect to it directly: `claude mcp add --transport http anarchy http://127.0.0.1:7777/mcp`.

The tools read the tree (`get_tree`, `find_instances`), read and set properties, create and delete instances, read and write scripts, get and set the explorers' selection, list classes and their Luau API, run Luau with `run_lua` and read what it printed, read the console with `get_output`, and start, pause, and stop a test with `playtest`. An instance is named by its id or by its path of names, such as `Folder.Part`. Edits go through undo like edits made by hand, and a chunk from `run_lua` shows in the console.

The server listens on the loopback address only, and refuses requests from a web page on another site. `ANARCHY_MCP_PORT` pins the port, and the server does not start when that port is taken. `ANARCHY_MCP=0` turns the server off, and `ANARCHY_MCP_TOKEN=<secret>` makes clients send `Authorization: Bearer <secret>`. The bridge sends the `ANARCHY_MCP_TOKEN` it was started with (with Claude Code, `claude mcp add ... -e ANARCHY_MCP_TOKEN=<secret> -- ...`; connecting directly, add `--header "Authorization: Bearer <secret>"`).
