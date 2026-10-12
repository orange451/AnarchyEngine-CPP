# Collision Groups editor

Date: 2026-10-12. Status: designed and built overnight at the user's request
("come up with a good Collision Group UI like you proposed earlier"); the user
was asleep, so every decision below is mine and is listed for review.

Builds on [collision groups](2026-10-11-collision-groups-design.md), and does
the item it left out of scope: a Studio editor, a Properties dropdown,
`RenameCollisionGroup`, and saving groups with the place (the user asked for
"the predefined ones" to save with the project).

## Goal

Set up collision groups in Studio without writing a script: see every group
and which pairs collide at a glance, change them with one click each, put
bodies in groups, and have it all saved with the place, undoable, and in
effect in Studio and when the game runs.

## Decisions

| Question | Decision |
| --- | --- |
| Where groups are saved | A hidden saved string property on Workspace, `CollisionGroups` (`lua_hidden(lua_json_text(lua_saved_property))`). Its value is the registry as JSON, `""` for Default alone. So a project save writes it, a load reads it, Stop restores it, and undo puts it back, all through the registry property machinery that Gravity already uses. `lua_json_text` makes the file hold the table itself rather than a string of it, so a project in git diffs by group; a load takes either form. |
| Source of truth | The DataModel's `CollisionGroups` registry, as now. The property reads it and writes it; it keeps no copy. |
| Play and Stop | No longer reset the groups. Play starts with the place's groups; scripts change them as before; Stop restores the place, Workspace's `CollisionGroups` included, which puts the authored groups back. |
| Script changes | Every PhysicsService change goes through the Workspace property, so it fires `Changed`, records undo inside an open recording (a plugin's, or the editor's), and marks the place unsaved in edit mode. During play, as for any play write, it is not on the edit stack. |
| Renaming | New `PhysicsService:RenameCollisionGroup(from, to)`. Keeps the group's bit, type, and pair settings, and moves every PhysicsBase in the place whose `CollisionGroup` is `from` to `to`. Default cannot be renamed; `to` must be non-empty and unregistered. |
| Where the editor lives | A dockable pane, "Collision Groups", in the Window menu, opened beside Properties' column like the other side windows. Also a "Collision Groups" card in a "Physics" group on the ribbon's Home tab, and an "Edit Collision Groups…" item in the Properties dropdown. |
| The matrix | A lower triangle of toggle cells, one per pair, the diagonal being a group against itself. Each column's group name sits on the hypotenuse, right of its diagonal cell, so no rotated text is needed. Hovering a cell lights its row and column and both names. |
| Group rows | Left of the matrix, aligned: a color dot, the name, how many bodies are in it, its filter type, and, while the row is hovered or selected, three icons: Select (its bodies), Assign (the selection to it), and delete. The icons sit beside the filter, not after the matrix, so a wide matrix never scrolls them out of a short dock. |
| Group colors | By registration order, round the hue circle by the golden angle from blue, never saved; Default is grey. Neighbours never share a hue (a hash of the name did), and the order is saved, so a group keeps its color. They tie a row to its column label. |
| Removing a group in use | Asks: "Move N bodies to Default" or Cancel. Unused groups go at once. |
| Bodies naming unregistered groups | A notice row lists those names and counts ("they collide as Default"), with Register, which registers them all. |
| Properties | `CollisionGroup` is a dropdown of the registered groups, then "Edit Collision Groups…". A value naming no registered group shows as an extra item, "Name (not registered)". |
| Undo | Each editor action is one undo step: "New Collision Group", "Rename Collision Group", "Delete Collision Group", "Set Collision Group Filter", "Set Groups Collidable", "Assign Collision Group", "Register Collision Groups". |
| During play | The pane edits the running game's groups; a banner says changes last until Stop. |

## 1. The saved form

`CollisionGroups::save_text()` and `load_text(text)`:

```json
{"groups":[["Default","Exclude"],["Players","Exclude"],["Ghosts","Include"]],
 "pairs":[["Players","Players",false],["Players","Ghosts",true]]}
```

A project file holds it as that table under Workspace's `"CollisionGroups"`
(`lua_json_text`), and omits it for Default alone.

- `groups`: every registered group in registration order, Default first,
  with its filter type's name.
- `pairs`: every explicit setting, once per unordered pair, earlier group
  first.
- `""` when the registry is Default alone, Exclude, with no settings, so a
  place without groups saves nothing.
- `load_text` rebuilds the registry in place (its revision only grows, so
  the physics world always notices). It refuses, changing nothing, text
  that is not this shape, a group without a name, a repeated name, a filter
  type that is not Exclude or Include, more than 62 groups, a first group
  that is not Default, or a pair naming a group not listed.
