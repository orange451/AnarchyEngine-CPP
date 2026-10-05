# Ambient Occlusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An `AmbientOcclusionEffect` under Lighting shades creases and contact with GTAO, at a Quality (`Enum.EffectQuality`) that trades cost for sharpness.

**Architecture:**

- `AmbientOcclusionEffect` is carried as `VisualAmbientOcclusion` → `runner::SceneOcclusion`.
- After the G-buffer, `occlusionPass` runs at the Quality's scale (2 or 1):
  - `gtao.frag` writes an R8 visibility;
  - `ao_blur.frag` blurs it across and down.
  Both compute in full-resolution coordinates and only sample on the Quality's grid.
- `occlusion.glsl`'s `occlusionAt(uv, depth)` upsamples it with depth awareness and raises it to Intensity. `ibl.frag` and `merge.frag` both read it.
- `skyLight` and `skyReflection` take the occlusion:
  - diffuse uses multi-bounce;
  - reflection uses specular occlusion.
  So the IBL pass and the SSR merge agree.
- `OcclusionMath` holds the formulas in pure C++.

**Tech Stack:** C++17 (Apple clang 13), OpenGL 3.3 core / GLSL 330 within GLSL ES 3.00, Catch2 (`sandbox`), `engine-tests`, `scene-render-check`.

**Spec:** `docs/superpowers/specs/2026-10-05-ambient-occlusion-design.md`. Background, algorithm and references: `docs/superpowers/plans/2026-10-02-ambient-occlusion.md`.

## Global Constraints

- Branch `ambient-occlusion` in `~/Documents/AnarchyEngine-CPP-ao`, with its own `build/`. Never commit on `main`. Never point `FETCHCONTENT_BASE_DIR` at the main checkout.
- Properties:
  - Enabled true; Intensity 1 (0–4); Radius 1 (0–10 studs); Quality `Enum.EffectQuality.Medium`.
  - Messages: "`<Property>` must be a finite number", "Quality must be an Enum.EffectQuality", "An AmbientOcclusionEffect must be in Lighting".
- `Enum.EffectQuality`: `Low` 0, `Medium` 1, `High` 2.
- Quality settings: Low (scale 2, 2 slices, blur radius 6); Medium (scale 2, 3 slices, blur radius 4); High (scale 1, 3 slices, blur radius 4). 6 steps per side for all.
- No instance, Enabled false, Intensity 0, or Radius 0: exactly today's pixels, no AO buffers, no AO passes.
- A program that cannot draw yet gives a frame without AO, never a failed frame.
- Programs are validated once until they or their buffers are made again (`occlusionValid_`), as `reflectionsValid_` is.
- GL errors are fatal in JadeFX (the host UI toolkit).

## Review Focus

1. **An open floor seen at a grazing angle** must not darken from depth precision. Task 4's "open floor" check.
2. **Halos at silhouettes.** The floor seen just past the cube's top edge, far behind it, must not be shaded. Task 4's "past the top edge" check, after the blur and the half-size upsample.
3. **A camera almost touching a surface** must give no NaN-black pixels. Task 4's "camera close" check.
4. **SSR and AO together.** Where SSR misses, the floor must look exactly as with AO alone. Task 4's "SSR together" check.
5. **Changing Quality at run time** must rebuild the buffers at the new scale, with no stale sizes. Task 4's "Quality change" check.

---

### Task 1: Enum.EffectQuality and the AmbientOcclusionEffect instance

**Files:**
- Create: `src/engine_instances/AmbientOcclusionEffect.hpp`, `src/engine_instances/AmbientOcclusionEffect.cpp`, `sandbox/ambient_occlusion_tests.cpp`
- Modify: `src/engine_datatypes/Enum.hpp`, `src/engine_datatypes/Enum.cpp`, `CMakeLists.txt` (engine_instances after `ScreenSpaceReflections.cpp`; sandbox after `sandbox/reflection_math_tests.cpp`), `src/engine_core/Containment.cpp`, `src/engine_core/Project.cpp`, `src/engine_core/ScriptBindings.cpp`, `src/engine_core/LuaApi.cpp`, `tests/LuauCompleteTest.cpp`, `README.md`, `src/engine_instances/README.md`

**Interfaces:**
- Produces:
  - `const engine_core::EnumType& engine_core::effect_quality_enum();` and `enum class engine_core::EffectQuality { Low = 0, Medium = 1, High = 2 };`
  - `engine_core::AmbientOcclusionEffect`:
    - getters `bool enabled()`, `double intensity()`, `double radius()`, `EffectQuality quality()`;
    - setters `set_enabled(bool)`, `set_intensity(double)`, `set_radius(double)`, `set_quality(int)`, each returning `std::optional<std::string>`;
    - constants `kDefaultEnabled`, `kDefaultIntensity` (1), `kMaxIntensity` (4), `kDefaultRadius` (1), `kMaxRadius` (10), `kDefaultQuality` (`EffectQuality::Medium`).

- [ ] **Step 1: Write the failing tests**

Create `sandbox/ambient_occlusion_tests.cpp`:

```cpp
// AmbientOcclusionEffect: shading where nearby geometry hides the sky, under
// Lighting, whose Enabled, Intensity, Radius, and Quality the snapshot carries.

#include "support.hpp"

#include "AmbientOcclusionEffect.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::AmbientOcclusionEffect;
using engine_core::EffectQuality;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

AmbientOcclusionEffect& add_occlusion(engine_core::DataModel& game, InstanceId parent) {
    AmbientOcclusionEffect& ao = game.create<AmbientOcclusionEffect>();
    game.set_parent(ao.id(), parent);
    return ao;
}

}  // namespace

TEST_CASE("AO0 Enum.EffectQuality has Low, Medium, and High", "[occlusion]") {
    const engine_core::EnumType& type = engine_core::effect_quality_enum();
    REQUIRE(engine_core::enum_item_value(type, "Low") == 0);
    REQUIRE(engine_core::enum_item_value(type, "Medium") == 1);
    REQUIRE(engine_core::enum_item_value(type, "High") == 2);
    REQUIRE(engine_core::enum_item_name(type, 3) == nullptr);
}

TEST_CASE("AO1 an AmbientOcclusionEffect's properties are checked, undo, save, and come back at Stop", "[occlusion]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("AmbientOcclusionEffect"));
    AmbientOcclusionEffect& ao = add_occlusion(game, game.scene_service("Lighting"));
    REQUIRE(ao.enabled());
    REQUIRE(ao.intensity() == 1.0);
    REQUIRE(ao.radius() == 1.0);
    REQUIRE(ao.quality() == EffectQuality::Medium);

    engine_core::PropertyBag saved;
    ao.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set Radius");
    REQUIRE_FALSE(ao.set_radius(2.5));
    end_step(game);
    REQUIRE(ao.radius() == 2.5);
    game.history().undo();
    REQUIRE(ao.radius() == 1.0);
    game.history().redo();
    REQUIRE(ao.radius() == 2.5);

    begin_step(game, "Set Quality");
    REQUIRE_FALSE(ao.set_quality(2));
    end_step(game);
    REQUIRE(ao.quality() == EffectQuality::High);
    game.history().undo();
    REQUIRE(ao.quality() == EffectQuality::Medium);
    REQUIRE(*ao.set_quality(5) == "Quality must be an Enum.EffectQuality");

    REQUIRE_FALSE(ao.set_intensity(9.0));
    REQUIRE(ao.intensity() == AmbientOcclusionEffect::kMaxIntensity);
    REQUIRE_FALSE(ao.set_intensity(-1.0));
    REQUIRE(ao.intensity() == 0.0);
    REQUIRE_FALSE(ao.set_radius(50.0));
    REQUIRE(ao.radius() == AmbientOcclusionEffect::kMaxRadius);
    REQUIRE_FALSE(ao.set_radius(-1.0));
    REQUIRE(ao.radius() == 0.0);
    REQUIRE(*ao.set_intensity(std::nan("")) == "Intensity must be a finite number");
    REQUIRE(*ao.set_radius(INFINITY) == "Radius must be a finite number");

    REQUIRE_FALSE(ao.set_enabled(false));
    REQUIRE_FALSE(ao.set_intensity(2.0));
    REQUIRE_FALSE(ao.set_radius(3.0));
    REQUIRE_FALSE(ao.set_quality(0));
    engine_core::PropertyBag changed;
    ao.save_properties(changed);
    for (const char* name : {"Enabled", "Intensity", "Radius", "Quality"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(ao.set_radius(0.5));
    REQUIRE_FALSE(ao.set_quality(2));
    game.stop_simulation();
    REQUIRE(ao.radius() == 3.0);
    REQUIRE(ao.quality() == EffectQuality::Low);
    REQUIRE_FALSE(ao.enabled());
}

TEST_CASE("AO2 an AmbientOcclusionEffect belongs under Lighting and nowhere else", "[occlusion]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    const std::string message = "An AmbientOcclusionEffect must be in Lighting";
    REQUIRE_FALSE(placement_error("Lighting", "AmbientOcclusionEffect", "Occlusion"));
    REQUIRE(reason(placement_error("Workspace", "AmbientOcclusionEffect", "Occlusion")) == message);

    const InstanceId lighting = game.scene_service("Lighting");
    AmbientOcclusionEffect& ao = add_occlusion(game, lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(ao.id(), folder.id()));
    REQUIRE(reason(game.parent_error(ao.id(), workspace_of(game))) == message);
    REQUIRE(engine_core::parent_suits("Lighting", "AmbientOcclusionEffect"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "AmbientOcclusionEffect"));
}

TEST_CASE("AO4 scripts make an AmbientOcclusionEffect and set it", "[occlusion]") {
    ScriptRig rig;
    add_script(rig.game, "Shade", R"(
        local ao = Instance.new("AmbientOcclusionEffect", game.Lighting)
        _G.defaults = ao.Enabled == true and ao.Intensity == 1 and ao.Radius == 1
            and ao.Quality == Enum.EffectQuality.Medium
        ao.Intensity = 2
        ao.Radius = 20
        ao.Quality = Enum.EffectQuality.High
        _G.set = ao.Intensity == 2 and ao.Radius == 10 and ao.Quality == Enum.EffectQuality.High
        ao.Quality = 0
        _G.by_value = ao.Quality == Enum.EffectQuality.Low
        ao.Quality = "Medium"
        _G.by_name = ao.Quality == Enum.EffectQuality.Medium
        _G.refused = not pcall(function() ao.Radius = 0 / 0 end)
            and not pcall(function() ao.Quality = Enum.TransformSpace.World end)
            and not pcall(function() ao.Quality = "Ultra" end)
            and not pcall(function() Instance.new("AmbientOcclusionEffect", workspace) end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"defaults", "set", "by_value", "by_name", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}
```

