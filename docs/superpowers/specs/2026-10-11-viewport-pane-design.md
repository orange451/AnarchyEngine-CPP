# ViewportPane Design

2026-10-11 · A GUI pane that draws its own little 3D scene, as Roblox's ViewportFrame does: the GameObjects, lights, and Skybox inside it, seen from its CurrentCamera, rendered off screen and shown in the UI, and rendered again only when something it shows changes.

## Goal

A `ViewportPane` sits in a ScreenGui, a BillboardGui, or a plugin DockWidget like any pane. The 3D instances inside it are a world of their own: they are drawn only in the pane, lit only by the pane's own lights and Ambient, and never touch the place's world, physics, lighting, or Scene View. The pane renders that world into a texture, shows it with any GUI children drawn over it, and keeps showing the same texture until something it draws changes.

What it is for, and what sets the budget:

- **Item and inventory icons**: dozens of small panes at once, mostly still. Each renders once and then costs nothing but a textured quad per frame.
- **Model previews in menus**: one or two larger panes, often spun by a script or animated, so re-rendered every frame.
- **Studio plugin UIs**: previews inside plugin DockWidgets, such as a model browser.

It must work on low-end hardware: fifty icons appearing at once must not hitch the frame, and fifty idle icons must cost no 3D rendering at all.

## Decisions

| Question | Decision |
| --- | --- |
| Approach | Pane scenes are built by the SnapshotPump into the VisualSnapshot, under the lock it already holds. Each GuiTree host (a Scene View, the player, a plugin pane) owns one `ViewportRenderer` that renders dirty panes into their own textures, and the pane's JadeFX node composites its texture with a new `Painter::drawTexture`. No readback to the CPU. |
| Class | `ViewportPane : GuiBasePane`, a sibling of ImagePane. CSS element type `viewportpane`. GUI children are laid out and drawn over the 3D image, as a StackPane's. |
| New properties | `CurrentCamera: Camera?` (nil), saved by GUID. `Ambient: Color3`, Lighting's default ambient (0.5, 0.5, 0.5). `ImageTransparency` (0), the existing slot, is the 3D image's transparency. |
| Camera | `CurrentCamera` may name any live Camera; its Transform is read as a position in the pane's world. When it is nil or names nothing, the pane uses the first Camera inside it in tree order. With no Camera at all the pane draws only its background and GUI children. |
| Lighting | Only what is inside the pane: its PointLights, SpotLights, and DirectionalLights, its Skybox, and its Ambient. No built-in light. Tone settings (Exposure, Saturation, Gamma, tone mapping) are a new Lighting's defaults, never the place's, so editing Lighting re-renders no pane. |
| Background | The renderer clears to transparent. The pane's own BackgroundColor and BackgroundTransparency draw under the 3D image, so an icon with BackgroundTransparency 1 sits on whatever is behind it. A Skybox fills the background where nothing is drawn. |
| Effects | No shadows, SSR, ambient occlusion, or bloom. FXAA stays on and smooths silhouettes against the transparent background too. |
| Contents that work | GameObjects (skinned ones included), Folders, Cameras, PointLight, SpotLight, DirectionalLight, a Skybox, Scripts (which run as anywhere under Gui), and GUI children. |
| Animation | Animators inside a pane step in play, as Animators in Workspace do, and a playing animation re-renders its pane every frame. |
| Inert inside a pane | Terrain, physics bodies (PhysicsObject, PlayerController, Brush), constraints, Velocity integration, Draggers, SoundEmitters, WireframeAdornments, BillboardGuis, and DynamicSky. Each is allowed and does nothing there. |
| Scope wall | Contents of a pane are never in Workspace or Lighting, wherever the pane is, so nothing inside leaks into the world, its physics, its lighting, or the Scene View. |
| When it re-renders | When anything it draws changes: a property of the pane or of anything inside, the tree inside it, its camera, a referenced Prefab, Model, Mesh, Material, or Texture, its Skybox, an animated pose, its pixel size, or a texture finishing streaming. Never otherwise. |
| Budget | Each host renders, per frame, dirty panes that were on screen the frame before, oldest change first, until 8 panes or 1 megapixel, always at least one. Panes not yet re-rendered keep showing their last image. |
| Resolution | The pane's size in device pixels, rounded up to a multiple of 8, at most 1024 a side, and scaled to the pane when composited. |
| Many Scene Views | Each Scene View renders its own copy of each pane, since each window has its own GL context. Accepted for v1. |

## 1. The class (`engine_instances/Gui.hpp`, `Gui.cpp`)

