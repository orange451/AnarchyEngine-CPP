# Property Groups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Properties panel shows each class's properties under named, foldable groups, in registration order, in place of the alphabetical "Data" list.

**Architecture:** A `lua_group("Name")` marker goes into the `LuaField` arrays. `register_lua_class` writes the marker's name into each following field's new `group` member and drops the marker. `read_sheet` orders the rows: Instance first, then named groups in the order they first appear in the base-first member list (rows within a group keep member order), then the ungrouped fields as "Data". `PropertiesPanel` makes one foldable header for each group name it meets, and remembers which are folded by name.

**Tech Stack:** C++20, MSVC, CMake, jadefx UI. Tests are plain executables with an `Expect()` helper (`tests/PropertiesTest.cpp`, target `properties-tests`).

**Spec:** `docs/superpowers/specs/2026-10-09-property-groups-design.md`

## Global Constraints

- Distances are "units", never "studs", in comments and UI text.
- Saves store properties by name. Reordering fields must not change any save file, default, or reference slot index (`read_reference<N>` keeps its N).
- Markers never show up in `lua_class_members`, `lua_class_own_members`, Lua completion, or MCP output.
- Name, Parent, ClassName are always the "Instance" group, in that order, at the top.
- Ungrouped fields go in a trailing group titled "Data".
- "Preview" stays the panel's last section.
- Every count passed to `register_lua_class` for an array that gains markers becomes `static_cast<int>(std::size(array))`. A hardcoded count would cut fields off.
- When a class registers a field again (Pane's Size, Brush's Anchored, SpotLight reusing PointLight's fields), its own array needs the marker above it, or the field drops to Data.
- Build: `cmake --build build --config Debug --target properties-tests`, run: `build/Debug/properties-tests.exe`. Finish with a Release build of `AnarchyStudio` (and `AnarchyPlayer`).

## Review Focus

1. **A field registered again in a subclass without a marker** silently moves to Data. Expected: it stays in its group. Task 3's sheet-order tests on PointLight, Brush, and Pane pin this.
2. **Multi-select of two classes with different groups** (a Part and a PointLight). Expected: rows and groups come from the first instance's class, with no crash and no duplicate headers. Task 2 pins it with a two-class selection.
3. **Folding a group, then selecting a class without that group, then going back.** Expected: still folded, and no stale header left visible. Task 2's fold test pins it.
4. **A group whose rows all get filtered out** (`shown_when` hides them, or a multi-select intersection drops them). Expected: no empty header. Task 2 pins it.
5. **A hardcoded `register_lua_class` count left after markers are added** cuts the last fields off. Task 3's sheet-order tests catch a missing last field.

---

### Task 1: `lua_group` markers in the registry

**Files:**
- Modify: `src/engine_core/LuaApi.hpp` (`LuaField` struct, around lines 55-112; helpers near `lua_hidden`, around line 177)
- Modify: `src/engine_core/LuaApi.cpp:152-174` (`register_lua_class`)
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Produces: `LuaField::group` (`const char*`, `nullptr` when ungrouped), `LuaField::group_marker` (`bool`), and `inline LuaField lua_group(const char* name)`.

- [ ] **Step 1: Write the failing test**

Add these to `tests/PropertiesTest.cpp`, after `RegisterProbe()`. The test classes subclass `Probe`, so they can reuse `ReadSpeed`/`WriteSpeed`/`ReadFlag`/`WriteFlag`.

```cpp
// Two classes with groups. GroupKid adds to GroupBase's "Look", registers
// Alpha again under "Extra", and has one field with no group.
class GroupBase : public Probe {
public:
    using Probe::Probe;
    const char* class_name() const override { return "GroupBase"; }
};

class GroupKid : public Probe {
public:
    using Probe::Probe;
    const char* class_name() const override { return "GroupKid"; }
};

void RegisterGroups() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    using engine_core::lua_group;
    using engine_core::lua_property;
    const engine_core::LuaField base[] = {
        lua_group("Look"),
        lua_property("Zeta", "number", true, ReadSpeed, WriteSpeed),
        lua_property("Alpha", "number", true, ReadSpeed, WriteSpeed),
        lua_group("Feel"),
        lua_property("Mid", "boolean", true, ReadFlag, WriteFlag),
    };
    engine_core::register_lua_class("GroupBase", "DataModel", base, static_cast<int>(std::size(base)));
    const engine_core::LuaField kid[] = {
        lua_property("Loose", "number", true, ReadSpeed, WriteSpeed),
        lua_group("Look"),
        lua_property("Kappa", "number", true, ReadSpeed, WriteSpeed),
        lua_group("Extra"),
        lua_property("Alpha", "number", true, ReadSpeed, WriteSpeed),
        lua_property("Beta", "number", true, ReadSpeed, WriteSpeed),
    };
    engine_core::register_lua_class("GroupKid", "GroupBase", kid, static_cast<int>(std::size(kid)));
}

const char* GroupOf(const std::vector<engine_core::LuaField>& fields, const char* name) {
    for (const engine_core::LuaField& field : fields) {
        if (field.name != nullptr && std::string_view(field.name) == name) {
            return field.group != nullptr ? field.group : "";
        }
    }
    return "missing";
}

void TestGroupMarkersRegister() {
    RegisterGroups();
    std::vector<engine_core::LuaField> members;
    engine_core::lua_class_members("GroupKid", members);
    Expect(std::string_view(GroupOf(members, "Zeta")) == "Look", "a field takes the marker above it");
    Expect(std::string_view(GroupOf(members, "Mid")) == "Feel", "a later marker starts a new group");
    Expect(std::string_view(GroupOf(members, "Loose")).empty(), "a field above any marker has no group");
    Expect(std::string_view(GroupOf(members, "Alpha")) == "Extra", "a replacing field takes its new group");
    for (const engine_core::LuaField& field : members) {
        Expect(!field.group_marker, "markers are not members");
        Expect(field.name == nullptr ||
                   (std::string_view(field.name) != "Look" && std::string_view(field.name) != "Feel" &&
                    std::string_view(field.name) != "Extra"),
               "no member is named after a group");
    }
    std::vector<engine_core::LuaField> own;
    engine_core::lua_class_own_members("GroupKid", own);
    Expect(own.size() == 4, "own members are the four fields, no markers");
}
```

Add `#include <iterator>` to the includes, and call `TestGroupMarkersRegister();` first in `main()`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --config Debug --target properties-tests`
Expected: compile error, `lua_group` / `group` / `group_marker` not members of `engine_core`.

- [ ] **Step 3: Implement**

In `LuaApi.hpp`, add to `LuaField` after `bool hidden = false;`:

```cpp
    // The Properties group the field shows under, named by the latest
    // lua_group above it in its register_lua_class array. Null shows it
    // under "Data".
    const char* group = nullptr;
    // A lua_group marker. register_lua_class reads it and does not keep it.
    bool group_marker = false;
```

Next to `lua_hidden`, add:

```cpp
// A marker for a register_lua_class array: the fields after it, up to the
// next marker, show under the Properties group `name`, in array order.
inline LuaField lua_group(const char* name) {
    LuaField field;
    field.name = name;
    field.group_marker = true;
    return field;
}
```

In `LuaApi.cpp`, replace the field loop in `register_lua_class`:

```cpp
    const char* group = nullptr;
    for (int index = 0; fields != nullptr && index < count; ++index) {
        if (fields[index].group_marker) {
            group = fields[index].name;
            continue;
        }
        LuaField field = fields[index];
        field.group = group;
        changed = append_unique(record->fields, field) || changed;
        if (!field.method && field.name != nullptr) {
            lua_property_id(field.name);
        }
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --config Debug --target properties-tests && build/Debug/properties-tests.exe`
Expected: `properties tests passed`

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/LuaApi.hpp src/engine_core/LuaApi.cpp tests/PropertiesTest.cpp
git commit -m "Registry: lua_group markers name each field's Properties group"
```

---

### Task 2: Sheet ordering and panel headers by group name

**Files:**
- Modify: `src/ide/PropertySheet.hpp:36-37` (remove `PropertyGroup`), `:72` (`PropertyRow::group`), and the `operator==` / `same_slot` implementations in `src/ide/PropertySheet.cpp`
- Modify: `src/ide/PropertySheet.cpp:27-37`, `:377`, `:428-436`
- Modify: `src/ide/PropertiesPanel.cpp:75-79`, `:582-587`, `:662-685`, `:976-986`, `:1323-1365`, `:1970-1985`, `:2084`, `:2336-2342`
- Modify: `src/ide/PropertiesPanel.hpp:72`, `:132-133`
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: `LuaField::group` from Task 1.
- Produces: `std::string PropertyRow::group` (`"Instance"`, a registered group name, or `"Data"`), `std::vector<std::string> PropertiesPanel::group_titles() const` (the headers shown, top to bottom, Preview included when shown), and `group_header(title)` working for any group title.

- [ ] **Step 1: Write the failing tests**

```cpp
std::vector<std::string> RowOrder(const ide::PropertySheet& sheet) {
    std::vector<std::string> names;
    for (const ide::PropertyRow& row : sheet.rows) {
        names.push_back(row.group + ":" + row.name);
    }
    return names;
}

void TestSheetGroupOrder() {
    RegisterGroups();
    Rig rig;
    GroupKid& kid = rig.game.create<GroupKid>();
    const std::vector<std::string> expected = {
        "Instance:Name", "Instance:Parent", "Instance:ClassName",
        "Look:Zeta",     "Look:Kappa",
        "Extra:Alpha",   "Extra:Beta",
        "Feel:Mid",
        "Data:Loose",
    };
    Expect(RowOrder(ide::read_sheet(rig.game, {kid.id()})) == expected,
           "Instance, then groups by first appearance with base rows first, then Data");

    // Mixed classes: the first instance's groups, no repeated header.
    GroupBase& base = rig.game.create<GroupBase>();
    const ide::PropertySheet mixed = ide::read_sheet(rig.game, {base.id(), kid.id()});
    const std::vector<std::string> mixed_expected = {
        "Instance:Name", "Instance:Parent", "Instance:ClassName", "Look:Zeta", "Extra:Alpha", "Feel:Mid",
    };
    Expect(RowOrder(mixed) == mixed_expected, "a mixed selection uses the first class's groups");
}

void TestGroupTitlesAndEmptyGroups() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    RegisterGroups();
    Rig rig;
    GroupKid& kid = rig.game.create<GroupKid>();
    GroupBase& base = rig.game.create<GroupBase>();
    rig.select({kid.id()});
    rig.frame();
    const std::vector<std::string> titles = {"Instance", "Look", "Extra", "Feel", "Data"};
    Expect(rig.panel.group_titles() == titles, "one header per group, in sheet order");

    rig.click(rig.panel.group_header("Look"));
    rig.frame();
    Expect(!rig.panel.editor("Zeta")->isVisible(), "a named group folds");
    rig.select({base.id(), kid.id()});
    rig.frame();
    const std::vector<std::string> mixed_titles = {"Instance", "Look", "Extra", "Feel"};
    Expect(rig.panel.group_titles() == mixed_titles, "a group with no rows left has no header");
    Expect(!rig.panel.editor("Zeta")->isVisible(), "Look stays folded across selections");
    rig.select({kid.id()});
    rig.frame();
    Expect(!rig.panel.editor("Kappa")->isVisible(), "and when coming back");
    rig.click(rig.panel.group_header("Look"));
    rig.frame();
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}
```

If `Rig` has no `create<T>` path, create them as `TestBooleanAndNumber` creates `Probe` (`rig.game.create<Probe>()`, around line 980). In `TestGroupsFold`, change `group_header("Data")` to `group_header("Modifier")` and the message "Data's rows show" to "Modifier's rows show". Material's Roughness and Color are in Modifier once Task 3 lands; until then this assertion fails, and Task 3 makes it pass. Call both new tests in `main()` after `TestGroupMarkersRegister()`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --config Debug --target properties-tests`
Expected: compile error, `group_titles` is not a member and `row.group` is not a string.

- [ ] **Step 3: Implement the sheet**

In `PropertySheet.hpp`, delete `enum class PropertyGroup` and its comment. Change the member to:

```cpp
    // "Instance", the field's registered group (lua_group), or "Data".
    std::string group = "Data";
```

Make `PropertyRow::operator==` and `same_slot` (in `PropertySheet.cpp`) also compare `group`, so moving a row to another group rebuilds the panel.

In `read_sheet`, replace line 377 with:

```cpp
        row.group = instance_rank(row.name) >= 0 ? "Instance" : field.group != nullptr ? field.group : "Data";
```

Replace the `stable_sort` at the end:

```cpp
    // Instance first, then groups in the order the member list first names
    // them, then Data. Rows in a group keep member order, base fields first.
    std::vector<std::string> order;
    for (const PropertyRow& row : sheet.rows) {
        if (row.group != "Instance" && row.group != "Data" &&
            std::find(order.begin(), order.end(), row.group) == order.end()) {
            order.push_back(row.group);
        }
    }
    const auto rank = [&order](const PropertyRow& row) -> std::size_t {
        if (row.group == "Instance") {
            return 0;
        }
        if (row.group == "Data") {
            return order.size() + 1;
        }
        return 1 + static_cast<std::size_t>(std::find(order.begin(), order.end(), row.group) - order.begin());
    };
    std::stable_sort(sheet.rows.begin(), sheet.rows.end(), [&](const PropertyRow& a, const PropertyRow& b) {
        const std::size_t ra = rank(a);
        const std::size_t rb = rank(b);
        if (ra != rb) {
            return ra < rb;
        }
        return ra == 0 && instance_rank(a.name) < instance_rank(b.name);
    });
```

Fix the comment at `PropertySheet.cpp:26` to say that every other row takes its field's group, or Data.

- [ ] **Step 4: Implement the panel**

In `PropertiesPanel.cpp`, replace the `kGroups` / `kPreviewGroup` / `kGroupTitles` constants with:

```cpp
// The panel's own last section, after every property group.
constexpr const char* kPreviewGroup = "Preview";
```

Replace `headers[kGroups]`, `header_arrows[kGroups]`, `group_folded[kGroups]` with:

```cpp
    // A section header: its title and the arrow that folds it. Made the
    // first time a sheet names the group, then kept.
    struct GroupHeader {
        std::shared_ptr<jadefx::Label> label;
        std::shared_ptr<PropertyDisclosure> arrow;
    };
    std::map<std::string, GroupHeader> headers;
    // Folded groups by title, kept across selections.
    std::set<std::string> folded_groups;
    // The headers placed by the last layout, top to bottom.
    std::vector<std::string> shown_groups;
```

Turn the creation loop at 662-684 into a method, and call `header(kPreviewGroup)` where the loop was:

```cpp
    GroupHeader& header(const std::string& title) {
        const auto found = headers.find(title);
        if (found != headers.end()) {
            return found->second;
        }
        std::weak_ptr<Impl> weak_self = weak_from_this();
        // The title and its arrow both fold it.
        auto fold = [weak_self, title](const jadefx::MouseEvent&) {
            if (const auto self = weak_self.lock()) {
                self->toggle_group(title);
            }
        };
        GroupHeader made;
        made.label = jadefx::make<jadefx::Label>(title);
        made.label->getClassList().add("properties-group");
        made.label->setStyle(
            "padding: 0 6px 0 20px; background-color: var(--ide-properties-group-color); "
            "color: var(--ide-properties-group-text-color);");
        made.label->setCursor(jadefx::Cursor::Pointer);
        made.label->setOnMouseClicked(fold);
        made.label->setVisible(false);
        body->getChildren().add(made.label);
        // After the title, so it paints over the title's background.
        made.arrow = jadefx::make<PropertyDisclosure>();
        made.arrow->setStyle("color: var(--ide-properties-group-text-color);");
        made.arrow->setOnMouseClicked(fold);
        made.arrow->setVisible(false);
        body->getChildren().add(made.arrow);
        return headers.emplace(title, std::move(made)).first->second;
    }
```

`preview_header = header(kPreviewGroup).label;`

Then:
- `open(view)`: `return !folded_groups.count(view.row.group) && (...)`. Delete `group_index`.
- `toggle_group(const std::string& title)`: if the erase from `folded_groups` removes something, return (it was folded, so this opens it). Otherwise insert the title, return if `title == kPreviewGroup`, and run the existing per-row loop over rows where `view->row.group == title`.
- `place_header(Place& place, const std::string& title, double left, double width)`: get `GroupHeader& h = header(title)`, place `*h.label` and `*h.arrow` as before, set `h.arrow->open = !folded_groups.count(title)`, `shown_groups.push_back(title)`, and return open.
- In the layout pass (around 1970): hide every entry in `headers` and clear `shown_groups`. Track `std::string group` instead of `PropertyGroup group`. Call `place_header(place, group, left, width)`.
- Line 2084: `place_header(place, kPreviewGroup, left, width)`.
- `same_layout`: unchanged. Rows now compare group through `same_slot`.
- `group_header(title)`: return `impl_->headers` lookup's `label.get()`, or `nullptr`.
- Add `std::vector<std::string> PropertiesPanel::group_titles() const { return impl_->shown_groups; }`. Declare it in `PropertiesPanel.hpp` under `group_header`, with the comment `// The section headers the last layout showed, top to bottom.` Update the header comments at :72 and :132 to describe the Instance, the class's groups, Data, and Preview.

Add `#include <map>` and `#include <set>` if they are missing.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --config Debug --target properties-tests && build/Debug/properties-tests.exe`
Expected: everything passes except `FAIL Modifier's rows show` and the fold checks that follow it in `TestGroupsFold` (Material has no groups yet). Write those down and go on. Task 3 closes them.

- [ ] **Step 6: Commit**

```bash
git add src/ide/PropertySheet.hpp src/ide/PropertySheet.cpp src/ide/PropertiesPanel.hpp src/ide/PropertiesPanel.cpp tests/PropertiesTest.cpp
git commit -m "Properties: a header per registered group, in registration order"
```

---

### Task 3: Groups for the existing classes

**Files:**
- Modify: `src/engine_instances/AssetInstances.cpp:892-926` (Material)
- Modify: `src/engine_services/Lighting.cpp:~300-328`
- Modify: `src/engine_instances/GameObject.cpp:~340-354`
- Modify: `src/engine_instances/Light.cpp:~355-414` (PointLight, SpotLight, DirectionalLight)
- Modify: `src/engine_instances/PhysicsBase.cpp:~230-245`, `PhysicsObject.cpp:~160-173`, `Brush.cpp:~385-403`, `PlayerController.cpp:~100-112`
- Modify: `src/engine_instances/SoundEmitter.cpp:~270-288`
- Modify: `src/engine_instances/DynamicSky.cpp:~295-314`
- Modify: `src/engine_instances/Gui.cpp:~555-625` (GuiBase, BillboardGui, GuiBasePane, Pane, ImagePane, HBox, VBox, Label, Button, TextField)
- Test: `tests/PropertiesTest.cpp`

**Interfaces:**
- Consumes: `lua_group` (Task 1), group-ordered `read_sheet` (Task 2).

Reorder each array to match the list below, and put `lua_group("…")` markers in. Change the elements' order only; keep every element's arguments as they are. Every array that gains a marker passes `static_cast<int>(std::size(array))`. For a class that registers through a helper (Gui's `add_class`), check that the helper passes the array straight through to `register_lua_class`.

| Class | Array order, with markers |
|---|---|
| Material | `lua_group("Surface")` DiffuseTexture, NormalTexture, RoughnessTexture, MetalnessTexture · `lua_group("Modifier")` Color, Transparency, Roughness, Metalness, Reflectivity, EmissiveTexture, Emissive · `lua_group("Terrain")` TextureScale, BlendSharpness, HeightTexture, HeightStrength |
| Lighting | `Appearance` Ambient, Brightness, Exposure, Saturation, Gamma, ToneMapping · `Quality` ShadingModel, Antialiasing, TerrainQuality |
| GameObject | `Behavior` Prefab · `Appearance` Color, Transparency · `Transform` Transform, Scale |
| PointLight | `Behavior` Enabled, Shadows · `Light` Color, Intensity, Radius · (hidden Scale stays where it is, under no new marker) |
| SpotLight | `Behavior` Enabled, Shadows · `Light` Color, Intensity, Radius, OuterFOV, InnerFOVScale |
| DirectionalLight | `Behavior` Enabled, Shadows, ShadowDistance · `Light` Direction, Color, Intensity |
| PhysicsBase | `Transform` Transform · `Physics` Anchored, Mass · `Motion` Velocity, LinearDamping · `Behavior` GameObject |
| PhysicsObject | `Physics` Friction, Bounciness · `Motion` AngularVelocity, AngularDamping · `Shape` Shape, Size, Mesh |
| Brush | `Physics` Anchored, CanCollide, Friction, Bounciness · `Motion` AngularVelocity, AngularDamping · `Appearance` Color, Transparency · (hidden Faces last, no marker) |
| PlayerController | `Physics` Friction · `Character` Radius, Height, StepHeight, MaxSlope, OnGround, IsSliding |
| SoundEmitter | `Playback` Sound, Volume, Pitch, Looped, TimePosition, IsPlaying · `Roll-off` RollOffMode, RollOffMinDistance, RollOffMaxDistance |
| DynamicSky | `Time` TimeOfDay, Latitude · `Sun & Moon` Brightness, Shadows, SunTexture, SunSize, MoonTexture, MoonSize · `Clouds` CloudCover, CloudDensity, WindDirection · `Quality` ReflectionQuality |
| GuiBase | `Layout` Size, Alignment · `Style` ClassList, Style · `Behavior` Visible, MouseTransparent |
| BillboardGui | `Behavior` Adornee, AlwaysOnTop |
| GuiBasePane | `Appearance` BackgroundColor, BackgroundTransparency |
| Pane | `Layout` Size |
| ImagePane | `Layout` Size · `Appearance` Image, ImageTransparency |
| HBox, VBox | `Layout` Spacing |
| Label | `Text` Text, TextColor, FontSize, TextScaled |
| Button | `Text` Text, TextScaled |
| TextField | `Text` Text, Prompt |

Group order on screen follows first appearance, base first. For GuiBase subclasses, that gives Layout, Style, Behavior, then the subclass's Appearance or Text. That's fine. The spec lists the groups each class has; it doesn't pin GUI header order beyond base-first.

- [ ] **Step 1: Write the failing tests**

```cpp
// The sheet's group:name rows for one new instance of class_name, without
// the Instance rows.
std::vector<std::string> GroupedRows(Rig& rig, const char* class_name, const char* parent_service) {
    const InstanceId id = MakeAsset(rig.game, class_name, "Probe", rig.game.service(parent_service));
    std::vector<std::string> out;
    for (const ide::PropertyRow& row : ide::read_sheet(rig.game, {id}).rows) {
        if (row.group != "Instance") {
            out.push_back(row.group + ":" + row.name);
        }
    }
    return out;
}

void TestClassGroups() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const std::vector<std::string> material = {
        "Surface:DiffuseTexture", "Surface:NormalTexture", "Surface:RoughnessTexture", "Surface:MetalnessTexture",
        "Modifier:Color", "Modifier:Transparency", "Modifier:Roughness", "Modifier:Metalness",
        "Modifier:Reflectivity", "Modifier:EmissiveTexture", "Modifier:Emissive",
        "Terrain:TextureScale", "Terrain:BlendSharpness", "Terrain:HeightTexture", "Terrain:HeightStrength",
    };
    Expect(GroupedRows(rig, "Material", "Materials") == material, "Material's groups");

    const std::vector<std::string> point = GroupedRows(rig, "PointLight", "Workspace");
    Expect(point.size() >= 3 && point[0] == "Behavior:Prefab" && point[1] == "Behavior:Enabled" &&
               point[2] == "Behavior:Shadows",
           "a PointLight opens with Prefab, Enabled, Shadows");
    Expect(std::find(point.begin(), point.end(), "Light:Color") != point.end(),
           "a light's own Color is in Light");

    const std::vector<std::string> brush = GroupedRows(rig, "Brush", "Workspace");
    Expect(std::find(brush.begin(), brush.end(), "Physics:Anchored") != brush.end(),
           "Brush's Anchored, registered again, stays in Physics");
    for (const std::string& row : brush) {
        Expect(row.rfind("Data:", 0) != 0, "Brush has nothing left in Data");
    }

    const std::vector<std::string> pane = GroupedRows(rig, "Pane", "Gui");
    Expect(std::find(pane.begin(), pane.end(), "Layout:Size") != pane.end(), "Pane's Size stays in Layout");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}
```

`MakeAsset` is the existing helper used by `TestGroupsFold`. If it rejects non-asset classes or a parent service name is wrong (check `rig.game.service` names in other tests: "Workspace", "Gui", "Materials"), use `rig.game.create<T>()` for those classes, the way `TestBooleanAndNumber` makes `Probe`. Call `TestClassGroups()` in `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --config Debug --target properties-tests && build/Debug/properties-tests.exe`
Expected: `FAIL Material's groups`, the PointLight/Brush/Pane failures, and the `TestGroupsFold` "Modifier" failures from Task 2.

- [ ] **Step 3: Reorder and mark every class in the table**

Example, Material:

```cpp
    const LuaField material_fields[] = {
        lua_group("Surface"),
        lua_saved_property("DiffuseTexture", "Texture?", read_reference<0>, write_reference<0>, "null"),
        lua_saved_property("NormalTexture", "Texture?", read_reference<1>, write_reference<1>, "null"),
        lua_saved_property("RoughnessTexture", "Texture?", read_reference<2>, write_reference<2>, "null"),
        lua_saved_property("MetalnessTexture", "Texture?", read_reference<3>, write_reference<3>, "null"),
        lua_group("Modifier"),
        lua_saved_property("Color", "Color3", read_material_color<&Material::color>,
                           write_material_color<&Material::set_color>, color.c_str()),
        lua_slider(lua_saved_property("Transparency", ...unchanged...), 0.0, 1.0),
        lua_slider(lua_saved_property("Roughness", ...unchanged...), 0.0, 1.0),
        lua_slider(lua_saved_property("Metalness", ...unchanged...), 0.0, 1.0),
        lua_slider(lua_saved_property("Reflectivity", ...unchanged...), 0.0, 1.0),
        lua_saved_property("EmissiveTexture", "Texture?", read_reference<4>, write_reference<4>, "null"),
        lua_saved_property("Emissive", "Color3", ...unchanged...),
        lua_group("Terrain"),
        lua_slider(lua_saved_property("TextureScale", ...unchanged...), 0.0, 16.0),
        lua_slider(lua_saved_property("BlendSharpness", ...unchanged...), 0.0, 1.0),
        lua_saved_property("HeightTexture", "Texture?", read_reference<5>, write_reference<5>, "null"),
        lua_slider(lua_saved_property("HeightStrength", ...unchanged...), 0.0, 1.0),
    };
```

("…unchanged…" means move the existing element text as is. Don't retype it.) `kMaterialRefs` keeps its order, because the indices in `read_reference<N>` already pin each slot.

SpotLight reuses PointLight's array elements by index. Rebuild its array with its own markers: `lua_group("Behavior")`, its Enabled and Shadows elements, `lua_group("Light")`, then Color, Intensity, Radius, OuterFOV, InnerFOVScale. If it picks elements by index, update the indices to PointLight's new order.

- [ ] **Step 4: Run all the affected tests**

Run: `cmake --build build --config Debug --target properties-tests && build/Debug/properties-tests.exe`
Expected: `properties tests passed`.

Then make sure nothing else depended on registration order:
Run: `cmake --build build --config Debug --target studio-tests && build/Debug/studio-tests.exe`
Expected: pass. Known flakes are listed in memory: "six frames", the jadefx tree-view, and the DS1/DS3 CloudCover default.

Also run any test target whose sources grep finds using `lua_class_members` or `lua_saved_fields` (for example `LuauCompleteTest.cpp`'s target). Expect a pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances src/engine_services tests/PropertiesTest.cpp
git commit -m "Group the properties of Material, Lighting, lights, physics, sound, sky, and GUI"
```

---

### Task 4: Release build and a live check

**Files:** none changed unless the check turns up a bug.

- [ ] **Step 1: Release build**

Run: `cmake --build build --config Release --target AnarchyStudio AnarchyPlayer`
Expected: builds cleanly.

- [ ] **Step 2: Live check over MCP**

Launch `build/Release/AnarchyStudio.exe` with the MCP port and token (see the memory "Drive studio over MCP"). Check:
- Select a Material: the headers read Instance, Surface, Modifier, Terrain, Preview, with the rows in the spec's order.
- Select a PointLight: Behavior (Prefab, Enabled, Shadows) is first.
- Fold Modifier, select another Material: Modifier is still folded. Unfold it.

Take one screenshot of the Material panel and show it to the user with SendUserFile. The screenshot tool leaves overlays out, which is fine here.

- [ ] **Step 3: Commit any fix with a test for it, then finish the branch with superpowers:finishing-a-development-branch.**