Add it to the sandbox list and run `cmake -S . -B build`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: `'AmbientOcclusionEffect.hpp' file not found`.

- [ ] **Step 3: Add the enum**

`Enum.hpp`, after the `AntialiasingMode` lines:

```cpp
// How much an effect spends for how good it looks: Low 0, Medium 1, High 2.
// AmbientOcclusionEffect.Quality; later effects share it.
const EnumType& effect_quality_enum();
// effect_quality_enum's items, by value.
enum class EffectQuality { Low = 0, Medium = 1, High = 2 };
```

`Enum.cpp`:
- after `kAntialiasingModeType`, add:

  ```cpp
  // How much an effect spends for how good it looks, shared by effects.
  const EnumEntry kEffectQualities[] = {{"Low", 0}, {"Medium", 1}, {"High", 2}};
  const EnumType kEffectQualityType{"EffectQuality", kEffectQualities, count_of(kEffectQualities)};
  ```

- append `&kEffectQualityType` to `kTypes`;
- add `const EnumType& effect_quality_enum() { return kEffectQualityType; }` after `antialiasing_mode_enum()`.

- [ ] **Step 4: Write the class**

Create `src/engine_instances/AmbientOcclusionEffect.hpp`:

```cpp
#pragma once

#include "DataModel.hpp"
#include "Enum.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Shading where nearby geometry hides the sky and the ambient light from a
// surface: in creases, under objects, where a wall meets a floor. Lights are
// not shaded by it; they have shadows. Like a Skybox it belongs under
// Lighting, at any depth through Folders, and nowhere else; when Lighting
// holds more than one, the first in the tree is used.
//
// Enabled    boolean             false shades nothing. True.
// Intensity  number              an exponent on how open a surface is: 1 is
//                                physical, above 1 darker, 0 none. 1, from 0
//                                to kMaxIntensity.
// Radius     number              how far, in studs, an occluder still counts;
//                                it fades over the last 60%. 1, from 0 to
//                                kMaxRadius.
// Quality    Enum.EffectQuality  Low and Medium shade at half resolution,
//                                High at full. Medium.
//
// Each is a saved registry property (lua_saved_property). The render
// snapshot reads them at every Prepare (VisualAmbientOcclusion).
class AmbientOcclusionEffect : public DataModel {
public:
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultIntensity = 1.0;
    static constexpr double kMaxIntensity = 4.0;
    static constexpr double kDefaultRadius = 1.0;
    static constexpr double kMaxRadius = 10.0;
    static constexpr EffectQuality kDefaultQuality = EffectQuality::Medium;

    AmbientOcclusionEffect(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
        : DataModel(tag, state, id) {}
    const char* class_name() const override { return "AmbientOcclusionEffect"; }

    bool enabled() const { return enabled_; }
    double intensity() const { return intensity_; }
    double radius() const { return radius_; }
    EffectQuality quality() const { return quality_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused, and the others are
    // clamped to their ranges; a Quality that is not an Enum.EffectQuality's
    // value is refused.
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_intensity(double value);
    std::optional<std::string> set_radius(double value);
    std::optional<std::string> set_quality(int value);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double max);

    bool enabled_ = kDefaultEnabled;
    double intensity_ = kDefaultIntensity;
    double radius_ = kDefaultRadius;
    EffectQuality quality_ = kDefaultQuality;
};

}  // namespace engine_core
```

Create `src/engine_instances/AmbientOcclusionEffect.cpp`:

```cpp
#include "AmbientOcclusionEffect.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

LuaSlot quality_slot(EffectQuality quality) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &effect_quality_enum();
    slot.number = static_cast<int>(quality);
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("AmbientOcclusionEffect setters run on SimulationThread");
    }
}

}  // namespace

std::optional<std::string> AmbientOcclusionEffect::set_enabled(bool value) {
    require_thread(*this);
    if (enabled_ == value) {
        return std::nullopt;
    }
    enabled_ = value;
    note_property_change("Enabled", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> AmbientOcclusionEffect::set_number(const char* property, double& slot, double value,
                                                              double max) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(property) + " must be a finite number";
    }
    value = std::clamp(value, 0.0, max);
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    note_property_change(property, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> AmbientOcclusionEffect::set_intensity(double value) {
    return set_number("Intensity", intensity_, value, kMaxIntensity);
}

std::optional<std::string> AmbientOcclusionEffect::set_radius(double value) {
    return set_number("Radius", radius_, value, kMaxRadius);
}

std::optional<std::string> AmbientOcclusionEffect::set_quality(int value) {
    require_thread(*this);
    if (enum_item_name(effect_quality_enum(), value) == nullptr) {
        return std::string("Quality must be an Enum.EffectQuality");
    }
    const EffectQuality next = static_cast<EffectQuality>(value);
    if (next == quality_) {
        return std::nullopt;
    }
    const EffectQuality previous = quality_;
    quality_ = next;
    note_property_change("Quality", quality_slot(previous), quality_slot(next));
    return std::nullopt;
}

void AmbientOcclusionEffect::on_reuse() {
    enabled_ = kDefaultEnabled;
    intensity_ = kDefaultIntensity;
    radius_ = kDefaultRadius;
    quality_ = kDefaultQuality;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

AmbientOcclusionEffect* occlusion_of(DataModel& object) { return dynamic_cast<AmbientOcclusionEffect*>(&object); }

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    out = bool_slot(ao->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    AmbientOcclusionEffect* ao = occlusion_of(object);
    return ao != nullptr && refuse(in, ao->set_enabled(in.flag));
}

template <double (AmbientOcclusionEffect::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    out = number_slot((ao->*Get)());
    return true;
}

template <std::optional<std::string> (AmbientOcclusionEffect::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    AmbientOcclusionEffect* ao = occlusion_of(object);
    return ao != nullptr && refuse(in, (ao->*Set)(in.number));
}

bool read_quality(DataModel&, DataModel& object, LuaSlot& out) {
    const AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    out = quality_slot(ao->quality());
    return true;
}

bool write_quality(DataModel&, DataModel& object, LuaSlot& in) {
    AmbientOcclusionEffect* ao = occlusion_of(object);
    if (ao == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &effect_quality_enum()) {
        in.error = "Quality must be an Enum.EffectQuality";
        return false;
    }
    return refuse(in, ao->set_quality(static_cast<int>(in.number)));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_ambient_occlusion_effect_lua) {
    // The defaults, as a file would hold them, from the class's own constants.
    static const std::string intensity = number_json(AmbientOcclusionEffect::kDefaultIntensity);
    static const std::string radius = number_json(AmbientOcclusionEffect::kDefaultRadius);
    using AO = AmbientOcclusionEffect;
    const LuaField fields[] = {
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled, AO::kDefaultEnabled ? "true" : "false"),
        lua_slider(lua_saved_property("Intensity", "number", read_number<&AO::intensity>,
                                      write_number<&AO::set_intensity>, intensity.c_str()),
                   0.0, AO::kMaxIntensity),
        lua_slider(lua_saved_property("Radius", "number", read_number<&AO::radius>, write_number<&AO::set_radius>,
                                      radius.c_str()),
                   0.0, AO::kMaxRadius),
        lua_saved_enum("Quality", effect_quality_enum(), read_quality, write_quality, "\"Medium\""),
    };
    register_lua_class("AmbientOcclusionEffect", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("AmbientOcclusionEffect", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
```

