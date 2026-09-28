# Conflicts Window Design

2026-09-28 · builds on the save guard (Milestone 1, branch `reload-from-disk`) and replaces the conflict dialog of the Reload From Disk tech spec's Milestone 2 (https://claude.ai/code/artifact/c955ce1b-1a6f-48b5-b8df-92b60c7f2a17).

## Goal

When files change on disk while the studio has the project open (Claude, git, an editor), the studio loads by itself every change it did not also make, and gathers the rest, property by property, in a dockable Conflicts window. There the user picks IDE or Disk for each row and applies the choices together. A count on the ribbon shows that conflicts exist, and Save's gate points to the window.

## Decisions

| Question | Decision |
| --- | --- |
| When the window fills | Live: a check runs when the studio regains focus, before Save, when a test stops, and from File > Reload from Disk. |
| What a row is | One property, grouped under its instance, as Find in Scripts groups lines under a script. A script's Source is one row. An instance deleted, moved, or added on one side while changed on the other is one instance-level row. |
| When a choice takes effect | Pick, then Apply: rows have an IDE / Disk toggle, and Apply applies every chosen row as one undo step. |
| Save gate | Keeps the alert, adds "To see more information, check the Conflicts window." and a Show Conflicts button beside Overwrite All and Cancel. |
| Ribbon | A warning icon and count at the right end, hidden at zero; clicking it reveals the window. |
| Reveal | A generic `IdeLayout::reveal_window(pane, open)` for any dockable pane: open it if closed, select its tab, raise its floating window. |
| Where merging happens | In JSON, inside `Project`: base (`files_`), disk (`PlanReader`), studio (`instance_bytes`), compared key by key. |
| Out of scope | A side-by-side Compare for scripts, and the live file watcher (Milestone 3). Each gets its own spec. |

## Architecture

### Engine (`engine_core`)

1. **`JsonMerge`** (new, pure). `merge_keys(base, disk, studio)` classifies each top-level key of three JSON objects as unchanged, changed on disk only, changed in the studio only, agreed (both changed to the same value), or conflicting. A value is compared as the JSON text `write_json` produces. A key missing on one side counts as the value "absent".
2. **One conflict type.** `SaveConflict` gains `key` (a property name, `Parent`, `Source`, or empty for an instance-level row), `studio`, and `disk` (display text). The guard, the check, the window, and Overwrite all use it; `operator==` compares every field, so Overwrite's "only what was listed" still holds.
3. **`Project::scan_disk()`** reads `src/` with `PlanReader` (throwing `ProjectError` when it does not read) and returns a `DiskScan`: the changes only on disk, and the conflicts.
4. **`Project::apply_disk(const DiskScan&, const std::vector<Choice>&)`** applies, as one undo step named "Changes from Disk", every disk-only change and every row chosen Disk; settles every row chosen IDE in the base. It scans again first and skips any row whose values changed since `scan`, returning the skipped rows.
5. **The base keeps each property file parsed.** Settling a key patches only that key; a file with no rows left takes the disk's bytes as its base.
6. **The guard becomes key-aware for property files.** A save stops only on a key both sides changed differently, or on a property file that does not parse. Script sources and structure keep the Milestone 1 rules.
7. **The title's `*`** compares the place with the base key by key (`Project::unsaved()`), so a disk file formatted differently from what the studio writes does not count as unsaved.

### Studio (`ide`)

8. **`IdeConflicts : IdePane`**, modeled on `IdeSearch`: a header with the count and All IDE / All Disk, group rows, property rows with a toggle, and a footer with the chosen count, Refresh, and Apply.
9. **`IdeLayout`** runs the checks, applies disk-only changes, holds the current conflicts, feeds the window and the ribbon, adds `reveal_window`, and gives the save gate its button.

### JadeFX

10. **`UtilityWindow::toFront()`** raises, focuses, and un-minimizes a floating window. It is a commit in the JadeFX_CPP repository (the sibling checkout this build uses), made before the studio calls it.

## Data flow

A check:

1. Waits while a rename or a Properties field is mid-edit, and runs when it commits or is cancelled. During a test it does nothing and says "Changes on disk will load when the test stops".
2. `flush_editors()`, so typing an editor has not written yet is the studio's side.
3. `scan_disk()` on the simulation thread. On `ProjectError`, applies nothing, shows a toast naming the file, and keeps the window's last list with a line saying why.
4. With disk-only changes, `apply_disk(scan, {})`, a toast such as `Loaded 3 changes from disk: Part, Door, and 1 more`, and the title's `*` recomputed.
5. The conflicts go to the window and the ribbon. A row that is the same conflict as before (same GUID, key, and both values) keeps its toggle.

Apply calls `apply_disk(scan, choices)`, then checks again. Rows with no choice stay.

Save checks first. When conflicts remain, the gate lists up to five rows as `Instance · Property`, says "To see more information, check the Conflicts window.", and offers Show Conflicts (reveal the window), Overwrite All (save over exactly the listed rows), and Cancel.

## Merge rules

### Property files, key by key

| Disk vs. base | Studio vs. base | Result |
| --- | --- | --- |
| same | any | the studio keeps its value |
| changed | same | disk-only: loads by itself |
| changed | changed to the same value | agreed: nothing to do |
| changed | changed to another value | a row |

