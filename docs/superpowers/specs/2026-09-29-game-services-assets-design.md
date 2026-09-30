# Game Services and the Assets Pane Design

2026-09-29 · builds on the scene services (commit 954bcd2) and the resources rules each project's `resources/README.md` states.

## Goal

A place keeps its assets in a fixed tree under `game`, the way it keeps its scene in `Workspace`. The tree is made of GameServices: services the Game Explorer does not show, that no one can move, rename, or destroy. Six asset classes live in it, and a dockable Assets pane, modeled on the macOS Finder, is where you browse and organize them.

```
game
├── Workspace, Lighting, Storage, Scripts   (scene services, unchanged)
└── Assets                                   (GameService)
    ├── Materials   Material, Folder
    ├── Prefabs     Prefab (holding Models), Folder
    ├── Meshes      Mesh, Folder
    ├── Textures    Texture, Folder
    └── Audio       Sound, Folder
```

## Decisions

| Question | Decision |
| --- | --- |
| Scope | GameServices, the Assets tree, six asset classes, reference properties, and the Assets pane. No Import, no thumbnails, no rendering of meshes or materials. |
| What "invisible" means | The Game Explorer does not list GameServices or anything under them. Scripts, Properties, and MCP see them as they see any instance. |
| Who may change a GameService | No one: scripts, the explorer, Properties, and MCP are all refused a move, rename, or destroy, as for scene services. |
| What a category holds | Its own asset class and Folder, at any depth. Assets holds only its five categories. |
| Where an asset may live | Only under its own category. A Model only in a Prefab. |
| Prefab | A Prefab's children are Models. A Model joins a Mesh and a Material. |
| Material | PBR: four Texture references, `DiffuseTexture`, `NormalTexture`, `RoughnessTexture`, `MetalnessTexture`. |
| The audio class | `Sound`, since `Audio` is the category's class name. |
| Pane views | Icons, List, and Columns, picked by a toggle in the toolbar and remembered with the layout. |
| Rule structure | One `Service` base for scene and game services, one table of the fixed tree, and one table of placement rules by class name, which `parent_error` checks for the whole subtree being moved and the project reader checks before any instance exists. |
| How a reference is held | By the target's GUID, resolved when read, so loads, Stop, and undo need no ordering and a file's references are written back as they were read. |

## Architecture

### Services (`engine_core`, `engine_services`)

1. **`Service : DataModel`** is the new base of every service, with `is_service()` true. `SceneService : Service` keeps `is_scene_service()` and its context actions (Paste only). **`GameService : Service`** adds `hidden_in_explorer()`, true, and the same context actions.
2. **`Assets`, `Materials`, `Prefabs`, `Meshes`, `Textures`, `Audio`** are GameServices. None is an `Instance`, so `Instance.new` refuses them. Each is registered for Lua under `GameService`, which is under `DataModel`.
3. **One table describes the fixed tree**, in order: `{class, parent class}` for Workspace, Lighting, Storage, Scripts, Assets, Materials, Prefabs, Meshes, Textures, Audio, in a new `engine_core/Containment.hpp`. A service's GUID is its class name in lowercase (`service_guid`); `scene_service_guid` stays as a name for it. `kSceneServiceClasses` and `is_scene_service_class` stay for the four scene services.
4. **`Game()`** makes every service from the table, parents before children, with history disabled as today.
5. **`DataModel::scene_service(name)` stays** (it has 94 callers, all about scene services). **`DataModel::service(name)`** is new: the service of that class, directly under game or under a service directly under game.
6. **`parent_error`, `rename_error`, `destroy_error`, and `destroy`** ask `is_service()` where they ask `is_scene_service()` today. Placing a service under its table parent once, when `Game()` builds the tree or a load adopts it, stays the one allowed move.

### Containment

7. **Placement rules are a table by class name**, in `Containment.{hpp,cpp}`, so the project reader can check a plan before it creates anything. `placement_error(holder_class, child_class, child_name)` says why a holder refuses a child:
   - `Game`: only services; otherwise today's message, `Only scene services can be children of game; put X in Workspace`.
   - `Assets`: only its five categories; otherwise `Assets holds only Materials, Prefabs, Meshes, Textures, and Audio`.
   - Each category: its class and `Folder`; otherwise, for Textures, `Textures holds Textures and Folders` (Audio: `Audio holds Sounds and Folders`).
   - `Prefab`: only `Model`; otherwise `A Prefab holds only Models`.
   - `Texture`, `Mesh`, `Sound`, `Material`, and `Model` hold nothing: `A Texture holds nothing`.
   - Any other class takes anything but an asset class (item 8).
   A `Folder` is never the holder: the rule that decides what goes in a Folder is that of the first ancestor that is not a Folder (`passes_rule_up`). A Folder chain that ends outside the tree has no rule.
