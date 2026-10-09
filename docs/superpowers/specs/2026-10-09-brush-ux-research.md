# Brush Editor UX Research: single 3D view, convex brushes (2026-10-09, second pass)

Scope: what makes TrenchBroom (TB) and Unity ProBuilder (PB) great for convex-brush level geometry, compared with Hammer/Hammer++ (H++), J.A.C.K., and NetRadiant-custom (NRC), and how that carries over to Anarchy Engine Studio's constraints. Those constraints are **one 3D perspective Scene View, a fly cam (RMB-hold + WASD/E/Q), physics-raycast picking, the existing move Draggers, and Valve 220 UVs.**

## Verification method (second pass)

- **TB:** I read the whole manual (`/manual/latest/`, 256 KB of text). The published page fills in key names at runtime from `shortcuts.js`, which is why the first-pass scrape showed blanks. This pass evaluated `shortcuts.js` + `shortcuts_helper.js` locally against the HTML, so **every TB key below comes from TB's own generated default-shortcut table**, rendered with Windows/Linux modifier names (Ctrl means Cmd on macOS).
- **PB:** I fetched the 6.0 pages (shape tool, poly shape, edit modes, extrude), the 5.0 predefined-shape workflow, and the 4.0 hotkey table. PB 5/6 moved most defaults into Unity's Shortcuts window. Where only the 4.0 table confirms a key, it is tagged **[PB4]**.
- **NRC:** I fetched the repo's `docs/mouse shortcuts.txt` verbatim. **H++:** I fetched its official features page. **J.A.C.K.:** I fetched its official features page.
- Anything still unconfirmed is tagged **[UNVERIFIED]**. The first pass used (≈); none of those marks remain.

Primary sources
- TB manual: https://trenchbroom.github.io/manual/latest/ ; key table: https://trenchbroom.github.io/manual/latest/shortcuts.js
- TB issues: #2287 measuring tool https://github.com/TrenchBroom/TrenchBroom/issues/2287 ; #2266 edge lengths https://github.com/TrenchBroom/TrenchBroom/issues/2266 ; #5182 draw on face normal https://github.com/TrenchBroom/TrenchBroom/issues/5182 ; #859 snap-to-face https://github.com/TrenchBroom/TrenchBroom/issues/859 ; #2973 3D grid clutter https://github.com/TrenchBroom/TrenchBroom/issues/2973 ; #3001 inward extrude split https://github.com/TrenchBroom/TrenchBroom/issues/3001 ; #398/#1270 primitives https://github.com/TrenchBroom/TrenchBroom/issues/398 ; #2878 J.A.C.K. edge extrude https://github.com/TrenchBroom/TrenchBroom/issues/2878
- PB: shape tool https://docs.unity3d.com/Packages/com.unity.probuilder@6.0/manual/shape-tool.html ; draw workflow https://docs.unity3d.com/Packages/com.unity.probuilder@5.0/manual/workflow-create-predefined.html ; snapping https://docs.unity3d.com/Packages/com.unity.probuilder@5.2/manual/shape-tool.html ; poly shape https://docs.unity3d.com/Packages/com.unity.probuilder@6.0/manual/polyshape.html ; modes https://docs.unity3d.com/Packages/com.unity.probuilder@6.0/manual/modes.html ; extrude https://docs.unity3d.com/Packages/com.unity.probuilder@6.0/manual/Face_Extrude.html ; hotkeys [PB4] https://docs.unity3d.com/Packages/com.unity.probuilder@4.0/manual/hotkeys.html
- H++: https://ficool2.github.io/HammerPlusPlus-Website/features.html ; Hammer wiki https://developer.valvesoftware.com/wiki/Hammer (VDC returned 403 to the fetcher this pass, so Hammer rows rely on the first pass and are tagged)
- NRC: https://github.com/Garux/netradiant-custom ; mouse map https://github.com/Garux/netradiant-custom/blob/master/docs/mouse%20shortcuts.txt
- J.A.C.K.: https://jack.hlfx.ru/en/features.html
- Community: func_msgboard archive https://www.quaddicted.com/webarchive/www.celephais.net/board/view_thread.php%3Fid=62151.html ; Leadwerks workflow thread https://leadwerks.com/community/topic/61597-editor-workflow-discussion/page/3 ; Quake Wiki https://quakewiki.org/wiki/TrenchBroom

