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
    - Billboard: a Material and a Size (width, height in units). A quad standing on the
      origin, turned about its up axis to face the camera (yaw only), drawn with the
      Material as a Model's surface is: its color, normal, roughness, metalness, and glow.
      **Bake card** draws the Prefab's own Models onto the card and makes the result the
      layer's Material and Size (below). A hand-made card is any Material, with or without
      a normal map.
    - None: nothing draws from that distance on.
- A layer draws from its Distance up to the next layer's Distance (the last one forever).
- Add LOD layer puts a layer 100 units past the last one. Every edit is one undo step.

## Data

`LodLayer`, a child of a Prefab beside its Models (the base layer).

| Property | Type | Default | Notes |
|---|---|---|---|
| Distance | number | 100 | units, >= 0 |
| Kind | Enum.LodKind | Model | Model, Billboard, None |
| Material | Material? | nil | Billboard's surface; no Material draws it white |
| Size | Vector2 | (4, 8) | Billboard's width and height in units, each > 0 |

Containment: a Prefab holds Models and LodLayers; a LodLayer holds only Models. A
LodLayer's Models are visual only: bounds, skeleton, and collision read the base layer.
Distances do not scale with the GameObject's Scale. Billboards cast no shadow.

## How it draws

- The snapshot's `VisualPrefab` gains `lods` (distance, kind, meshes, the billboard's
  Material as a `VisualMesh`, and size), filled from the Prefab's LodLayers, nearest first.
- A scene view's templates for a Prefab carry a distance band: base meshes [0, first), each
  layer's [its distance, next). Every band's draws are kept (RowDrawCache) as today.
- Culling drops a draw whose band does not hold the camera's distance to the row's origin,
  so moving the camera rebuilds nothing. Shadow casters take the same bands.
- A billboard is a built-in unit quad drawn with its Material, sized by Size times the
  Scale; geometry.vert turns it toward the camera (uBillboard), so the CPU writes its
  instance as it is.

## Bake card

One draw of the Prefab's own Models, level from in front through a narrow lens, framed to
the card (centered on the origin, twice the widest reach in X or Z wide, from the origin to
the top). The G-buffer is read back, not the lit image, so the scene lights the card: its
albedo (the cutout in alpha), view-space normals (the card's tangent space), metalness,
roughness, reflectivity, and glow. Drawn at twice the size and shrunk with alpha weights;
each clear pixel takes the nearest drawn one's values, so mipmaps never mix in black. A map
is saved only where the card varies: one roughness or metalness everywhere is the
Material's number, no glow is no map, and a flat card has no normal map. Reflectivity,
which a Material holds only as a number, is the card's average. The PNGs go under
`textures/billboards/`, named by their bytes; the Material lives in Materials/Billboards
and its Textures in Textures/Billboards, made once and reused by later bakes, all in one
undo step. `billboard-bake` bakes every Prefab under a folder the same way.

## Testing

Sandbox: LodLayer properties, enum, containment, undo, save and load; banded culling and
casters; a billboard's instance as the row made it. Prefab editor tests: the LOD section,
its Material slot and drops, a bake's Material and Textures as one undo step, the card's
maps shrunk and filled. Scene render check: a billboard stands upright and turns to a
camera at its side; a bake draws a box's colors, normals, roughness, and metalness.
