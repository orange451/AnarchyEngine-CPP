# Studio Plugins Design

2026-10-09 · Four sub-projects, built in order: instance files, plugin loading, the `plugin` global with ribbon tabs, then dock widgets. Builds on the Core service (`2026-10-02-core-service-design.md`) and the plugin VM.

## Goal

Users write plugins the way Roblox Studio plugins are written. A plugin is a Folder: Scripts that define it, ModuleScripts it requires, and premade GUI. Right-clicking the Folder and choosing **Save as Plugin** writes it to the studio's config folder. The studio loads every saved plugin at startup and reloads one whenever its file is added, changed, or removed. A plugin's Scripts see a `plugin` global, which game scripts never have. Through it a plugin adds toolbar buttons to a **Plugins** ribbon tab and dockable widgets that show its GUI, themed by the IDE's CSS like every other pane.

## Decisions

| Question | Decision |
| --- | --- |
| Plugin root | A Folder. Every enabled Script under it runs, in tree order; ModuleScripts run only through `require`. No special "init" name. |
| One plugin, one object | All of a plugin's Scripts, and the modules they require, share one `plugin` object. |
| File formats | `.aeinst`: a serialized instance tree, one JSON file. `.aeplugin`: the same format, one Folder root, loaded from the plugins folder. Never called a "model". |
| Where plugins live | `config_directory()/plugins/*.aeplugin` (`%APPDATA%\AnarchyEngine\plugins` on Windows). |
| Plugin name | The file stem, which is the Folder's name, cleaned up for the filesystem. |
| Icons | A path under the IDE's own `resources/icons/`, written `"icons/<file>"`. Plugin files carry no images. |
| Reload | Polled once a second, and whenever the window regains focus, by modification time and size. Only the plugin whose file changed reloads. |
| Theming | The IDE's theme and `--ide-*` variables, as for any pane. No `settings()`. |
| Built-in plugins | SceneCamera and MoveTool stay `.luau` files loaded as they are today. They gain the `plugin` global for free. |
| Trust | Plugins have the plugin VM's existing access: everything, including Core. No capability system in v1. |
| Out of scope (v1) | `plugin:Activate` and viewport mouse capture, `PluginAction` and rebindable shortcuts, plugin menus, `GetSetting`/`SetSetting`, a plugin manager window, images bundled in plugin files, live-linking a plugin to its source Folder. |

## Architecture

### 1. Instance files (`engine_core/InstanceFile`)

An instance file is the clipboard saved to disk. The clipboard's in-memory copy (`CopiedNode`, `copy_set`, `paste_copies` in `src/ide/CutSet`) already handles per-class properties, refusals, and parenting; instance files only add JSON on top of it, so the two cannot drift apart.

1. `CopiedNode`, `copy_set`, and `paste_copies` move from `src/ide/CutSet.{hpp,cpp}` to a new `src/engine_core/InstanceFile.{hpp,cpp}`, because the plugin loader needs them outside the IDE. `CutSet` keeps its IDE-only functions and includes the new header.
2. The format:
   ```json
   {
     "format": "aeinst",
     "version": 1,
     "roots": [
       { "class": "Folder", "name": "MyPlugin", "properties": { },
         "children": [
           { "class": "Script", "name": "init", "properties": {"Enabled": true}, "source": "print('hi')" }
         ] }
     ]
   }
   ```
   `roots` may hold several trees. A node's `properties` is its `PropertyBag`; `source` appears only on a `LuaSource`; `children` is left out when empty. Script source is inline: one file matters more than per-script diffs.
3. Functions:
   - `JsonValue write_instance_file(const std::vector<CopiedNode>& roots);`
   - `bool read_instance_file(const JsonValue& json, std::vector<CopiedNode>& roots, std::string& error);`
   - `bool save_instance_file(const std::filesystem::path& path, const std::vector<CopiedNode>& roots, std::string& error);` writes `<path>.tmp` and renames it over `path`, so a reader never sees half a file.
   - `bool load_instance_file(const std::filesystem::path& path, std::vector<CopiedNode>& roots, std::string& error);`
4. Reading is strict. A `format` other than `aeinst`, a `version` other than 1, a node missing `class` or `name`, an unregistered class, or a wrong JSON kind fails the whole file, with an error naming the JSON path (`roots[0].children[2].class`). Unknown property keys are kept in the bag, as the project reader keeps them. A failed read builds nothing.
5. Instance references (such as `GameObject.Prefab`) follow copy and paste, which does not remap them. A reference is written as its target's GUID and resolves only if an instance with that GUID exists when the file loads. Plugins rarely hold references, so remapping can come later.