Add `src/engine_instances/AmbientOcclusionEffect.cpp` to `engine_instances` after `ScreenSpaceReflections.cpp`.

- [ ] **Step 5: Register it**

- `Containment.cpp`: the rule becomes

  ```cpp
      // These affect the whole place, so they live where the place's lighting does.
      if ((child_class == "Skybox" || child_class == "BloomEffect" || child_class == "ScreenSpaceReflections" ||
           child_class == "AmbientOcclusionEffect") &&
          holder_class != "Lighting") {
          const bool vowel = child_class.front() == 'A';
          return std::string(vowel ? "An " : "A ") + std::string(child_class) + " must be in Lighting";
      }
  ```

  Check that the existing messages for Skybox, BloomEffect and ScreenSpaceReflections are unchanged: none starts with A.
- `Project.cpp`: include `AmbientOcclusionEffect.hpp` (first in the sorted list, after `AssetInstances.hpp`). Then:

  ```cpp
          out.push_back({"AmbientOcclusionEffect",
                         [](DataModel& world) -> DataModel& { return world.create<AmbientOcclusionEffect>(); }});
  ```

  It goes after the ScreenSpaceReflections `push_back`.
- `ScriptBindings.cpp`: include it. Add `DataModel& create_ambient_occlusion_effect(DataModel& world) { return world.create<AmbientOcclusionEffect>(); }` and `register_lua_creatable("AmbientOcclusionEffect", create_ambient_occlusion_effect);` beside the SSR ones.
- `LuaApi.cpp`, after the ScreenSpaceReflections docs:

  ```cpp
      add("AmbientOcclusionEffect", "Enabled", "When false, this AmbientOcclusionEffect shades nothing.", "boolean",
          false, {});
      add("AmbientOcclusionEffect", "Intensity",
          "How strongly creases and contact are shaded, from 0 to 4. 1 is physical; above 1 is darker.", "number",
          false, {});
      add("AmbientOcclusionEffect", "Radius",
          "How far, in studs, from 0 to 10, nearby geometry still hides the sky from a surface.", "number", false, {});
      add("AmbientOcclusionEffect", "Quality",
          "Enum.EffectQuality: Low and Medium shade at half resolution, High at full.", "Enum.EffectQuality", false,
          {});
  ```

- `LuauCompleteTest.cpp` `testInsertFilter`: insert `"AmbientOcclusionEffect",` before `"Attachment",`.
- `README.md`: after the ScreenSpaceReflections paragraph, add:

  ```markdown
  `AmbientOcclusionEffect` shades surfaces where nearby geometry hides the sky and the ambient light from them, in the Scene View and the player: in creases, under objects, where a wall meets a floor, so things look grounded. Lights are not shaded by it; they have shadows. It lives only under `Lighting` (through Folders too); anywhere else it is refused with "An AmbientOcclusionEffect must be in Lighting". `Intensity` (0 to 4, 1 by default) is an exponent on how open a surface is, darker above 1. `Radius` (0 to 10 studs, 1 by default) is how far an occluder still counts. `Quality`, an `Enum.EffectQuality`, is `Low` or `Medium` (the default) at half resolution, or `High` at full. See-through surfaces are not shaded. With none, nothing runs and the frame costs what it did.
  ```

- `src/engine_instances/README.md` line 3: add `` `AmbientOcclusionEffect`, `` after `` `ScreenSpaceReflections`, ``.

- [ ] **Step 6: Run the tests to see them pass**

```bash
cmake --build build --parallel && ./build/sandbox "[occlusion],[reflections],[bloom],[skybox]" && ./build/engine-tests
```

Expected: all pass. If `engine-tests` fails on a filter list (as "scr" did for SSR), update that expectation to include the new class in its documented order, and ledger a ruling.

- [ ] **Step 7: Commit**

```bash
git add -A src/engine_instances/AmbientOcclusionEffect.* src/engine_datatypes/Enum.* sandbox/ambient_occlusion_tests.cpp CMakeLists.txt \
  src/engine_core/Containment.cpp src/engine_core/Project.cpp src/engine_core/ScriptBindings.cpp src/engine_core/LuaApi.cpp \
  tests/LuauCompleteTest.cpp README.md src/engine_instances/README.md
git commit -m "Add AmbientOcclusionEffect and Enum.EffectQuality

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Carry it in the render snapshot

**Files:** `src/engine_core/SnapshotPump.hpp`, `src/engine_core/SnapshotPump.cpp`, `src/runner/SceneFeed.cpp`, `tests/SceneFeedTest.cpp`, `sandbox/ambient_occlusion_tests.cpp`

**Interfaces:**
- Produces:
  - `engine_core::VisualAmbientOcclusion { bool present = false; bool enabled = true; float intensity = 1.f; float radius = 1.f; int quality = 1; }`.
  - `VisualSnapshot::occlusion`.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/ambient_occlusion_tests.cpp`:

```cpp
TEST_CASE("AO3 the snapshot carries the first AmbientOcclusionEffect under Lighting", "[occlusion][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE_FALSE(pump.front().occlusion.present);

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    AmbientOcclusionEffect& first = add_occlusion(game, folder.id());
    AmbientOcclusionEffect& second = add_occlusion(game, lighting);
    REQUIRE_FALSE(first.set_enabled(false));
    REQUIRE_FALSE(first.set_intensity(2.0));
    REQUIRE_FALSE(first.set_radius(3.0));
    REQUIRE_FALSE(first.set_quality(2));
    REQUIRE_FALSE(second.set_radius(7.0));
    frame();
    {
        const engine_core::VisualAmbientOcclusion& ao = pump.front().occlusion;
        REQUIRE(ao.present);
        REQUIRE_FALSE(ao.enabled);
        REQUIRE(ao.intensity == 2.f);
        REQUIRE(ao.radius == 3.f);
        REQUIRE(ao.quality == 2);
    }
    game.destroy(first.id());
    frame();
    REQUIRE(pump.front().occlusion.radius == 7.f);
    REQUIRE(pump.front().occlusion.quality == 1);
    game.destroy(second.id());
    frame();
    REQUIRE_FALSE(pump.front().occlusion.present);
}
```

In `tests/SceneFeedTest.cpp`:
- after the `snapshot.reflections.max_distance` line, add `snapshot.occlusion.present = true;` and `snapshot.occlusion.radius = static_cast<float>(number);`;
- in `Whole`, after the reflections condition, add `snapshot.occlusion.present && snapshot.occlusion.radius == static_cast<float>(snapshot.frame) &&`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox engine-tests --parallel`
Expected: `no member named 'occlusion' in 'engine_core::VisualSnapshot'`.

- [ ] **Step 3: Implement**

`SnapshotPump.hpp`, after `VisualReflections`:

```cpp
// The first AmbientOcclusionEffect under Lighting, in tree order, as the
// renderer reads it. present is false with none, and nothing is shaded.
struct VisualAmbientOcclusion {
    bool present = false;
    bool enabled = true;
    float intensity = 1.f;
    // Studs.
    float radius = 1.f;
    // Enum.EffectQuality's value: Low 0, Medium 1, High 2.
    int quality = 1;
};
```

Add `VisualAmbientOcclusion occlusion;` after `VisualReflections reflections;`, and name `base_.occlusion` in `resolve_lighting`'s comment.

`SnapshotPump.cpp`:
- include `AmbientOcclusionEffect.hpp`;
- add a static_assert tying `VisualAmbientOcclusion{}` to `AmbientOcclusionEffect::kDefault*`, with `quality == static_cast<int>(kDefaultQuality)` and the message "a place with no AmbientOcclusionEffect carries its defaults";
- in `resolve_lighting`, after the reflections block:

```cpp
    const AmbientOcclusionEffect* shading =
        lighting != nullptr ? find_first<AmbientOcclusionEffect>(game, lighting->id()) : nullptr;
    VisualAmbientOcclusion& occlusion = base_.occlusion;
    occlusion = VisualAmbientOcclusion{};
    if (shading != nullptr) {
        occlusion.present = true;
        occlusion.enabled = shading->enabled();
        occlusion.intensity = static_cast<float>(shading->intensity());
        occlusion.radius = static_cast<float>(shading->radius());
        occlusion.quality = static_cast<int>(shading->quality());
    }
