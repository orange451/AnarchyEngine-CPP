# BillboardGui Design

2026-10-04 · A GUI drawn in the 3D world, as Roblox's BillboardGui is: the ScreenGui tree, CSS, and input, centred on a PVInstance and facing the camera, sized in world units through CSS percentages, and hidden by the world in front of it unless it is always on top.

## Goal

A `BillboardGui` holds the same GuiBase children and CSS a ScreenGui does and lays out by the same JadeFX rules. Instead of filling the Scene View it floats at a point in the world, its anchor, and always faces the camera. On the billboard itself, a CSS percentage is world units: `width: 100%` is one unit at the billboard's distance from the camera, so `calc(200% + 32px)` is two units, shrinking and growing with distance, plus 32 screen pixels. Its elements take the mouse as a ScreenGui's do. With `AlwaysOnTop` false the world in front of it hides it pixel by pixel, and a hidden pixel does not take the mouse.

It must never trail the scene: a billboard on a moving part, seen through a camera turning fast, sits exactly where that frame draws the part.

## Decisions

| Question | Decision |
| --- | --- |
| Approach | Each billboard is a JadeFX node in the Scene View's GuiLayer, placed and sized every frame by projecting its anchor. No render-to-texture: a camera-facing billboard lies at one depth, so a 2D draw tested against that depth looks the same as a quad in the world, and the 2D nodes keep GuiLayer's input, focus, and CSS as they are. |
| Class | `BillboardGui : GuiBase`, a sibling of ScreenGui. CSS element type `billboardgui`. |
| New properties | `Adornee: PVInstance?` (nil) and `AlwaysOnTop: boolean` (false), saved registry properties. |
| Anchor | Adornee when it names a live PVInstance; else the parent when it is a PVInstance; else the world origin. The anchor point is the translation of the PVInstance's `transform()`. The parent link is never written to Adornee. |
| Where it draws | In Workspace or Core, at any depth. Anywhere else it is only data. One inside a ScreenGui, a GuiBase, or another BillboardGui is drawn by neither. |
| Edit mode | Drawn in edit mode and in play, as ScreenGuis are, and hidden with them by the Scene View's eye. |
| Units | A percentage width or height on the BillboardGui resolves against `pixelsPerUnit`, the screen pixels one world unit covers at the anchor's depth. A child's percentage is of its parent, as in any CSS. |
| Size | `Size` means what it means on any GuiBase: a preferred size in points. A billboard with neither Size nor a CSS width or height takes its content's size, a constant pixel size at any distance. |
| Timing | Placement comes from the same VisualSnapshot the 3D pass draws that frame, never from the DataModel and never from a different snapshot. |
| Occlusion | With AlwaysOnTop false, JadeFX discards each billboard fragment whose scene depth is nearer than the billboard's, sampling the renderer's own depth texture as the floor grid and outlines do. |
| Occluded input | A depth-tested billboard is mouse-transparent where the scene under the cursor is nearer than it. The cursor depth comes from an asynchronous one-pixel read, one frame old. Drawing is never late; only hover and press at a moving edge may be. |
| Order | Depth-tested billboards far to near, then AlwaysOnTop billboards far to near, then the ScreenGuis over them. |
| Offset | None yet. A billboard at its adornee's centre is partly hidden by the adornee's own surface while depth tested. An offset property comes later, in a shape not yet chosen; nothing here assumes the anchor is the billboard's centre beyond `BillboardGui::anchor()`. |

## Architecture

### 1. The class (`engine_instances/Gui.hpp`, `Gui.cpp`)

`BillboardGui` derives from `GuiBase`, so it has ClassList, Style, Size, Alignment, Visible, MouseTransparent, and the five mouse events, and takes the children a ScreenGui takes.

`AlwaysOnTop` is a new `GuiProperty` slot, boolean, false, checked and recorded by `GuiValues::set_value` like every other slot. `Adornee` is not a LuaSlot value but an `InstanceRef`, held by the target's GUID so a load, undo, or Stop can set it before the target exists; it is read and written through `instance_reference_slot` and `set_instance_reference`, the helpers PhysicsObject's `GameObject` uses, with the type `PVInstance?`. A value that is not a PVInstance is refused with a message. `on_reuse` clears it.