- A key removed on disk resets the property to its class default, or erases the extra.
- `Name` is a key like any other.
- `Parent` is derived from the folder a file sits in: a move on disk is a `Parent` change, a row only when the studio also reparented the instance.
- `children` (sibling order) is a key like any other.
- `Source` is one value compared whole; its row shows the first differing line of each side.

### Whole instances

| Case | IDE | Disk |
| --- | --- | --- |
| Deleted on disk, changed in the studio | keep it; Save writes it back | delete it in the studio |
| Deleted in the studio, changed on disk | the delete stands; Save removes the file | bring it back from disk, same GUID |
| Added on disk inside a folder the studio moved or deleted | Save removes the file | create it in the studio |
| `class` changed on disk, the studio changed the instance | keep the studio's | recreate it from disk, same GUID |

Deleted or added on disk while the studio left the instance alone is a disk-only change. A lone `.meta.json` without its `.luau` makes `src/` unreadable, so checks wait; Save still writes the missing half back.

### Applying

One undo step: create, then update and reparent, then destroy. Instance ids stay, so the selection and open editors follow; an idle editor shows the new text. Undoing "Changes from Disk" makes those keys studio-only changes, which the next Save writes.

## UI

### Conflicts window

```
┌ Conflicts ─────────────────────────────────────────────────────────┐
│ 4 conflicts in 3 instances               [All IDE]  [All Disk]      │
├─────────────────────────────────────────────────────────────────────┤
│ ▾ ■ Part      Workspace                2            ( IDE | Disk )  │
│     Color     IDE  0.25, 0.5, 0.75    Disk  1, 0, 0  (•IDE | Disk )  │
│     Position  IDE  0, 2, 0            Disk  0, 3, 0  ( IDE |•Disk )  │
│ ▾ ▤ Bounce    Workspace.Demo           1            ( IDE | Disk )  │
│     Source    IDE  print(2)           Disk  print("hi") ( IDE | Disk )│
│ ▾ ■ Crate     Workspace                1                            │
│     deleted on disk, changed in the studio          (•IDE | Disk )  │
├─────────────────────────────────────────────────────────────────────┤
│ 3 of 4 chosen                            [Refresh]  [Apply]         │
└─────────────────────────────────────────────────────────────────────┘
```

- Docks beside the left explorer, as Search does; listed in the Window menu.
- Group row: class icon, name, path, count; its toggle sets every row under it.
- Row: property, IDE value, Disk value, toggle (unset at first). Long values are cut; hovering shows all of it.
- Apply applies the chosen rows as one undo step; disabled with nothing chosen, and during a test ("Stop the test to apply").
- Clicking a row outside its toggle selects the instance in the explorers.
- Empty: "No conflicts. Changes made outside the studio load when you switch back to it."
- File > Reload from Disk runs a check.

### Ribbon

`[▶ Test] [⏸ Pause] [⏵ Resume] [■ Stop] ···· [⚠ 4]`: after a spacer that takes the free width. Tooltip "4 conflicts with files on disk". Hidden at zero. Click: `reveal_window` on the window. A new 16 px `resources/icons/Warning.png` in the icon set's style.

### Save gate

```
 ⚠  4 properties conflict with files changed on disk.
    Part · Color
    Part · Position
    Bounce · Source
    and 1 more
    To see more information, check the Conflicts window.
              [Show Conflicts]  [Overwrite All]  [Cancel]
```

### `reveal_window(pane, open)`

Not docked: `open()`. Docked: `IdeDock::select(pane)`; when that dock is in a floating window (found through `floating_`), `UtilityWindow::toFront()`. Find in Scripts and the opening half of each Window menu item use it.

## Errors and edge cases

- `src/` does not read: nothing applies; toast names the file; the window keeps its list with the reason; the next check retries.
- Apply on an out-of-date list: `apply_disk` rescans and skips rows that changed; they come back, with a toast such as "1 row changed on disk and wasn't applied".
- A row's instance deleted in the studio after the check: skipped; the next check drops or re-lists it.
- A property file on disk that is not valid JSON, at Save: an instance-level row "changed on disk and can't be read"; Overwrite All writes the studio's version.
- MCP edits run on the simulation thread, as checks do, so they never interleave.

## Testing

- `JsonMerge`: Catch2 cases in the sandbox for every row of the key table, missing keys, and agreement.
- Scan and apply (`sandbox/project_tests.cpp`): disk-only loads; conflicting and agreeing values; `Parent` moves; `Source` rows; each instance-level case; `class` changes; IDE and Disk choices; a stale Apply skipped; undo of "Changes from Disk"; the key-by-key `*`; a hand-reformatted file is no conflict. Milestone 1's G tests that expect file-level `EditedOutside` on property files move to key rows.
- Studio (headless): the window's groups and rows; toggles and Apply; the ribbon count and its hiding; the save gate's Show Conflicts selects the window's tab; `reveal_window` docks a closed pane; a focus change pushed to the scene loads a disk-only change.
- By hand: `UtilityWindow::toFront()` (floating windows do not open headless); a `git checkout` with the studio open; Claude editing files beside it.
