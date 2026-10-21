# Shader Editor and vertex stages

## Intent

A Shader is more than one text file. Opening a Shader should open a home for
it, the way a Prefab opens the Prefab Editor: a docked window that shows what
the Shader looks like, whether it builds, what it declares, and who uses it,
and from which each of its two stages opens in the text editor. The two
stages are new: alongside today's per-pixel `surface()`, a Shader can have a
vertex stage that moves a mesh's vertices on the GPU every frame. That is
what an ocean needs: real waves in the geometry, not only in the lighting.

The motivating case is the ocean: a dense plane mesh whose vertices the
Shader lifts into waves every frame, shadows that follow the waves, and a
fragment stage that adds foam, fine ripples, and scattering.

## Goals

- Double-clicking a Shader, or Edit on it, docks a Shader Editor beside the
  Scene View. The text editor opens from it, never directly.
- A Shader holds a vertex text and a fragment text. Either opens in the
  shader text editor from the Shader Editor.
- A surface Shader's vertex stage displaces each vertex of every mesh,
  Brush face, and card drawn with a Material using it, every frame, in the
  G-buffer, see-through, and shadow passes.
- Kind and render modes are Shader properties, set from the editor.
- Existing Shaders, projects, and scripts keep working.

## Non-goals

- A node graph. Shaders stay text.
- Post shader vertex stages. A post Shader has only a fragment stage.
- Tessellation, geometry shaders, or compute.
- Generating the ocean mesh. A dense plane comes from the existing
  `MeshEditor:AddPlane` or a Brush; the ocean demo builds one.
- Terrain drawn through a Shader (still Later, as in the custom shaders spec).

## Anti-goals

- Opening a Shader drops you straight into a text editor (the behavior this
  replaces).
- A Shader without a vertex stage costs anything new per vertex or per frame.
- The preview, or the editor being open, slows the Scene View.

## The Shader Editor (layout A)

Double-click a Shader in Assets or the explorer, or pick Edit, and a Shader
Editor docks beside the Scene View, its tab named after the Shader. Each
Shader has one Shader Editor at most; editing it again brings its tab
forward. Like Prefab editors, Shader Editors are not kept in the saved layout.

- **Header:** the Shader's name, its Kind (Surface or Post, a dropdown), and
  its build state: Built, or the first error. On the right, the preview's
  mesh: Sphere, Cube, Plane, or Mesh… (a picker of the place's Meshes).
- **Left column, Stages:** a Vertex row and a Fragment row. Each shows that
  stage's state (built, its line count, or `✕ line N` with the message as a
  tooltip) and an Edit button that opens that stage in the shader text
  editor. A post Shader shows only Fragment. A surface Shader with no vertex
  text shows Vertex as "none" and its button reads Add vertex stage, which
  fills in the starter vertex text and opens it.
- **Left column, Render modes:** toggles for transparent, two-sided, and no
  shadow (surface only). Each toggle is one undo step.
- **Middle, preview:** a large live render of the chosen mesh drawn with the
  Shader, using its uniforms' defaults, turned by dragging. It animates, as
  `TIME` moves. It is lit as the material ball is lit; a shader that fails
  to build shows the plain material and the error stays in the header.
- **Right column, Uniforms:** each uniform the Shader declares, in order,
  with its type and default, grouped under its `group`. Read-only here: the
  values are set per Material in Properties.
- **Right column, Used by:** each Material whose Shader is this one. Clicking
  one selects it, so Properties shows its uniform rows.

## The shader text editor

The existing shader text editor (phase 1) now edits one stage. Its tab is
named `<Name>.vertex.glsl` or `<Name>.fragment.glsl`; each stage has one tab
at most. Typing writes that stage's source after the same pause, as one undo
step. Its error marking shows that stage's errors. Problems lists each
stage's error under the Shader, and opening one opens that stage's text
editor at the line.

## The Shader asset

- `VertexSource` and `FragmentSource`, strings, saved. `Source` stays as an
  alias of `FragmentSource` for scripts and MCP written against phase 1.
- `Kind` (`Enum.ShaderKind`), now saved and settable, Surface by default.
  Setting it is a plain property write: nothing else changes. A post Shader
  ignores its vertex text. A Material already holding it keeps the reference
  and draws plain (as `Material::shader` already returns null for a post
  Shader); a new pick still refuses it.
- `Transparent`, `TwoSided`, `CastsShadow` (true by default): booleans,
  saved, surface only. These replace `render_mode`.
- `DisplacementMargin`, units, 1 by default, not below 0, surface only: how
  far the vertex stage may move a vertex. Culling grows each draw's box by it.
- `Error`, read-only: the first error of either stage, `"<stage> <line>:
  <message>"`, e.g. `"vertex 12: 'wave' undeclared"`. Each stage's own
  error is readable too (`VertexError`, `FragmentError`).
- Files: `Ocean.vertex.glsl` and `Ocean.fragment.glsl` beside
  `Ocean.meta.json`; a stage with empty text writes no file.

### Reading phase 1 files

A Shader saved by phase 1 (one `Ocean.glsl`) loads its text as the fragment
stage. Its `shader surface;` / `shader post;` line sets Kind and its
`render_mode` line sets the three booleans, and both lines are then taken out
of the text (as spaces, so line numbers stay). The next save writes the new
files.