`ViewportPane` derives from `GuiBasePane`, so it has the pane properties (Size, BackgroundColor, BackgroundTransparency, Alignment, Visible, MouseTransparent, ClassList, Style), the mouse events, and the children a Pane takes. Its default Size is 100 by 100, as Pane's and ImagePane's.

`CurrentCamera` is an `InstanceRef`, read and written through `instance_reference_slot` and `set_instance_reference`, the way ImagePane's `Image` is. It is registered as `lua_saved_property("CurrentCamera", "Camera?", ..., "null")`, so save, load, undo, Stop restore, the Properties reference row, and paste remapping all come with it: copying a pane together with its Camera points the copy at the copied Camera. A value that is not a Camera is refused with `CurrentCamera must be a Camera`. A change calls `touch()`, and `on_reuse` clears it. `current_camera_id()` resolves without copying strings, as `BillboardGui::adornee_id` does, and caches a miss until `tree_revision` moves, so a dangling GUID does not scan every slot each frame.

`Ambient` is a plain member registered with `lua_saved_property`, not a new `GuiProperty` slot, since every GUI instance carries a full slot array. Its setter records the change with `note_property_change` and calls `touch()`. `ImageTransparency` reuses the existing slot, registered for ViewportPane.

```cpp
class ViewportPane : public GuiBasePane {
public:
    LuaSlot current_camera() const;
    std::optional<std::string> set_current_camera(const LuaSlot& value);
    // CurrentCamera's id when it names a live Camera, else 0.
    InstanceId current_camera_id() const;
    ColorRgb ambient() const;
    std::optional<std::string> set_ambient(ColorRgb color);
    bool viewport_pane() const override { return true; }
    ...
};
```

`viewport_pane()` is a new class flag beside `billboard_gui()`. It tags the entity with `ecs::ViewportTag`, and a cached query with a `DataModel::viewport_panes(out)` lister, modelled on `billboards`, lets the pump find every pane without walking the tree.

Registration everywhere a class is listed: `Project.cpp`'s class registry (loading, and so the player), `register_lua_creatable` in `ScriptBindings.cpp` (Instance.new, paste, Insert, completion, MCP), `ClassOrder.hpp` (rank 12 with the panes), an Insert description ("Shows the 3D objects inside it, seen from its CurrentCamera."), the icon (`IconFileOverride` to the shipped Fugue `monitor-window-3d.png`), `LuaApi.cpp` member docs, the Gui.hpp header comment and element type list, `engine_instances/README.md`, and the Screen GUIs section of `README.md`.

## 2. Containment

- A Skybox may be in Lighting or in a ViewportPane (Folders between pass the rule up, as for Lighting). The refusal reads `Skybox must be in Lighting or a ViewportPane`. DynamicSky, BloomEffect, ScreenSpaceReflections, and AmbientOcclusionEffect stay Lighting-only.
- Insert ranking: `ViewportPane` is added to the suited parents of GameObject, Camera, PointLight and SpotLight, DirectionalLight, Skybox, and Folder, so Insert on a pane offers them beside the GUI classes.
- Nothing else is refused inside a pane. The inert classes are allowed, as Parts are allowed in a Roblox ViewportFrame, and the docs say they do nothing there.

## 3. The scope wall (`engine_core/DataModel.cpp`, `Ecs.hpp`)

Scope tags today pass through GUI instances, so a GameObject in a pane under a BillboardGui in Workspace would be drawn in the world and simulated, one in a plugin DockWidget (Core) would be drawn in every Scene View, and a light in a pane under Lighting would light the place. A pane is therefore a wall.

A new ECS scope tag, `InViewport`, is set by `refresh_scope` and `apply_scope` on every instance whose parent is a ViewportPane or is itself `InViewport`. Below the wall `InWorkspace` and `InLighting` are false. `InGame` and `InCore` pass through unchanged, because `core_holds` (save, history, Core placement) and plugin scripts depend on `InCore`, and `runs_here` decides scripts by walking, not by tags. `apply_scope`'s unchanged-tags prune compares `InViewport` too, and an `InViewport` change notes `VisualField::Ancestry` as a Workspace, Lighting, or Core change does. `clear_hierarchy` clears it. `in_viewport()` reads the tag in O(1).

Because `InWorkspace` stops at the wall, the Workspace-keyed queries skip pane contents with no change: physics bodies, terrain, constraints and assemblies, Brush visuals, and the Workspace render query. The rest need one explicit term each:

| Where | Change |
| --- | --- |
| `core_render_query` | `.without<InViewport>()` |
| `SnapshotPump::has_row` | the `in_core` branch also requires `!in_viewport` |
| `physics_query` (Velocity integration) | `.without<InViewport>()` |
| `dragger_query`, `source_query` (SoundEmitter) | `.without<InViewport>()` |
| `WireframeAdornment::drawn` | `&& !in_viewport()` |
| `SnapshotPump::resync` DirectionalLight and light walk | stops at a ViewportPane |
| `animator_query` | a second query, Animator `InViewport`, stepped by `step_animators` beside the Workspace one, in play only |
| GameView camera list walk | does not descend into a ViewportPane |
| `GameView::frameSelection` (F) | skips `in_viewport` instances |
| `Workspace.CurrentCamera` | refuses a Camera inside a ViewportPane: `CurrentCamera cannot be a Camera inside a ViewportPane` |

## 4. Pane scenes in the snapshot (`engine_core/SnapshotPump`)

```cpp
// One ViewportPane's world, as drawn: immutable once published, and shared
// between frames until something it draws changes.
struct VisualViewport {
    InstanceId pane = 0;            // with its generation
    std::uint64_t revision = 0;     // moves whenever anything below changes
    bool has_camera = false;
    Matrix4 camera;                 // the Camera's Transform, in the pane's world
    double field_of_view = 70.0;
    ColorRgb ambient;
    std::vector<VisualInstance> rows;   // GameObjects and lights
    std::vector<std::shared_ptr<const VisualPrefab>> prefabs;  // rows' prefab indexes point here
    std::optional<VisualSky> sky;   // the first Skybox inside, in tree order
};

struct VisualSnapshot {
    ...
    // Every ViewportPane in the game, by pane id.
    std::vector<std::shared_ptr<const VisualViewport>> viewports;
};
```

`viewports` is copied by pointer in both hand-written copy sites, `SnapshotPump::blit` and `SceneFeed::perform`. Pane rows never go in `instances`, so the Scene View never draws or lights with them. Prefabs are the pane's own `shared_ptr` copies, never indexes into the frame's `prefabs`, which are reused between frames.

`take_changes` gains `resolve_viewports(game)`, after `resolve_poses`, under the Prepare lock. It keeps one entry per pane: the published `VisualViewport`, the pane's last revision, the resolved camera id and value, the sky value, the member list, and the revisions of the prefabs and poses it used. A pane is rebuilt only when it is dirty, and it is dirty when:

1. **An invalidation names something inside it.** In the existing destructive drain, each invalidation whose id is `in_viewport` walks up to its owning pane (the nearest ViewportPane) and marks it. The ids are then dropped as today, since they have no main-scene row.
2. **`tree_revision` moved.** Every pane's member list is walked again and compared; a pane whose list differs is dirty. This catches contents arriving, leaving, moving between panes, and being destroyed or revived, none of which are seen by `on_parent_changed`.
3. **Its own `revision()` moved**: CurrentCamera, Ambient, ImageTransparency, or any pane property.
4. **A value it reads from outside changed.** Each Prepare compares, per pane: the resolved camera's id, Transform, and FieldOfView (the camera may be outside the pane); the Skybox's `VisualSky`; each used prefab's content revision; and each skinned row's `Pose` revision.
5. **A resync or Stop** marks every pane dirty.

Prefab content changes push no invalidation today; the pump re-reads every prefab each Prepare. The per-Prefab body of `resolve_prefabs` is factored into `fill_visual_prefab(game, guid, cached_id, VisualPrefab&)`, which compares before it assigns and reports whether anything changed. The pane side keeps its own GUID-keyed, reference-counted prefab table, resolved each Prepare as the main one is, and bumps an entry's content revision when it changed.

A rebuild walks the pane's subtree once, with reusable buffers so a quiet Prepare allocates nothing: GameObject rows through the same field readers as `apply_live`, light rows through `light_of`, poses through `GameObject::pose()`, and the sky through `find_first<Skybox>(game, pane)`. The new `VisualViewport` gets the next revision and replaces the old pointer. An idle pane costs a handful of comparisons per Prepare and no allocation.

## 5. JadeFX: `Painter::drawTexture` (sibling repo, first)

```cpp
enum class TextureAlpha { Straight, Premultiplied };
// Draws an app-owned GL 2D texture over x, y, w, h (points), clipped and
// occluded as any image: flipV for a texture whose row 0 is its bottom, as a
// framebuffer's is. opacity scales color and alpha alike when Premultiplied.
void drawTexture(unsigned texture, float x, float y, float w, float h, float opacity,
                 TextureAlpha alpha, bool flipV);
```

