# Custom shaders

2026-10-20 · Research: `docs/research/2026-10-20-custom-shaders-in-other-engines.md`. Builds on the Material (`2026-09-29-game-services-assets-design.md`), the Lighting effects (`2026-10-05-bloom-design.md` and its siblings), and the CSS editor.

## Goal

A user writes a short GLSL file in Studio and sees it on their geometry, or over the whole frame, as they type. Two kinds:

- A **surface shader** changes how a Material's surface looks: dissolve, hologram, scrolling lava, snow on top faces, triplanar rock, fake water. It goes on a `Material`, so it reaches everything a Material reaches today: prefab models, brush faces, LOD cards, and the Assets pane's material balls.
- A **post shader** changes the finished frame: vignette, pixelation, CRT, color grading, underwater wobble, depth fog, edge detection. It goes under `Lighting` beside `BloomEffect`, in the same chain, saved the same way.

The shape follows the model every engine's users praise and Unity's lost: the user fills in one function, declares a few uniforms with hints, and the engine owns every pass, the lighting, the shadows, the Properties panel rows, Lua access, saving, undo, and errors. A shader that fails to compile never breaks the scene: the Material draws plain, the effect does nothing, and the error goes where script errors go.

Nothing new is bolted on beside the existing systems. A shader is an asset like a Texture; its uniforms are **attributes** of the Material or effect that holds it, a general feature every Instance gets with this work; a post shader is a Lighting effect like Bloom; its source is edited like CSS; its errors show in Problems like Luau's.

## What the user does

### A dissolve material

1. In the Assets pane, Shaders category, Insert → Shader, named Dissolve. It opens in a shader editor tab with a starter source. The user replaces it with:

```glsl
shader surface;

uniform float Progress : range(0, 1) = 0.0;
uniform vec3 EdgeColor : color = vec3(1.0, 0.5, 0.1);
uniform sampler2D Noise : repeat;

void surface() {
    float n = texture(Noise, UV).r;
    if (n < Progress) {
        discard;
    }
    EMISSIVE += EdgeColor * 4.0 * (1.0 - smoothstep(Progress, Progress + 0.06, n));
}
```

2. On the Material, in the Surface group, the new `Shader` picker (a `Shader?` reference, like `DiffuseTexture`) is set to Dissolve. A **Dissolve** group appears below with Progress (a slider), EdgeColor (a color), and Noise (a texture picker): the shader's parameters, each an attribute of the Material, shown at the shader's default until set. The material ball in Assets shows the effect.
3. Every object using that Material now dissolves. A script animates it: `material:SetAttribute("Progress", t)`.
4. The user edits the source. Half a second after the last keystroke the Scene View shows the change. A typo puts a red mark in the editor's gutter and a row in Problems: `Dissolve (Assets.Shaders): 9: 'smoothstepp' : no matching overloaded function found`, and the objects draw with the plain Material until it is fixed.

### A vignette

1. Insert → Shader, named Vignette:

```glsl
shader post;

uniform float Strength : range(0, 1) = 0.5;
uniform float Radius : range(0, 1) = 0.75;

void post() {
    float d = length(SCREEN_UV - 0.5) / 0.7071;
    COLOR.rgb *= 1.0 - Strength * smoothstep(Radius, 1.0, d);
}
```

2. Under `Lighting`, Insert → ShaderEffect, and set its `Shader` to Vignette. Strength and Radius appear in its Properties as attributes. The Scene View and the player both show it.
3. Two ShaderEffects run in `Order`, then tree order.

### From Lua

```lua
local shader = Instance.new("Shader")
shader.Name = "Tint"
shader.Source = [[
shader surface;
uniform vec3 Tint : color = vec3(1.0);
void surface() { ALBEDO *= Tint; }
]]
shader.Parent = game.Assets.Shaders

local material = game.Assets.Materials.Brick
material.Shader = shader
material:SetAttribute("Tint", Color3.new(1, 0.8, 0.8))
print(material:GetAttribute("Tint"))          -- the value set, or the shader's default
material:GetAttributeChangedSignal("Tint"):Connect(function() ... end)

local vignette = Instance.new("ShaderEffect")
vignette.Shader = game.Assets.Shaders.Vignette
vignette:SetAttribute("Strength", 0.8)
vignette.Parent = game.Lighting

game:GetService("RunService").RenderStepped:Connect(function(dt)
    material:SetAttribute("Progress", (os.clock() % 4) / 4)
end)
```