```cpp
// The PVInstance it floats over: Adornee, else a PVInstance parent, else 0
// for the world origin.
InstanceId anchor_instance() const;
// The anchor's world position. The origin when anchor_instance() is 0.
Vec3 anchor() const;
```

The class comment in `Gui.hpp` gains BillboardGui's entry, and the comment over the GUI classes says ScreenGuis draw over the view and BillboardGuis in it.

Registration: `register_lua_class("BillboardGui", "GuiBase", ...)` with the two fields; `register_suited_parents("BillboardGui", {"Workspace", "PVInstance"})`; the GuiBasePane and control lists, and CSS's, gain `BillboardGui` beside `ScreenGui`. Core needs no entry: tools create their instances in code.

### 2. The snapshot (`engine_core/SnapshotPump.hpp`, `.cpp`; `runner/SceneFeed.cpp`)

```cpp
// A drawn BillboardGui: where its anchor was in the same pass that took the
// instances' transforms, so it and its adornee's row never disagree.
struct VisualBillboard {
    InstanceId id = 0;
    Vec3 anchor{};
    bool always_on_top = false;
};
// VisualSnapshot gains:
std::vector<VisualBillboard> billboards;
```

The pump adds a row for each BillboardGui `DataModel::in_workspace` or `in_core` finds that is Visible and not nested in another GUI, taking `anchor()` in the pass that copies transforms, under the same lock. `SceneFeed::perform` copies `billboards` with the other fields.

### 3. One snapshot per frame (`runner/GameView.cpp`)

Today `collectMeshes`, in the paint, calls `feed_->latest()`, while GuiLayer syncs and lays out earlier, in `layoutChildren`. A snapshot published between the two would put the billboards a frame behind the scene.

`GameView::layoutChildren` calls `feed_->latest()` once, at its top, and keeps the reference as `frameSnapshot_`; `collectMeshes` and the paint use it instead of calling `latest()` again. `latest()` already promises the buffer is unchanged until the next call. Layout runs every frame before the paint, and a paint reached without a layout reads `latest()` itself, as it does now.

The camera follows the same rule: `followCamera` runs from `frameSnapshot_` in layout, so the camera the billboards are projected with is the one `renderer_.draw` uses.

### 4. Placement and units (`runner/GuiLayer.hpp`, `.cpp`)

GuiLayer gains a second set of roots beside its ScreenGuis: one node per drawn BillboardGui, built by the same `build` and `apply`, element type `billboardgui`, its own area not picked (`setPickOnBounds(false)`), as a ScreenGui's is not.

`sync` still builds and updates the nodes from the DataModel, so a Text or CSS change appears on the next sync. Placement is separate:

```cpp
// The camera and pane of this frame, and its billboard rows.
void placeBillboards(const engine_core::VisualSnapshot& snapshot, const BillboardView& view);
```

`BillboardView` carries the camera matrix and field of view the renderer will draw with, the pane's size, and the renderer's depth function (section 5). For each row with a node:

- depth = the anchor's distance along the camera's forward axis. At or behind the near plane, the node is hidden for the frame and takes no mouse.
- the screen point = the anchor projected through the camera.
- `pixelsPerUnit = paneHeight / (2 · depth · tan(fov / 2))`.
- the node is laid out with `pixelsPerUnit` as its available width and height, so JadeFX's `available · percent + pixels` turns `calc(200% + 32px)` into two units and 32 pixels, then centred on the screen point.
- `sceneDepth` = the renderer's depth for the anchor, kept for the paint and the mouse.

The layer's `layoutChildren` lays the ScreenGuis out as now and the billboards as placed. Its children are ordered depth-tested far to near, AlwaysOnTop far to near, then the ScreenGuis, so JadeFX paints and picks them in that order.

Each frame lays out every billboard again when its depth changes, which a moving camera does every frame. That is fine for dozens. Skipping layout when `pixelsPerUnit` barely moves, or the billboard has no percentage sizes, is left for when a scene needs it.

### 5. Occlusion in the draw (JadeFX `src/gl/UiRenderer`; `runner/Renderer`)

The 3D pass leaves no scene depth in the window's framebuffer: depth is the renderer's G-buffer `depthTexture_`, and the tone map draws into the window with the depth test off. The floor grid and outlines already hide themselves by sampling that texture, with `whiteTexture_` standing in on a frame that drew no meshes or sky. Billboards use the same texture the same way.