`UiRenderer::drawTexture` is `drawImage` without the upload: the same program choice (`image_` or, inside an occluder, `imageOccluded_`), uniforms, scissor clip, and quad. Premultiplied blends `ONE, ONE_MINUS_SRC_ALPHA`, and `image.frag` gains a uniform that scales RGB by opacity as well as alpha. No new shader file. It leaves texture unit 7 and `GL_TEXTURE0` as the occluder contract requires. JadeFX tests cover straight and premultiplied draws, opacity, flipping, and clipping. It lands and builds in JadeFX before the engine uses it, as SubScene did.

## 6. Renderer changes (`runner/Renderer`)

**Transparent output.** `setTransparentOutput(bool)`, off by default and off for the Scene View. On, the target is cleared to alpha 0, the final composite blends alpha `ONE, ONE_MINUS_SRC_ALPHA` instead of `ZERO, ONE`, and the output is premultiplied, which merge and the tone map already produce. FXAA filters alpha with the same weights as color, and its edge test also sees alpha, so a dark silhouette on a transparent background is smoothed as a light one is. A Skybox makes every pixel opaque, as now.

**Pooled targets.** The size-dependent targets (G-buffer, accumulation, transparency, merge, LDR, and the lazy bloom, SSR, and AO buffers) move into a `TargetSet`. `ensureTargets` keeps the current set on an exact size match, swaps in a parked set of that size if one exists, and otherwise makes a new one, parking the old. Parked sets are evicted least-recently-used while their total exceeds 64 MiB. One Renderer can then draw panes of several sizes in turn without rebuilding, and on macOS a new size fails `CanDraw` for one frame instead of every frame. The Scene View's Renderer has one size and behaves as today.

**Sky cache.** The Renderer keeps up to three `EnvironmentMap`s keyed by sky image revision, least-recently-used, so panes with different Skyboxes do not refilter cubes on every switch. The Scene View, with one sky, behaves as today.

**Shared scene translation.** The parts of `GameView::collectMeshes` that turn snapshot data into draws move to free functions in `runner/SceneDraws.{hpp,cpp}`: prefab templates from `VisualPrefab`s and a `MeshCache`/`TextureCache`, instance rows to `MeshDraw`s with slots, tint, scale, and poses, `VisualLight` to `LightDraw`, and `VisualSky` to `SceneSky`. GameView and ViewportRenderer both call them, so a GameObject draws the same in a pane as in the world.

## 7. Drawing panes (`runner/ViewportRenderer`, `runner/GuiTree`)