```

- in `blit`, add `dst.occlusion = base_.occlusion;`.

`SceneFeed.cpp`: add `out->occlusion = front.occlusion;` after `out->reflections = front.reflections;`.

- [ ] **Step 4: Run them to see them pass**

```bash
cmake --build build --parallel && ./build/sandbox "[occlusion],[reflections],[bloom],[skybox]" && ./build/engine-tests
```

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/SnapshotPump.hpp src/engine_core/SnapshotPump.cpp src/runner/SceneFeed.cpp tests/SceneFeedTest.cpp sandbox/ambient_occlusion_tests.cpp
git commit -m "Carry the first AmbientOcclusionEffect under Lighting in the render snapshot

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: OcclusionMath

**Files:**
- Create: `src/runner/OcclusionMath.hpp`, `src/runner/OcclusionMath.cpp`, `sandbox/occlusion_math_tests.cpp`
- Modify: `CMakeLists.txt` (`STUDIO_CORE_SOURCES` after `ReflectionMath.cpp`; sandbox after `sandbox/ambient_occlusion_tests.cpp`)

**Interfaces:**
- Produces, in `runner`:
  - `struct OcclusionQuality { int scale; int slices; int blurRadius; };` and `OcclusionQuality QualitySettings(int quality);` (quality is `Enum.EffectQuality`'s value);
  - `constexpr int kOcclusionStepsPerSide = 6;`, `constexpr float kOcclusionMaxRadiusFraction = 0.25f;`, `constexpr float kOcclusionFalloffRange = 0.6f;`;
  - `float PixelRadius(float radius, float viewDepth, float projectionScale, float bufferHeight);`
  - `float Falloff(float distance, float radius);`
  - `float ArcVisibility(float n, float h0, float h1, float projectedLength);`
  - `float MultiBounce(float visibility, float albedo);`
  - `float SpecularOcclusion(float visibility, float NdotV, float roughness);`

- [ ] **Step 1: Write the failing tests**

Create `sandbox/occlusion_math_tests.cpp`:

```cpp
// OcclusionMath: ambient occlusion's settings and formulas, with no GL.
// gtao.frag, ao_blur.frag, occlusion.glsl, and image_lighting.glsl copy them.

#include "runner/OcclusionMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using namespace runner;

namespace {
constexpr float kHalfPi = 1.57079632679f;
}

TEST_CASE("OM1 each Quality's resolution, slices, and blur", "[occlusion]") {
    const OcclusionQuality low = QualitySettings(0);
    const OcclusionQuality medium = QualitySettings(1);
    const OcclusionQuality high = QualitySettings(2);
    REQUIRE((low.scale == 2 && low.slices == 2 && low.blurRadius == 6));
    REQUIRE((medium.scale == 2 && medium.slices == 3 && medium.blurRadius == 4));
    REQUIRE((high.scale == 1 && high.slices == 3 && high.blurRadius == 4));
    // Anything else is Medium.
    REQUIRE(QualitySettings(9).slices == 3);
}

TEST_CASE("OM2 the pixel radius shrinks with depth and stops at a quarter of the buffer", "[occlusion]") {
    REQUIRE(PixelRadius(1.f, 10.f, 500.f, 1000.f) == Approx(50.f));
    REQUIRE(PixelRadius(1.f, 20.f, 500.f, 1000.f) == Approx(25.f));
    REQUIRE(PixelRadius(1.f, 0.01f, 500.f, 1000.f) == Approx(250.f));
}

TEST_CASE("OM3 the falloff is 1 within 40% of Radius, 0 at Radius, and smooth between", "[occlusion]") {
    REQUIRE(Falloff(0.f, 1.f) == 1.f);
    REQUIRE(Falloff(0.4f, 1.f) == Approx(1.f));
    REQUIRE(Falloff(1.f, 1.f) == Approx(0.f).margin(1e-6));
    REQUIRE(Falloff(2.f, 1.f) == 0.f);
    float previous = 1.f;
    for (float d = 0.4f; d <= 1.f; d += 0.01f) {
        const float f = Falloff(d, 1.f);
        REQUIRE(f <= previous + 1e-6f);
        previous = f;
    }
}

TEST_CASE("OM4 the arc integral: open is 1, a wall at the view direction half", "[occlusion]") {
    // A surface facing the camera, nothing above its tangent plane on either side.
    REQUIRE(ArcVisibility(0.f, -kHalfPi, kHalfPi, 1.f) == Approx(1.f));
    // One side closed straight up the view direction.
    REQUIRE(ArcVisibility(0.f, -kHalfPi, 0.f, 1.f) == Approx(0.5f));
    REQUIRE(ArcVisibility(0.f, 0.f, 0.f, 1.f) == Approx(0.f).margin(1e-6));
}

TEST_CASE("OM5 multi-bounce never darkens below the visibility, and white loses least", "[occlusion]") {
    REQUIRE(MultiBounce(1.f, 0.f) == 1.f);
    REQUIRE(MultiBounce(1.f, 1.f) == 1.f);
    for (float v = 0.f; v <= 1.f; v += 0.1f) {
        INFO(v);
        REQUIRE(MultiBounce(v, 0.f) >= v - 1e-6f);
        REQUIRE(MultiBounce(v, 1.f) >= MultiBounce(v, 0.f) - 1e-6f);
    }
}