## Decisions

| Area | Decision | Why |
| --- | --- | --- |
| Language | GLSL 330, the engine's own, with a one-line header and uniform hints | No new language, no transpiler; the user's text goes to the driver nearly as written, so every GLSL tutorial applies |
| Model | Fill in `surface()` or `post()`; built-ins are uppercase globals | The model users praise in Godot and old Unity; the engine keeps every pass |
| Asset | `Shader` under `Assets.Shaders`, source in a `.glsl` file beside its `.meta.json` | Like Script's `.luau`: diffable, editable outside, packed into exports already |
| Attachment | `Material.Shader` (a `Shader?`) and `ShaderEffect` under Lighting | No new material class; a shader layers on the Material the user already has |
| Parameters | Each uniform is an attribute of the Material or ShaderEffect, typed by its hint | Attributes are a general per-instance feature (`SetAttribute`, `GetAttribute`, signals), saved as one property, so Properties, Lua, save, undo, Stop, and MCP come with them, and the class registry stays static |
| Variants | Per surface shader: G-buffer, see-through, shadow. Per post shader: one | Three programs, not a matrix; compiled in milliseconds on first use |
| Errors | Old program keeps drawing; failure draws plain; message with a line number in the editor and in Problems | Never pink, never a blank frame |
| Reload | Source change → recompile at the next draw | Hot reload for free, the way CSS restyles |

## The Shader asset