8. **An asset class belongs to one container.** `Texture`, `Mesh`, `Sound`, `Material`, and `Prefab` may be parented only under their own category (directly or through Folders); `Model` only directly under a `Prefab`. Any other holder refuses them: `A Texture must be in Assets.Textures`, `A Model must be in a Prefab`.
9. **`parent_error(id, new_parent)`** checks `id` against the holder of `new_parent`, then every descendant of `id` against the holder of its own parent, found as though `id` had already moved. The first refusal is the result. Moving to `kNoParent` (out of the tree) checks nothing, as today. Every caller already asks `parent_error` first: scripts, the explorer, cut and paste, Properties, and MCP. The explorer's insert asks it too, and destroys what it made when the place refuses it.

### Asset classes (`engine_instances`)

10. Six `Instance` classes, each saved through `lua_saved_property`:

    | Class | Properties |
    | --- | --- |
    | `Texture` | `Path: string` |
    | `Mesh` | `Path: string` |
    | `Sound` | `Path: string` |
    | `Material` | `DiffuseTexture`, `NormalTexture`, `RoughnessTexture`, `MetalnessTexture`: `Texture` reference |
    | `Prefab` | none |
    | `Model` | `Mesh: Mesh` reference, `Material: Material` reference |

11. **`Path`** is relative to the project's resources root, such as `textures/brick.png`, with `/` separators. A write refuses an absolute path, a drive letter, a backslash, and any `..` segment: `Path must be relative to the resources folder`. The default is `""`. Nothing checks that the file exists.

### Reference properties (`engine_core`)

12. **A saved property whose type is a registered class name with `?`**, such as `Texture?`, is a reference to an instance of that class (`reference_class`).
13. **The class holds the target's GUID** in an `InstanceRef`, with the id it last resolved to as a cache. A `LuaSlot` for a reference carries that GUID in `text`, and the resolved id in `id`.
14. **A write** takes `nil` (which clears), a live instance for which `lua_class_inherits(its class, "Texture")` (which stores its GUID), or a slot whose `text` is a GUID (a load, Stop, or undo, which stores it as it is). A live instance of another class is refused: `DiffuseTexture must be a Texture`.
15. **A read** gives the live instance holding the GUID, or `nil` when there is none, as after a destroy. Undo revives a destroyed instance with its GUID, so the reference comes back with no extra bookkeeping.
16. **JSON**: `slot_to_json` writes the GUID as a string, or `null`. `slot_from_json` reads a GUID string or `null`. The default is `null`. A GUID that no instance holds loads as it is, reads as `nil`, and is written back unchanged, so a file never changes because of a reference it could not resolve.
17. **Nothing else changes.** Undo, `Changed`, Stop's restore, place capture, disk merge and conflicts, and MCP reads and writes go through the saved-property path they already use.

### Scripts

18. `game.Assets`, `game.Assets.Textures`, and `FindFirstChild` work as for any child. `game:GetChildren()` includes `Assets`. `GetService` finds a service directly under `game` by class, so `GetService("Assets")` works and `GetService("Textures")` does not. `register_lua_service("Assets")` puts it in completion. `ScriptRuntime::runs_here` is unchanged, so nothing under Assets runs, and no category takes a script anyway.
19. The `.` completion at the start of a line offers `Assets` among the services.

### Projects

20. **Files** follow the existing rules: `src/Assets.assets/` holds `Textures.textures/`, and so on, and an asset is `<Name>.<guid>.json`. A Prefab with Models is a folder with its own `init.json`.
21. **`adopt_scene_services` becomes `adopt_services`** and walks the table: each service must sit under its table parent with its fixed GUID, and a service the files lack is made at its defaults. A project saved before this change loads with an empty Assets tree, and the next Save writes it. A reserved GUID used by another instance fails the load, as today.
22. **Containment is checked on load** with `placement_error` over the planned tree, before anything is created. A file that breaks it fails the load, naming the file: `src/Workspace.workspace/Brick.<guid>.json: A Texture must be in Assets.Textures`. As with any file that does not read, the Conflicts window says which.
23. **`clear_world` and `reset_scene_service`** keep and reset every service, not only scene services.

### Studio (`ide`)

24. **Game Explorer.** An instance whose nearest service is hidden (`hidden_in_explorer()`) gets no row: not in the tree, not from Filter, not from selection reveal. Insert does not offer the six asset classes. Something inserted or pasted at the top of the tree still goes into Workspace.
25. **Properties.**
    - A reference row is a `PropertyKind::Ref`, drawn and edited as `Parent` is today: the target's name, its path on hover, a click on the name to take the next instance clicked in the Assets pane or an explorer, a second click to cancel, and × to clear. A wrong class is refused with a toast. Dropping an asset dragged from the Assets pane on the row sets it.
    - `Name` and `Parent` are read-only for every service.
    - `Path` is a string row.