TEST_CASE("OM6 specular occlusion is 1 when open, and falls faster for smooth surfaces", "[occlusion]") {
    REQUIRE(SpecularOcclusion(1.f, 0.5f, 0.2f) == 1.f);
    REQUIRE(SpecularOcclusion(0.5f, 0.5f, 0.05f) < SpecularOcclusion(0.5f, 0.5f, 0.9f));
    REQUIRE(SpecularOcclusion(0.f, 0.f, 0.5f) == 0.f);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --target sandbox --parallel`
Expected: `'runner/OcclusionMath.hpp' file not found`.

- [ ] **Step 3: Implement**

`src/runner/OcclusionMath.hpp`:

```cpp
#pragma once

// Ambient occlusion's settings and formulas, worked out with no GL context so
// they can be tested. gtao.frag, ao_blur.frag, occlusion.glsl, and
// image_lighting.glsl copy them line for line.
namespace runner {

constexpr int kOcclusionStepsPerSide = 6;
// No sample reaches farther than this part of the buffer's height.
constexpr float kOcclusionMaxRadiusFraction = 0.25f;
// Samples fade out over this last part of the radius.
constexpr float kOcclusionFalloffRange = 0.6f;

struct OcclusionQuality {
    // The buffers are the pane's size divided by this: 2 is half, 1 is full.
    int scale = 2;
    int slices = 3;
    int blurRadius = 4;
};
// Enum.EffectQuality's value: Low 0, Medium 1, High 2. Anything else is Medium.
OcclusionQuality QualitySettings(int quality);
// How many full-size pixels Radius covers at a view depth, capped at
// kOcclusionMaxRadiusFraction of bufferHeight. projectionScale is pixels per
// stud at depth 1.
float PixelRadius(float radius, float viewDepth, float projectionScale, float bufferHeight);
// How much a sample this far away counts: 1 out to 40% of radius, falling
// linearly to 0 at radius.
float Falloff(float distance, float radius);
// GTAO's closed-form, cosine-weighted visible arc of one slice between the
// horizon angles h0 and h1 (radians from the view direction), for a normal
// at angle n in the slice whose projection has length projectedLength.
float ArcVisibility(float n, float h0, float h1, float projectedLength);
// Jimenez et al. 2016's fit for light bouncing between occluders, for one
// channel of albedo: never below visibility, and exactly 1 when open.
float MultiBounce(float visibility, float albedo);
// Lagarde and de Rousiers 2014: how much of a reflection the occlusion
// keeps; 1 when open, falling faster for smooth surfaces.
float SpecularOcclusion(float visibility, float NdotV, float roughness);

}  // namespace runner
```

`src/runner/OcclusionMath.cpp`:

```cpp
#include "OcclusionMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

OcclusionQuality QualitySettings(int quality) {
    switch (quality) {
        case 0:
            return {2, 2, 6};
        case 2:
            return {1, 3, 4};
        default:
            return {2, 3, 4};
    }
}

float PixelRadius(float radius, float viewDepth, float projectionScale, float bufferHeight) {
    return std::min(radius * projectionScale / viewDepth, kOcclusionMaxRadiusFraction * bufferHeight);
}

float Falloff(float distance, float radius) {
    const float range = kOcclusionFalloffRange * radius;
    const float from = radius - range;
    return std::clamp(distance * (-1.f / range) + (from / range + 1.f), 0.f, 1.f);
}

float ArcVisibility(float n, float h0, float h1, float projectedLength) {
    const float arc0 = std::cos(n) + 2.f * h0 * std::sin(n) - std::cos(2.f * h0 - n);
    const float arc1 = std::cos(n) + 2.f * h1 * std::sin(n) - std::cos(2.f * h1 - n);
    return projectedLength * 0.25f * (arc0 + arc1);
}

float MultiBounce(float visibility, float albedo) {
    if (visibility >= 1.f) {
        return 1.f;
    }
    const float a = 2.0404f * albedo - 0.3324f;
    const float b = -4.7951f * albedo + 0.6417f;
    const float c = 2.7552f * albedo + 0.6903f;
    return std::max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

float SpecularOcclusion(float visibility, float NdotV, float roughness) {
    if (visibility >= 1.f) {
        return 1.f;
    }
    return std::clamp(std::pow(NdotV + visibility, std::exp2(-16.f * roughness - 1.f)) - 1.f + visibility, 0.f, 1.f);
}

}  // namespace runner
```

- [ ] **Step 4: Run them to see them pass**

Run: `cmake --build build --target sandbox --parallel && ./build/sandbox "[occlusion]"`
Expected: OM1–OM6 and AO0–AO4 pass. If OM4's 0.5 case is off, recompute it by hand from the formula before changing either. The formula is the reference's.

- [ ] **Step 5: Commit**

```bash
git add src/runner/OcclusionMath.hpp src/runner/OcclusionMath.cpp sandbox/occlusion_math_tests.cpp CMakeLists.txt
git commit -m "Add OcclusionMath: ambient occlusion's settings and formulas, with no GL

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The passes, and shading the sky and ambient light

**Files:**
- Create: `resources/shaders/pipeline/gtao.frag`, `resources/shaders/pipeline/ao_blur.frag`, `resources/shaders/pipeline/occlusion.glsl`
- Modify: `resources/shaders/pipeline/image_lighting.glsl`, `ibl.frag`, `forward.frag`, `merge.frag`, `ssr.frag`, `src/runner/gl.hpp`, `src/runner/Renderer.hpp`, `src/runner/Renderer.cpp`, `src/runner/GameView.cpp`, `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Consumes: `VisualAmbientOcclusion` (Task 2); `QualitySettings`, `kOcclusion*` (Task 3).
- Produces:
  - `enum class runner::SceneQuality { Low = 0, Medium = 1, High = 2 };`
  - `runner::SceneOcclusion { bool enabled = false; float intensity = 1.f; float radius = 1.f; SceneQuality quality = SceneQuality::Medium; }`, and `SceneLighting::occlusion`.

- [ ] **Step 1: Write the failing pixel checks**

In `tests/SceneRenderCheck.cpp`, after the SSR block's closing brace, add:

```cpp
            // Ambient occlusion: a cube resting on a wide floor slab.
            {
                runner::SceneLighting plain;
                plain.antialiasing = runner::SceneAntialiasing::None;
                runner::MeshDraw floor = draw;
                floor.model = engine_core::matrix4_identity();
                floor.model.m[0] = 20.f;
                floor.model.m[5] = 0.1f;
                floor.model.m[10] = 20.f;
                floor.model.m[13] = -0.55f;
                const runner::MeshDraw scene[] = {draw, floor};
                const auto drawScene = [&](const runner::MeshDraw* meshes, int count) {
                    for (int pass = 0; pass < 2; ++pass) {
                        renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count);
                    }
                };
                const auto snap = [&] {
                    runner::ViewPixels pixels;
                    renderer.read(0, 0, kSize, kSize, kSize, kSize, pixels);
                    return pixels.rgba;
                };
                // Where a world point lands, with the default camera, in framebuffer pixels.
                const engine_core::Vec3 up{0.f, 1.f, 0.f};
                const engine_core::Matrix4 view = engine_core::matrix4_inverse(engine_core::matrix4_look_at(
                    {runner::Renderer::kCameraEye[0], runner::Renderer::kCameraEye[1], runner::Renderer::kCameraEye[2]},
                    {0.f, 0.f, 0.f}, up));
                const float focal = 1.f / std::tan(0.5f * runner::Renderer::kCameraFovYDegrees * 0.01745329252f);
                const auto at = [&](float x, float y, float z) {
                    const engine_core::Vec3 p = engine_core::matrix4_point(view, {x, y, z});
                    const int px = static_cast<int>((focal * p.x / -p.z * 0.5f + 0.5f) * static_cast<float>(fbWidth));
                    const int py = static_cast<int>((focal * p.y / -p.z * 0.5f + 0.5f) * static_cast<float>(fbHeight));
                    return ReadPixel(px, py);
                };
                renderer.setLighting(plain);
                drawScene(scene, 2);
                const std::vector<unsigned char> unshaded = snap();
                const Pixel contactOff = at(0.f, -0.5f, 0.58f);
                const Pixel openOff = at(2.2f, -0.5f, 2.2f);
                const Pixel pastTopOff = at(0.f, -0.5f, -4.f);

                runner::SceneLighting shaded = plain;
                shaded.occlusion.enabled = true;
                renderer.setLighting(shaded);
                const bool first = renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2);
                Expect(first, "the first frame with ambient occlusion still draws");
                drawScene(scene, 2);
                Expect(Sum(at(0.f, -0.5f, 0.58f)) + 10 < Sum(contactOff),
                       "the floor where the cube stands on it is shaded (" + Text(at(0.f, -0.5f, 0.58f)) + " vs " +
                           Text(contactOff) + ")");
                Expect(std::abs(Sum(at(2.2f, -0.5f, 2.2f)) - Sum(openOff)) <= 2,
                       "the open floor is not shaded (" + Text(at(2.2f, -0.5f, 2.2f)) + ")");
                Expect(std::abs(Sum(at(0.f, -0.5f, -4.f)) - Sum(pastTopOff)) <= 2,
                       "the floor seen just past the cube's top edge, far behind it, is not shaded (" +
                           Text(at(0.f, -0.5f, -4.f)) + ")");

                // Each Quality shades the contact; High and Medium agree on it.
                int contact[3] = {};
                for (int quality = 0; quality < 3; ++quality) {
                    runner::SceneLighting q = shaded;
                    q.occlusion.quality = static_cast<runner::SceneQuality>(quality);
                    renderer.setLighting(q);
                    drawScene(scene, 2);
                    contact[quality] = Sum(at(0.f, -0.5f, 0.58f));
                    Expect(contact[quality] + 10 < Sum(contactOff),
                           "Quality " + std::to_string(quality) + " shades the contact");
                }
                Expect(std::abs(contact[2] - contact[1]) <= 12, "High and Medium agree on the contact (" +
                                                                    std::to_string(contact[2]) + " vs " +
                                                                    std::to_string(contact[1]) + ")");

                // Off four ways: exactly the frame without it.
                for (int way = 0; way < 4; ++way) {
                    runner::SceneLighting off = shaded;
                    if (way == 0) {
                        off.occlusion.enabled = false;
                    } else if (way == 1) {
                        off.occlusion.intensity = 0.f;
                    } else if (way == 2) {
                        off.occlusion.radius = 0.f;
                    } else {
                        off = plain;
                    }
                    renderer.setLighting(off);
                    drawScene(scene, 2);
                    Expect(snap() == unshaded, "off draws exactly as without occlusion (way " + std::to_string(way) + ")");
                }

                // A camera almost at the floor: no black (NaN) pixels that were not black without it.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, -0.45f, 1.5f}, {0.f, -0.5f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
                renderer.setLighting(plain);
                drawScene(scene, 2);
                const std::vector<unsigned char> closeOff = snap();
                renderer.setLighting(shaded);
                drawScene(scene, 2);
                const std::vector<unsigned char> closeOn = snap();
                int newBlack = 0;
                for (std::size_t i = 0; i + 3 < closeOn.size(); i += 4) {
                    const bool black = closeOn[i] == 0 && closeOn[i + 1] == 0 && closeOn[i + 2] == 0;
                    const bool wasBlack = closeOff[i] == 0 && closeOff[i + 1] == 0 && closeOff[i + 2] == 0;
                    newBlack += black && !wasBlack ? 1 : 0;
                }
                Expect(newBlack == 0, "a camera almost at a surface leaves no black pixels (" +
                                          std::to_string(newBlack) + ")");
                renderer.setCamera(engine_core::matrix4_inverse(view), runner::Renderer::kCameraFovYDegrees);

                // With SSR too, a mirror floor where rays find nothing matches occlusion alone.
                runner::MeshDraw mirror = floor;
                mirror.metalness = 1.f;
                mirror.roughness = 0.f;
                const runner::MeshDraw mirrored[] = {draw, mirror};
                renderer.setLighting(shaded);
                drawScene(mirrored, 2);
                const Pixel besideAlone = at(-0.62f, -0.5f, 0.f);
                runner::SceneLighting both = shaded;
                both.reflections.enabled = true;
                renderer.setLighting(both);
                drawScene(mirrored, 2);
                const Pixel besideBoth = at(-0.62f, -0.5f, 0.f);
                Expect(std::abs(Sum(besideBoth) - Sum(besideAlone)) <= 1,
                       "where SSR misses, the floor matches occlusion alone (" + Text(besideBoth) + " vs " +
                           Text(besideAlone) + ")");

                // Size and Quality changes make the buffers again.
                renderer.setLighting(shaded);
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, kSize / 2.0, kSize / 2.0, kSize, kSize, scene, 2);
                }
                runner::SceneLighting high = shaded;
                high.occlusion.quality = runner::SceneQuality::High;
                renderer.setLighting(high);
                drawScene(scene, 2);
                renderer.setLighting(shaded);
                drawScene(scene, 2);
                Expect(Sum(at(0.f, -0.5f, 0.58f)) + 10 < Sum(contactOff), "after size and Quality changes it still shades");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "ambient occlusion leaves no GL error");
                renderer.setLighting(runner::SceneLighting{});
            }
```

Check `kCameraEye` is public in `Renderer.hpp` (it is a public `static constexpr`). Check that `matrix4_look_at` and `matrix4_point` exist in `Matrix4.hpp`, as the old plan used them.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --target scene-render-check --parallel`
Expected: `no member named 'occlusion' in 'runner::SceneLighting'`.

- [ ] **Step 3: Shaders**

`src/runner/gl.hpp`, next to `RT_GL_RGBA16F`:

```cpp
constexpr GLenum RT_GL_R8 = 0x8229;
constexpr GLenum RT_GL_RED = 0x1903;
```

Create `resources/shaders/pipeline/occlusion.glsl`:

```glsl
// Ambient occlusion as the passes that light surfaces read it (ibl.frag,
// merge.frag): gtao.frag's visibility, blurred, upsampled to this pixel and
// raised to the AmbientOcclusionEffect's Intensity. 1 with none. No
// #version: Renderer puts it in after lighting.glsl, whose viewPositionAt
// it uses.

uniform sampler2D uOcclusion;
// 1 when this frame shaded occlusion; else uOcclusion is a white texel.
uniform float uOcclusionEnabled;
uniform float uOcclusionIntensity;
// The occlusion buffer is the full-size buffers divided by this: 1 or 2.
uniform float uOcclusionScale;

float occlusionAt(vec2 uv, float depth) {
    if (uOcclusionEnabled < 0.5) {
        return 1.0;
    }
    float visibility;
    if (uOcclusionScale < 1.5) {
        visibility = texture(uOcclusion, uv).r;
    } else {
        // The four half-size texels around this pixel, weighted bilinearly and
        // by how near each one's surface is to this pixel's, so shade does not
        // bleed across a silhouette.
        vec2 size = vec2(textureSize(uOcclusion, 0));
        vec2 fullSize = vec2(textureSize(uDepth, 0));
        vec2 position = uv * size - 0.5;
        vec2 base = floor(position);
        vec2 f = position - base;
        float center = -viewPositionAt(uv, depth).z;
        float sum = 0.0;
        float total = 0.0;
        for (int i = 0; i < 4; ++i) {
            ivec2 offset = ivec2(i & 1, i >> 1);
            ivec2 texel = clamp(ivec2(base) + offset, ivec2(0), ivec2(size) - 1);
            ivec2 full = texel * 2;
            float tapDepth = -viewPositionAt((vec2(full) + 0.5) / fullSize, texelFetch(uDepth, full, 0).r).z;
            float bilinear = (offset.x == 1 ? f.x : 1.0 - f.x) * (offset.y == 1 ? f.y : 1.0 - f.y);
            float w = bilinear / (1e-3 + abs(tapDepth - center) / center);
            sum += texelFetch(uOcclusion, texel, 0).r * w;
            total += w;
        }
        visibility = total > 0.0 ? sum / total : 1.0;
    }
    return pow(clamp(visibility, 0.0, 1.0), uOcclusionIntensity);
}
```

`occlusion.glsl` names `uDepth`, so each program that includes it must declare `uniform sampler2D uDepth;` before the libraries are spliced in. `ibl.frag` and `merge.frag` already do, but after the `#version` line, where Renderer splices the libraries. So move the `uDepth` declaration into `occlusion.glsl`, and delete it from both `ibl.frag` and `merge.frag`:

```glsl
uniform sampler2D uDepth;
```

goes at the top of `occlusion.glsl`, before `uOcclusion`. Do this in Step 4 when you edit those files.

Create `resources/shaders/pipeline/gtao.frag`. It is the 2026-10-02 plan's `gtao.frag`, adapted to sample on the Quality's grid, with uniform slices:

```glsl
#version 330 core
// Ground-truth ambient occlusion (Jimenez et al. 2016, after Intel's XeGTAO):
// how much of the sky above each opaque surface its neighbors leave open, 1
// open and 0 shut in, into an R8 buffer uOcclusionScale times smaller than
// the full-size buffers. Positions and steps are in full-size pixels; only
// the buffer is coarser. ao_blur.frag smooths the noise, and occlusion.glsl
// reads the result. Renderer puts lighting.glsl in after the #version line.
in vec2 vUv;
out vec4 outOcclusion;

uniform sampler2D uDepth;
uniform sampler2D uNormal;
// 1 over the full-size buffers' size in pixels.
uniform vec2 uTexel;
uniform float uOcclusionRadius;
// Full-size pixels per stud at view depth 1.
uniform float uProjectionScale;
// 1 or 2: this buffer's texel covers that many full-size pixels a side.
uniform float uOcclusionScale;
uniform float uSlices;

const float kPi = 3.14159265359;
const float kHalfPi = 1.57079632679;
const int kMaxSlices = 3;
const int kStepsPerSide = 6;
const float kMaxRadiusFraction = 0.25;
const float kFalloffRange = 0.6;

float sliceNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float stepNoise(vec2 pixel) {
    return fract(dot(pixel, vec2(0.7548776662, 0.5698402910)));
}

float safeAcos(float x) {
    return acos(clamp(x, -1.0, 1.0));
}

float horizonCosAt(vec2 uv, vec3 P, vec3 V, float low, float falloffMul, float falloffAdd) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
        return low;
    }
    ivec2 texel = ivec2(uv / uTexel);
    float depth = texelFetch(uDepth, texel, 0).r;
    if (depth >= 1.0) {
        return low;
    }
    vec3 delta = viewPositionAt((vec2(texel) + 0.5) * uTexel, depth) - P;
    float distance = length(delta);
    float cosine = dot(delta, V) / max(distance, 1e-6);
    float weight = clamp(distance * falloffMul + falloffAdd, 0.0, 1.0);
    return mix(low, cosine, weight);
}

void main() {
    // The full-size pixel this texel stands for.
    ivec2 pixel = ivec2(gl_FragCoord.xy) * int(uOcclusionScale);
    vec2 uv = (vec2(pixel) + 0.5) * uTexel;
    float depth = texelFetch(uDepth, pixel, 0).r;
    if (depth >= 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    vec3 P = viewPositionAt(uv, depth);
    vec3 V = normalize(-P);
    vec3 N = normalize(texelFetch(uNormal, pixel, 0).xyz);

    float radiusPixels = min(uOcclusionRadius * uProjectionScale / -P.z, kMaxRadiusFraction / uTexel.y);
    if (radiusPixels < 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    float falloffRange = kFalloffRange * uOcclusionRadius;
    float falloffFrom = uOcclusionRadius - falloffRange;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = falloffFrom / falloffRange + 1.0;

    float angleNoise = sliceNoise(gl_FragCoord.xy);
    float offsetNoise = stepNoise(gl_FragCoord.xy);
    int slices = int(uSlices);
    float visibility = 0.0;
    for (int slice = 0; slice < kMaxSlices; ++slice) {
        if (slice >= slices) {
            break;
        }
        float phi = (float(slice) + angleNoise) * (kPi / uSlices);
        vec2 omega = vec2(cos(phi), sin(phi));
        vec3 direction = vec3(omega, 0.0);
        vec3 orthoDirection = direction - dot(direction, V) * V;
        vec3 axis = normalize(cross(orthoDirection, V));
        vec3 projectedNormal = N - axis * dot(N, axis);
        float projectedLength = length(projectedNormal);
        float signNormal = sign(dot(orthoDirection, projectedNormal));
        float cosNormal = clamp(dot(projectedNormal, V) / max(projectedLength, 1e-6), 0.0, 1.0);
        float n = signNormal * safeAcos(cosNormal);

        float low0 = cos(n + kHalfPi);
        float low1 = cos(n - kHalfPi);
        float horizonCos0 = low0;
        float horizonCos1 = low1;
        for (int s = 0; s < kStepsPerSide; ++s) {
            float t = (float(s) + offsetNoise) / float(kStepsPerSide);
            vec2 offset = omega * max(t * t * radiusPixels, float(s) + 1.0) * uTexel;
            horizonCos0 = max(horizonCos0, horizonCosAt(uv + offset, P, V, low0, falloffMul, falloffAdd));
            horizonCos1 = max(horizonCos1, horizonCosAt(uv - offset, P, V, low1, falloffMul, falloffAdd));
        }

        float h0 = -safeAcos(horizonCos1);
        float h1 = safeAcos(horizonCos0);
        h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
        h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
        float arc0 = cosNormal + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n);
        float arc1 = cosNormal + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n);
        visibility += projectedLength * 0.25 * (arc0 + arc1);
    }
    outOcclusion = vec4(clamp(visibility / uSlices, 0.0, 1.0));
}
```

Create `resources/shaders/pipeline/ao_blur.frag`:

```glsl
#version 330 core
// One direction of the blur that smooths gtao.frag's noise, run across and
// then down, at the occlusion buffer's size. A neighbor counts less the
// farther its surface sits from the middle pixel's tangent plane, as a part
// of the middle's depth, and the more its normal turns away, so shade does
// not bleed across a silhouette or a crease. Positions come from the
// full-size depth buffer. Renderer puts lighting.glsl in after the #version
// line.
in vec2 vUv;
out vec4 outOcclusion;

uniform sampler2D uOcclusionSource;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
// 1 over the full-size buffers' size in pixels.
uniform vec2 uTexel;
uniform float uOcclusionScale;
// (1, 0) across, (0, 1) down.
uniform vec2 uBlurDirection;
uniform float uBlurRadius;

const int kMaxBlurRadius = 6;
const float kPlaneTolerance = 0.02;
const float kNormalPower = 8.0;

vec3 positionAt(ivec2 texel, out float depth) {
    ivec2 full = texel * int(uOcclusionScale);
    depth = texelFetch(uDepth, full, 0).r;
    return viewPositionAt((vec2(full) + 0.5) * uTexel, depth);
}

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float depth;
    vec3 P = positionAt(texel, depth);
    if (depth >= 1.0) {
        outOcclusion = vec4(1.0);
        return;
    }
    vec3 N = normalize(texelFetch(uNormal, texel * int(uOcclusionScale), 0).xyz);
    float tolerance = kPlaneTolerance * -P.z;
    ivec2 last = textureSize(uOcclusionSource, 0) - 1;
    ivec2 stride = ivec2(uBlurDirection);
    // A Gaussian whose sigma is half the radius.
    float sigma = 0.5 * uBlurRadius;
    float sum = texelFetch(uOcclusionSource, texel, 0).r;
    float total = 1.0;
    int radius = int(uBlurRadius);
    for (int i = 1; i <= kMaxBlurRadius; ++i) {
        if (i > radius) {
            break;
        }
        float gaussian = exp(-0.5 * float(i * i) / (sigma * sigma));
        for (int side = -1; side <= 1; side += 2) {
            ivec2 tap = clamp(texel + stride * (i * side), ivec2(0), last);
            float tapDepth;
            vec3 Ps = positionAt(tap, tapDepth);
            if (tapDepth >= 1.0) {
                continue;
            }
            float planeWeight = clamp(1.0 - abs(dot(Ps - P, N)) / tolerance, 0.0, 1.0);
            vec3 Ns = normalize(texelFetch(uNormal, tap * int(uOcclusionScale), 0).xyz);
            float normalWeight = pow(clamp(dot(N, Ns), 0.0, 1.0), kNormalPower);
            float weight = gaussian * planeWeight * normalWeight;
            sum += texelFetch(uOcclusionSource, tap, 0).r * weight;
            total += weight;
        }
    }
    outOcclusion = vec4(sum / total);
}
```

- [ ] **Step 4: Shade with it**

`image_lighting.glsl`, above `struct SkyReflection`, add the copies of `OcclusionMath`:

```glsl
// OcclusionMath's MultiBounce, per channel: exactly 1 when open.
vec3 multiBounce(float visibility, vec3 albedo) {
    if (visibility >= 1.0) {
        return vec3(1.0);
    }
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

// OcclusionMath's SpecularOcclusion: exactly 1 when open.
float specularOcclusion(float visibility, float NdotV, float roughness) {
    if (visibility >= 1.0) {
        return 1.0;
    }
    return clamp(pow(NdotV + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility, 0.0, 1.0);
}
```

Give `skyReflection` and `skyLight` a trailing `float occlusion` (1 is open). In `skyReflection`:
- the no-Skybox branch multiplies `r.light` by `multiBounce(occlusion, albedo)`, because `ambientLight` as a whole gets that factor in `skyLight`;
- the Skybox branch multiplies `r.light` by `specularOcclusion(occlusion, NdotV, roughness)`, with NdotV computed as in `skySurface`.

`r.weight` is unchanged: traced light is what is actually on screen. In `skyLight`:
- no-Skybox: `return ambientLight(...) * multiBounce(occlusion, albedo);`
- Skybox: multiply the irradiance term and the flat ambient term by `multiBounce(occlusion, albedo)`, and pass `occlusion` to `skyReflection`.

With occlusion 1 every factor is exactly 1, so frames without AO are unchanged bit for bit.

Callers:
- `forward.frag`: pass `1.0`. See-through surfaces get no occlusion.
- `ssr.frag`: pass `1.0`. It only uses `.weight`, which occlusion doesn't touch.
- `ibl.frag`:
  - remove `uniform sampler2D uDepth;`;
  - compute `float occlusion = occlusionAt(vUv, depth);` and pass it.
- `merge.frag`:
  - remove `uniform sampler2D uDepth;`;
  - in the SSR resolve, pass `occlusionAt(vUv, depth)` to `skyReflection`.

- [ ] **Step 5: Renderer**

`Renderer.hpp`:
- before `struct SceneLighting`:

  ```cpp
  // Enum.EffectQuality, as the renderer reads it.
  enum class SceneQuality { Low = 0, Medium = 1, High = 2 };

  // The AmbientOcclusionEffect, as the renderer reads it. The defaults shade nothing.
  struct SceneOcclusion {
      bool enabled = false;
      float intensity = 1.f;
      // Studs.
      float radius = 1.f;
      SceneQuality quality = SceneQuality::Medium;
  };
  ```

- `SceneOcclusion occlusion;` in `SceneLighting` after `reflections`.
- `Program` gains `occlusionRadius`, `projectionScale`, `occlusionScale`, `slices`, `blurDirection`, `blurRadius`, `occlusionEnabled`, `occlusionIntensity`, under the comment `// Ambient occlusion (gtao.frag, ao_blur.frag, occlusion.glsl).`
- Declarations:

  ```cpp
      // Ambient occlusion into occlusionTexture_ at the Quality's scale. False,
      // with the light pass reading white, when none is asked for, the buffers
      // are refused, or a program cannot draw yet.
      bool occlusionPass(const float* projection, const float* inverseProjection);
      bool ensureOcclusionBuffers(int width, int height, int scale);
      void destroyOcclusionBuffers();
      // Points program's occlusion uniforms and unit at this frame's result, or white.
      void bindOcclusion(const Program& program);
  ```

- `Program gtao_; Program aoBlur_;`
- Members:

  ```cpp
      // Ambient occlusion, one channel, 1 open: the trace and the blur's halfway
      // buffer, the pane's size divided by occlusionScale_. Made on the first
      // frame that shades, apart from the other buffers.
      unsigned occlusionFbo_ = 0;
      unsigned occlusionTexture_ = 0;
      unsigned occlusionBlurFbo_ = 0;
      unsigned occlusionBlurTexture_ = 0;
      int occlusionWidth_ = 0;
      int occlusionHeight_ = 0;
      int occlusionScale_ = 0;
      int occlusionRefusedWidth_ = 0;
      int occlusionRefusedHeight_ = 0;
      int occlusionRefusedScale_ = 0;
      bool occlusionValid_ = false;
      // Whether this frame shaded occlusion: when not, the passes read white.
      bool occlusionReady_ = false;
  ```

`Renderer.cpp`:

1. After `kUnitReflections`:

   ```cpp
   // Ambient occlusion shares the metalness map's unit: none of the passes that
   // read it (the light pass, the merge, its own blur) reads a Material.
   constexpr int kUnitOcclusion = kUnitMetalnessMap;
   ```

2. `buildProgram`: read the new uniforms (`uOcclusionRadius`, `uProjectionScale`, `uOcclusionScale`, `uSlices`, `uBlurDirection`, `uBlurRadius`, `uOcclusionEnabled`, `uOcclusionIntensity`). Add `sampler("uOcclusion", kUnitOcclusion);` and `sampler("uOcclusionSource", kUnitScene);`. The blur reads its source on unit 11; the merge reads the reflection trace there too, but not in the same pass.
3. `initialize`:
   - the IBL program's libraries become `{"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/occlusion.glsl", "pipeline/image_lighting.glsl"}`;
   - the merge's become `{"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/occlusion.glsl", "pipeline/image_lighting.glsl"}`;
   - forward and ssr keep theirs. They call `skyLight` and `skyReflection` with `1.0`, so they need no `occlusion.glsl`.
   - Add:

   ```cpp
           buildProgram(gtao_, "Ambient occlusion", "pipeline/fullscreen.vert", "pipeline/gtao.frag",
                        {"pipeline/lighting.glsl"}) &&
           buildProgram(aoBlur_, "Occlusion blur", "pipeline/fullscreen.vert", "pipeline/ao_blur.frag",
                        {"pipeline/lighting.glsl"}) &&
   ```

   - Add both to `shutdown`'s program list.
   - Reset `occlusionValid_ = false;` where `reflectionsValid_` is reset in `initialize`.
   - Reset the three `occlusionRefused*_` to 0 in `shutdown`.
4. `destroyTargets` calls `destroyOcclusionBuffers();` beside `destroyReflectionBuffers();`.
5. Add after `destroyReflectionBuffers`:

```cpp
bool Renderer::ensureOcclusionBuffers(int width, int height, int scale) {
    if (occlusionFbo_ != 0 && width == occlusionWidth_ && height == occlusionHeight_ && scale == occlusionScale_) {
        return true;
    }
    if (width == occlusionRefusedWidth_ && height == occlusionRefusedHeight_ && scale == occlusionRefusedScale_) {
        return false;
    }
    destroyOcclusionBuffers();
    const int w = std::max(width / scale, 1);
    const int h = std::max(height / scale, 1);
    occlusionTexture_ = MakeTarget(RT_GL_R8, RT_GL_RED, GL_UNSIGNED_BYTE, w, h);
    occlusionBlurTexture_ = MakeTarget(RT_GL_R8, RT_GL_RED, GL_UNSIGNED_BYTE, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    bool complete = true;
    glGenFramebuffers(1, &occlusionFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    complete = Attach({occlusionTexture_}, 0) && complete;
    glGenFramebuffers(1, &occlusionBlurFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionBlurFbo_);
    complete = Attach({occlusionBlurTexture_}, 0) && complete;
    if (!complete) {
        std::fprintf(stderr,
                     "The Scene View's %d by %d occlusion buffers are not supported; drawing without ambient "
                     "occlusion.\n",
                     w, h);
        destroyOcclusionBuffers();
        occlusionRefusedWidth_ = width;
        occlusionRefusedHeight_ = height;
        occlusionRefusedScale_ = scale;
        return false;
    }
    occlusionWidth_ = width;
    occlusionHeight_ = height;
    occlusionScale_ = scale;
    return true;
}

void Renderer::destroyOcclusionBuffers() {
    for (unsigned* fbo : {&occlusionFbo_, &occlusionBlurFbo_}) {
        if (*fbo != 0) {
            glDeleteFramebuffers(1, fbo);
            *fbo = 0;
        }
    }
    DeleteTexture(occlusionTexture_);
    DeleteTexture(occlusionBlurTexture_);
    occlusionWidth_ = 0;
    occlusionHeight_ = 0;
    occlusionScale_ = 0;
    occlusionValid_ = false;
}
```

   The blur reads `uDepth` and `uNormal` at full size and its source at the buffer's size. The integer division `width / scale` matches `gtao.frag`'s `pixel * scale` mapping.

6. Add after `geometryPass`:

```cpp
bool Renderer::occlusionPass(const float* projection, const float* inverseProjection) {
    occlusionReady_ = false;
    const SceneOcclusion& occlusion = lighting_.occlusion;
    if (!occlusion.enabled || !(occlusion.intensity > 0.f) || !(occlusion.radius > 0.f)) {
        return false;
    }
    const OcclusionQuality settings = QualitySettings(static_cast<int>(occlusion.quality));
    if (!ensureOcclusionBuffers(targetWidth_, targetHeight_, settings.scale)) {
        return false;
    }
    RENDER_PASS("Ambient occlusion");
    const int w = std::max(targetWidth_ / settings.scale, 1);
    const int h = std::max(targetHeight_ / settings.scale, 1);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);
    glBindVertexArray(emptyVao_);
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitNormal, normalTexture_);
    const float texelX = 1.f / static_cast<float>(targetWidth_);
    const float texelY = 1.f / static_cast<float>(targetHeight_);

    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    glUseProgram(gtao_.id);
    glUniformMatrix4fv(gtao_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(gtao_.texel, texelX, texelY);
    glUniform1f(gtao_.occlusionRadius, occlusion.radius);
    // Full-size pixels per stud at view depth 1: half the height times the projection's [1][1].
    glUniform1f(gtao_.projectionScale, 0.5f * static_cast<float>(targetHeight_) * projection[5]);
    glUniform1f(gtao_.occlusionScale, static_cast<float>(settings.scale));
    glUniform1f(gtao_.slices, static_cast<float>(settings.slices));
    // Validated until it passes, then trusted until the programs or buffers are made again.
    if (!occlusionValid_ && !CanDraw(gtao_.id)) {
        glViewport(0, 0, targetWidth_, targetHeight_);
        return false;
    }
    DrawFullscreen(emptyVao_);

    // Across into the halfway buffer, then down back into occlusionTexture_.
    glUseProgram(aoBlur_.id);
    glUniformMatrix4fv(aoBlur_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(aoBlur_.texel, texelX, texelY);
    glUniform1f(aoBlur_.occlusionScale, static_cast<float>(settings.scale));
    glUniform1f(aoBlur_.blurRadius, static_cast<float>(settings.blurRadius));
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionBlurFbo_);
    BindTexture(kUnitScene, occlusionTexture_);
    glUniform2f(aoBlur_.blurDirection, 1.f, 0.f);
    if (!occlusionValid_ && !CanDraw(aoBlur_.id)) {
        glViewport(0, 0, targetWidth_, targetHeight_);
        return false;
    }
    DrawFullscreen(emptyVao_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    BindTexture(kUnitScene, occlusionBlurTexture_);
    glUniform2f(aoBlur_.blurDirection, 0.f, 1.f);
    DrawFullscreen(emptyVao_);
    glViewport(0, 0, targetWidth_, targetHeight_);
    occlusionValid_ = true;
    occlusionReady_ = true;
    return true;
}

void Renderer::bindOcclusion(const Program& program) {
    glUniform1f(program.occlusionEnabled, occlusionReady_ ? 1.f : 0.f);
    glUniform1f(program.occlusionIntensity, std::min(lighting_.occlusion.intensity, 4.f));
    glUniform1f(program.occlusionScale, static_cast<float>(std::max(occlusionScale_, 1)));
    // With none, any texture keeps the sampler loadable.
    BindTexture(kUnitOcclusion, occlusionReady_ ? occlusionTexture_ : whiteTexture_);
}
```

   `#include "OcclusionMath.hpp"` at the top of `Renderer.cpp`.

7. In `draw`, split the chain after `geometryPass`:

```cpp
        drawn = cubesReady && shadowPass(meshes, meshCount, projection) &&
                geometryPass(meshes, meshCount, projection);
        // Occlusion never fails the frame: without it, surfaces are lit as if open.
        if (drawn) {
            occlusionPass(projection, inverseProjection.m);
        } else {
            occlusionReady_ = false;
        }
        drawn = drawn && lightPass(projection, inverseProjection.m) && skyPass(inverseProjection.m);
```

   The reflections line and the rest are unchanged.
8. `lightPass`: after `bindSky(ibl_);`, add `bindOcclusion(ibl_);`. Shadows use units 0 and 1, the occlusion unit 3. After the IBL draw, the light programs that follow need nothing from it.
9. `mergePass`: after `bindSky(merge_);`, add `bindOcclusion(merge_);`.

- [ ] **Step 6: GameView**

After the reflections lines:

```cpp
    // The AmbientOcclusionEffect, if any; with none, or one turned off, nothing is shaded.
    const engine_core::VisualAmbientOcclusion& occlusion = snapshot.occlusion;
    lighting.occlusion.enabled = occlusion.present && occlusion.enabled;
    lighting.occlusion.intensity = occlusion.intensity;
    lighting.occlusion.radius = occlusion.radius;
    lighting.occlusion.quality = occlusion.quality == 0   ? SceneQuality::Low
                                 : occlusion.quality == 2 ? SceneQuality::High
                                                          : SceneQuality::Medium;
```

- [ ] **Step 7: Run the checks**

```bash
cmake --build build --parallel && ./build/scene-render-check; echo "exit $?"
```

Expected: no FAIL lines.

- If "open floor" fails by a few units, that is depth-precision self-occlusion (Review Focus 1). Do not widen the tolerance. In `horizonCosAt`, ignore samples within `0.02 × −P.z` of P's tangent plane (`abs(dot(delta, N)) < 0.02 * -P.z` → return `low`). That needs N passed in.
- If "past the top edge" fails, the upsample or blur is bleeding across the silhouette. Print the four taps' weights before changing anything.
- If "where SSR misses" fails, the merge's `skyReflection` occlusion does not match `skyLight`'s. Compare the two call sites' arguments.

- [ ] **Step 8: Run every suite**

```bash
(cd build && ctest --output-on-failure) && ./build/sandbox "[occlusion],[reflections],[antialiasing],[bloom],[skybox],[render]"
```

- [ ] **Step 9: Commit**

```bash
git add resources/shaders/pipeline/gtao.frag resources/shaders/pipeline/ao_blur.frag resources/shaders/pipeline/occlusion.glsl \
  resources/shaders/pipeline/image_lighting.glsl resources/shaders/pipeline/ibl.frag resources/shaders/pipeline/forward.frag \
  resources/shaders/pipeline/merge.frag resources/shaders/pipeline/ssr.frag src/runner/gl.hpp src/runner/Renderer.hpp \
  src/runner/Renderer.cpp src/runner/GameView.cpp tests/SceneRenderCheck.cpp
git commit -m "Shade the sky and ambient light with ground-truth ambient occlusion, at the effect's Quality

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: See it in the studio, and time it

- [ ] **Step 1:** `cmake --build build --target bundle-resources --parallel && cmake --build build --parallel`.
- [ ] **Step 2:** Copy Mitsuba into the scratchpad as `AoDemo`, launch this worktree's studio on it, and select it by pid.
- [ ] **Step 3:** Screenshot without AO. Insert an `AmbientOcclusionEffect` under Lighting and screenshot at Medium, then at High. Crop and enlarge the pedestal's base and the stand poles' feet.
- [ ] **Step 4:** Check by eye:
  - contact shade under the pedestal and poles;
  - no dark ring around silhouettes;
  - no speckle with the camera still;
  - the open floor unchanged.
- [ ] **Step 5:** Read `get_profile` with AO off, then at Low, Medium, and High, each after settling. Record the GPU "3D scene" difference and the "Ambient occlusion" CPU scope.
- [ ] **Step 6:** Send the screenshots, crops, and timings to the user (`proactive`). Quit the studio by pid.
