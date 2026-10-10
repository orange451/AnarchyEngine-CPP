# Property groups in the Properties panel

## Goal

The Properties panel shows related properties together under foldable group
headers. Within each group, properties appear in the order the class
registers them. Today every property except Name, Parent, and ClassName goes
into one "Data" section, sorted alphabetically.

## Registration

A new helper, `lua_group("Appearance")`, returns a marker `LuaField`. A class
places markers in the field array it already passes to `register_lua_class`:

```cpp
LuaField fields[] = {
    lua_group("Appearance"),
    lua_saved_property("Brightness", ...),
    lua_slider(lua_saved_property("Exposure", ...), 0.0, 2.0),
    lua_group("Quality"),
    lua_saved_enum("Antialiasing", ...),
};
```

- `LuaField` gets a new `const char* group = nullptr`, plus a `group_marker`
  flag that only `lua_group` sets.
- `register_lua_class` walks the array, sets `group` on each field to the name
  of the latest marker above it, and does not store the markers. Markers never
  appear in member lists, Lua completion, or MCP output.
- A field with no marker above it in its array keeps `group == nullptr`.
  Separate `register_lua_class` calls for the same class each start without a
  group.
- When a field replaces an earlier field with the same name, the new field's
  group applies.

## Display order

`read_sheet` (src/ide/PropertySheet.cpp) builds the groups from the first
selected instance's `lua_class_members`, where base-class fields come first in
registration order:

1. **Instance** always comes first: Name, Parent, ClassName, in the existing
   `kInstanceRows` order, whatever group they were registered in.
2. Next come the named groups, ordered by where each name first appears in the
   member list. Groups that share a name merge into one. A derived class that
   adds to a base class's "Appearance" puts its fields after the base's.
3. **Data** comes last and holds every field with `group == nullptr`, so
   classes nobody has grouped yet still show all their properties.

Within a group, rows keep member-list order. The alphabetical sort is removed.

When several instances are selected, rows are filtered as they are today, and
the group and order come from the first instance's class. A group left with no
rows is not shown.

## Panel

- `PropertyGroup` (enum) is replaced by `std::string group` on `PropertyRow`.
  The sheet carries the ordered list of group names it shows.
- `PropertiesPanel` builds one foldable header per group in place of the fixed
  `kGroupTitles` Instance/Data headers. The panel-only "Preview" section stays
  last.
- Fold state is keyed by group name, so a folded group stays folded when the
  selection changes to another class that has the same group.
- `same_layout` compares group names as well as rows, so a selection change
  that changes the groups rebuilds the panel.

## Groups for existing classes

This change gives groups to the classes below. Groups are listed in display
order, and fields within a group are in registration order. Some groups are
written in a base class and extended by a subclass, so a subclass's fields
come after the base's within those groups. A field that a class registers
again takes the group named at its new place. Fields are reordered inside the
`LuaField` arrays to match. Saves store properties by name, and reference
slots use fixed indices, so reordering changes nothing on disk.

- **Material**
  - Surface: DiffuseTexture, NormalTexture, RoughnessTexture, MetalnessTexture
  - Modifier: Color, Transparency, Roughness, Metalness, Reflectivity,
    EmissiveTexture, Emissive
  - Terrain: TextureScale, BlendSharpness, HeightTexture, HeightStrength
- **Lighting**
  - Appearance: Ambient, Brightness, Exposure, Saturation, Gamma, ToneMapping
  - Quality: ShadingModel, Antialiasing, TerrainQuality
- **GameObject**
  - Behavior: Prefab
  - Appearance: Color, Transparency
  - Transform: Transform, Scale
- **PointLight and SpotLight** (from GameObject)
  - Behavior: Enabled, Shadows. These merge into GameObject's Behavior, so
    they come right after Prefab.
  - Light: Color, Intensity, Radius, plus OuterFOV and InnerFOVScale on
    SpotLight
- **DirectionalLight**
  - Behavior: Enabled, Shadows, ShadowDistance
  - Light: Direction, Color, Intensity
- **PhysicsBase and subclasses**
  - Transform: Transform
  - Physics: Anchored, CanCollide, Mass, Friction, Bounciness
  - Motion: Velocity, AngularVelocity, LinearDamping, AngularDamping
  - Shape: Shape, Size, Mesh
  - Behavior: GameObject
  - Brush adds Appearance: Color, Transparency.
  - PlayerController adds Character: Radius, Height, StepHeight, MaxSlope,
    OnGround, IsSliding.
- **SoundEmitter**
  - Playback: Sound, Volume, Pitch, Looped, TimePosition, IsPlaying
  - Roll-off: RollOffMode, RollOffMinDistance, RollOffMaxDistance
- **DynamicSky**
  - Time: TimeOfDay, Latitude
  - Sun & Moon: Brightness, Shadows, SunTexture, SunSize, MoonTexture,
    MoonSize
  - Clouds: CloudCover, CloudDensity, WindDirection
  - Quality: ReflectionQuality
- **GuiBase and subclasses**
  - Layout: Size, Alignment, Spacing
  - Appearance: BackgroundColor, BackgroundTransparency, Image,
    ImageTransparency
  - Text: Text, TextColor, FontSize, TextScaled, Prompt
  - Style: ClassList, Style
  - Behavior: Visible, MouseTransparent, AlwaysOnTop, Adornee

All other classes stay ungrouped, and their properties show under Data: the
effects, Skybox, Camera, Terrain, Attachment, Dragger, the file assets, Model,
Script, Slider, DockWidget, and Workspace.

## Testing

- Registration: markers set each field's group, markers do not show up in
  `lua_class_members`, fields with no marker have no group, and a replacing
  field takes the new group.
- `read_sheet`: Instance comes first; groups follow first-appearance order;
  base and derived groups with the same name merge with the base's fields
  first; registration order holds within a group; ungrouped fields go into a
  trailing Data group; empty groups are dropped.
- Panel: one header per group, and fold state survives a selection change
  between classes that share a group. Checked live in the studio over MCP.