---

## 1. Brush creation in 3D

| Tool | Interaction | Plane choice | Height / 3rd axis |
|---|---|---|---|
| **TB Simple Shape Tool** (permanent; active whenever no other tool is) | LMB-drag in 3D from the point under the cursor to the current point under the cursor, snapped to the grid | Plane under the cursor at drag start. In practice this is XY-oriented: TB draws axis-aligned only, and #5182 asked for "build off the normal of the clicked brush" | Default height = **grid size**. **Shift** = X==Y, **Shift+Alt** = cube, **Alt** = change only the height while dragging (manual "Creating Simple Shapes"). Uses the *current material* |
| TB shape types (new in latest manual) | Same drag; shape picked in the controls above the view | Same | Cuboid, **Stairs, Arch, Cylinder (optionally hollow), Cone, UV-sphere, Ico-sphere**. Shared "sides" + **circle mode** (edge-aligned / vertex-aligned / **scalable CZG**: 12/24/48/96 sides with grid-aligned vertices) so pieces mate. The arch is half a hollow cylinder, and drawing it taller than a semicircle extends the supports |
| **TB Brush Tool** (**B**, modal, 3D-only) | Click face points (grid-snapped onto existing brush faces); **double-click a face** adds all its vertices; **drag on a face** adds 4 rect corners; **Shift+drag** the polygon along its normal (duplicate-extrude); **Return** builds the convex hull; **Esc** discards all | Points must sit on existing brushes | Hull. Points can't be edited individually once placed |
| **TB Sweep Tool** (**Y**) | Select faces, then a ghost "destination cap" + handle: drag the center to move, rings to rotate, the green handle to scale; **arrows/PgUp/PgDn** step a grid unit, **Alt+arrows** step an angle, **[ / ]** scale; Path = Arc/Straight/S-bend; Segments; Iterations (spiral stairs); **Return** commits, **Esc** resets then exits | From selected faces | Generates a run of convex brushes. A clean model for pipes/arches/stairs |
| **PB New Shape** | **Press and drag** the base on X/Z, **release**, **move the mouse** for height (Y), **click** to finish (5.0 workflow). **Shift+hover** previews a stamp of the last shape and click places a copy, repeatable. **Esc** exits. **Ctrl/Cmd+Shift+K** opens the tool [5.0 doc] | **On top of the mesh under the cursor**, else the Unity grid plane | Two-phase, or typed X/Y/Z in the Shape Settings panel *before* drawing (the shape conforms). Pivot = first corner or center. Snapping uses Unity grid snapping (Global handle only); **Ctrl/Cmd** = increment snap |
| PB Poly Shape | Click points, close the loop on the first point, then drag the **center handle** for height (Extrusion field). The tool stays active for repeated shapes. Edit later: drag points, click the perimeter to add, Backspace deletes a point | Plane of the clicks | Handle or field |
| **NRC** | **m1 drag** = create/resize/move by context; **m1+Alt** (held during the drag) = "adjust height of brush being created; move stuff vertically"; **drag+Shift** = "new brush is quadratic"; **drag+Ctrl** = "new brush is cube" | Hit face plane in 3D (README: "Fully supported editing in 3D view (brush and entity creation, all manipulating tools)") | Alt during the drag |
| Hammer | Block tool. Creation is designed around the 2D views. Shapes: block, cylinder, cone, sphere, wedge, spike, arch (Arch dialog: wall width, arc°, start angle, sides, add height) | 2D axes | Other 2D view. **[UNVERIFIED this pass: VDC 403]** |
| **H++** | The features page does **not** claim 3D brush creation (the first-pass claim was **wrong**). It adds a 3D **Gizmo** (translate/rotate/scale; **RMB cycles mode**, **X** cycles none/box/gizmo, **Ctrl+J** global/local, **Ctrl+P** pivot, per-axis lock), a 3D **clipper** with handles and red cut preview, an overhauled **3D grid** (toggle **P**), a microgrid of 0.125–0.5, a **Merge** tool (**Ctrl+Shift+M**), and "Move to nearest floor/ceiling, optionally aligned to surface normal" | n/a | n/a |
| J.A.C.K. | "Exact Cone/Cylinder/Sphere" primitives; 3D drag-select | 2D-centric | n/a |

