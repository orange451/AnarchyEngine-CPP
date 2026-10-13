# Prefab LOD layers

Date: 2026-10-13. Approved in chat ("assign a LOD on each tree, but that LOD draws nothing... see if FPS improves").

## Goal

A Prefab draws something cheaper, or nothing, past a distance from the camera. On the
100k vegetation map, drawing nothing past 400 units took the GPU from 13.3 ms to 3.6 ms
a frame (the cull experiment, 2026-10-13); billboards keep the far forest for little more.

## What the person sees

- The Prefab editor's Model cards are the **base layer**: what draws when no LOD layer
  has taken over (a Prefab with no LOD layers draws exactly as today).
- An **LOD** section under them lists the Prefab's LOD layers, nearest first. Each has:
  - **Distance**: in units, from the camera to the GameObject's origin, where it takes over.
  - **Kind**: Model, Billboard, or None.
    - Model: its own Model cards (Mesh + Material, as the base layer's), swapped in.
    - Billboard: a Texture and a Size (width, height in units). A quad standing on the
      origin, turned about its up axis to face the camera (yaw only).
    - None: nothing draws from that distance on.
- A layer draws from its Distance up to the next layer's Distance (the last one forever).
- Add LOD layer puts a layer 100 units past the last one. Every edit is one undo step.

## Data

`LodLayer`, a child of a Prefab beside its Models (the base layer).

| Property | Type | Default | Notes |
|---|---|---|---|
| Distance | number | 100 | units, >= 0 |
| Kind | Enum.LodKind | Model | Model, Billboard, None |
| Texture | Texture? | nil | Billboard's image |
| Size | Vector2 | (4, 8) | Billboard's width and height in units, each > 0 |

Containment: a Prefab holds Models and LodLayers; a LodLayer holds only Models. A
LodLayer's Models are visual only: bounds, skeleton, and collision read the base layer.
Distances do not scale with the GameObject's Scale. Billboards cast no shadow.

## How it draws

- The snapshot's `VisualPrefab` gains `lods` (distance, kind, meshes, billboard texture and
  size), filled with the base Models from the Prefab's LodLayers, nearest first.
- A scene view's templates for a Prefab carry a distance band: base meshes [0, first), each
  layer's [its distance, next). Every band's draws are kept (RowDrawCache) as today.
- Culling drops a draw whose band does not hold the camera's distance to the row's origin,
  so moving the camera rebuilds nothing. Shadow casters take the same bands.
- A billboard is a built-in unit quad with the Texture, sized by Size times the Scale, and
  turned toward the camera each frame where the instances are written (BuildBatches).

## Testing

Sandbox: LodLayer properties, enum, containment, undo, save and load; snapshot lods;
banded culling and casters. Studio tests: templates with bands. Scene render check: a
billboard faces the camera from any yaw; a layer swaps at its distance; None draws nothing.