- A place being loaded (Workspace's write: a load, undo, or Stop) uses
  `load_text(text, repair)` instead, which keeps what is sound and never
  refuses, so a table a git merge damaged cannot stop a project opening.

## 2. Edits through the property

`edit_collision_groups(world, edit)` (engine_core): copies the registry,
applies `edit` to the copy, and when it succeeded and the saved text
changed, swaps it in and calls `note_property_change("CollisionGroups",
before, after)` on Workspace. Every PhysicsService method that changes the
registry, the property's own write, and the editor's writes use it. A
registry edited directly (`collision_groups().register_group`, as tests do)
still works; it just records nothing.

## 3. The pane

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ [▦] Collision Groups                                    [+ New Group]    │
 │     4 of 62 groups                                                       │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ ⚠ 3 bodies name groups that don't exist: Crates (3). They collide as     │
 │   Default.                                                  [Register]   │
 ├──────────────────────────────────────────────────────────────────────────┤
 │  GROUP           BODIES  FILTER               COLLIDES WITH              │
 │                                               ● Default                  │
 │  ● Default        1,204  [Exclude|Include]     ■ ● Players               │
 │  ● Players            4  [Exclude|Include] ⬚⇩✕ ■ ■ ● Ghosts              │
 │  ● Ghosts             0  [Exclude|Include]     □ ■ □ ● Debris            │
 │  ● Debris           120  [Exclude|Include]     ■ ■ □ ■                   │
 └──────────────────────────────────────────────────────────────────────────┘
```

- **Header:** the pane's icon in a badge, the title, "N of 62 groups", and
  New Group, which adds "New Group" (or "New Group 2", ...) and starts
  renaming it. Disabled at 62.
- **Rows:** a click selects a row (Delete removes it, F2 or a double-click
  on the name renames it, Escape cancels a rename). Default has no rename,
  no delete, and a lock in their place. The filter pill toggles Exclude and
  Include, with a tooltip saying what each means. The count is the bodies
  whose `CollisionGroup` is exactly that name.
- **Hover actions:** Select selects every body in the group; Assign puts the
  selected bodies, and the bodies under selected Models and Folders, in the
  group (dimmed with nothing selected); delete removes it.
- **Column names:** each sits in the row above its column, its dot centered
  over the column's cells, so the staircase reads as column headers.
- **Cells:** filled with a check when the pair collides, hollow when it does
  not; a click toggles it. Tooltip: "Players and Ghosts collide" or "don't
  collide"; the diagonal says "Players bodies collide with each other".
- **Rename:** trims spaces; refuses an empty name or one in use, with a
  toast. Renaming moves the group's bodies with it.
- **Empty state:** with only Default, a line under the rows says what groups
  are for and how to start.
- **Scrolling:** rows and matrix scroll together, both ways.
- **Play:** a banner, "Playing: changes here last until Stop."

## 4. Properties

`LuaField` gains `choices`, a function that lists a string property's
choices for the current world; `lua_choices(field, fn)` sets it.
`read_sheet` fills `PropertyRow::choices` from it (rows of mixed classes
take the first's). A String row with choices is a dropdown; picking
"Edit Collision Groups…" opens the pane (through a hook the layout sets)
and leaves the value as it was. `CollisionGroup` on PhysicsBase lists the
registered groups.

## 5. Out of scope

Coloring bodies by group in the Scene View, group search, reordering
groups, and a per-pair "back to the filter type's default".

## Testing

- Sandbox: `save_text`/`load_text` round trip and refusals; `rename_group`
  keeps bit, type, and pairs; Workspace `CollisionGroups` saves and loads
  through a project bag; undo and redo of a PhysicsService change inside a
  recording; Play keeps the authored groups and Stop puts them back after a
  script changes them; `RenameCollisionGroup` moves bodies and refuses
  Default, a taken name, and an unknown one; physics in edit mode honors
  saved groups (a ray in a group set apart passes through).
- collision-groups-tests (new): the model's view (rows, counts, matrix,
  unregistered names) and each write as one undo step; the pane in a
  headless scene: New Group starts a rename, a cell click toggles, the
  filter pill toggles, Delete on a used group asks, Assign Selection,
  Select Bodies, Register, and a script's change shows without reopening.
- properties-tests: CollisionGroup is a dropdown of the registered groups,
  a pick writes it as one undo step, an unregistered value shows as its own
  item, and Edit Collision Groups… calls the hook and writes nothing.
- studio-tests: the Window menu lists Collision Groups and opening it docks
  the pane.
- collision-groups-demo (by hand): the pane in light and dark, saved as PNGs.