**The node.** `GuiTree::makeNode` makes `GuiNode<ViewportNode>` for `ViewportPane`, element type `viewportpane`; `IsPaneClass` includes it, so background color and transparency apply. `ViewportNode` is a `jadefx::StackPane`. `GuiTree::build` hands it the pane's `VisualViewport` from the held snapshot, found by pane id, and passes the same pointer to the host's ViewportRenderer, so the renderer always has the latest scene. GuiTree's hosts give it the snapshot's `viewports` in `sync`: GameView from `frameSnapshot_`, as it does for billboards, and PluginWidgetPane from `Runner::feed()`. When it renders, the node draws its background, then its texture with `drawTexture` (premultiplied, flipped, at the node's opacity times `1 - ImageTransparency`), then its children, and tells the renderer it was shown and at what pixel size. The node owns no GL.

**The renderer.** `ViewportRenderer` is owned by each GuiTree host: each GameView (Scene Views and the player) and each PluginWidgetPane. It holds one `Renderer` used only for panes, never the Scene View's, whose depth texture is the billboard occluder. It shares the host GameView's `MeshCache` and `TextureCache` (same context), so an asset used by the world and a pane is uploaded once. A PluginWidgetPane owns its own pair, rooted at the project's `resources_root()`. Each pane entry holds an FBO and an RGBA8 color texture, linear-filtered, at the rounded pixel size, plus the revision and size it last drew and whether that draw was complete.

The host calls `renderPending()` in its `renderContent` before painting children (GameView after its 3D draw, PluginWidgetPane before its tree), with the context current and no clip or occluder active. It renders entries that were shown last frame and are dirty (a new revision, a new pixel size, or an incomplete last draw), oldest change first, within the budget. For each it sets the camera, `SceneLighting{}` with the pane's Ambient and sky, transparent output, shadows off, binds the pane's FBO, sets the viewport, disables scissor, draws, and restores, as MaterialBall does. A draw is complete only when `draw()` returned true, the TextureCache is not `loading()`, and the sky's cubes were ready. An incomplete draw is shown anyway and tried again on the next frame, so a pane never freezes blurry or empty.

Entries not shown for 60 frames are deleted in paint. `release()` deletes everything from the host's `sceneChanged`, while the old context is current and not tearing down, as GameView releases its own. Destructors touch no GL. Pane renders sit under their own profiler scope, "Viewport panes", inside "GUIs and overlays".

**ViewportSize.** When the pane's camera is inside the pane, the node posts `Camera::set_viewport_size` with its size in points through `Engine::on_simulation` when the size changes, so `Camera:ViewportPointToRay` and `WorldToViewportPoint` work for clicks in a preview.

## 8. Studio notes

- Pane contents get no selection outlines, and F does not fly the Scene View camera into a pane's world. The Move tool still moves a selected GameObject inside a pane (its handles draw in the Scene View at the pane-world position); the pane re-renders to show the move. Editing in the pane itself is later work.
- The MCP Scene View capture reads the 3D image before the GUI is drawn, so it never shows a pane. Panes are checked with the demo below.

## Tests

**Sandbox (engine):**

- The class: base, defaults, a fresh pane saves nothing, `CurrentCamera` and `Ambient` save and load, Lua writes, a non-Camera refused, undo, Stop restore, a reference loaded before its Camera, and copy remapping a pane with its Camera.
- Containment: a Skybox in a pane, and through a Folder in a pane; DynamicSky in a pane refused; the Skybox message.
- The scope wall: under a ScreenGui, a BillboardGui in Workspace, a DockWidget in Core, and Lighting, pane contents are `InViewport`, not `InWorkspace` or `InLighting`, keep `InGame` and `InCore`, have no main snapshot row, are not physics bodies, are not lit into the world, and do not integrate Velocity; moving an instance in and out of a pane flips the tags; a Camera in a pane is refused as `Workspace.CurrentCamera`.
- Animation: an Animator in a pane steps in play, not while stopped.
- The pump: a pane's `VisualViewport` holds its rows, lights, sky, prefabs, and camera (CurrentCamera, the first Camera when nil, none); its revision moves for a Transform write, a Material color, a Texture path, a Prefab's Model, a Skybox property, the camera's Transform outside the pane, a pose, a child added, removed, moved to another pane, destroyed, and undone, a pane property, and a resync; and stays put across Prepares when nothing changes.

**Studio (headless):**

- GuiTree makes a `viewportpane` node, 3D children make no nodes, GUI children do, and the node gets its pane's scene.
- Insert offers GameObject, Camera, lights, and Skybox under a pane.
- The Scene View camera list leaves out a Camera in a pane under Workspace.
- `ViewportRenderer` scheduling, with the draw injected as MaterialPreviews' is: the budget by count and pixels, oldest first, panes not shown are not drawn, incomplete draws retried, size rounding and the cap, eviction after 60 frames.

**GL (`scene-render-check`):**

- A pane render with transparent output: corners alpha 0, the model opaque, edge pixels between.
- With a Skybox: every pixel opaque.
- The target pool: alternating two sizes makes no new targets after the first of each.
- Two Skyboxes alternating build each sky's cubes once.

**JadeFX:** `drawTexture` tests in the JadeFX repo.

**Demo:** `viewport-demo`, after BillboardDemo: a window with a ScreenGui holding an icon pane (transparent, a model and a light) and a preview pane (a Skybox and a spinning model), saving PNGs of the window after paint. Added to the warnings list in CMake.

## Build and order

1. JadeFX `drawTexture` (sibling repo).
2. Renderer: transparent output, pooled targets, sky cache, `SceneDraws` extraction (GameView unchanged in behaviour).
3. The class, containment, factories, and docs.
4. The scope wall and its exclusions, and pane Animators.
5. Pane scenes in the pump, with dirtiness.
6. `ViewportNode`, `ViewportRenderer`, and their wiring in GameView, PluginWidgetPane, and the player.
7. Studio touches, the demo, and docs.

"Rebuild" means the Release studio, which also builds the player. When testing an export, hide `../JadeFX_CPP/res/shaders` so its fallback does not mask a missing shader.

## Not done here

- Shadows in panes, and DynamicSky, Bloom, SSR, and AO in panes.
- Terrain, physics, and constraints inside a pane (a WorldModel equivalent).
- Sharing one pane render between Scene Views or windows.
- Splitting Renderer into shared programs and per-view state.
- A near plane closer than 0.2 units for very small models.
- Editing a pane's contents in the pane itself, with outlines and handles there.
- A re-baked mesh file showing in a pane before the pane re-renders for another reason.
