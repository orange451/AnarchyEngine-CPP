# Dragger Phase 4: Drawing the Handles

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every scene view draws each active Dragger's handles over the scene, sized for that view, colored by axis, with hover and drag states.

**Architecture:** The render snapshot carries one `VisualDragger` row per active Dragger (frame, hovered, active), rebuilt at every `take_changes`. `DraggerMath::handle_mesh` turns a row into colored world-space triangles for a view, with the same size and visibility rules picking uses. GameView builds the mesh for its own camera and size and hands it to `Renderer::setHandles`; `handlePass` draws it after the outline pass, depth test off, straight alpha.

**Tech Stack:** C++17, OpenGL 3.3 core, GLSL 330.

**Spec:** `docs/superpowers/specs/2026-10-03-dragger-design.md` (section 6, and handle_mesh in section 3)

## Global Constraints

- A GL error is fatal in the studio: the new pass must leave no GL error (checked by `scene-render-check`).
- The pass restores the array buffer binding it changes, as the outline pass does.
- Colors: X (0.90, 0.20, 0.20), Y (0.30, 0.85, 0.30), Z (0.25, 0.45, 0.95), opaque; a plane square takes its normal axis's color at alpha 0.4. Hovered mixes 40 percent toward white. While a drag runs, the active handle is (1.0, 0.85, 0.2) and every other handle's alpha is multiplied by 0.35.
- Shafts are 3 px wide camera-facing quads to 80 percent of the arrow; cones have 8 sides, base radius 6 px, tip at the arrow's end.

## Review Focus

- A Dragger behind the camera or past the far plane: no mesh, no GL error, no NaN vertices.
- A view whose camera is not the Dragger world's CurrentCamera: it still draws, sized for its own camera.
- Zero draggers: the pass does not run (no empty draw).

---

### Task 1: Snapshot rows and handle_mesh

**Files:** `src/engine_core/SnapshotPump.{hpp,cpp}` (`VisualDragger`, rows built in `take_changes`, copied in `blit`), `src/engine_core/DraggerMath.{hpp,cpp}` (`HandleVertex`, `handle_mesh`), `sandbox/dragger_tests.cpp`.

- [ ] RD1: an active Dragger has a row whose origin is the target's position, with the hovered and active handles; a Dragger under a Folder has none. RD3: `handle_mesh` for a frame in view gives triangles (a multiple of 3 vertices) containing red, green, and blue; a hidden arrow (pointing at the camera) gives no vertices of its color; a frame behind the camera gives none at all; a dragging state yellows only the active handle.
- [ ] Implement; run; commit.

### Task 2: Renderer pass and GameView

**Files:** `resources/shaders/pipeline/handle.{vert,frag}`, `src/runner/Renderer.{hpp,cpp}` (`setHandles`, `handlePass`, program, VAO/VBO), `src/runner/GameView.{hpp,cpp}` (remember the followed camera; build the mesh), `tests/SceneRenderCheck.cpp` (RD2).

- [ ] RD2: with a camera at the origin looking down -Z and a Dragger frame at (0, 0, -10), `setHandles(handle_mesh(...))` then `draw` gives a red pixel on the X shaft, green on the Y shaft, and no GL error.
- [ ] Implement; run `./build/scene-render-check`, the sandbox, and ctest; commit.