26. **`IdeAssets : IdePane`**, a new pane in `src/ide/IdeAssets.{hpp,cpp}`:
    - **Opening.** Window > Assets, with a check, opens it; Window keeps it while closed (`keep_closed`, as Terminal and Search). By default it docks as a tab beside the console. Reopened, it shows the folder it showed last.
    - **Toolbar.** Back and forward buttons (a navigation history of folder ids); a path bar with a clickable crumb per level from `Assets` down; the view toggle, Icons ▦, List ☰, and Columns ▥; and a Search field with ×.
    - **Search.** A non-empty search replaces the view with a flat list of every instance under the current folder whose name contains the text, ignoring case, each with its path. Clearing it restores the view.
    - **Sidebar.** Icons and List show an "Assets" heading and the five categories. A click opens that category. Columns view has no sidebar; its first column lists the categories.
    - **Icons view.** A wrapping grid of tiles, one per child in sibling order: the class's icon and the name.
    - **List view.** A tree table with Name, Kind (class name), and Path (for Texture, Mesh, and Sound). Folders and Prefabs expand in place. A click on a column header sorts by it; clicking again reverses the order.
    - **Columns view.** One column per level from the categories to the current folder. Selecting a Folder or Prefab opens the next column. A selected asset ends in a preview column with its icon, name, class, and its Path or references.
    - **Selection.** A click selects, Ctrl/Cmd+click toggles, and Shift+click extends, through the place's shared selection, so Properties and the explorers agree. The pane does not change folder when something outside it selects an asset.
    - **Actions.**
      - Double-click opens a Folder or Prefab.
      - Clicking a selected item again after a pause renames it in place; Enter keeps the name, and Escape or a click elsewhere drops it.
      - Delete deletes the selection.
      - Cut and Paste move items. Paste from the keyboard goes into the current folder. Paste from an item's menu goes into that item when it is a Folder or Prefab, else into the current folder.
      - Dragging moves the selection onto a folder, a Prefab, a sidebar category, or a crumb.
      - A right-click on an item offers Rename, Cut, Paste, and Delete. A right-click on empty space offers New Folder and New *kind*, where kind is the category's class (New Model inside a Prefab), and Paste.
      - A new item is selected and its name is opened for renaming.
    - **Refusals and undo.** Every move asks `parent_error`, and a refusal is a toast. Each action is one undo step.
    - **Updates.** The pane rebuilds when the tree's revision changes, as the explorer does, and follows play-mode changes live.
    - **Remembered view.** The view is kept in `preferences.json` as `"assetsView": "icons" | "list" | "columns"`, since `layout.json` holds only docks and tabs.
    - **A model under the view.** `AssetBrowser` (in `studio_core`, no widgets) holds the current folder, back and forward, the crumbs, the rows of each view, search, and sort, read from the place under its read lock. `IdeAssets` draws it, so the navigation is tested without a scene.
27. **Icons.** `IdeIcons` gets an icon for each service class and each asset class.
28. **MCP.** The tools read and edit the Assets tree like the rest of the place. A refused move, rename, or destroy returns the refusal's message.

## Testing

Engine tests are written first, in `sandbox/game_services_tests.cpp`:

- **Tree:**
  - A new place has Assets and its five categories, in order, with fixed GUIDs.
  - Each GameService refuses a move, rename, and destroy from C++ and from Lua.
  - `Instance.new("Textures")` fails.
- **Containment:**
  - Each category takes its class and Folder, and refuses the others.
  - A Folder under a category follows it.
  - Moving a Folder holding a Texture into Workspace or Meshes is refused.
  - A Texture cannot go in Workspace.
  - A Prefab takes only Models, and a Model cannot leave a Prefab.
  - The same refusals come from Lua's `Parent` write and from paste.
- **References:**
  - A wrong class is refused.
  - A destroyed target reads as nil, and undoing the destroy restores it.
  - Stop restores a reference set during play.
  - `Changed` fires.
  - Save writes the GUID, and load resolves it.
  - A GUID no instance holds loads, reads as nil, and saves back unchanged.
- **Path:** absolute, drive-letter, backslash, and `..` paths are refused.
- **Projects:**
  - The Assets tree round-trips through Save and load.
  - A project from before this change loads with default Assets and writes them at the next Save.
  - A file that breaks containment fails the load and names the file.
- **Scripts:**
  - `game.Assets` and `GetService("Assets")` work, and `GetService("Textures")` does not.
  - A Script cannot be put under Assets.

Studio tests:

- **`PropertiesTest`:**
  - A reference row shows its target.
  - The pick takes a click.
  - A wrong class is refused.
  - × clears the reference.
  - Service `Name` and `Parent` are read-only.
- **Explorer tests:** Assets has no row, even through Filter, and Insert offers no asset class.
- **New `AssetsPaneTest`:**
  - Navigation by sidebar, crumbs, back and forward, and double-click.
  - Each of the three views over the same folder.
  - Search.
  - New Folder and New *kind*.
  - Rename, delete, cut and paste, and drag-move, each one undo step.
  - A refused drop's toast.
  - Selection shared with Properties.
  - The view restored with the layout.
- **`McpTest`:** Assets is listed, and a refused move returns its message.

The finish is a run of the studio, with a screenshot of the pane in each view.

## Out of scope

Import (copying a file into `resources/<kind>/` and making its instance), image thumbnails, loading or rendering meshes, textures, materials, or sounds, placing a Prefab in Workspace, and `Clone`. Each gets its own spec.
