# Save and Disk-Check Speed Plan

**Goal:** A save, and the disk check before it, cost time in proportion to what changed, not to the size of the project, with the Conflicts UI catching every outside edit exactly as it does today.

**Measured (2026-10-13, copy of "H1Z1 Z1 Vegetation 100k", 154,484 instance files, Release):**

| Case | Now |
|---|---|
| Edit one object in the studio, save | ~3.0 s (save 2.8 s, check 0.4 s) |
| Edit one file outside (an LLM), then save | ~17 s (check 14.1 s: two full compares of ~6 s each; save 2.8 s) |

Inside one full compare: read and parse every file 2.5-3.3 s, scratch build to normalize every file 1.35 s, studio tree with every property 0.6-0.8 s, key-by-key loop over every instance 0.8-1.1 s. Inside the save: three folder walks (~0.35 s each), plan_files deep-copying every unchanged instance's JSON and bytes 0.55 s, then freeing them 0.35 s, and other O(N) passes.

**Hard constraint:** conflicts. A file an outside writer changed must never be silently overwritten or silently absorbed into the base. Every rule of `compare_disk` (merge by key, class change, moves, adds, deletes, refused placements) must give the same rows and actions as today.

## Design

1. **DiskIndex** replaces the digest-only `DiskStamp`: per file, its size and modified time, from one directory walk (no file opened). A file modified within `kRacySeconds` of the walk is racy and is proven by its bytes, as now.
2. **Disk mirror.** The disk side of a comparison (plan, normalized docs, parents, GUID index) is kept between checks behind a `shared_ptr<const DiskSide>`, valid for the index it was read at. A new check walks once, diffs the index, and re-reads only files whose stamp changed. Anything structural (a file added, removed, or renamed; a folder's children order) rebuilds the mirror with today's full read. Changed files are normalized in a scratch place of their own.
3. **Verify mode.** `ANARCHY_VERIFY_DISK_MIRROR=1` (and every sandbox test) also does the full read and fails on any difference in plan or docs. This is how isolated normalization is proven equal to the full build, class by class.
4. **Compare only what changed.** The key-by-key loop runs for instances changed on disk (index diff since the last sync), changed in the studio (authored dirty set), or not in the base; every other instance has disk == base == studio and cannot produce a row or an action. The studio tree carries properties only for those. `refresh_base` takes from disk only those.
5. **One walk per save.** `apply_disk` marks the base synced with the index it walked before reading (a file that changes after that walk shows up as changed next time, so this is strictly safe). The save reuses the check's index instead of walking again, verifies each file's stamp right before overwriting it (a mismatch is a conflict, so a write landing mid-save still stops the save), and updates the index for only the files it wrote.
6. **No deep copies in the save.** `Files` holds its parsed JSON and bytes behind shared, copy-on-write storage, so planning a save shares unchanged instances instead of copying ~220 MB.

## Tasks

1. DiskIndex replaces DiskStamp; `disk_matches_base` diffs indexes. Existing project and conflict tests pass unchanged.
2. Disk mirror with incremental re-read of changed files; full rebuild on any structural change; verify mode; tests that edit, add, remove, rename and reorder files from outside and compare against the full read.
3. compare_disk restricted to changed instances; studio tree with properties only where needed; refresh_base restricted. Verify mode also runs the full compare and requires identical rows and actions.
4. apply_disk: one walk, synced from the pre-read index.
5. Save: reuse the check's index, pre-write stamp verification, incremental index update. New tests: an outside write between the check and the save to a file the save writes is a conflict; to a file it does not write is found by the next check.
6. Save CPU: copy-on-write `Files` content; plan_files and the bookkeeping stop copying unchanged instances.
7. Measure both cases again on the copy; remove the temporary timers; full Release test run; merge and push.

**Targets:** studio edit then save under 0.3 s; outside edit then save under 1 s, on the copy of the 100k project.

## Background save (added after the first measurements)

Save from the menu, Ctrl+S, or the status bar chip, with a project open and no
test running, no longer blocks the studio:

1. A worker walks src/ (`Project::walk_disk`).
2. On the simulation thread, briefly: `apply_disk(choices, walk)` loads what
   only the disk changed and finds conflicts; with none, `begin_save` captures
   the incremental save.
3. The worker writes the files (`Project::write_save`).
4. On the simulation thread: `end_save` makes the base what was written.
5. On the UI: toasts, the conflict prompt, or an error. A file changed under
   the write (`Written::Stale`), or a disk that could not be read, falls back
   to the synchronous save, which checks again and asks.

While it runs the chip says "Saving...", disk checks wait, a second Save runs
after it, and anything that saves, checks, opens, closes, exports, or starts a
test calls `IdeLayout::wait_for_save` first. MCP `save_place`, Save As, the
conflict prompt's saves, and saves with a follow-up stay synchronous.