JadeFX's `UiRenderer` gains an occluder:

```cpp
// While set, a fragment whose scene depth, sampled from depthTexture at its
// window pixel through the pane rectangle, is nearer than depth is discarded.
// Setting or clearing it flushes the pending batch.
void setOccluder(unsigned depthTexture, int x, int y, int width, int height, float depth);
void clearOccluder();
```

The box, text, and image shaders gain one uniform-gated sample and `discard`; with no occluder they draw as today. The texture unit is one UiRenderer uses for nothing else. Batching keeps boxes across draws, so the flush on change keeps a billboard's boxes out of a ScreenGui's draw.

`Renderer` gains:

```cpp
// This frame's scene depth: depthTexture_, or whiteTexture_ when nothing was drawn.
unsigned sceneDepth() const;
// A world point's value in sceneDepth(), by this frame's projection.
float depthAt(Vec3 world) const;
```

Using the renderer's own matrices keeps the billboard's depth in the scene's encoding. GuiLayer paints a depth-tested billboard between `setOccluder(sceneDepth(), pane…, depthAt(anchor))` and `clearOccluder()`. AlwaysOnTop billboards and ScreenGuis paint with none. The paint follows `renderer_.draw` in the same `renderContent`, so the depth is that frame's.

### 6. Occluded input (`runner/GameView.cpp`, `runner/GuiLayer.cpp`)

Each paint, GameView starts an asynchronous read of the one depth value under the cursor from `depthTexture_`, into a pixel buffer object made once, and collects the previous frame's. Outside the pane, before the first read, or on a frame that drew nothing, there is no value.

Before a mouse event reaches a depth-tested billboard, GuiLayer compares: when the cursor depth is nearer than the billboard's, the billboard is mouse-transparent for that event, which passes to whatever is under it, a farther billboard or the Scene View. ScreenGuis, over every billboard, are unaffected. For tests, the cursor depth can be set directly in place of the read.

### 7. GL safety

A GL error in a JadeFX frame quits the studio. The occluder binds only its own texture unit; the depth read's buffer is made once and resized only with the pane; the renderer's `SavedState` restore is unchanged. Each new draw is gated as the renderer's passes are.

## Testing

**`BillboardGuiTest` (new, headless, ctest).** Defaults; save and load, undo and redo, and restore at Stop for both properties. `anchor()`: Adornee wins, else a PVInstance parent, else the origin; moved out from under its parent it stops following; destroying the Adornee falls back, and undoing the destroy restores the link. A non-PVInstance Adornee is refused. Drawn in Workspace and Core at any depth; not drawn in Storage, in Gui, or nested in a ScreenGui, a GuiBase, or another BillboardGui.

**`SceneFeedTest`.** A drawn billboard publishes one row with its anchor and AlwaysOnTop; an undrawn one none. Moving the adornee, the billboard's row and the adornee's instance row agree in every snapshot.

**`GuiStyleTest`.** With a known camera and field of view, a billboard ten units away gets the expected `pixelsPerUnit`: `width: 100%` is one unit, `calc(200% + 32px)` two units and 32 pixels, a child's `50%` half the billboard. It is centred on the anchor's projection and hidden behind the camera. Changing the camera and adornee in one snapshot, one layout and paint place the billboard by that snapshot. Order is depth tested, then AlwaysOnTop, then ScreenGuis. A click on a billboard Button fires Action; with a cursor depth set nearer than the billboard, the same click reaches the Scene View; with AlwaysOnTop it reaches the Button again.

**JadeFX tests.** Setting or clearing the occluder flushes the pending batch. Against a depth texture near on its left half and far on its right, an occluded box draws only on the right; with no occluder, on both.

**By hand.** `scene-render-check` gains a cube in front of half of a depth-tested billboard, which hides that half, and an AlwaysOnTop one it never hides. The occluder and depth read also run in `build/assets-demo <out> dark`, a real JadeFX window, where a bare context has missed macOS GL failures before. Last, in the studio: a billboard on a moving part while the camera turns fast.

## Out of scope

An offset from the anchor. World-oriented GUIs (Roblox's SurfaceGui). Distance limits and fading. Skipping per-frame layout for many billboards.