**Key lessons:** (1) The working plane comes from the raycast hit at drag start and stays **locked** for the whole drag. (2) Height comes from either a **held modifier** (TB/NRC Alt) or a **second phase** (PB). Support both. (3) Parametric shapes must share "sides + circle mode" so they mate (TB). (4) PB's **typed size before drawing** and **Shift-stamp repeat** are cheap precision and speed wins. (5) Our improvement over TB: orient the footprint to a **slanted** hit face's normal (#5182) as an option, with the default staying world-axis-aligned.

## 2. Grid

| | TB (verified) | PB/Unity | H++ | NRC |
|---|---|---|---|---|
| Sizes | 0.125, 0.25, 0.5, 1…256 | Arbitrary increment | 0.125–0.5 microgrid + custom | Pow2 |
| Change | **+ / -** ; **1..9** = 1,2,4…256 (no modifier) ; **Ctrl+Alt+wheel** | Grid & Snap overlay | View > Set Grid Size | **[UNVERIFIED]** |
| Show / snap | **0** show grid ; **Alt+0** snap to grid | Snap toggle overlay ; **Ctrl/Cmd** held = increment | **P** toggles 3D grid | n/a |
| Where drawn in 3D | **Projected onto brush faces** (distorts on non-axial faces); brightness pref. #2973 asks for a 3D-only grid off switch (clutter vs texture judging) | Single plane (XZ default) | "3D grid overhauled" | n/a |

Correction from the first pass: TB grid keys are **not** `[ / ]` or `Ctrl+0..9`. `[ / ]` belong to the Sweep scale handle.

Angle snap: TB rotate tool defaults to **15°** (editable field). Keyboard rotations without the tool are fixed at **90°** about the grid-snapped bbox center.

**3D-only adaptation:** (a) a grid on hovered/selected faces (TB), **plus** (b) a fading patch on the *active working plane* around the cursor during any drag, plus (c) a HUD readout of grid size and snap state, plus (d) a separate "show grid in viewport" toggle (#2973).

## 3. Selection

| | TB (verified) | PB | NRC (verified) | H++/J.A.C.K. |
|---|---|---|---|---|
| Modes | **Modeless**: click = object; **Shift+click** = face | Explicit: GameObject context vs ProBuilder context; **G** cycles vertex/edge/face [6.0 doc]; Esc returns to object. [PB4] H cycles modes, optional H/J/K unique modes | Modeless; **Ctrl+m1** face select in 3D | n/a |
| Add/toggle | **Ctrl+click** toggle; **Ctrl+Shift+click** add face | Shift add / Ctrl toggle **[UNVERIFIED for PB6]** | **Shift+m1** multi | n/a |
| Overlaps | **Ctrl+wheel** "drill": up pushes the selection away, down pulls it closer (after selecting the frontmost) | Repeat click cycles **[UNVERIFIED]** | **m1 click repeatedly = "tunnel selector (cycle through matches)"**; Shift+m2 tunnel | n/a |
| Paint select | **Ctrl+drag** from an unselected object (starting on a selected one duplicates!) ; faces: **Ctrl+Shift+drag** | n/a | **Shift+m1 drag** objects ; **Ctrl+m1 drag** faces | H++ **Ctrl+LMB drag** |
| Marquee in 3D | Objects: no (use *selection brushes* + **Ctrl+T** touching / **Ctrl+E** inside). Vertex/edge/face tools: rect lasso (toggles; **Ctrl** = force-select) | Rect select of elements; **Select Hidden** option in Tool Settings | **Shift+m2 drag** rect (select/deselect/toggle, complete/partial) ; **Ctrl+m2 drag** faces | J.A.C.K. 3D drag-select |
| Grouped | **Double-click** / **Ctrl+B** select siblings | n/a | n/a | n/a |
| Faces | **Shift+dbl-click** = all faces of brush; **Shift+Alt+dbl-click** = **coplanar flood fill** across touching brushes (+Ctrl to add) | [PB4] Alt+G grow, Alt+Shift+G shrink, Alt+L loop, Alt+R ring, Ctrl+Shift+I invert | n/a | n/a |
| Global | **Ctrl+A** all, **Ctrl+Shift+A** none, **Ctrl+Alt+A** invert; material browser right-click "Select Faces/Brushes" | n/a | n/a | n/a |
| Hide/isolate | **Ctrl+Alt+I** hide, **Ctrl+I** isolate, **Ctrl+Shift+I** show all (all undoable) | Unity scene visibility | n/a | n/a |

Lesson: TB's **modeless** "click = brush, Shift+click = face" is its speed secret. PB's **explicit modes** are its beginner secret. Best in class is the modeless default plus a HUD that says what a click *will* hit (hover pre-highlight), with optional sticky element modes. NRC's **click-again-to-tunnel** is the most discoverable occlusion solution for a single view, since it needs no modifier.

## 4. Face drag, vertex/edge/face tools, PB modeling

- **TB extrude (permanent, no tool switch):** with brushes selected, **hold Shift and hover** near a face, which gets a yellow outline, then **LMB-drag** to move it along its normal. TB refuses moves that would add or remove faces. **Ctrl at drag start** splits off a *new* brush instead, and dragging inward with Ctrl splits the brush in two (the #3001 request was implemented). Coplanar *identical* faces across brushes move together, and opposing normals are allowed. Snapping does both at once: the distance snaps to the grid, **and** the face snaps when any vertex component lands on a grid plane. In 2D only, **Shift+Alt** moves the face freely. The first pass said "Ctrl+Shift". Precisely, it is Shift (hover) + Ctrl (when the drag starts).
- **TB move:** drag on the XY plane. **Alt** (at start *or mid-drag*) moves vertically. **Shift** locks to the axis with the largest displacement so far, and releasing it lifts the lock. A **trace line** is drawn and gets thicker when locked. Hovering a selection draws **bbox spikes** from its corners for alignment, plus bbox dimension labels. **Ctrl+drag** duplicates and moves.
- **TB Scale (T)/Shear (G):** drag bbox side/edge/corner. **Shift** = proportional, **Alt** = anchor at center. Shear: **Alt** for vertical.
- **TB Vertex (V)/Edge (E)/Face (F):** hover shows a red outline + coordinates. Click to select, **Ctrl+click** to multi-select, LMB rect lasso (toggles; **Ctrl** forces select). Drag on XY, **Alt** for vertical. Snap is relative by default, and **Ctrl mid-drag toggles absolute** (vertex tool only; edge/face are relative-only). **Shift+Alt+click** a target vertex to snap the selection onto it. **Shift+hover** shows a grid point, and dragging it **adds a vertex**. **Del** deletes the selected elements, but only if every brush stays 3D. Concavity is avoided by auto-triangulating incident faces, or deleting the vertex and ending the move. Coincident vertices fuse. Coincident vertices across brushes move together ("clumping"). Arrow keys/PgUp/PgDn nudge. R/T/G rotate/scale/shear the selected vertices. The Face tool's handles also reach back-facing faces. **UV Lock (U)** keeps UVs during vertex edits.
- **H++ vertex tool:** realtime; "**faces turn red when the shape is invalid**". **J.A.C.K.:** "validity restrictions", triangulate non-planar faces, vertex-snap using the selected vertices.
- **PB:** extrude with **Shift+drag** on the move/rotate/scale handle (**Shift+scale = inset**) or **Ctrl/Cmd+E** (Extrude By: Face Normals / Vertex Normals (default) / Individual Faces; Distance 0.5). [PB4]: Alt+E connect, Alt+U insert edge loop, Alt+S subdivide, Alt+V weld, Alt+C collapse, Alt+X split vertex, Alt+B bridge, **P** toggles handle orientation (Global/Local/**Normal**).
- **NRC:** **Ctrl+Alt+m1** = "extrude pointed/selected brush faces"; **Alt+m1** in 3D = alternative resize; "Quick vertices drag / brush faces shear shortcut"; bbox manipulator for any affine transform with a draggable origin.
- **Precision gaps (TB users):** edge lengths are not shown on non-orthogonal edits (#2266, open); no measuring tool (#2287); no "snap face to face" (#859, open). TB precision is only the **Ctrl+Alt+M Move…** dialog and the rotate tool fields.
- **Convexity for us:** extrude/split = new brush (TB model). Inset/bevel must output several convex brushes. Vertex moves validate live (H++ red faces) and refuse the commit.

## 5. Clip & CSG

| | TB (verified) | H++ | NRC | Hammer |
|---|---|---|---|---|
| Activate | **C** | Clipper; **C** toggles 3-point/2-point | **Ctrl+m1** quick clipper (2D) | Shift+X **[UNVERIFIED this pass]** |
| Points | LMB click = point; LMB drag = 2 points; in 3D **only on existing faces**, snapped to the *face-projected grid* and glued to the face; orange preview sphere; 2 points = guessed plane, 3rd point = exact; drag points to edit; **Del** removes the last point | 3D handles, plane drawn, **cut part red** | **m1 x2 on a point = do clip** | 2D line |
| Face match | **Double-click a face** = clip plane = that face (exact plane points, no microleaks) | n/a | n/a | n/a |
| Keep side | **Ctrl+Return** cycles front/back/both | n/a | n/a | Click cycles |
| Apply/cancel | **Return** / Esc (removes the last point, exits when none are left) | n/a | n/a | n/a |

CSG in TB: **Ctrl+J** convex merge (hull, so it can fill voids), **Ctrl+K** subtract (the selection is subtracted from *all visible* brushes, so hide what you want excluded with **Ctrl+Alt+I**), **Ctrl+L** intersect, **Ctrl+Shift+K** hollow (wall = grid size). Resulting faces **inherit the material and attributes of a coplanar input face**, else the current material. H++: Merge (**Ctrl+Shift+M**) only when the result is convex, which is safer than a hull. NRC: "CSG Tool (aka shell modifier)", "uniform merge". **Pitfalls:** subtract fragments brushes into many slivers, off-grid vertices, hull-merge fills gaps, and hollow overlaps at corners (Hammer Carve wiki, first pass). Mitigations: a minimal-fragment subtract, snap the results, warn on slivers, merge only when convex (H++), and preview the result in red/ghost before committing.

## 6. Texturing (Valve 220)

- **TB, verified:** clicking a material in the browser applies it to the selected faces (or to all faces of selected brushes) and sets the *current material*. **Transfer from the selected face:** select the source with **Shift+click**, then on targets use **Alt** (project attributes), **Alt+Shift** (*rotate* the UV axes onto the target, Valve only: the clean "wrap"), or **Alt+Ctrl** (material only). Applied with **click** (one face), **drag** (each face continues from the last: *wrap painting*), or **double-click** (whole brush). **Ctrl+C / Ctrl+V** on faces copies attributes. **In the 3D view with faces selected:** arrows move the texture (**Shift** coarse, **Ctrl** fine), **PgUp/PgDn** rotate, **Ctrl+F / Ctrl+Alt+F** flip H/V, **Shift+R** reset, **Alt+Shift+R** reset to world-aligned, all camera-relative. UV editor panel: drag to offset (snaps to face vertices), drag grid lines to scale, drag the circle or **Ctrl+drag** to rotate (snaps to edges), **Alt+drag** to shear; plus Align (cycles edges), Justify ×4, Fit H/V (repeat clicks cycle integer repeats), rot ±90, and UV-grid subdivision for trim sheets. Spin-box deltas follow the grid (Shift = 2×). **Alignment lock** (pref) covers transforms; **UV Lock (U)** covers vertex/face edits.
- The first pass described an "Alt+LMB drag in 3D UV tool" for TB. **That does not exist in TB.** NRC has a "UV Tool" mode, and its 3D mouse map is: **m3** copies texture+alignment, **Shift+m3/drag** pastes name+alignment, **Ctrl+m3/drag** "paste texture **seamlessly** between brush faces", **Alt+m3** pastes name only, and **Ctrl+Shift+m3** projects from the copied face.
- J.A.C.K.: "NULL to Unselected" (quick backface caulk), "Apply (texture + values + axes)". H++: Face Edit edits UV vectors directly, and has randomize shift and apply nodraw.
- PB [PB4]: **Alt+1..0** apply a palette material; in the UV editor, **Ctrl+click** auto-stitches an adjacent face and **Ctrl+Shift+click** copies UVs. Auto UV fill is Fit/Tile/Stretch.
- Hammer Face Edit (Shift+A), Alt+RMB apply with wrap: from the first pass, **[UNVERIFIED this pass]**.

## 7. Feel

| Feature | Best verified example |
|---|---|
| Hover pre-highlight | TB yellow face outline under Shift; TB red handle outline + coordinate label in vertex tools |
| Live dims | TB bbox dimension labels + **corner spikes** on hover; TB rotate angle shown at the handle center |
| Trace line | TB draws the move path, which gets thicker when axis-locked |
| Undo | TB: unlimited; a whole drag = one step; **keyboard nudges within a time window merge**; selection/hide/lock are undoable as transactions |
| Repeat | TB **Ctrl+R** repeats the last command (e.g. duplicate+move = arrays); **Ctrl+Shift+R** clears |
| Camera | TB: RMB look, **wheel = forward** (optional "toward cursor"), **MMB pan**, keys **W/A/S/D, Q up, X down**, **RMB+wheel = fly speed**, **Alt+RMB orbit** about the clicked point (wheel = radius), **Ctrl+U** focus selection, **Shift+wheel** temporary zoom (FOV), **Ctrl+Alt+Z** reset. NRC: **Tab** focus, **Alt+m2** orbit, zoom to cursor. Unity: **F** frame, Alt+LMB orbit |
| Duplicate | TB **Ctrl+D** in place (white flash); **Ctrl+arrows/PgUp/PgDn** dup+move by grid; **Ctrl+drag** dup-drag; **Ctrl+Shift+D** linked duplicate (groups). The first pass's "Alt+drag" was wrong for TB |
| Rotate/flip | TB **Alt+Left/Right** yaw, **Alt+Up/Down** roll, **Alt+PgUp/PgDn** pitch (90°, or the tool angle when R is active); **Ctrl+F** flip on the camera-right axis, **Ctrl+Alt+F** flip on Z |
| Nudge | TB 3D: **Left/Right** = sideways, **Up/Down** = forward/back on the editing plane relative to the camera, **PgUp/PgDn** = ±Z; one grid step each |
| Paste | TB **Ctrl+V** pastes *under the cursor onto the surface*; **Ctrl+Alt+V** pastes at the original position. Entities dropped in 3D land on the brush under the cursor, bbox grid-snapped |

## 8. Complaints & praise (sourced)

- **TB praise:** quake mappers "use TB's 3D mode exclusively", treating 2D as "an afterthought" (Leadwerks thread). Practically everything is "faster in the 3D viewport", and "the 3D grid works fine and allows more freedom" (func_msgboard 62151). TB was designed as a 3D-only experiment (Quake Wiki).
- **TB complaints (issues):** no measure tool (#2287); no edge lengths on skewed edits (#2266); 3D grid noise hides texture patterns (#2973); no face-normal-based creation (#5182); no face-to-face snap (#859); primitives were missing for years (#398, #1270, now addressed by shape types + sweep); extrude edge cases (#3994); UV editor gets hard at small scales (#2808, #4286, #2747). Dense modifiers: the same key means different things per tool (Ctrl = toggle select / dup-drag / split-extrude / absolute snap). **[Reddit sources not reached this pass]**
- **PB:** mode switching moved into overlays and contexts in 5/6, so the Shape tool needs the Global handle for snapping; you must "reset the shape" after topology edits before parameters work again (5.2 doc). These are meshes, not convex brushes.
- **Hammer/H++:** H++ fixed the 3D view's passivity mainly through the **gizmo, 3D clipper, 3D grid, and realtime vertex validity**, not 3D block drawing. **NRC:** powerful but dense mouse chords, and m3 texture copy/paste is loved but obscure. **J.A.C.K.:** Hammer-paradigm improvements (vertex validity, texture-apply modes).
- PB is beginner-friendly because of its visible modes, gizmos, parametric shapes, numeric fields, and Shift-hover tooltips [PB4]. TB is fast because of modeless clicks, face-aware drawing, snap by default, single-key tools, camera-relative nudges, and Shift-hover resize without a tool switch.

## 9. Adapting to one 3D view (core design)

| Problem | Pattern (source) |
|---|---|
| Which plane to draw on | Raycast at mouse-down. Hit → that face's plane (optionally oriented to a slanted normal, #5182). Miss → world-horizontal plane at the **working height** (last brush base, else 0). **Lock the plane for the whole drag** (TB/PB). Show a grid patch on it |
| Height | Both: **Alt held** = height-only (TB/NRC) **and** PB two-phase (release, move, click). Default = last used height (TB uses grid size, which is a worse default) |
| Depth for moves | Drag on the plane of the picked face, else XY (TB). **Alt** = vertical, switchable mid-drag (TB). **Shift** = lock to the dominant axis so far, released to unlock (TB). Thicker trace line when locked (TB). The Draggers stay for explicit axes |
| Numeric entry mid-drag | Typing digits during any drag/draw sets the active dimension. Tab cycles X/Y/Z(/height), Enter commits, Backspace edits (Blender-style). TB lacks this (users rely on dialogs, #2287/#2266). PB's pre-draw X/Y/Z fields are the fallback |
| Precision readouts | Bbox dims + spikes (TB), **edge lengths of edited edges** (#2266), delta vector, angle at the pivot |
| Occlusion | Click-again tunnel cycling (NRC) + **Ctrl+wheel drill** (TB) + hide/isolate + Select Hidden for element modes (PB) |
| On-surface placement | Paste and drop onto the surface under the cursor (TB), "move to nearest floor/ceiling, align to normal" (H++), PB stamp on the hit mesh |
| Working-plane grid | World-aligned lines projected on hovered/selected faces + a cursor patch on the active plane, fade by distance, 3D-only toggle (#2973) |
| Camera conflicts | All edit gestures on LMB. RMB = fly; Alt+RMB = orbit (TB). Tool letters are ignored while RMB is held. No tool letter may collide with W/A/S/D/E/Q |

## 10. Prioritized feature set

**v1 must-haves**
1. Raycast plane-locked box draw, grid-snapped; **Alt** = height, **Shift** = square, **Shift+Alt** = cube; two-phase height as well; Esc cancels; remember the last height.
2. Grid pow2 0.125–1024: **+/-** and **1–9** direct; HUD readout; snap toggle; grid on faces + active-plane patch; viewport grid toggle.
3. Modeless selection: click = brush, Shift+click = face, Ctrl toggle, click-again tunnel + Ctrl+wheel drill, coplanar flood (Shift+Alt+dbl), Esc/void deselect, hide/isolate/show-all.
4. Shift-hover face resize along the normal (grid + vertex-plane snap); Ctrl at start = split/extrude a new brush (inward too); multi-brush identical faces.
5. Move: plane drag, **Alt vertical mid-drag**, **Shift dominant-axis lock**, trace line, bbox spikes + dimension labels.
6. **Numeric entry during drag/draw** (type, Tab, Enter) and a Move/Rotate-by dialog.
7. Vertex/edge/face tools: live convexity validation (red invalid), absolute/relative snap toggle, snap-to-vertex click, add vertex on grid, Del with validity check, coincident-vertex clumping.
8. Clip: points glued to face-projected grid, 2-point guess / 3-point exact, double-click face match, Ctrl+Return cycle side, red discard preview.
9. Duplicate Ctrl+D, Ctrl+drag dup-move, Ctrl+arrow dup-nudge, **Repeat (Ctrl+R)**; camera-relative arrows, PgUp/PgDn Z; 90° rotate and flip keys.
10. Texturing: browser click-apply, current material, Valve 220 face inspector, **Alt/Alt+Shift/Alt+Ctrl transfer with click/drag/double-click**, Ctrl+C/V attributes, 3D-view arrow texture nudges, reset/world-reset, Fit/Justify/Align, alignment lock + UV lock.
11. Undo: one step per drag, merged nudges, undoable selection/hide.
12. CSG: convex-only merge (H++) plus a hull merge option, subtract with minimal fragments, hollow (thickness = grid), intersect; coplanar material inheritance.

**v2**
- Parametric shapes with shared sides + circle modes (CZG scalable): cylinder, cone, arch, stairs, spheres, as re-editable brush groups. Sweep tool (arc/straight/S-bend, iterations = spiral stairs).
- Edge-length labels, measure tool, snap face-to-face (#859), creation on slanted face normals (#5182).
- UV editor panel (offset/scale/rotate/shear handles, trim-sheet subdivisions); NRC-style m3 "seamless paste".
- Element-mode marquee with Select Hidden, paint-select, select-by-material, grow/shrink.
- Normal-oriented handles (PB P), inset/bevel producing convex sets, H++-style move to floor/ceiling.

**Nice-to-have**
- Convex-hull brush tool (B-style points), PB Shift-stamp repeat, NULL-to-unselected caulk, linked duplicates, selection brushes (select touching/inside), sliver/leak checker.

### Recommended keymap (one 3D view, fly cam safe; tool letters ignored while RMB is held)

| Action | Key | Precedent |
|---|---|---|
| Fly | RMB + W/A/S/D, E/Q up/down, Shift fast, **RMB+wheel = speed** | TB uses Q/X for up/down; ours keeps engine E/Q |
| Orbit / pan / frame | **Alt+RMB** orbit about the clicked point; MMB pan; **F** frame selection for any instance (Unity); no Ctrl+U alias | TB, Unity |
| Select / toggle / drill | LMB / **Ctrl+LMB**; click again = tunnel cycle; **Ctrl+wheel** drill; Esc or click void = deselect; Ctrl+A all, Ctrl+Shift+A none, Ctrl+Alt+A invert | TB, NRC |
| Face select | **Shift+LMB**; Ctrl+Shift+LMB add; Shift+dbl = all faces; Shift+Alt+dbl = coplanar flood; Ctrl+Shift+drag = paint | TB |
| Tools | **B** brush/hull, **C** clip, **V** vertex, **G** edge (TB uses E, which is our fly-down), **H** face (TB uses F, which we keep for frame), **R** rotate, **T** scale, **Y** sweep; **Shift+Esc** exit tool | TB (E remapped) |
| Draw modifiers | **Alt** = height only, **Shift** = square, **Shift+Alt** = cube; type digits = numeric size | TB, NRC |
| Move modifiers | **Alt** = vertical (mid-drag OK), **Shift** = dominant-axis lock, **Ctrl at drag start** = duplicate | TB |
| Face resize / split | **Shift+hover** to highlight, LMB-drag = resize; **Ctrl at drag start** = new brush / split inward | TB |
| Grid | **+ / -** size, **1–9** = 1…256, **0** show grid, **Alt+0** snap toggle, Ctrl+Alt+wheel size | TB |
| Nudge | Arrows (camera-relative on plane), **PgUp/PgDn** Z, one grid step; **Ctrl+** these = duplicate+nudge | TB |
| Duplicate / repeat | **Ctrl+D**; **Ctrl+R** repeat last; Ctrl+Shift+R clear | TB |
| Rotate / flip | **Alt+Left/Right** yaw, **Alt+PgUp/PgDn** pitch, **Alt+Up/Down** roll (90° or the rotate-tool angle); **Ctrl+F** flip on camera-right, **Ctrl+Alt+F** flip Z | TB |
| CSG | **Ctrl+J** merge, **Ctrl+K** subtract, **Ctrl+L** intersect, **Ctrl+Shift+K** hollow | TB |
| Hide | **Ctrl+Alt+I** hide, **Ctrl+I** isolate, **Ctrl+Shift+I** show all | TB |
| Texture | Alt/Alt+Shift/Alt+Ctrl + click/drag/dbl = transfer; Ctrl+C/V face attributes; arrows/PgUp/PgDn (Shift coarse, Ctrl fine) nudge/rotate texture when faces are selected; **Shift+R** reset, **Alt+Shift+R** world reset; **U** UV lock | TB |
| Clip | Return apply, **Ctrl+Return** cycle side, Del removes the last point, double-click face = match | TB |
| Vertex tools | Ctrl mid-drag = absolute snap, **Shift+Alt+click** = snap to vertex, Shift+hover = add vertex, Del = delete | TB |
| Apply / cancel | Return / Esc everywhere; Del removes | TB |

Keymap notes (resolving the first pass's unfinished rows): (1) Rotation uses TB's **Alt+arrows/PgUp/PgDn**, which collides with nothing in our fly scheme since arrows are not fly keys. The first pass's "Z/X" proposal is dropped. (2) Face selection is settled as **Shift+LMB**, with Ctrl for additive. (3) **F** conflicts between "frame" (Unity habit) and "face tool" (TB). Recommendation: **F = frame**, face tool = **H** ("handle faces"). Edge tool = **G** because E is fly-down. TB users can rebind. (4) Unity's QWER gizmo keys are not adopted because W/E/Q are fly keys. H++'s RMB-click-to-cycle gizmo mode does not transfer (RMB = fly), so gizmo modes use the toolbar plus **R** (rotate) / **T** (scale), with move as the default. (5) TB's Shear (G) is demoted to the toolbar because G is taken by the edge tool.