A script or MCP writing `Source` (or `FragmentSource`) with those lines is
read the same way, as one undo step: a `shader` line sets Kind, a
`render_mode` line sets all three booleans (a mode not named is off), and a
text with no `render_mode` line leaves them as they are. Reading `Source`
gives the stored text, without the lines.

## The language

- The fragment stage is phase 1's language minus the header and
  `render_mode`: uniforms with hints, `group`, helper functions, and
  `void surface()` or `void post()`.
- The vertex stage is the same language with `void vertex()`. Its
  built-ins: `VERTEX` (vec3, world space, writable), `NORMAL` (vec3, world
  space, writable), `UV`, `COLOR`, `TIME`, and the uniforms. `vertex()` runs
  after skinning and the instance's world matrix, before the view.
- Uniforms: either stage may declare them; the Shader's uniforms are the two
  lists merged, fragment first, then the vertex stage's that the fragment
  does not have. A uniform declared in both must have the same type, hints,
  and default, or both stages report "<Name> is declared differently in the
  vertex stage". Materials, Properties rows, and attributes work as now.
- The starter vertex text: a `vertex()` that leaves `VERTEX` as it is, with
  a comment showing a wave.

## Rendering

- A surface Shader with vertex text builds its G-buffer, see-through, and
  shadow programs with the vertex stage spliced into `geometry.vert` and
  `shadow.vert`, as `surface_shader.glsl` is spliced into the fragment
  programs today. Errors map to the stage's own lines.
- Its shadow program always exists (the shape changed), whether or not the
  fragment stage cuts holes; shadows redraw every frame when the vertex
  stage names `TIME`, as cutting shaders do today.
- Culling tests a vertex-stage draw against its box grown by the Shader's
  `DisplacementMargin` on every side. LOD and card bands still go by distance
  to the instance's origin, as today, so a forest with a wind Shader keeps
  its LODs and cards.
- Cards: a `LodLayer` card whose own Material has a vertex stage runs it on
  the card's corners after the card turns to the camera. A Prefab's card
  bake renders the base Models through their Shaders as they are at bake
  time; the cards do not move with them afterwards.
- The selection outline, hover, picking, and the placement depth probe use
  the mesh's rest shape in this version.
- `CastsShadow` false is today's `no_shadow`; `TwoSided` and `Transparent`
  are today's render modes.

## The ocean

Delivered with the feature, as a Shader and a demo project, not engine code:

- **Ocean** Shader. Vertex: the summed-wave field of afl_ext's "Very fast
  procedural ocean" (MIT; credited in the source), with drag, lifting
  `VERTEX.y` and setting `NORMAL` from the wave field. Fragment: finer wave
  octaves as normal detail, faded with distance; deep and scattering colors
  lifted at wave crests; foam at the steepest crests; low roughness, high
  reflectivity, so the engine's Fresnel, sky, and reflections do the rest.
  Uniforms for wave height, scale, choppiness, speed, detail, colors, foam.
- **Ocean** demo project, in the user's Anarchy Engine Projects folder like
  Shader Playground (not in this repository): a dense plane with the Ocean
  Material, a DynamicSky, a camera near the water looking toward the horizon,
  and a few rocks. The plane is built by `MeshEditor:AddPlane(width, depth,
  position?, segments?)`, which gains `segments` (default 1, quads per side):
  256 by 256 quads, about 66,000 vertices, over 400 units.

## Tests

- A phase 1 single-file Shader with `shader post;` and `render_mode` lines
  loads as Kind Post with the modes set, and saves as the new files.
- A vertex stage that lifts `VERTEX.y` by 1 draws a cube 1 unit higher, in
  the G-buffer and in its shadow.
- A vertex stage error reports `vertex <line>` at the user's line.
- A uniform declared differently in the two stages errors in both.
- A draw displaced within its `DisplacementMargin` outside its mesh's box is
  not culled; LOD bands are unchanged by a vertex stage.
- `AddPlane` with segments makes (segments+1)² vertices.
- Writing `Source` with a `shader post;` line sets Kind Post as one undo
  step; reading it back has no header line.
- The Shader Editor: double-click opens it, not a text tab; Edit vertex
  opens `<Name>.vertex.glsl`; a toggle sets the property as one undo step;
  Used by lists the Materials and selects one.
- A Shader without vertex text builds exactly the programs it does today.

## Calls I made

- Kind and render modes become properties set from the editor; the text
  lines still parse, so nothing breaks (the user agreed).
- `VERTEX` and `NORMAL` are world space, so waves line up across meshes and
  instances.
- Culling grows a vertex-stage draw's box by a per-Shader
  `DisplacementMargin` instead of not culling, so forests with wind keep
  their culling, LODs, and cards.
- Card bakes catch the displaced shape at bake time; live cards move only
  through their own LodLayer Material.
- Outline, hover, picking, and the depth probe stay on the rest shape.
- `Source` writes with header lines keep working; a missing `render_mode`
  line leaves the modes alone.
- `AddPlane` gains `segments`, the one engine API addition the ocean needs.
- The preview uses the uniforms' defaults, not a chosen Material's values.

## Left to the builder

- How the preview renders (the material ball's renderer, or a pane like the
  ViewportPane's) and its size.
- How the two stages' text is split when compiling, so one stage's function
  never reaches the other's program.
- The editor's widgets, styling, and empty states.