### 2. Plugin loading (`ide/PluginLoader`, `engine_core/ScriptRuntime`)

1. **Registration.** `PluginLoader` loads each `.aeplugin` with `load_instance_file`, refuses a file whose single root is not a Folder, builds it with `paste_copies` under `Core`, and calls `ScriptRuntime::register_plugin(folder)`. `start_core_scripts` treats a Script under any registered plugin root as nested, as it treats one under another Script today, and does not register it on its own. `run_plugin` then runs the Folder's enabled Scripts.
2. **Bookkeeping.** `PluginLoader` keeps, per loaded user plugin: name, path, modification time, size, root id. Built-in plugins keep their existing list.
3. **Watching.** On the UI thread, once a second and on window refocus (next to the existing `check_pending_` path), list `plugins/*.aeplugin` with modification time and size and compare with what is loaded. The differences are posted to the simulation thread:
   - **Added:** load.
   - **Removed:** unload.
   - **Changed:** unload, then load.

   The plugins folder is created when missing.
4. **Unloading,** in this order: fire `plugin.Unloading` (section 3) and run its handlers, `unregister_plugin` (kills threads and connections, commits an open recording), destroy the root Folder. Its toolbars and widgets go with it (sections 3 and 4).
5. **Errors.** A file that fails to load writes `Plugin "X" failed to load: <reason>` to Console and leaves nothing in Core. If a changed file fails, the old copy stays unloaded. A half-written file fails to parse and is retried when its modification time changes again. Runtime errors keep today's prefix, the Script's name.
6. **Save as Plugin.** `InstanceAction` gains `SaveAsPlugin` (label "Save as Plugin"). `Folder` overrides `context_actions` to offer it, and only Folders offer it. The handler runs `copy_set` on the folder on the simulation thread, then on the UI thread:
   1. If `plugins/<name>.aeplugin` exists, it asks "Replace plugin "<name>"?" and stops on No.
   2. It writes the file with `save_instance_file`.
   3. It triggers the watcher at once instead of waiting for the next poll.
   4. It shows `Saved plugin "<name>"`, or the error.
7. **Open Plugins Folder.** A File menu item that opens the plugins folder in the system file browser, creating it first if needed.

### 3. The `plugin` global and ribbon tabs

1. **The global.** `ScriptRuntime::new_thread`, for a thread in the plugin VM, sets `plugin` next to `script`: the `Plugin` userdata of the plugin root that owns the thread's script. A ModuleScript's thread gets the requiring plugin's object. Play and Console threads never set it, so `plugin` reads `nil` there.
2. **API (v1):**
   ```lua
   plugin.Name                       -- read-only: the file stem (a built-in's Script name)
   plugin.Unloading                  -- signal, fires before teardown
   local tb  = plugin:CreateToolbar("Terrain Tools")
   local btn = tb:CreateButton("Smooth", "Smooth the terrain", "icons/Brush.png", "Smooth")
   btn.Click:Connect(function() end)
   btn:SetActive(true)               -- lit, as the Grid toggle is when on
   btn.Enabled = false               -- dimmed, takes no clicks
   ```
   `Plugin`, `PluginToolbar`, and `PluginToolbarButton` are host userdata, not instances: never in the tree, never reparented.
3. **Rules.** `CreateButton` with an id already used in that toolbar raises a Lua error. An icon path must start with `icons/` and contain no `..`; anything else raises an error. It resolves against `resources/icons/` through `icon_file`. A missing file gives a text-only button and one Console warning.
4. **Threading.** `ScriptRuntime` owns a `PluginUi` registry (`engine_core/PluginUi`) of plugins, toolbars, and buttons, whose `revision()` moves on every change. Once a frame, under the short read lock it already takes, the IDE reads `PluginUi::toolbars()` if the revision has moved. A click is posted to the simulation thread as `PluginUi::click(button)`, which fires `Click` in the plugin VM. There is no host interface, so headless tests drive `PluginUi` directly.
5. **Ribbon tabs.** A thin tab row above the ribbon: **Home** and **Plugins**. Home holds today's buttons unchanged. Plugins holds one group per toolbar, in plugin load order (by file name, built-ins first). Each group is a caption with its toolbar's name, its buttons, then a separator. An empty Plugins tab shows a dim "No plugins installed — right-click a Folder → Save as Plugin". Styles go in `kStylesheet` using `--ide-*` variables. When a plugin unloads, its groups are removed; a reload puts them back in the same place, because the order comes from the file name.

### 4. Dock widgets