`Shader` is an Instance under `Assets.Shaders`, a new Assets category beside Materials and Textures (`Containment.cpp`'s table). It is script-creatable and shown in the Assets pane with its own icon.

Properties:

| Property | Type | Rule |
| --- | --- | --- |
| Source | string | The GLSL text. Not a saved registry property: like `LuaSource`, it lives in its own file, `<Name>.<guid>.glsl` beside `<Name>.<guid>.meta.json`. `source_version` moves on every change |
| Kind | `Enum.ShaderKind` (Surface, Post), read-only | Parsed from the header. Shown in Properties so a Material picker can refuse a post shader with "Vignette is a post shader" |
| Error | string, read-only, not saved | Empty when the last parse and compile succeeded; otherwise the first error, `line: message`. A parse error is written at once on the simulation thread; a compile error comes back from the render side through a new write-back keyed by Shader id and `source_version`, modelled on the path `Texture.Loaded` takes (which is keyed by file, so it is a model, not a reuse) |

`LuaSource`'s file pairing in `Project.cpp` is generalized: a class says its source extension (`.luau` or `.glsl`) and the loader and saver pair by GUID as they do now. Copy, paste, and `.aeinst` files carry the source through `write_place` / `read_place` as scripts do.

Double-click or Edit opens the **shader editor**, a tab like the CSS editor: GLSL highlighting (keywords, types, built-ins, the engine's uppercase names, numbers, comments, preprocessor lines), auto-indent and brace pairing, Source written back half a second after the last keystroke as one undo step, find and replace. Compile errors mark lines in the gutter and the scroll bar's right half, from `Error`. No completion in the first version.

A new Shader starts with a commented starter source: the header, one uniform with a hint, and an empty `surface()`, so the shape is visible without reading docs. Insert from the Assets pane's Shaders category.

## The language

A shader is GLSL 330 (the engine's `#version`) with three additions the engine strips before the driver sees the text. Everything else is plain GLSL: functions, `const`, `#define`, `#if`, structs, loops.

### The header

```glsl
shader surface;                 // or: shader post;
render_mode transparent, two_sided;   // optional, surface only in v1
```

`shader` must be the first statement. `render_mode` takes a comma list from a fixed set per kind; an unknown mode is an error naming the known ones.

Surface modes:

| Mode | Effect |
| --- | --- |
| `transparent` | The Material draws in the see-through pass whatever its Transparency, blending by `ALPHA × (1 − Transparency)`. Without it, `ALPHA` is a cutout at today's thresholds: below 0.25 in the G-buffer pass, below 0.1 in the see-through pass |
| `two_sided` | No face culling; `FRONT_FACING` tells the sides apart. The normal already flips for back faces |
| `no_shadow` | The Material casts no shadow. Otherwise a shader whose source uses `discard` runs `surface()` in the shadow pass too, so a dissolve's holes let light through (see Shadows) |

Post modes:

| Mode | Effect |
| --- | --- |
| `hdr` | Runs on the linear, unbounded merge image, before bloom and the tone map. Without it, the shader runs after the tone map on 0 to 1 display colors, before FXAA |

### Uniforms and hints

```glsl
uniform float Progress : range(0, 1) = 0.5;
uniform int Steps : range(1, 8) = 4;
uniform bool Flip = false;
uniform vec2 Scroll = vec2(0.1, 0.0);
uniform vec3 Tint : color = vec3(1.0);
uniform vec3 Offset;
uniform sampler2D Noise : repeat, white;
uniform sampler2D Detail : color, hidden;
group Edge;
uniform float EdgeWidth : range(0, 0.2) = 0.05;
```

Each `uniform` becomes a parameter the holder's attribute of the same name feeds. Its name is any GLSL identifier; attributes are their own namespace, so a uniform named `Color` is fine and reads the attribute "Color", not the Material's `Color` property. The engine's own names (`ALBEDO`, `TIME`, `u` followed by a capital) are refused.

| GLSL type | Attribute type | Hints |
| --- | --- | --- |
| `float`, `int` | number | `range(min, max)` gives a slider; a value outside is clamped |
| `bool` | boolean | |
| `vec2` | Vector2 | |
| `vec3` | Vector3, or Color3 with `color` | A `color` value is uploaded linear (`toLinear` as `surface.glsl` does for `uColor`), so the shader does its math in light |
| `sampler2D` | `Texture?` | `color` samples sRGB (a photo); without it the texture is data (a mask, as `RoughnessTexture` is). `normal` loads it as a normal map. `white` or `black` is what an unset texture reads (default white). `repeat` or `clamp` sets wrapping (default repeat). `nearest` turns filtering off |
| any | | `hidden` keeps it out of Properties; scripts still set it |

`= default` sets what the shader reads while the holder has no attribute of that name; without one it is 0, false, white, or nil. Defaults live in the shader: a holder that never set the attribute follows the shader when the shader changes, and a save writes only the attributes that were set. `group Name;` puts the uniforms after it under a Properties group of that name; uniforms before any `group` go under a group named after the Shader. Up to 64 uniforms per shader, and up to 3 samplers in a surface shader or 4 in a post shader, in the first version: GL 3.3 guarantees 16 fragment texture units, the Material's own maps hold 0 to 3 and 5, the see-through pass holds 12 to 15 for the sky and 4 for depth, so 6 to 8 are the units free in every surface pass (see Binding).

Not supported in v1, each refused with a message: `vec4`, matrices, arrays, `sampler3D`, `samplerCube`, uniform blocks, and declaring the engine's own names.

### Surface shaders

The engine calls `surface()` once per fragment after `readSurface` has read the Material's textures and values, in the G-buffer pass, the see-through pass, and the shadow pass. Inside it the surface is a set of global variables the function reads and writes; what it leaves alone keeps the Material's value.

Writable:

| Name | Type | Holds on entry |
| --- | --- | --- |
| `ALBEDO` | vec3 | DiffuseTexture × Color × vertex color, linear |
| `ALPHA` | float | the diffuse alpha × vertex alpha |
| `NORMAL` | vec3 | the view-space normal after the normal map, unit length |
| `METALNESS`, `ROUGHNESS`, `REFLECTIVITY` | float | the Material's, 0 to 1 |
| `EMISSIVE` | vec3 | Emissive × EmissiveTexture, linear |

Read-only:

| Name | Type | Meaning |
| --- | --- | --- |
| `UV` | vec2 | the mesh's texture coordinate |
| `COLOR` | vec4 | the vertex color times the GameObject's tint |
| `VIEW_POSITION`, `WORLD_POSITION` | vec3 | the fragment's position |
| `VIEW_NORMAL`, `WORLD_NORMAL` | vec3 | the geometric normal, before the normal map |
| `SCREEN_UV` | vec2 | 0 to 1 across the pane |
| `FRONT_FACING` | bool | |
| `TIME` | float | seconds since the place started drawing, wrapping every hour |
| `CAMERA_POSITION` | vec3 | world space |
| `VIEW`, `PROJECTION`, `INV_VIEW` | mat4 | |
| `TEXTURE_*` | sampler2D | the Material's own maps: `TEXTURE_DIFFUSE`, `TEXTURE_NORMAL`, `TEXTURE_ROUGHNESS`, `TEXTURE_METALNESS`, `TEXTURE_EMISSIVE`, white when unset |

`discard` works and makes a hole in every pass, shadows included unless `no_shadow`. Writing `NORMAL` writes it to the G-buffer, so lighting, SSR, and GTAO follow it. The vertex stage is not exposed in v1 (see Later).

### Post shaders

The engine calls `post()` once per pixel on a full-screen triangle, reading the stage's image and writing the next one.

| Name | Type | Meaning |
| --- | --- | --- |
| `COLOR` | vec4, writable | the pixel on entry; the result on exit. In the LDR stage rgb is 0 to 1 display color; in `hdr` it is linear light. Alpha is the pane's coverage, premultiplied: leave it alone unless the effect means to |
| `SCREEN` | sampler2D | the whole input image, for neighbors |
| `SCREEN_UV`, `SCREEN_SIZE`, `TEXEL` | vec2 | position, size in pixels, 1 / size |
| `DEPTH`, `NORMALS`, `ALBEDO`, `MATERIAL` | sampler2D | the G-buffer: depth (raw), octahedral view normals, albedo, metalness/roughness/reflectivity, as the merge reads them |
| `depthAt(uv)`, `viewPositionAt(uv)`, `worldPositionAt(uv)`, `normalAt(uv)` | functions | linear depth in units, positions, and the decoded view-space normal at a pixel; depth 1 is the sky |
| `TIME`, `CAMERA_POSITION`, `VIEW`, `PROJECTION`, `INV_VIEW`, `INV_PROJECTION` | | as above |

See-through surfaces and particles are in `SCREEN` but not in the G-buffer, as every engine's post pass has it.

## Attachment

### Material.Shader

`Material` gains `Shader`, a `Shader?` reference in the Surface group after the textures. Its six reference slots are all taken today (`HeightTexture` is the sixth), so `ReferenceAsset::kMaxReferences` goes to 8. A post shader is refused: "Vignette is a post shader; a Material takes a surface shader". The uniforms read the Material's attributes, as the Attributes section says.

The Material keeps meaning what it means. `Transparency` still chooses the see-through pass (or `render_mode transparent` does), `Color` still tints, the textures still bind. `TerrainMaterial`s ignore a Shader in v1: the terrain program is its own, and the terrain roadmap already parks per-material shaders there.

### ShaderEffect

`ShaderEffect` is an Instance under `Lighting`, through Folders, refused elsewhere like `BloomEffect`. Unlike Bloom, every ShaderEffect under Lighting runs, not only the first.

| Property | Type | Default | Rule |
| --- | --- | --- | --- |
| Enabled | boolean | true | |
| Shader | `Shader?` | nil | A surface shader is refused. Nil draws nothing |
| Order | number | 0 | Effects run from the lowest Order up; equal Orders run in tree order. A finite number; the Explorer cannot reorder siblings, so this is how a user orders a chain |

Its uniforms read its attributes, shown under their groups.

## Attributes

A shader's parameters are values whose existence depends on another instance: a Material's Progress exists only while its Shader declares it. The class registry is static (a class's fields are one array in its registration), so these values are not properties. They are **attributes**: named values any Instance can carry, which this work adds to the engine as a feature of its own, modelled on the attributes every Roblox instance has, and which shaders are the first client of.

### The feature

Every `DataModel` keeps an attribute map, name → value, empty by default.

| Member | Meaning |
| --- | --- |
| `SetAttribute(name, value)` | Sets it. `nil` removes it. Refused with a message for a name that is empty, over 100 characters, or not made of letters, digits, and underscores, and for a value of a kind attributes cannot hold |
| `GetAttribute(name)` | The value, or `nil` |
| `GetAttributes()` | A table of every attribute, name → value |
| `GetAttributeChangedSignal(name)` | A Signal that fires after that attribute is set or removed |
| `AttributeChanged` | A Signal that fires with the name, after any attribute is set or removed. `Changed` fires too, with "Attributes", the one property name every attribute edit reports, so the panels that watch properties refresh |

Values: a number, boolean, string, Vector2, Vector3, Color3, Matrix4, or an Instance. An Instance is held by GUID as reference properties are, reads back as the live instance or `nil` when it is gone, and is what a texture parameter is. Every other `LuaSlot` kind (a Signal, a curve, an input object) is refused.

Storage: one hidden saved property, `Attributes`, a JSON object of name → value in `PropertyReflection`'s encodings with a kind tag for the ones JSON cannot tell apart (`{"kind":"Color3","value":[r,g,b]}`), registered on `DataModel` itself as `lua_hidden(lua_json_text(...))`, so every class has it and save, load, copy, paste, `.aeinst` files, Stop, and undo work through the existing property path. `SetAttribute` records one undo step holding the whole object before and after, the way `SceneService`'s table is recorded. It runs on the simulation thread, notes the change, and queues the two signals and the invalidation, so the snapshot sees it next Prepare.

Scripts, the command line, and MCP reach attributes through the four methods. Completion and analysis need nothing new: the methods are declared on `Instance`, and `GetAttribute` returns `any`.

### In the Properties panel

Every instance's Properties gets an **Attributes** group at the end, one row per attribute, typed by its value (a number field, a checkbox, a text field, a color, a vector, a reference picker), with Add (a name and a type) and, on each row, Remove. This is the Roblox shape, and it stands on its own: scripts can tag anything, and designers can see and edit the tags.

A Material or ShaderEffect with a Shader shows the Shader's parameters first, in the Shader's groups, before any remaining attributes: one row per parameter, reading the attribute when set and the shader's default when not (shown dimmed, with Reset removing the attribute again), writing `SetAttribute` with the parameter's hint applied: a `range` is a slider and clamps, a `color` vec3 is a color picker, a sampler is a picker filtered to Textures. A row whose attribute holds a value of the wrong kind shows it with a warning and the shader reads the default. Attributes the Shader does not declare stay in the Attributes group, inert but kept, as a Unity material keeps stale entries, so swapping shaders back loses nothing. When a Shader's source changes its parameters, every holder's rows refresh (the sheet rebuilds when the row set changes, as it does on a selection change) and the holder's invalidation marks it dirty so the snapshot re-reads its values.

### How a shader reads them

The `Shader` parses its source on the simulation thread whenever `Source` changes, keeping a `ShaderParameters` list: name, GLSL type, attribute kind, hints, default, group, in declaration order. Parse errors (bad header, bad hint, unsupported type, an engine name) go to `Error` at once, before any GL work, with their line.

At each Prepare that a holder's invalidation marks, the snapshot fills the holder's value table from its attributes: for each parameter, the attribute when its kind fits (a number for `float` or `int`, a boolean for `bool`, a Vector2 or Vector3 for the vectors, a Color3 for a `color` vec3, a Texture for a sampler), else the shader's default. A mismatch is never an error at draw time; it is the warning on the row.

## Rendering

### Templates, not a second path

The engine's own programs become the "no shader" case of the same templates. `deferred.frag`, `forward.frag`, and `shadow.frag` each get one hook where, under `#ifdef SURFACE_SHADER`, the `Surface` from `readSurface` is unpacked into the uppercase globals, `surface()` is called, and the globals are packed back. `geometry.vert` and `shadow.vert` pass the world position and the world normal on when the define is set. One new file, `surface_shader.glsl`, declares the globals, the matrices, `TIME`, and the Material sampler aliases. A new `post.frag` holds the post template: the samplers, the helpers, `COLOR = texture(SCREEN, SCREEN_UV); post(); outColor = COLOR;`.

A custom program's text is assembled by `LoadShader`'s splice: the main file's `#version`, `#define SURFACE_SHADER`, the libraries, `surface_shader.glsl`, then the user's text with its header and hints stripped and its `uniform` lines rewritten to plain GLSL, then the main file's remainder. Line numbers in a driver's log are mapped back by subtracting the prefix's line count, so `Error` says the user's line.

### Programs per shader

The renderer keeps a `ShaderPrograms` cache keyed by Shader id: the `source_version` it was built from, the programs, and each user uniform's location per program. A surface shader builds two or three: G-buffer (`geometry.vert` + `deferred.frag`), see-through (`geometry.vert` + `forward.frag`), and, only when its source contains the `discard` token and it is not `no_shadow`, a shadow program (`shadow.vert` + `shadow.frag` with `CUTOUT`). A post shader builds one (`fullscreen.vert` + `post.frag`). Building happens on the GL thread at the first draw that needs it, or the first after the version moves, and takes milliseconds for shaders this size. Until a rebuilt program links, the previous one keeps drawing; when it fails, the entry records the error, the Material draws with the plain program (or the effect is skipped), and nothing is retried until the version moves again. Programs of a Shader nobody has drawn for a minute are deleted, as the bloom chain and shadow tiles are reclaimed. Each `Renderer` has its own cache, so the Assets pane's `MaterialBall` compiles what it previews.

### Shadows

`shadow.vert` carries only position, bones, the model matrix, and (under `CUTOUT`) UV; the light is the camera, and nothing is cached per fragment but depth. So the shadow variant of `surface()` is narrower than the others, and runs only for shaders that need it:

- It is built only when the source contains `discard` (a text scan for the token outside comments; `ALPHA` alone cannot make a hole in a depth pass). A shader without `discard` casts with the plain shadow program, at today's cost.
- In it, `UV`, `WORLD_POSITION`, `COLOR` (the vertex color, which `shadow.vert` gains under `SURFACE_SHADER`), `TIME`, and the `TEXTURE_*` aliases are real. `NORMAL`, `VIEW_NORMAL`, `WORLD_NORMAL`, `VIEW_POSITION`, `SCREEN_UV`, `CAMERA_POSITION`, `FRONT_FACING`, and the matrices are defined but neutral (zero, false, identity), and the docs say so: a `discard` that depends on the camera will not match in the shadow. Writes to `ALBEDO` and the rest are accepted and dropped.
- Caster runs are grouped by mesh plus cutout texture today (`ShadowRenderer`'s `CasterRun`); a shadow-casting shader adds its Shader id to that grouping, and its values to the bind, as the cutout texture is bound.
- The planner's caster fingerprint (`ShadowPlanner`'s `ShadowCaster`), which decides whether a cascade or a light's map can be kept, gains the Shader id, its `source_version`, and a hash of the holder's values, so editing the source or animating `Progress` redraws the shadows that depend on it. A shadow-casting shader whose source reads `TIME` is fingerprinted with the frame, so it redraws its shadows every frame; the docs say that too, and `no_shadow` is the way out for a shader that only glows.

### LOD cards

A Prefab's LOD cards are baked images (`BillboardBaker` renders the model into `textures/billboards/<name>.<map>.<hash>.png`). A model whose Material holds a Shader bakes what the shader draws at its defaults and `TIME` 0, through the baker's own `Renderer`, which compiles it like any other; the card's hash takes in the Shader's `source_version` and the Material's values, so a change rebakes the cards. A `LodLayer`'s own Material may hold a Shader as well; it then runs live on the card quad, where `UV` is the card's. A shader that animates by `TIME` is still on the baked card as it was at 0, which is the trade a baked card makes.

### Binding

Values live in one table per Material in the snapshot, not in each `VisualMesh`: `VisualShaderValues`, keyed by the holder's id, holding a small vector of floats and ints in parameter order plus up to the sampler cap of texture paths with their FlipY, streaming flags, and sampling state. `VisualMesh` carries the Shader id and the holder's id; `MeshDraw` gains `shader` (the Shader id, 0 for none) and a pointer to the resolved values, which `SceneDraws::ApplyMaterial` resolves once per Material per frame, loading the textures through `TextureCache` as the Material's maps are. So `material.Progress = t` every frame touches one table entry, and never re-resolves the Models that use the Material, which a place with a hundred thousand trees on one Material needs. `bindMaterial` binds the Material as now, then the user uniforms by location and the user textures to units 6, 7, and 8, the units no surface pass reads for anything else (the Material's maps hold 0 to 3 and 5, the see-through pass holds 4 and 12 to 15). `TIME` and `INV_VIEW` are set once per pass. `SceneLighting` carries the ShaderEffect list the same way: id, enabled, order, values, textures; a post program binds its five scene samplers and then up to four user textures on the units above them.

`BatchKey` puts the Shader id above the slot, so runs that share a program are adjacent and the geometry pass switches programs once per shader, not per run. Instancing is untouched: a slot is one Prefab Model, which has one Material, which has one Shader. Brush faces, which draw alone at slot 0, bind per face as they do now.

### The post chain

Two insertion points in `Renderer::draw`, mirroring Unreal's Before Bloom and After Tonemapping:

- `hdr` effects run after `mergePass` and before `bloomPass`, ping-ponging between `mergeTexture_` and a second RGBA16F target of the same size made on first use, so bloom and the tone map see the result.
- LDR effects run after `toneMapPass` and before `fxaaPass`. When any LDR effect is enabled, the tone map always goes to `ldrTexture_` (today it does only under FXAA), the effects ping-pong between it and a second RGBA8 target, and the last result goes to the pane, through FXAA when on.

Each effect is a full-screen triangle with its program, `SCREEN` bound to the stage's current image and the G-buffer bound as the merge binds it. A disabled effect, a nil Shader, or a failed program is skipped at no cost. Effects run in the Scene View and the player; a `ViewportPane` and the material balls, which set their own lighting, run none, as they run no bloom.

### Snapshot

`SnapshotPump` carries a `VisualShader` row per Shader in use: id, `source_version`, and the source text when the version moved since the last snapshot (the renderer needs the text only to compile; the rest of the time it needs the version). `VisualMesh` gains the Shader id and the holder's id; the values are in the per-holder table above, filled from the holder's attributes and refilled only for holders the invalidation queue marked. `VisualLighting` gains the ShaderEffect list. `Shader.Error` comes back from the renderer keyed by id and version, so an error for a version the user has already replaced is dropped.

## Studio

- **Assets pane**: a Shaders category, with a shader icon; Insert → Shader; double-click opens the editor. A Material's ball shows its Shader.
- **Properties**: `Material.Shader` and `ShaderEffect.Shader` use the reference picker filtered to Shaders of the right Kind; the parameter rows follow with their sliders, colors, and texture pickers, a group per `group`, reading and writing attributes; every instance gets the Attributes group with Add and Remove.
- **Shader editor**: as described under the asset. Its tab is titled `Dissolve.glsl`.
- **Problems**: a Shader with an `Error` is a source with one error row, the message at its line; clicking it opens the editor there. The checker's own rows are untouched.
- **MCP**: `Shader.Source` reads and writes as a Script's does; attributes read and write through the four methods, and the instance inspection tool lists them beside the properties.
- **Explorer**: ShaderEffect under Lighting with an effect icon, listed with the other effects in `ClassOrder`.

## Files and export

`src/Assets/Shaders/Dissolve.<guid>.glsl` beside `Dissolve.<guid>.meta.json`, the way scripts are laid out; `.gitattributes` marks `*.glsl` as LF text as it marks `*.luau`. Exports already pack `src/`, so the player gets the sources and compiles them at load. The engine's own `resources/shaders` is unchanged in kind.

## Limits in the first version

- GLSL 330 on desktop. A phone build would compile the same text as GLSL ES 3.00 with `precision` lines added by the template; the user must then avoid desktop-only functions, which the error reports.
- No vertex stage, no custom lighting, no unlit mode, no extra passes, no includes between Shaders, no per-GameObject values, no terrain. Each is listed under Later with how it would fit.
- A shader can still hang the GPU with an endless loop, as in every engine. The watchdog reset is the driver's.
- Compiling runs on the GL thread at the next draw, so a long shader can cost one slow frame. Fine for an editor; a game compiles its shaders at load, where every Material's first draw is anyway.

## Later

Each of these has a slot in this design and needs no rework of it:

- **`vertex()`**: a second function with `VERTEX`, `NORMAL`, `UV`, `COLOR` writable in local space, spliced into `geometry.vert` and `shadow.vert` under the same define. Wind, waves, and bulges.
- **`render_mode unlit`** and **`light()`**: unlit stores a flag in the G-buffer's material target's unused alpha, which the light pass honors; `light()` would be a see-through-only feature at first, as the deferred light pass is shared.
- **Per-GameObject values**: a GameObject's own attributes overriding its Material's for the parameters a shader marks `instance`, uploaded as instance vertex attributes (there are free slots after 15) for up to four floats, the way `tint` rides today. Unreal's Custom Primitive Data, Godot's instance uniforms; the attribute map is already there, so this is a snapshot and vertex-layout change only.
- **Shader includes**: `#include "Common"` naming another Shader of a `Library` kind, spliced in, with errors mapped to the right file.
- **Global values**: a `Lighting`-level `ShaderValues` every shader can read, for weather and time of day; `TIME` covers most cases until then.
- **Terrain**: a Shader on a TerrainMaterial splits chunk triangles per shader; the terrain roadmap owns the blending question.
- **A graph editor**: would emit this text, as Godot's VisualShader does, so nothing here precludes it.

## Tests

- `sandbox/attribute_tests.cpp`: `SetAttribute` for every value kind and the refusals (bad names, a Signal, a curve), `nil` removing, `GetAttributes`, both signals firing once per set with the name, an Instance value surviving save and load by GUID and reading `nil` once destroyed, undo of a set and of a remove as one step each, Stop restoring, copy and paste and `.aeinst` carrying them, `Changed` not firing for an attribute.
- `sandbox/shader_tests.cpp`: the header and hint parser (every type and hint, defaults, groups, the refusals and their messages, engine names); `Shader` save and load with its `.glsl` file, copy and paste, undo of Source; `Material.Shader` refusing a post shader; parameters: a holder with no attribute yields the default, a set attribute of the right kind yields it, one of the wrong kind yields the default, a range clamps in the Properties write, attributes the shader does not declare survive a shader swap; `ShaderEffect` placement and Order; scripts creating and setting all of it.
- `sandbox/shader_splice_tests.cpp`: a user source through the splice gives valid GLSL with the uniforms rewritten, and a driver log line maps back to the user's line.
- `tests/SceneRenderCheck.cpp`, with no GL errors throughout: a surface shader that sets `ALBEDO` red turns a white cube's pixels red; one that discards half by `UV` leaves the clear color there and leaves its shadow with a hole; `render_mode transparent` blends; a shader with a syntax error draws the plain Material and reports its line; editing the source back fixes it on the next frame; a shader without `discard` keeps the plain shadow program and a `TIME`-driven discard redraws its shadow each frame; a value change on a Material shared by a thousand Models re-resolves no Model; a Prefab with a shader Material bakes cards that show it and rebakes when the source changes; a post shader inverting `COLOR` inverts the pane; an `hdr` effect runs before bloom (an emissive cube blacked out by it blooms nothing); two effects run in Order; disabling one changes nothing but it; the material ball draws the shader.
- `tests/SceneFeedTest.cpp`: `VisualShader` and the values ride through the feed; a version move carries the source once.

## Calls I made

Marked here so they can be checked while reading:

- Uppercase globals (`ALBEDO`) rather than a struct parameter: Godot's shape, unmistakable beside user uniforms, and no name to pick for the parameter.
- The header is two statements (`shader`, `render_mode`), not `#pragma` lines, and hints use `name : hint` after the type: the Godot syntax a reader will already know. Our preprocessor strips both; the driver never sees them.
- `color` vec3 uniforms upload linear. The alternative, raw sRGB with the user calling `toLinear`, is what `uColor` does internally, and the one thing beginners get wrong.
- Post shaders default to after the tone map, as Unreal's do; `hdr` is opt-in.
- `Order` on ShaderEffect instead of relying on sibling order, which the Explorer cannot change.
- Three samplers in a surface shader, four in a post shader, and 64 values per shader, from the unit budget; all are refusals with a message, not silent truncation.
- Programs of unused shaders are reclaimed after a minute, like the other optional targets.
- The shadow variant runs `surface()` only for a shader whose source uses `discard`, with the camera-dependent built-ins neutral there, and `no_shadow` opts out. A shadow-casting shader that reads `TIME` redraws its shadows every frame.
- LOD cards bake the shader at its defaults and `TIME` 0 and rebake when the shader or the values change; a card's own Material may run a shader live.
- Values live in one per-Material table in the snapshot, so animating a uniform on a Material shared by many Models costs one entry, not a re-resolve of every Model.
- Attributes are the user's call. Within it, mine: an Instance is an allowed attribute value (held by GUID), which Roblox does not allow, because a texture parameter needs one and reference properties already work that way; Matrix4 is allowed for the same reason Vector3 is; the whole map is one hidden JSON property and one undo step per set; a wrong-kind attribute is a warning on the row and the shader reads its default, never a draw-time error; a shader's parameter rows come before the plain Attributes group in Properties.
- A new `Attributes` group with Add and Remove appears on every instance's Properties, not only shader holders, since the feature is general and that is what makes it discoverable.

## Left to the builder

The hint grammar's exact tokenizer; how `ShaderParameters` is shared with the snapshot (by value with the version is fine); the GL log line-number patterns to match (NVIDIA `0(12)`, Mesa and Intel `0:12(3)`, Apple `ERROR: 0:12:`); the reference picker's Kind filter; the highlighter's word lists; the starter source text; where the second ping-pong targets sit in `ensureTargets`'s pool.