1. **API:**
   ```lua
   local widget = plugin:CreateDockWidget("Panel", {
       Title = "Terrain Tools", InitialDock = "Right",   -- Left | Right | Bottom | Center | Float
       Enabled = false, Width = 300, Height = 400, MinWidth = 200, MinHeight = 150,
   })
   script.Parent.Gui.Parent = widget
   ```
   Every option may be left out. `Title` defaults to the id. A second widget with the same id in one plugin raises an error.
2. **The instance.** `DockWidget` is a new GUI instance class, made under the plugin's root Folder. Its properties are `Title` (string) and `Enabled` (bool). `Enabled` is linked both ways: closing the pane's tab sets it to false, and setting it to true opens the pane. Its GuiBase children render as a ScreenGui's do; a ScreenGui child fills it. `CSS` instances directly under it style the widget.
3. **Rendering.**
   - `runner::GuiLayer` does two jobs today: building nodes for the GUI tree, and handling ScreenGuis and billboards over the viewport.
   - The first job moves into a new `runner::GuiTree`. It builds and diffs the nodes for one root instance, joins CSS, fires instance events, writes TextField text back, and loads ImagePane images.
   - `GuiLayer` keeps viewport placement, billboards, and `GuiInput`, using `GuiTree` internally. Game behaviour is unchanged.
   - `DockWidget` is a `GuiBase`, so `BillboardGui::drawn()` already skips a billboard inside one. A test pins this.
4. **The pane.**
   - `ide::PluginWidgetPane` is an `IdePane` that holds a `GuiTree` rooted at its `DockWidget`, placed directly in the IDE scene with no `SubScene`. The IDE theme, `--ide-*` variables, and the shell stylesheet reach it as they reach Explorer. Plugin CSS layers on top.
   - Mouse and keyboard stay in the pane and never reach `UserInputService`.
   - An ImagePane's Texture path inside a widget resolves against the IDE's `resources/`.
5. **IDE integration.** The pane's name is `plugin:<PluginName>/<id>`: stable across reloads and unable to collide with built-in panes. The Window menu gains a **Plugins** submenu listing every live widget by `Title`. A new widget opens in its `InitialDock`'s home dock, or floating at `Width`×`Height`.
6. **Saved layout.**
   - A widget's pane is saved in `layout.json` under its name, as any window is.
   - When a restore meets a `plugin:` name that has no pane yet, it records which dock that name was in. When the widget is made, it docks there and opens if that dock still exists; otherwise it uses `InitialDock` and `Enabled`.
   - When a plugin unloads, each of its open widgets records its dock the same way first, so a reload puts it back in the same dock.
   - A restore does not rebuild a dock that held only plugin widgets, so those widgets fall back to `InitialDock`. A record whose widget never appears is dropped when the layout is next saved.
7. **Teardown.** Destroying the plugin's Folder destroys the `DockWidget`, which closes its pane, removes its Window menu entry, and releases the `GuiTree`.

## Testing

- **`sandbox/instance_file_tests.cpp` (new):**
  - Round trip of a Folder holding a Script with source, a ModuleScript, GUI instances, and CSS.
  - Multiple roots.
  - Malformed JSON, an unknown class, a wrong format or version: each gives a path-naming error and builds nothing.
  - Unknown property keys survive a round trip.
- **`sandbox/plugin_tests.cpp` (extended), using a temporary plugins folder:**
  - A Folder with two Scripts registers as one plugin, and both run.
  - Removing the file unloads the plugin and kills its threads.
  - Changing the file reloads it with the new source.
  - A malformed file logs an error and leaves Core clean.
  - Save as Plugin, then load: the plugin runs the same way.
  - `plugin` is `nil` in Play and Console, and is one object across a plugin's Scripts and their modules.
  - Toolbar and button calls show up in `PluginUi::toolbars()` and move its revision; a duplicate button id raises an error.
  - A posted click fires `Click`.
  - `Unloading` fires before teardown.
- **Widgets:**
  - `GuiTree` builds the nodes `GuiLayer` builds today, and the existing GuiLayer tests pass.
  - Duplicate widget ids give an error.
  - `Enabled` is linked both ways.
  - BillboardGuis inside a DockWidget are excluded from the viewport.
  - A widget docks back into its recorded dock after a layout load and after a reload.
- **Live check through the studio MCP (Release build):**
  1. Build a sample plugin Folder: one toolbar button that toggles a widget holding a Button and a Label.
  2. Save it as a plugin and confirm the Plugins tab and the docked widget.
  3. Click the widget's Button and see its event in Console.
  4. Edit the plugin file on disk and see it reload in place.
