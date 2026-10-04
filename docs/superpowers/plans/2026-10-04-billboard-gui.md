# BillboardGui Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `BillboardGui` instance class: the ScreenGui tree drawn in the 3D world at a PVInstance, facing the camera, sized in world units by CSS percentages, hidden by nearer geometry unless `AlwaysOnTop`, taking the mouse like a ScreenGui, and never a frame behind the scene.

**Architecture:** Each BillboardGui is a JadeFX node in the Scene View's `GuiLayer`. The render thread publishes each billboard's anchor in the `VisualSnapshot`; the Scene View holds one snapshot per frame for both its layout (where billboards are placed and sized by projecting the anchor with the renderer's own camera) and its paint (where the 3D scene is drawn). Occlusion is a fragment discard in JadeFX's UI shaders against the renderer's depth texture; occluded input uses a one-pixel asynchronous depth read under the cursor.

**Tech Stack:** C++17 (Xcode 13 / libc++ 13: no `std::ranges`, no `<format>`), OpenGL 3.3/4.1 core through `runner/gl.hpp`'s loader, JadeFX (sibling repo), flecs, Catch2 (`sandbox`), plain `Expect` executables (`engine-tests`, `studio-tests`, `jadefx-tests`).

**Spec:** `docs/superpowers/specs/2026-10-04-billboard-gui-design.md`

## Global Constraints

- Two repositories. JadeFX work happens on branch `billboard-occluder` in worktree `/Users/yaoli/Documents/JadeFX_CPP/.claude/worktrees/billboard-occluder`. Engine work happens on branch `billboard-gui` in worktree `/Users/yaoli/Documents/AnarchyEngine-CPP/.claude/worktrees/billboard-gui`. Never commit on `main`/`master` of either repo; other sessions move `main` under you.
- The engine worktree has no `../JadeFX_CPP` sibling, so always configure it with `-DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP/.claude/worktrees/billboard-occluder`.
- Engine build (from the engine worktree root): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP/.claude/worktrees/billboard-occluder && cmake --build build --parallel --target <targets>`. Tests run from the worktree root (they find `resources/` there).
- JadeFX build (from the JadeFX worktree root): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target jadefx-tests --parallel && ./build/jadefx-tests`.
- Any GL error in a JadeFX frame quits the app with "OpenGL error during UI frame". GL changes are checked in a real JadeFX window (Task 9), not only in a bare context.
- Match the surrounding code: comments are full sentences saying what a thing is or does, no `TODO`s, `// namespace` closers, `engine_core::` / `runner::` / `jadefx::` namespaces as the neighbouring files use them.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Before each commit, `git show --stat HEAD` afterwards must list only this task's files.
- Property names, exact: `Adornee` (type `PVInstance?`, default nil), `AlwaysOnTop` (type `boolean`, default `false`). Class name `BillboardGui`; CSS element type `billboardgui`.
- A percentage width or height on the BillboardGui resolves against `pixelsPerUnit = paneHeight / (2 · distance · tan(fovY / 2))`, in points. A child's percentage is of its parent.
- Draw order in the layer: depth-tested billboards far to near, then AlwaysOnTop billboards far to near, then ScreenGuis.

## Review Focus

- **Adornee destroyed mid-play, or set to an instance in another service.** Expected: the billboard falls back to its parent or the origin without a crash; undoing the destroy restores the link. Pinned by GUI9 (Task 2).
- **Two Scene Views open (Window > New Scene View).** Expected: neither view draws a torn or recycled snapshot, and both place billboards exactly. Pinned by the `SceneFeed` hold tests (Task 4).
- **Camera right at or behind the anchor (distance ≤ near plane), or field of view near 180°.** Expected: the billboard hides for that frame and takes no mouse; no NaN or infinite sizes reach JadeFX. Pinned by RM10–RM11 (Task 6) and the hidden-behind-camera check (Task 7).
- **A billboard far away (ppu tiny) or nearly touching the camera (ppu huge).** Expected: sizes clamp to finite values and layout does not explode; `calc(200% + 32px)` at a great distance tends to 32px. Pinned by RM12 (Task 6).
- **BillboardGui inside Storage, Gui, or a ScreenGui, then moved into Workspace during play.** Expected: it draws only once it is in Workspace or Core and not inside another GUI, and stops at Stop when Stop restores it elsewhere. Pinned by GUI10 (Task 2) and BB2 (Task 5).

---

## File Structure

**JadeFX (`/Users/yaoli/Documents/JadeFX_CPP/.claude/worktrees/billboard-occluder`)**
- Create `src/gl/Occluder.hpp`: the occluder's state, a plain struct (testable without GL).
- Modify `src/gl/UiRenderer.hpp/.cpp`: `setOccluder` / `clearOccluder` / `occluder()`, uniforms on the three programs.
- Modify `res/shaders/box.frag`, `text.frag`, `text_gray.frag`, `image.frag`: the uniform-gated discard.
- Modify `include/jadefx/scene/Painter.hpp`, `src/scene/Painter.cpp`: the public entry points.
- Create `tests/occluder_tests.cpp`; modify `tests/layout_tests.cpp` (runner) and `CMakeLists.txt` (test list).

**Engine (`/Users/yaoli/Documents/AnarchyEngine-CPP/.claude/worktrees/billboard-gui`)**
- `src/engine_instances/Gui.hpp/.cpp`: the `BillboardGui` class, `GuiProperty::AlwaysOnTop`, registration.
- `src/engine_core/ScriptBindings.cpp`, `src/engine_core/Project.cpp`, `src/ide/IdeIcons.cpp`, `src/engine_instances/README.md`: creatable, loadable, icon, docs.
- `src/engine_core/DataModel.hpp/.cpp`, `DataModelPlace.cpp`, `DataModelState.hpp`, `Ecs.hpp/.cpp`: the `BillboardTag` and `DataModel::billboards`.
- `src/runner/SceneFeed.hpp/.cpp`: pooled buffers and `hold()`.
- `src/engine_core/SnapshotPump.hpp/.cpp`: `VisualBillboard` rows.
- Create `src/runner/BillboardMath.hpp/.cpp`: `PlaceBillboard`, pure. `src/runner/RenderMath.hpp`: shared `kSceneNear`/`kSceneFar`.
- `src/runner/Renderer.hpp/.cpp`: camera accessors, `sceneDepth()`, the cursor depth probe. `src/runner/gl.hpp/.cpp`: buffer mapping entry points.
- `src/runner/GuiLayer.hpp/.cpp`: billboard nodes, placement, layout, order, occluded input, occluded draw.
- `src/runner/GameView.hpp/.cpp`: one held snapshot per frame, placement call, cursor tracking, paint wiring, test hook.
- Tests: `sandbox/gui_tests.cpp`, `sandbox/render_math_tests.cpp`, `sandbox/billboard_tests.cpp` (new), `tests/SceneFeedTest.cpp`, `tests/BillboardLayerTest.cpp` (new), `tests/StudioLayoutTest.cpp`, `tests/BillboardDemo.cpp` (new, by hand), `CMakeLists.txt`.

---

### Task 0: Worktrees and a clean baseline

**Files:** none.

- [ ] **Step 1: Create the JadeFX worktree**

```bash
cd /Users/yaoli/Documents/JadeFX_CPP
git worktree add -q .claude/worktrees/billboard-occluder -b billboard-occluder master
```

- [ ] **Step 2: Baseline build and tests, both repos**

```bash
cd /Users/yaoli/Documents/JadeFX_CPP/.claude/worktrees/billboard-occluder
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target jadefx-tests --parallel && ./build/jadefx-tests
cd /Users/yaoli/Documents/AnarchyEngine-CPP/.claude/worktrees/billboard-gui
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJADEFX_CPP_DIR=/Users/yaoli/Documents/JadeFX_CPP/.claude/worktrees/billboard-occluder
cmake --build build --parallel --target sandbox engine-tests studio-tests
./build/sandbox "[gui]" && ./build/engine-tests && ./build/studio-tests
```

Expected: all pass. If anything fails on the untouched baseline, stop and report it; do not fix unrelated failures.

---

### Task 1: JadeFX occluder

**Files:**
- Create: `src/gl/Occluder.hpp`
- Modify: `src/gl/UiRenderer.hpp`, `src/gl/UiRenderer.cpp`
- Modify: `res/shaders/box.frag`, `res/shaders/text.frag`, `res/shaders/text_gray.frag`, `res/shaders/image.frag`
- Modify: `include/jadefx/scene/Painter.hpp`, `src/scene/Painter.cpp`
- Test: `tests/occluder_tests.cpp` (new), `tests/layout_tests.cpp`, `CMakeLists.txt`

**Interfaces:**
- Produces (public, `jadefx::Painter`):
  ```cpp
  void setOccluder(unsigned depthTexture, int x, int y, int width, int height, float depth);
  void clearOccluder();
  ```
  `x, y, width, height` are framebuffer pixels, origin bottom left (GL's), the rectangle the depth texture covers. A fragment inside that rectangle whose sampled depth is less than `depth` is discarded. Outside the rectangle nothing is discarded.

- [ ] **Step 1: Write the failing test** — `tests/occluder_tests.cpp`

```cpp
#include "gl/Occluder.hpp"
#include "gl/UiRenderer.hpp"

#include <cstdio>

// The occluder UiRenderer hides UI fragments behind: its state, which a draw
// reads, kept without a GL context.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

void TestStartsInactive() {
    jadefx::UiRenderer renderer;
    Expect(!renderer.occluder().active(), "a renderer starts with no occluder");
}

void TestSetAndClear() {
    jadefx::UiRenderer renderer;
    const unsigned before = renderer.occluder().revision;
    renderer.setOccluder(7, 10, 20, 300, 200, 0.5f);
    const jadefx::Occluder& set = renderer.occluder();
    Expect(set.active(), "a texture and a rectangle make it active");
    Expect(set.texture == 7 && set.rect[0] == 10.f && set.rect[1] == 20.f && set.rect[2] == 300.f &&
               set.rect[3] == 200.f && set.depth == 0.5f,
           "it keeps what it was given");
    Expect(set.revision != before, "setting it moves the revision");
    const unsigned afterSet = set.revision;
    renderer.setOccluder(7, 10, 20, 300, 200, 0.5f);
    Expect(renderer.occluder().revision == afterSet, "setting the same values again does not");
    renderer.clearOccluder();
    Expect(!renderer.occluder().active(), "clearing it makes it inactive");
    Expect(renderer.occluder().revision != afterSet, "and moves the revision");
}

void TestEmptyIsInactive() {
    jadefx::UiRenderer renderer;
    renderer.setOccluder(0, 0, 0, 100, 100, 0.5f);
    Expect(!renderer.occluder().active(), "no texture is no occluder");
    renderer.setOccluder(3, 0, 0, 0, 100, 0.5f);
    Expect(!renderer.occluder().active(), "nor is an empty rectangle");
}

}  // namespace

int RunOccluderTests() {
    TestStartsInactive();
    TestSetAndClear();
    TestEmptyIsInactive();
    return gFailures;
}
```

In `tests/layout_tests.cpp`, beside the other declarations near line 1947 add `int RunOccluderTests();`, and beside `gFailures += RunGlyphAtlasTests();` add `gFailures += RunOccluderTests();`. In `CMakeLists.txt`, add `tests/occluder_tests.cpp` after `tests/glyph_atlas_tests.cpp` in `jadefx-tests`.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --target jadefx-tests --parallel`
Expected: compile error, `gl/Occluder.hpp` not found.

- [ ] **Step 3: Add the state** — `src/gl/Occluder.hpp`

```cpp
#pragma once

namespace jadefx {

// What UiRenderer hides UI fragments behind while it is set: a depth texture,
// the framebuffer rectangle it covers (x, y from the bottom left, width,
// height, in pixels), and the depth of what is being drawn. A fragment in the
// rectangle whose texel is nearer than depth is not drawn. revision moves on
// every change, so a program sends the uniforms again only then.
struct Occluder {
    unsigned texture = 0;
    float rect[4] = {0.f, 0.f, 0.f, 0.f};
    float depth = 0.f;
    unsigned revision = 0;

    bool active() const { return texture != 0 && rect[2] > 0.f && rect[3] > 0.f; }
};

}  // namespace jadefx
```

- [ ] **Step 4: UiRenderer API** — in `src/gl/UiRenderer.hpp` add `#include "gl/Occluder.hpp"`, and in the public section after `popClip()`:

```cpp
    // Hide later draws' fragments behind a depth texture, as Occluder says,
    // until clearOccluder. Used to draw UI inside a 3D view.
    void setOccluder(unsigned depthTexture, int x, int y, int width, int height, float depth);
    void clearOccluder();
    const Occluder& occluder() const { return occluder_; }
```

In the private section, after `int imageTinted_ = -1;`:

```cpp
    // Each program's occluder uniforms, and the Occluder revision it last sent.
    struct OccluderSlots {
        int texture = -1;
        int rect = -1;
        int depth = -1;
        int on = -1;
        unsigned sent = ~0u;
    };
    void sendOccluder(OccluderSlots& slots);
    OccluderSlots boxOccluder_;
    OccluderSlots textOccluder_;
    OccluderSlots imageOccluder_;
    Occluder occluder_;
    // The unit the depth texture is bound to; JadeFX samples nothing else there.
    static constexpr int kOccluderUnit = 7;
```

- [ ] **Step 5: UiRenderer implementation** — in `src/gl/UiRenderer.cpp`

After the existing `imageTinted_ = Location(...)` line in `initialize()`:

```cpp
    const auto occluderSlots = [](unsigned program, OccluderSlots& slots) {
        slots.texture = Location(program, "uOccluder");
        slots.rect = Location(program, "uOccluderRect");
        slots.depth = Location(program, "uOccluderDepth");
        slots.on = Location(program, "uOccluded");
        slots.sent = ~0u;
    };
    occluderSlots(boxProgram_, boxOccluder_);
    occluderSlots(textProgram_, textOccluder_);
    occluderSlots(imageProgram_, imageOccluder_);
```

New functions (place after `popClip`):

```cpp
void UiRenderer::setOccluder(unsigned depthTexture, int x, int y, int width, int height, float depth) {
    Occluder next;
    next.texture = depthTexture;
    next.rect[0] = static_cast<float>(x);
    next.rect[1] = static_cast<float>(y);
    next.rect[2] = static_cast<float>(width);
    next.rect[3] = static_cast<float>(height);
    next.depth = depth;
    if (next.texture == occluder_.texture && next.depth == occluder_.depth && next.rect[0] == occluder_.rect[0] &&
        next.rect[1] == occluder_.rect[1] && next.rect[2] == occluder_.rect[2] && next.rect[3] == occluder_.rect[3]) {
        return;
    }
    next.revision = occluder_.revision + 1;
    occluder_ = next;
}

void UiRenderer::clearOccluder() {
    if (!occluder_.active() && occluder_.texture == 0) {
        return;
    }
    const unsigned revision = occluder_.revision + 1;
    occluder_ = Occluder{};
    occluder_.revision = revision;
}

void UiRenderer::sendOccluder(OccluderSlots& slots) {
    const bool on = occluder_.active();
    if (on) {
        // Bound on every draw: a 3D view drawn between UI draws binds its own textures.
        glActiveTexture(GL_TEXTURE0 + kOccluderUnit);
        glBindTexture(GL_TEXTURE_2D, occluder_.texture);
        glActiveTexture(GL_TEXTURE0);
    }
    if (slots.sent == occluder_.revision) {
        return;
    }
    slots.sent = occluder_.revision;
    glUniform1i(slots.texture, kOccluderUnit);
    glUniform4f(slots.rect, occluder_.rect[0], occluder_.rect[1], occluder_.rect[2], occluder_.rect[3]);
    glUniform1f(slots.depth, occluder_.depth);
    glUniform1f(slots.on, on ? 1.f : 0.f);
}
```

Call `sendOccluder(boxOccluder_);` right after `glUseProgram(boxProgram_);` in `drawBox`, `sendOccluder(textOccluder_);` right after `glUseProgram(textProgram_);` in `text`, and `sendOccluder(imageOccluder_);` right after `glUseProgram(imageProgram_);` in `drawImage`. If `gl/gl.hpp` lacks `GL_TEXTURE0` or `glActiveTexture`, add them there the way its neighbours are declared and loaded.

Note: `textProgram_` may be linked from `text_gray.frag` instead (line ~106), so both text shaders need the uniforms.

- [ ] **Step 6: Shaders** — add to each of `box.frag`, `text.frag`, `text_gray.frag`, `image.frag`, after the existing uniforms:

```glsl
// While uOccluded is 1, a fragment inside uOccluderRect (framebuffer pixels:
// x, y from the bottom left, width, height) whose depth in uOccluder is
// nearer than uOccluderDepth is not drawn: UI drawn inside a 3D view, behind
// what the view drew in front of it.
uniform sampler2D uOccluder;
uniform vec4 uOccluderRect;
uniform float uOccluderDepth;
uniform float uOccluded;

bool occluded() {
    if (uOccluded < 0.5) {
        return false;
    }
    vec2 uv = (gl_FragCoord.xy - uOccluderRect.xy) / uOccluderRect.zw;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return false;
    }
    return texture(uOccluder, uv).r < uOccluderDepth;
}
```

and make the first statement of each `main()`:

```glsl
    if (occluded()) {
        discard;
    }
```

- [ ] **Step 7: Painter** — in `include/jadefx/scene/Painter.hpp` after `popClip()`:

```cpp
    // Hide later draws behind a depth texture covering this framebuffer
    // rectangle (pixels, origin bottom left): a fragment whose texel there is
    // nearer than depth is not drawn. For UI drawn inside a 3D view.
    void setOccluder(unsigned depthTexture, int x, int y, int width, int height, float depth);
    void clearOccluder();
```

In `src/scene/Painter.cpp`:

```cpp
void Painter::setOccluder(unsigned depthTexture, int x, int y, int width, int height, float depth) {
    renderer_.setOccluder(depthTexture, x, y, width, height, depth);
}

void Painter::clearOccluder() { renderer_.clearOccluder(); }
```

- [ ] **Step 8: Run the tests**

Run: `cmake --build build --target jadefx-tests --parallel && ./build/jadefx-tests`
Expected: all pass, no `FAIL` lines.

- [ ] **Step 9: Check the shaders still link** — run a sample in a real window for a few seconds: `cmake --build build --target jadefx-hello --parallel && (./build/jadefx-hello.app/Contents/MacOS/jadefx-hello & sleep 4; kill %1)`. Expected: the window draws; no "OpenGL error" or shader link error on stderr.

- [ ] **Step 10: Commit** (in the JadeFX worktree)

```bash
git add src/gl/Occluder.hpp src/gl/UiRenderer.hpp src/gl/UiRenderer.cpp res/shaders/box.frag res/shaders/text.frag res/shaders/text_gray.frag res/shaders/image.frag include/jadefx/scene/Painter.hpp src/scene/Painter.cpp tests/occluder_tests.cpp tests/layout_tests.cpp CMakeLists.txt
git commit -m "Let a Painter hide UI behind a depth texture, for UI drawn inside a 3D view

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The BillboardGui class

**Files:**
- Modify: `src/engine_instances/Gui.hpp`, `src/engine_instances/Gui.cpp`
- Modify: `src/engine_core/ScriptBindings.cpp`, `src/engine_core/Project.cpp`, `src/ide/IdeIcons.cpp`, `src/engine_instances/README.md`
- Test: `sandbox/gui_tests.cpp`

**Interfaces:**
- Consumes: `DataModel::set_instance_reference`, `DataModel::instance_reference_slot`, `PVInstance::transform()`.
- Produces:
  ```cpp
  // engine_core, Gui.hpp
  enum class GuiProperty { ..., AlwaysOnTop, Count };   // new slot, before Count
  class BillboardGui : public GuiBase {
      LuaSlot adornee() const;
      InstanceId adornee_id() const;                       // live target or 0
      std::optional<std::string> set_adornee(const LuaSlot& value);  // SimulationThread
      InstanceId anchor_instance() const;                  // Adornee, else PVInstance parent, else 0
      Vec3 anchor() const;                                 // anchor_instance()'s translation, or origin
      bool drawn() const;                                  // in Workspace or Core, not inside a GuiBase
      bool always_on_top() const { return flag(GuiProperty::AlwaysOnTop); }
  };
  ```

- [ ] **Step 1: Write the failing tests** — append to `sandbox/gui_tests.cpp` (add `#include "GameObject.hpp"`, `#include "Folder.hpp"` and `#include "Matrix4.hpp"` to the includes):

```cpp
namespace {

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::LuaSlot boolean(bool value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

engine_core::GameObject& part_at(engine_core::DataModel& game, float x, float y, float z) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_parent(part.id(), game.scene_service("Workspace"));
    part.set_transform(engine_core::matrix4_translation(x, y, z));
    return part;
}

}  // namespace

TEST_CASE("GUI7 BillboardGui is a GuiBase with Adornee and AlwaysOnTop", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::lua_creatable_known("BillboardGui"));
    REQUIRE(engine_core::project_class_known("BillboardGui"));
    REQUIRE(engine_core::lua_class_inherits("BillboardGui", "GuiBase"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("BillboardGui", "ScreenGui"));
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    REQUIRE(std::string(board.class_name()) == "BillboardGui");
    REQUIRE_FALSE(board.always_on_top());
    REQUIRE(board.adornee().kind == engine_core::LuaSlot::Kind::Nil);
    engine_core::PropertyBag saved;
    board.save_properties(saved);
    REQUIRE(saved.empty());
}

TEST_CASE("GUI8 its anchor is the Adornee, else a PVInstance parent, else the origin", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& parent = part_at(game, 1.f, 2.f, 3.f);
    engine_core::GameObject& other = part_at(game, -4.f, 5.f, -6.f);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE(board.anchor_instance() == 0);
    REQUIRE(board.anchor().x == 0.f);

    game.set_parent(board.id(), parent.id());
    REQUIRE(board.anchor_instance() == parent.id());
    REQUIRE(board.anchor().y == 2.f);
    // The parent link is never written to Adornee.
    REQUIRE(board.adornee().kind == engine_core::LuaSlot::Kind::Nil);

    REQUIRE_FALSE(board.set_adornee(instance_slot(other.id())));
    REQUIRE(board.anchor_instance() == other.id());
    REQUIRE(board.anchor().z == -6.f);

    // Moving the part moves the anchor with it.
    other.set_transform(engine_core::matrix4_translation(7.f, 0.f, 0.f));
    REQUIRE(board.anchor().x == 7.f);

    // Moved out from under its parent, with no Adornee, it stops following.
    REQUIRE_FALSE(board.set_adornee(engine_core::LuaSlot{}));
    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE(board.anchor_instance() == 0);
}

TEST_CASE("GUI9 Adornee refuses a non-PVInstance, saves, undoes, and survives its target's destroy", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = part_at(game, 0.f, 9.f, 0.f);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), game.scene_service("Workspace"));
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(*board.set_adornee(instance_slot(folder.id())) == "Adornee must be a PVInstance");

    begin_step(game, "Set Adornee");
    REQUIRE_FALSE(board.set_adornee(instance_slot(part.id())));
    end_step(game);
    engine_core::PropertyBag saved;
    board.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Adornee") != nullptr);
    game.history().undo();
    REQUIRE(board.adornee_id() == 0);
    game.history().redo();
    REQUIRE(board.adornee_id() == part.id());

    begin_step(game, "Delete");
    game.destroy(part.id());
    end_step(game);
    REQUIRE(board.adornee_id() == 0);
    REQUIRE(board.anchor_instance() == 0);
    game.history().undo();
    REQUIRE(board.adornee_id() != 0);
    REQUIRE(board.anchor().y == 9.f);
}

TEST_CASE("GUI10 a BillboardGui is drawn in Workspace or Core, and not inside another GUI", "[gui][billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    REQUIRE_FALSE(board.drawn());
    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE(board.drawn());
    engine_core::GameObject& part = part_at(game, 0.f, 0.f, 0.f);
    game.set_parent(board.id(), part.id());
    REQUIRE(board.drawn());
    game.set_parent(board.id(), game.core());
    REQUIRE(board.drawn());
    game.set_parent(board.id(), game.scene_service("Storage"));
    REQUIRE_FALSE(board.drawn());
    game.set_parent(board.id(), game.scene_service("Gui"));
    REQUIRE_FALSE(board.drawn());

    engine_core::BillboardGui& outer = game.create<engine_core::BillboardGui>();
    game.set_parent(outer.id(), game.scene_service("Workspace"));
    game.set_parent(board.id(), outer.id());
    REQUIRE_FALSE(board.drawn());
    engine_core::Pane& pane = game.create<engine_core::Pane>();
    game.set_parent(pane.id(), game.scene_service("Workspace"));
    game.set_parent(board.id(), pane.id());
    REQUIRE_FALSE(board.drawn());
}

TEST_CASE("GUI11 scripts set Adornee and AlwaysOnTop, and both fire Changed", "[gui][billboard]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local part = Instance.new("GameObject", workspace)
        local board = Instance.new("BillboardGui", part)
        _G.defaults = board.AlwaysOnTop == false and board.Adornee == nil and board:IsA("GuiBase")
        local changed = {}
        board.Changed:Connect(function(name) changed[name] = true end)
        board.AlwaysOnTop = true
        board.Adornee = part
        _G.set = board.AlwaysOnTop == true and board.Adornee == part
        _G.refused = not pcall(function() board.Adornee = workspace end)
            and not pcall(function() board.AlwaysOnTop = 3 end)
        task.wait()
        _G.changed = changed.AlwaysOnTop == true and changed.Adornee == true
        _G.label = pcall(function() Instance.new("Label", board) end)
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    require_globals(rig, {"defaults", "set", "refused", "changed", "label"});
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: compile errors, `BillboardGui` is not a member of `engine_core`.

- [ ] **Step 3: The property slot** — in `Gui.hpp`, add `AlwaysOnTop,` to `enum class GuiProperty` just before `Count`. In `Gui.cpp`, append to `kSpecs` (it is in GuiProperty order, so it must be the last entry):

```cpp
    {"AlwaysOnTop", "boolean", LuaSlot::Kind::Bool, 0, 0},
```

and in `GuiValues::default_value`, before `case GuiProperty::Count:`:

```cpp
    case GuiProperty::AlwaysOnTop:
        return bool_slot(false);
```

- [ ] **Step 4: The class** — in `Gui.hpp`, add `#include "InstanceRef.hpp"` and `#include <optional>`. Extend the header comment's class list after the `ScreenGui` paragraph:

```cpp
// BillboardGui  a GuiBase drawn in the 3D world, not over it: centred on its
//            anchor and facing the camera, while it is in Workspace or Core
//            and not inside another GUI (runner::GuiLayer draws it).
//   Adornee      PVInstance?  what it floats over. Nil: the parent, if it is
//                             a PVInstance, else the world origin. nil.
//   AlwaysOnTop  boolean      drawn over the world, which never hides it;
//                             otherwise nearer surfaces do. false.
//   A percentage width or height on the BillboardGui itself is world units:
//   100% is one unit at its distance from the camera, so calc(200% + 32px)
//   is two units and 32 pixels. Its children's percentages are of their
//   parents, as in any CSS.
```

and replace the sentence "Every ScreenGui under the Gui service, directly or through Folders, is drawn over the Scene View; one anywhere else is only data." with "Every ScreenGui under the Gui service, directly or through Folders, is drawn over the Scene View, and every BillboardGui in Workspace or Core inside it; either anywhere else is only data." Add `billboardgui` to the element type list in the "The Name of a GuiBase..." paragraph.

After `class ScreenGui`:

```cpp
class BillboardGui : public GuiBase {
public:
    BillboardGui(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;

    bool always_on_top() const { return flag(GuiProperty::AlwaysOnTop); }
    // Adornee as a script reads it, and its live target, or 0.
    LuaSlot adornee() const;
    InstanceId adornee_id() const;
    // SimulationThread. nil clears it; anything but a live PVInstance is
    // refused, and returns why.
    std::optional<std::string> set_adornee(const LuaSlot& value);
    // What it floats over: Adornee, else the parent when that is a PVInstance,
    // else 0 for the world origin.
    InstanceId anchor_instance() const;
    // anchor_instance()'s Transform translation, or the origin.
    Vec3 anchor() const;
    // In Workspace or Core, at any depth, and not inside a GuiBase.
    bool drawn() const;

protected:
    void on_reuse() override;

private:
    InstanceRef adornee_ref_;
};
```

- [ ] **Step 5: Its implementation** — in `Gui.cpp` add `#include "PVInstance.hpp"`, and after `const char* ScreenGui::class_name() ...`:

```cpp
BillboardGui::BillboardGui(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : GuiBase(tag, state, id) {
    reset_values();
}

const char* BillboardGui::class_name() const { return "BillboardGui"; }

LuaSlot BillboardGui::adornee() const { return instance_reference_slot(adornee_ref_, "PVInstance"); }

InstanceId BillboardGui::adornee_id() const {
    const LuaSlot slot = adornee();
    return slot.kind == LuaSlot::Kind::Instance ? slot.id : 0;
}

std::optional<std::string> BillboardGui::set_adornee(const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    return set_instance_reference("Adornee", "PVInstance", adornee_ref_, value);
}

InstanceId BillboardGui::anchor_instance() const {
    if (const InstanceId linked = adornee_id(); linked != 0) {
        return linked;
    }
    const InstanceId above = parent(id());
    if (above == kNoParent || above == 0) {
        return 0;
    }
    return dynamic_cast<const PVInstance*>(instance(above)) != nullptr ? above : 0;
}

Vec3 BillboardGui::anchor() const {
    const InstanceId target = anchor_instance();
    const auto* object = target != 0 ? dynamic_cast<const PVInstance*>(instance(target)) : nullptr;
    if (object == nullptr) {
        return Vec3{};
    }
    const Matrix4 transform = object->transform();
    return Vec3{transform.m[12], transform.m[13], transform.m[14]};
}

bool BillboardGui::drawn() const {
    if (!in_workspace(id()) && !in_core(id())) {
        return false;
    }
    for (InstanceId above = parent(id()); above != kNoParent && above != 0; above = parent(above)) {
        if (dynamic_cast<const GuiBase*>(instance(above)) != nullptr) {
            return false;
        }
    }
    return true;
}

void BillboardGui::on_reuse() {
    GuiValues::on_reuse();
    adornee_ref_.set_guid(std::string());
}
```

- [ ] **Step 6: Registration** — in `Gui.cpp`'s anonymous namespace before `ANARCHY_LUA_REGISTER`:

```cpp
bool read_adornee(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* board = dynamic_cast<const BillboardGui*>(&object);
    if (board == nullptr) {
        return false;
    }
    out = board->adornee();
    return true;
}

bool write_adornee(DataModel&, DataModel& object, LuaSlot& in) {
    auto* board = dynamic_cast<BillboardGui*>(&object);
    if (board == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = board->set_adornee(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}
```

Inside `register_gui_lua`, after `register_lua_class("ScreenGui", "GuiBase", nullptr, 0);`:

```cpp
    const LuaField billboard[] = {
        lua_saved_property("Adornee", "PVInstance?", read_adornee, write_adornee, "null"),
        gui_field<GuiProperty::AlwaysOnTop>("BillboardGui"),
    };
    add_class("BillboardGui", "GuiBase", billboard);
```

Change the suited-parent lines at the end to:

```cpp
    register_suited_parents("ScreenGui", {"Gui"});
    register_suited_parents("BillboardGui", {"Workspace", "PVInstance"});
    register_suited_parents("GuiBasePane", {"ScreenGui", "BillboardGui", "GuiBasePane"});
    for (const char* control : {"Label", "Button", "TextField"}) {
        register_suited_parents(control, {"ScreenGui", "BillboardGui", "GuiBasePane"});
    }
```

and update the comment above them: "A ScreenGui goes only in Gui, a BillboardGui in Workspace or on a PVInstance. Panes and controls go in either, or a pane; ..."

- [ ] **Step 7: Creatable, loadable, icon, README**
  - `src/engine_core/ScriptBindings.cpp`: `DataModel& create_billboard_gui(DataModel& world) { return world.create<BillboardGui>(); }` after `create_screen_gui`, and `register_lua_creatable("BillboardGui", create_billboard_gui);` after the ScreenGui line.
  - `src/engine_core/Project.cpp`: after the `"ScreenGui"` entry, `out.push_back({"BillboardGui", [](DataModel& world) -> DataModel& { return world.create<BillboardGui>(); }});`
  - `src/ide/IdeIcons.cpp:54`: `if (class_name == "Gui" || class_name == "ScreenGui" || class_name == "BillboardGui") {`
  - `src/engine_instances/README.md` line 19: add `BillboardGui` after `ScreenGui` in the list, and append the sentence: "A `BillboardGui` is drawn in the 3D world at its anchor (`anchor()`: `Adornee`, else a PVInstance parent, else the origin) while `drawn()`: in Workspace or Core and not inside another GUI."

- [ ] **Step 8: Run the tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[gui]"`
Expected: GUI1–GUI11 pass. If `lua_class_inherits("GameObject", "PVInstance")` turns out false so `set_adornee` refuses a GameObject, stop and report: the spec assumes GameObject and PhysicsObject are PVInstances in the Lua registry as in C++.

- [ ] **Step 9: Run the whole sandbox** — `./build/sandbox` — expected all pass (catches anything that counted GuiProperty slots or class lists).

- [ ] **Step 10: Commit**

```bash
git add src/engine_instances/Gui.hpp src/engine_instances/Gui.cpp src/engine_core/ScriptBindings.cpp src/engine_core/Project.cpp src/ide/IdeIcons.cpp src/engine_instances/README.md sandbox/gui_tests.cpp
git commit -m "Add BillboardGui, a GuiBase with an Adornee and AlwaysOnTop that floats over a PVInstance

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Find billboards without a walk

**Files:**
- Modify: `src/engine_core/DataModel.hpp`, `src/engine_core/DataModel.cpp`, `src/engine_core/DataModelPlace.cpp`, `src/engine_core/DataModelState.hpp`, `src/engine_core/Ecs.hpp`, `src/engine_core/Ecs.cpp`, `src/engine_instances/Gui.hpp`
- Test: `sandbox/billboard_tests.cpp` (new), `CMakeLists.txt`

**Interfaces:**
- Produces: `virtual bool DataModel::billboard_gui() const` (false; true on BillboardGui) and `void DataModel::billboards(std::vector<InstanceId>& out) const` — every BillboardGui under game, in no set order, `out` cleared first. Callers filter with `BillboardGui::drawn()`.

- [ ] **Step 1: Write the failing test** — `sandbox/billboard_tests.cpp`

```cpp
// BillboardGui in the engine: how the snapshot finds and publishes it.

#include "support.hpp"

#include "GameObject.hpp"
#include "Gui.hpp"
#include "Matrix4.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

TEST_CASE("BB1 DataModel lists every BillboardGui under game, and no other class", "[billboard]") {
    SimRole role;
    engine_core::Game game;
    std::vector<engine_core::InstanceId> found;
    game.billboards(found);
    REQUIRE(found.empty());
    engine_core::BillboardGui& a = game.create<engine_core::BillboardGui>();
    engine_core::BillboardGui& b = game.create<engine_core::BillboardGui>();
    engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
    game.set_parent(a.id(), game.scene_service("Workspace"));
    game.set_parent(b.id(), game.scene_service("Storage"));
    game.set_parent(screen.id(), game.scene_service("Gui"));
    game.billboards(found);
    std::sort(found.begin(), found.end());
    std::vector<engine_core::InstanceId> expected{a.id(), b.id()};
    std::sort(expected.begin(), expected.end());
    REQUIRE(found == expected);
    game.set_parent(b.id(), engine_core::DataModel::kNoParent);
    game.billboards(found);
    REQUIRE(found == std::vector<engine_core::InstanceId>{a.id()});
}
```

Add `sandbox/billboard_tests.cpp` after `sandbox/gui_tests.cpp` in the `sandbox` target in `CMakeLists.txt`.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: compile error, no member `billboards` in `Game`.

- [ ] **Step 3: The tag, following `dragger()` exactly**
  - `Ecs.hpp`: after `struct DraggerTag {};` add `// A BillboardGui: its class's billboard_gui() is true.` / `struct BillboardTag {};`; in `EcsIds` add `ecs_id_t billboard = 0;` after `dragger`.
  - `Ecs.cpp`: after the `ids.dragger = ...` line, `ids.billboard = world.component<ecs::BillboardTag>().id();`
  - `DataModel.hpp`: after `virtual bool dragger() const ...` add
    ```cpp
    // True for a class runner::GuiLayer draws in the 3D world (BillboardGui).
    // Read once, when its entity is issued.
    virtual bool billboard_gui() const { return false; }
    ```
    and after `void draggers(...) const;`:
    ```cpp
    // The BillboardGuis under game (billboard_gui()), in no set order, into
    // out, which is cleared first. Whether each is drawn is BillboardGui::drawn.
    void billboards(std::vector<InstanceId>& out) const;
    ```
  - `DataModelState.hpp`: after `dragger_query`, `// BillboardGuis under game: Instance (in), with BillboardTag and InGame.` / `flecs::query<> billboard_query;`
  - `DataModel.cpp`: build `world.billboard_query` exactly as `dragger_query` with `ecs::BillboardTag`; in the create path after the `dragger()` block add `if (object->billboard_gui()) { ecs_add_id(ecs_world(), world.slots[index].entity, world.ecs_ids.billboard); }`; add `DataModel::billboards` as a copy of `DataModel::draggers` using `billboard_query`.
  - `DataModelPlace.cpp`: after its `sound_source()` block (a place load creates saved instances, and BillboardGuis are saved, unlike Draggers): `if (object->billboard_gui()) { ecs_add_id(ecs_world(), part.entity, state_->ecs_ids.billboard); }`
  - `Gui.hpp`: in `BillboardGui`, `bool billboard_gui() const override { return true; }`.

- [ ] **Step 4: Add a load case to BB1** — append to BB1 (add `#include "Project.hpp"`):

```cpp
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& saved = project.datamodel();
        engine_core::BillboardGui& board = saved.create<engine_core::BillboardGui>();
        saved.set_parent(board.id(), saved.scene_service("Workspace"));
        project.save();
    }
    engine_core::Game loadedGame;
    engine_core::Project loaded = engine_core::Project::load(dir.path, loadedGame);
    loadedGame.billboards(found);
    REQUIRE(found.size() == 1);
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[billboard]" && ./build/sandbox`
Expected: BB1 passes; the whole sandbox passes.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelPlace.cpp src/engine_core/DataModelState.hpp src/engine_core/Ecs.hpp src/engine_core/Ecs.cpp src/engine_instances/Gui.hpp sandbox/billboard_tests.cpp CMakeLists.txt
git commit -m "List the BillboardGuis under game through an ECS tag, as Draggers are

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: A snapshot a Scene View can hold from layout to paint

**Why:** Two Scene Views share one `SceneFeed`. `latest()` keeps its buffer only until anyone's next `latest()`, so view A holding a snapshot from its layout to its paint would have it recycled by view B's layout in between.

**Files:**
- Modify: `src/runner/SceneFeed.hpp`, `src/runner/SceneFeed.cpp`
- Test: `tests/SceneFeedTest.cpp`

**Interfaces:**
- Produces: `std::shared_ptr<const engine_core::VisualSnapshot> SceneFeed::hold();` — the newest finished snapshot, unchanged for as long as the caller keeps the pointer, whatever other readers do. `latest()` keeps its contract (now implemented on `hold()`).

- [ ] **Step 1: Write the failing test** — add to `tests/SceneFeedTest.cpp` before the function that runs the tests, and call it from there with the others:

```cpp
void TestHeldFramesAreNotWrittenOver() {
    runner::SceneFeed feed;
    feed.perform(Frame(1, 3));
    // Two views each hold the frame from their layout to their paint.
    const std::shared_ptr<const engine_core::VisualSnapshot> a = feed.hold();
    feed.perform(Frame(2, 3));
    const std::shared_ptr<const engine_core::VisualSnapshot> b = feed.hold();
    for (std::uint64_t n = 3; n < 40; ++n) {
        feed.perform(Frame(n, 5));
        (void)feed.latest();
    }
    Expect(a->frame == 1 && a->instances.size() == 3 && Whole(*a), "a held frame stays as it was");
    Expect(b->frame == 2 && Whole(*b), "so does a second view's");
    Expect(feed.hold()->frame == 39, "hold gives the newest frame");
    Expect(feed.hold().get() == feed.hold().get(), "with no new frame, the same one");
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --parallel --target engine-tests`
Expected: compile error, no member `hold`.

- [ ] **Step 3: Pooled buffers** — replace the class body in `SceneFeed.hpp`:

```cpp
// Hands each published VisualSnapshot from the engine's render thread to the
// Scene Views, which draw on the UI thread. The engine calls perform() with
// its front snapshot; a view calls hold() in its layout and keeps the pointer
// through its paint, so both see the same frame. perform() writes only a
// buffer no one holds, so neither side waits on the other's copy or draw, and
// a view's frame is never written over however many views read.
class SceneFeed : public engine_core::IRenderer {
public:
    SceneFeed();

    // RenderThread. Copies front, unless it is the frame already handed over.
    void perform(const engine_core::VisualSnapshot& front) override;
    void present() override {}

    // UI thread. The newest snapshot perform() finished, unchanged while held.
    // Empty before the first frame.
    std::shared_ptr<const engine_core::VisualSnapshot> hold();
    // UI thread. The same, held by the feed until the next latest() call.
    const engine_core::VisualSnapshot& latest();

private:
    std::mutex mu_;
    // Every buffer. One only the pool holds is free to write. Reused, so
    // vectors and strings keep their storage.
    std::vector<std::shared_ptr<engine_core::VisualSnapshot>> pool_;
    std::shared_ptr<engine_core::VisualSnapshot> newest_;
    std::shared_ptr<const engine_core::VisualSnapshot> latest_;
    // RenderThread only.
    std::uint64_t last_frame_ = 0;
};
```

Add `#include <memory>` and `#include <vector>`.

- [ ] **Step 4: Implementation** — `SceneFeed.cpp`:

```cpp
SceneFeed::SceneFeed() {
    newest_ = std::make_shared<engine_core::VisualSnapshot>();
    pool_.push_back(newest_);
}

void SceneFeed::perform(const engine_core::VisualSnapshot& front) {
    // A Prepare that missed the lock presents the previous frame again.
    if (front.frame == last_frame_ && front.frame != 0) {
        return;
    }
    last_frame_ = front.frame;
    std::shared_ptr<engine_core::VisualSnapshot> out;
    {
        // Only the pool holding a buffer means no reader can reach it: readers
        // copy only newest_, under this lock. Copying it into out marks it taken.
        std::lock_guard<std::mutex> guard(mu_);
        for (const auto& buffer : pool_) {
            if (buffer.use_count() == 1) {
                out = buffer;
                break;
            }
        }
        if (!out) {
            out = std::make_shared<engine_core::VisualSnapshot>();
            pool_.push_back(out);
        }
    }
    // Element by element into a buffer that held a recent frame, so its
    // vectors and strings keep their storage.
    out->frame = front.frame;
    out->camera = front.camera;
    out->instances.resize(front.instances.size());
    std::copy(front.instances.begin(), front.instances.end(), out->instances.begin());
    out->prefabs.resize(front.prefabs.size());
    std::copy(front.prefabs.begin(), front.prefabs.end(), out->prefabs.begin());
    out->lighting = front.lighting;
    out->sky = front.sky;
    out->draggers = front.draggers;
    out->resources_root = front.resources_root;
    std::lock_guard<std::mutex> guard(mu_);
    newest_ = std::move(out);
}

std::shared_ptr<const engine_core::VisualSnapshot> SceneFeed::hold() {
    std::lock_guard<std::mutex> guard(mu_);
    return newest_;
}

const engine_core::VisualSnapshot& SceneFeed::latest() {
    latest_ = hold();
    return *latest_;
}
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --parallel --target engine-tests && ./build/engine-tests`
Expected: all pass, including the existing torn-frame and ordering tests.

- [ ] **Step 6: Commit**

```bash
git add src/runner/SceneFeed.hpp src/runner/SceneFeed.cpp tests/SceneFeedTest.cpp
git commit -m "Let a Scene View hold a snapshot from its layout to its paint, whatever other views read

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Billboard rows in the snapshot

**Files:**
- Modify: `src/engine_core/SnapshotPump.hpp`, `src/engine_core/SnapshotPump.cpp`, `src/runner/SceneFeed.cpp`
- Test: `sandbox/billboard_tests.cpp`, `tests/SceneFeedTest.cpp`

**Interfaces:**
- Produces, in `SnapshotPump.hpp`:
  ```cpp
  struct VisualBillboard {
      InstanceId id = 0;
      // anchor_instance(), or 0 for the origin.
      InstanceId anchor_instance = 0;
      Vec3 anchor{};
      bool always_on_top = false;
  };
  // VisualSnapshot gains: std::vector<VisualBillboard> billboards;
  ```
  A row exists for each BillboardGui that is `drawn()` and Visible. When `anchor_instance` has a row in the same snapshot's `instances`, `anchor` is that row's world translation, after overrides, so the two can never disagree.

- [ ] **Step 1: Write the failing tests** — append to `sandbox/billboard_tests.cpp`:

```cpp
namespace {

const engine_core::VisualSnapshot& publish(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
    return pump.front();
}

engine_core::GameObject& part_at(engine_core::DataModel& game, float x, float y, float z) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_parent(part.id(), game.scene_service("Workspace"));
    part.set_transform(engine_core::matrix4_translation(x, y, z));
    return part;
}

engine_core::LuaSlot boolean(bool value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

}  // namespace

TEST_CASE("BB2 a drawn, visible BillboardGui publishes one row; others none", "[billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), game.scene_service("Storage"));
    REQUIRE(publish(pump, game).billboards.empty());

    game.set_parent(board.id(), game.scene_service("Workspace"));
    REQUIRE_FALSE(board.set_value(engine_core::GuiProperty::AlwaysOnTop, boolean(true)));
    {
        const engine_core::VisualSnapshot& shot = publish(pump, game);
        REQUIRE(shot.billboards.size() == 1);
        REQUIRE(shot.billboards[0].id == board.id());
        REQUIRE(shot.billboards[0].anchor_instance == 0);
        REQUIRE(shot.billboards[0].anchor.x == 0.f);
        REQUIRE(shot.billboards[0].always_on_top);
    }
    REQUIRE_FALSE(board.set_value(engine_core::GuiProperty::Visible, boolean(false)));
    REQUIRE(publish(pump, game).billboards.empty());
}

TEST_CASE("BB3 a billboard's anchor and its adornee's row come from the same frame", "[billboard]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::GameObject& part = part_at(game, 0.f, 0.f, -10.f);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), part.id());
    for (int step = 0; step < 20; ++step) {
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(step), 1.f, -10.f));
        const engine_core::VisualSnapshot& shot = publish(pump, game);
        REQUIRE(shot.billboards.size() == 1);
        const engine_core::VisualInstance* row = nullptr;
        for (const engine_core::VisualInstance& inst : shot.instances) {
            if (inst.id == part.id()) {
                row = &inst;
            }
        }
        REQUIRE(row != nullptr);
        REQUIRE(shot.billboards[0].anchor_instance == part.id());
        REQUIRE(shot.billboards[0].anchor.x == row->world.m[12]);
        REQUIRE(shot.billboards[0].anchor.x == static_cast<float>(step));
    }
    // A path-C override moves the drawn row; the anchor follows it.
    engine_core::SnapshotOverride moved;
    moved.id = part.id();
    moved.transform = engine_core::matrix4_translation(50.f, 0.f, 0.f);
    pump.override_visual(moved);
    REQUIRE(publish(pump, game).billboards[0].anchor.x == 50.f);
}
```

In `tests/SceneFeedTest.cpp`, extend `Frame()` to add `engine_core::VisualBillboard board; board.id = 99; board.anchor.x = static_cast<float>(number); snapshot.billboards.push_back(board);`, and make `Whole()` also require `snapshot.frame == 0 || (snapshot.billboards.size() == 1 && snapshot.billboards[0].anchor.x == static_cast<float>(snapshot.frame))`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --parallel --target sandbox engine-tests`
Expected: compile errors, no member `billboards` in `VisualSnapshot`.

- [ ] **Step 3: The row type** — in `SnapshotPump.hpp` after `struct VisualDragger {...};`:

```cpp
// A BillboardGui runner::GuiLayer draws: drawn() and Visible. anchor is its
// anchor_instance()'s world translation; when that instance has a row in the
// same snapshot, it is that row's, after overrides, so a billboard and what
// it floats over are always where the same frame put them.
struct VisualBillboard {
    InstanceId id = 0;
    // 0 for the world origin.
    InstanceId anchor_instance = 0;
    Vec3 anchor{};
    bool always_on_top = false;
};
```

Add to `VisualSnapshot` after `draggers`: `// Rebuilt at every take_changes, like draggers.` / `std::vector<VisualBillboard> billboards;`. In `SnapshotPump`'s private section: `void resolve_billboards(DataModel& game);`, `void anchor_billboards(VisualSnapshot& dst) const;`, and `std::vector<InstanceId> billboard_ids_;` beside `dragger_ids_`.

- [ ] **Step 4: The pump** — in `SnapshotPump.cpp` add `#include "Gui.hpp"`, and:

```cpp
void SnapshotPump::resolve_billboards(DataModel& game) {
    base_.billboards.clear();
    game.billboards(billboard_ids_);
    // In id order, so the rows keep one order from frame to frame.
    std::sort(billboard_ids_.begin(), billboard_ids_.end());
    for (InstanceId id : billboard_ids_) {
        const auto* board = dynamic_cast<const BillboardGui*>(game.instance(id));
        if (board == nullptr || !board->drawn() || !board->flag(GuiProperty::Visible)) {
            continue;
        }
        VisualBillboard row;
        row.id = id;
        row.anchor_instance = board->anchor_instance();
        row.anchor = board->anchor();
        row.always_on_top = board->always_on_top();
        base_.billboards.push_back(row);
    }
}

void SnapshotPump::anchor_billboards(VisualSnapshot& dst) const {
    for (VisualBillboard& row : dst.billboards) {
        if (row.anchor_instance == 0) {
            continue;
        }
        const int position = base_ids_.position(row.anchor_instance);
        if (position < 0 || static_cast<std::size_t>(position) >= dst.instances.size()) {
            continue;
        }
        const VisualInstance& inst = dst.instances[static_cast<std::size_t>(position)];
        if (inst.id == row.anchor_instance && inst.alive) {
            row.anchor = Vec3{inst.world.m[12], inst.world.m[13], inst.world.m[14]};
        }
    }
}
```

Call `resolve_billboards(game);` after `resolve_draggers(game);` in `take_changes`. In `blit`, add `dst.billboards = base_.billboards;` after `dst.draggers = ...`. In `finish_copy`, call `anchor_billboards(back);` right after `apply_overrides(back);`.

- [ ] **Step 5: The feed** — in `SceneFeed::perform`, after `out->draggers = front.draggers;` add `out->billboards = front.billboards;`.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build --parallel --target sandbox engine-tests && ./build/sandbox "[billboard]" && ./build/engine-tests && ./build/sandbox`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine_core/SnapshotPump.hpp src/engine_core/SnapshotPump.cpp src/runner/SceneFeed.cpp sandbox/billboard_tests.cpp tests/SceneFeedTest.cpp
git commit -m "Publish each drawn BillboardGui's anchor in the snapshot, from the same frame as its adornee

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Billboard placement math, shared with the renderer

**Files:**
- Create: `src/runner/BillboardMath.hpp`, `src/runner/BillboardMath.cpp`
- Modify: `src/runner/RenderMath.hpp`, `src/runner/Renderer.hpp`, `src/runner/Renderer.cpp`, `CMakeLists.txt`
- Test: `sandbox/render_math_tests.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // runner, RenderMath.hpp
  constexpr float kSceneNear = 0.1f;
  constexpr float kSceneFar = 1000.f;
  // runner, BillboardMath.hpp
  struct BillboardPlacement {
      bool visible = false;      // false at or behind the near plane, or for bad input
      float x = 0.f;             // screen point in pane points, origin top left
      float y = 0.f;
      float pixelsPerUnit = 0.f; // points one world unit covers at this distance
      float distance = 0.f;      // along the camera's forward axis
      float depth = 0.f;         // window depth, 0 near to 1 far, as the depth texture holds it
  };
  BillboardPlacement PlaceBillboard(const engine_core::Matrix4& view, float fovYDegrees, float paneWidth,
                                    float paneHeight, engine_core::Vec3 anchor);
  // Renderer gains:
  const engine_core::Matrix4& view() const { return view_; }
  float fovYDegrees() const { return fovYDegrees_; }
  ```

- [ ] **Step 1: Write the failing tests** — append to `sandbox/render_math_tests.cpp` (add `#include "runner/BillboardMath.hpp"`):

```cpp
TEST_CASE("RM10 a billboard ahead of the camera is centred, sized by distance, at the renderer's depth", "[RM][billboard]") {
    // The default camera: at the origin, looking down -Z.
    const engine_core::Matrix4 view = engine_core::matrix4_identity();
    const runner::BillboardPlacement at10 = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -10.f});
    REQUIRE(at10.visible);
    REQUIRE(at10.x == Approx(400.f));
    REQUIRE(at10.y == Approx(300.f));
    // tan(45) is 1: one unit at ten units away is 600 / 20 points.
    REQUIRE(at10.pixelsPerUnit == Approx(30.f));
    REQUIRE(at10.distance == Approx(10.f));
    const engine_core::Matrix4 projection =
        runner::Perspective(90.f, 800.f / 600.f, runner::kSceneNear, runner::kSceneFar);
    const float ndc = matrix4_point(projection, {0.f, 0.f, -10.f}).z;
    REQUIRE(at10.depth == Approx(ndc * 0.5f + 0.5f));
    const runner::BillboardPlacement at20 = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -20.f});
    REQUIRE(at20.pixelsPerUnit == Approx(15.f));
    REQUIRE(at20.depth > at10.depth);
    // Up and to the right on screen is +X and +Y in view space.
    const runner::BillboardPlacement offset = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {5.f, 5.f, -10.f});
    REQUIRE(offset.x == Approx(400.f + 5.f * 30.f));
    REQUIRE(offset.y == Approx(300.f - 5.f * 30.f));
}

TEST_CASE("RM11 a billboard at or behind the near plane is hidden", "[RM][billboard]") {
    const engine_core::Matrix4 view = engine_core::matrix4_identity();
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, 10.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, 0.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -0.05f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 180.f, 800.f, 600.f, {0.f, 0.f, -10.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 0.f, 600.f, {0.f, 0.f, -10.f}).visible);
    REQUIRE_FALSE(runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {std::nanf(""), 0.f, -10.f}).visible);
}

TEST_CASE("RM12 a far billboard stays finite and visible", "[RM][billboard]") {
    const engine_core::Matrix4 view = engine_core::matrix4_identity();
    const runner::BillboardPlacement far = runner::PlaceBillboard(view, 60.f, 800.f, 600.f, {0.f, 0.f, -5000.f});
    REQUIRE(far.visible);
    REQUIRE(std::isfinite(far.pixelsPerUnit));
    REQUIRE(far.pixelsPerUnit > 0.f);
    REQUIRE(far.pixelsPerUnit < 1.f);
    // Past the far plane nothing is drawn to hide it; its depth stays at most 1.
    REQUIRE(far.depth <= 1.f);
}

TEST_CASE("RM13 the camera's own turn moves the billboard on screen", "[RM][billboard]") {
    // Turned 90 degrees left about Y, the camera looks down -X.
    const engine_core::Matrix4 world = engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 3.14159265 / 2.0);
    const engine_core::Matrix4 view = engine_core::matrix4_inverse(world);
    const runner::BillboardPlacement ahead = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {-10.f, 0.f, 0.f});
    REQUIRE(ahead.visible);
    REQUIRE(ahead.x == Approx(400.f).margin(0.01));
    // What was straight ahead of the unturned camera is now off to the side, or hidden.
    const runner::BillboardPlacement old = runner::PlaceBillboard(view, 90.f, 800.f, 600.f, {0.f, 0.f, -10.f});
    REQUIRE((!old.visible || std::abs(old.x - 400.f) > 100.f));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build build --parallel --target sandbox`
Expected: compile error, `runner/BillboardMath.hpp` not found.

- [ ] **Step 3: Shared planes** — in `RenderMath.hpp` after `kDegree`:

```cpp
// The Scene View's near and far planes, which the renderer and billboard
// placement both project with, so a billboard's depth is the scene's.
constexpr float kSceneNear = 0.1f;
constexpr float kSceneFar = 1000.f;
```

In `Renderer.cpp`, delete `constexpr float kNear = 0.1f;` and `constexpr float kFar = 1000.f;` (line ~85) and replace their uses (`Perspective(... kNear, kFar)` near line 660, `camera.nearZ = kNear;` near line 1026, and any other) with `kSceneNear` / `kSceneFar`. In `Renderer.hpp` public section, after `setCamera`:

```cpp
    // The view setCamera last took, world to view space, and its vertical angle.
    const engine_core::Matrix4& view() const { return view_; }
    float fovYDegrees() const { return fovYDegrees_; }
```

- [ ] **Step 4: The math** — `src/runner/BillboardMath.hpp`:

```cpp
#pragma once

#include "Matrix4.hpp"

// Where a BillboardGui's anchor lands in a Scene View, by the same camera and
// projection the renderer draws with (RenderMath), so the billboard sits
// exactly on what it floats over. Needs no GL context.
namespace runner {

struct BillboardPlacement {
    // False at or behind the near plane, or for a view, angle, or pane that
    // cannot be drawn: then nothing else is set.
    bool visible = false;
    // The anchor on screen, in pane points from the top left.
    float x = 0.f;
    float y = 0.f;
    // The points one world unit covers at the anchor's distance: what a
    // percentage of 100 on the billboard is.
    float pixelsPerUnit = 0.f;
    // Along the camera's forward axis, in world units.
    float distance = 0.f;
    // As the depth texture holds it: 0 at the near plane, 1 at the far.
    float depth = 0.f;
};

BillboardPlacement PlaceBillboard(const engine_core::Matrix4& view, float fovYDegrees, float paneWidth,
                                  float paneHeight, engine_core::Vec3 anchor);

}  // namespace runner
```

`src/runner/BillboardMath.cpp`:

```cpp
#include "BillboardMath.hpp"

#include "RenderMath.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

BillboardPlacement PlaceBillboard(const engine_core::Matrix4& view, float fovYDegrees, float paneWidth,
                                  float paneHeight, engine_core::Vec3 anchor) {
    BillboardPlacement out;
    if (!(fovYDegrees > 0.f && fovYDegrees < 180.f) || !(paneWidth > 0.f) || !(paneHeight > 0.f)) {
        return out;
    }
    const engine_core::Vec3 eye = engine_core::matrix4_point(view, anchor);
    const float distance = -eye.z;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !(distance > kSceneNear)) {
        return out;
    }
    const engine_core::Matrix4 projection =
        Perspective(fovYDegrees, paneWidth / paneHeight, kSceneNear, kSceneFar);
    const engine_core::Vec3 ndc = engine_core::matrix4_point(projection, eye);
    const float halfTan = std::tan(fovYDegrees * 0.5f * kDegree);
    out.visible = true;
    out.distance = distance;
    out.x = (ndc.x * 0.5f + 0.5f) * paneWidth;
    out.y = (0.5f - ndc.y * 0.5f) * paneHeight;
    out.pixelsPerUnit = paneHeight / (2.f * distance * halfTan);
    out.depth = std::min(1.f, ndc.z * 0.5f + 0.5f);
    return out;
}

}  // namespace runner
```

`matrix4_point` is declared in `Matrix4.hpp` (the tests use it as `engine_core::matrix4_point`) and divides by w; confirm it does, and if it does not, divide `ndc` by the clip w yourself.

Add `src/runner/BillboardMath.cpp` to the CMake source list beside `src/runner/RenderMath.cpp` (search `RenderMath.cpp` in `CMakeLists.txt`; add it to every list RenderMath.cpp is in).

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --parallel --target sandbox && ./build/sandbox "[RM]"`
Expected: RM1–RM13 pass (the existing RM tests prove the renderer change kept its numbers).

- [ ] **Step 6: Commit**

```bash
git add src/runner/BillboardMath.hpp src/runner/BillboardMath.cpp src/runner/RenderMath.hpp src/runner/Renderer.hpp src/runner/Renderer.cpp sandbox/render_math_tests.cpp CMakeLists.txt
git commit -m "Place a billboard's anchor by the renderer's own camera and planes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Billboards in the GUI layer, placed from the frame's snapshot

**Files:**
- Modify: `src/runner/GuiLayer.hpp`, `src/runner/GuiLayer.cpp`, `src/runner/GameView.hpp`, `src/runner/GameView.cpp`
- Test: `tests/BillboardLayerTest.cpp` (new), `tests/StudioLayoutTest.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `VisualSnapshot::billboards` (Task 5), `SceneFeed::hold()` (Task 4), `PlaceBillboard`, `Renderer::view()`, `Renderer::fovYDegrees()` (Task 6), `DataModel::billboards`, `BillboardGui::drawn()` (Tasks 2–3).
- Produces:
  ```cpp
  // runner, GuiLayer.hpp
  struct BillboardView {
      engine_core::Matrix4 view = engine_core::matrix4_identity();
      float fovYDegrees = 60.f;
      // The 3D pane in window points: what Renderer::draw is given.
      double paneX = 0, paneY = 0, paneWidth = 0, paneHeight = 0;
  };
  void GuiLayer::placeBillboards(const std::vector<engine_core::VisualBillboard>& rows, const BillboardView& view);
  // The cursor's scene depth from the last paint, or none. A depth-tested
  // billboard farther than it takes no mouse.
  void GuiLayer::setCursorDepth(std::optional<float> depth);
  // Children in paint order, for tests.
  std::vector<jadefx::Node*> GuiLayer::paintOrder() const;
  // For the occluded draw (Task 8): the depth a billboard node draws at, and
  // whether it is depth tested. Null for a node that is not a placed billboard.
  struct PlacedBillboard { float depth; bool alwaysOnTop; };
  const PlacedBillboard* GuiLayer::placedFor(const jadefx::Node* node) const;
  // runner, GameView.hpp
  void GameView::setSnapshotForTest(std::shared_ptr<const engine_core::VisualSnapshot> snapshot);
  ```

- [ ] **Step 1: Write the failing test** — `tests/BillboardLayerTest.cpp`:

```cpp
#include "ide/IdeLayout.hpp"
#include "runner/GameView.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "SnapshotPump.hpp"

#include "jadefx/jadefx.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

// BillboardGuis in the Scene View's GUI layer: placed from the frame's
// snapshot by its camera, sized in world units by CSS percentages, stacked by
// depth under the ScreenGuis, and passing the mouse where the scene hides them.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

bool Near(double a, double b) { return std::abs(a - b) < 0.75; }

runner::GameView* FindGameView(jadefx::Scene& scene) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* view = dynamic_cast<runner::GameView*>(node)) {
            return view;
        }
    }
    return nullptr;
}

}  // namespace

int RunBillboardLayerTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    double time = scene.timeSeconds() + 0.01;
    auto frame = [&] {
        for (int i = 0; i < 2; ++i) {
            scene.layout(1280, 800, time);
            time += 0.02;
        }
    };
    frame();
    runner::GameView* view = FindGameView(scene);
    Expect(view != nullptr, "the studio has a Scene View");
    if (view == nullptr) {
        return gFailures;
    }
    engine_core::Engine& engine = layout.simulation();

    engine_core::InstanceId camera = 0;
    engine_core::InstanceId near = 0;
    engine_core::InstanceId far = 0;
    engine_core::InstanceId top = 0;
    engine_core::InstanceId label = 0;
    engine_core::InstanceId button = 0;
    engine_core::InstanceId screen = 0;
    std::string cameraGuid;
    engine.on_simulation([&](engine_core::DataModel& game) {
        const engine_core::InstanceId workspace = game.scene_service("Workspace");
        camera = engine_core::lua_create_instance(game, "Camera")->id();
        game.set_parent(camera, workspace);
        cameraGuid = game.guid(camera);
        auto board = [&](const char* name) {
            const engine_core::InstanceId id = engine_core::lua_create_instance(game, "BillboardGui")->id();
            game.set_name(id, name);
            game.set_parent(id, workspace);
            return id;
        };
        near = board("Near");
        far = board("Far");
        top = board("Top");
        label = engine_core::lua_create_instance(game, "Label")->id();
        game.set_parent(label, near);
        button = engine_core::lua_create_instance(game, "Button")->id();
        game.set_parent(button, near);
        const engine_core::InstanceId sheet = engine_core::lua_create_instance(game, "CSS")->id();
        game.set_parent(sheet, near);
        dynamic_cast<engine_core::Css*>(game.instance(sheet))
            ->set_text(engine_core::GuiProperty::Source,
                       "#Near { width: calc(200% + 32px); height: 50%; } label { width: 50%; } "
                       "button { width: 10px; height: 10px; }");
        screen = engine_core::lua_create_instance(game, "ScreenGui")->id();
        game.set_parent(screen, game.scene_service("Gui"));
    });
    frame();
    view->linkCamera(cameraGuid);

    // The frame both the layout and the paint use: the Camera at the origin,
    // looking down -Z with a 90 degree view, and three billboards ahead.
    auto shot = std::make_shared<engine_core::VisualSnapshot>();
    shot->frame = 1;
    engine_core::VisualInstance cam;
    cam.id = camera;
    cam.field_of_view = 90.f;
    shot->instances.push_back(cam);
    auto row = [&](engine_core::InstanceId id, float z, bool onTop) {
        engine_core::VisualBillboard board;
        board.id = id;
        board.anchor = engine_core::Vec3{0.f, 0.f, z};
        board.always_on_top = onTop;
        shot->billboards.push_back(board);
    };
    row(near, -10.f, false);
    row(far, -20.f, false);
    row(top, -50.f, true);
    view->setSnapshotForTest(shot);
    frame();

    runner::GuiLayer& layer = view->guiLayer();
    jadefx::Node* nearNode = layer.nodeFor(near);
    jadefx::Node* labelNode = layer.nodeFor(label);
    Expect(nearNode != nullptr && labelNode != nullptr, "a BillboardGui in Workspace and its Label are drawn");
    if (nearNode == nullptr || labelNode == nullptr) {
        return gFailures;
    }
    Expect(std::string(nearNode->getElementType()) == "billboardgui", "its element type is billboardgui");

    const double ppu = view->getHeight() / 20.0;
    Expect(Near(nearNode->getWidth(), 2.0 * ppu + 32.0), "calc(200% + 32px) is two world units and 32 points");
    Expect(Near(nearNode->getHeight(), 0.5 * ppu), "50% tall is half a world unit");
    Expect(Near(labelNode->getWidth(), 0.5 * nearNode->getWidth()), "a child's 50% is half the billboard");
    Expect(Near(nearNode->getAbsoluteX() + nearNode->getWidth() / 2, view->getAbsoluteX() + view->getWidth() / 2) &&
               Near(nearNode->getAbsoluteY() + nearNode->getHeight() / 2,
                    view->getAbsoluteY() + view->getHeight() / 2),
           "it is centred on its anchor's point on screen");

    const std::vector<jadefx::Node*> order = layer.paintOrder();
    Expect(order.size() == 4 && order[0] == layer.nodeFor(far) && order[1] == nearNode &&
               order[2] == layer.nodeFor(top) && order[3] == layer.nodeFor(screen),
           "depth-tested billboards far to near, then AlwaysOnTop ones, then ScreenGuis");

    // The same frame moves the camera and the anchor together: the billboard
    // follows that frame, not the one before.
    auto moved = std::make_shared<engine_core::VisualSnapshot>(*shot);
    moved->frame = 2;
    moved->instances[0].world = engine_core::matrix4_translation(3.f, 0.f, 0.f);
    moved->billboards[0].anchor = engine_core::Vec3{3.f, 0.f, -10.f};
    view->setSnapshotForTest(moved);
    frame();
    Expect(Near(nearNode->getAbsoluteX() + nearNode->getWidth() / 2, view->getAbsoluteX() + view->getWidth() / 2),
           "a camera and anchor moved in one frame leave it centred");

    // Where the scene under the cursor is nearer, a depth-tested billboard passes the mouse.
    // The Button sits at the billboard's top left (Alignment TopLeft); aim at its middle.
    jadefx::Node* buttonNode = layer.nodeFor(button);
    Expect(buttonNode != nullptr, "the billboard's Button is drawn");
    if (buttonNode == nullptr) {
        return gFailures;
    }
    const double cx = buttonNode->getAbsoluteX() + buttonNode->getWidth() / 2;
    const double cy = buttonNode->getAbsoluteY() + buttonNode->getHeight() / 2;
    layer.setCursorDepth(0.5f);
    frame();
    Expect(layer.pick(cx, cy) != buttonNode, "a Button behind nearer scene takes no click");
    layer.setCursorDepth(1.f);
    frame();
    Expect(layer.pick(cx, cy) == buttonNode, "with the scene farther, it does");
    layer.setCursorDepth(std::nullopt);

    // Behind the camera, it is hidden.
    auto behind = std::make_shared<engine_core::VisualSnapshot>(*shot);
    behind->billboards[0].anchor = engine_core::Vec3{0.f, 0.f, 10.f};
    view->setSnapshotForTest(behind);
    frame();
    Expect(!nearNode->isVisible(), "a billboard behind the camera is hidden");

    view->setSnapshotForTest(nullptr);
    engine.on_simulation([&](engine_core::DataModel& game) {
        for (engine_core::InstanceId id : {near, far, top, screen, camera}) {
            game.destroy_tree(id);
        }
    });
    frame();
    Expect(layer.nodeFor(near) == nullptr, "removing a BillboardGui takes its node away");
    if (gFailures == 0) {
        std::printf("billboard layer tests passed\n");
    }
    return gFailures;
}
```

In `tests/StudioLayoutTest.cpp` declare `int RunBillboardLayerTests(ide::IdeLayout& layout, jadefx::Scene& scene);` beside `RunGuiStyleTests`, and add `failures += RunBillboardLayerTests(layout, *scene);` right after `failures += RunGuiStyleTests(layout, *scene);`. Add `tests/BillboardLayerTest.cpp` after `tests/GuiStyleTest.cpp` in `studio-tests` in `CMakeLists.txt`.

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --parallel --target studio-tests`
Expected: compile errors: no `setSnapshotForTest`, `paintOrder`, `setCursorDepth`.

- [ ] **Step 3: GuiLayer declarations** — in `GuiLayer.hpp` add `#include "SnapshotPump.hpp"`, `#include "Matrix4.hpp"`, `#include <optional>`; before `class GuiLayer` add `BillboardView` exactly as in Interfaces. Extend the class comment with a paragraph:

```cpp
// Every BillboardGui in Workspace or Core that is not inside another GUI is a
// node too, element type billboardgui, built and updated as a ScreenGui is.
// placeBillboards puts each where the frame's snapshot says, by the camera
// the renderer draws that frame with: centred on its anchor's point on screen
// and laid out with the points one world unit covers there as its available
// width and height, so a percentage on the billboard is world units. Their
// paint order is depth tested far to near, then AlwaysOnTop far to near, then
// the ScreenGuis. A depth-tested billboard takes no mouse while the scene
// under the cursor (setCursorDepth) is nearer than it.
```

Public additions: `placeBillboards`, `setCursorDepth`, `paintOrder`, `PlacedBillboard`, `placedFor` as in Interfaces. Private additions:

```cpp
    // A drawn BillboardGui's node and where this frame puts it.
    struct Placement {
        engine_core::InstanceId id = 0;
        std::shared_ptr<jadefx::Node> node;
        // False until placeBillboards finds it in front of the camera.
        bool placed = false;
        bool alwaysOnTop = false;
        // Its anchor's point on screen, in window points.
        double x = 0;
        double y = 0;
        double pixelsPerUnit = 0;
        double distance = 0;
        PlacedBillboard drawn{};
    };
    // The drawn BillboardGuis' nodes, made or brought up to date, in id order.
    void collectBillboards(std::vector<engine_core::InstanceId>& ids, std::vector<std::shared_ptr<jadefx::Node>>& nodes);
    // Sets the children to the billboards in paint order, then the ScreenGuis,
    // when that order changed.
    void restack();
    std::vector<Placement> placements_;
    std::vector<std::shared_ptr<jadefx::Node>> screens_;
    // The children as restack last set them.
    std::vector<jadefx::Node*> order_;
    std::optional<float> cursorDepth_;
```

Remove `shown_`: `screens_`, `placements_`, and `order_` replace it. `paintOrder()` returns `order_`.

- [ ] **Step 4: Nodes for billboards** — in `GuiLayer.cpp` add `#include "BillboardMath.hpp"` and `#include <algorithm>`.
  - In `makeNode`, after the `ScreenGui` branch:
    ```cpp
    } else if (className == "BillboardGui") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("billboardgui", input, false);
        // Like a ScreenGui, only what is in it takes the mouse.
        node->setPickOnBounds(false);
    ```
  - In `apply`, store `entry.visible = gui.flag(GuiProperty::Visible); entry.mouseTransparent = gui.flag(GuiProperty::MouseTransparent);` before setting them. Leave the Size block as it is: a BillboardGui honours Size as any GuiBase does, and only a ScreenGui ignores it.
  - In `build`, the nested-GUI skip must cover billboards too: `if (entry.container != nullptr && dynamic_cast<const engine_core::ScreenGui*>(object) == nullptr && dynamic_cast<const engine_core::BillboardGui*>(object) == nullptr)`.

- [ ] **Step 5: Sync** — in `sync()`, after the screens and the service CSS are collected, replace everything from `std::vector<jadefx::Node*> shown;` through the `if (shown != shown_) {...}` block with:

```cpp
    std::vector<engine_core::InstanceId> boardIds;
    std::vector<std::shared_ptr<jadefx::Node>> boardNodes;
    collectBillboards(boardIds, boardNodes);
    // Each billboard keeps last frame's placement until placeBillboards runs.
    std::vector<Placement> placements;
    placements.reserve(boardNodes.size());
    for (std::size_t i = 0; i < boardNodes.size(); ++i) {
        Placement next;
        for (const Placement& was : placements_) {
            if (was.id == boardIds[i] && was.node == boardNodes[i]) {
                next = was;
                break;
            }
        }
        next.id = boardIds[i];
        next.node = boardNodes[i];
        placements.push_back(std::move(next));
    }
    placements_ = std::move(placements);
    screens_ = std::move(screens);
    restack();
```

The sweep after it, which erases entries not seen this pass, stays as it is.

```cpp
void GuiLayer::collectBillboards(std::vector<engine_core::InstanceId>& ids,
                                 std::vector<std::shared_ptr<jadefx::Node>>& nodes) {
    std::vector<engine_core::InstanceId> found;
    game_.billboards(found);
    std::sort(found.begin(), found.end());
    for (engine_core::InstanceId id : found) {
        const auto* board = dynamic_cast<const engine_core::BillboardGui*>(game_.instance(id));
        if (board != nullptr && board->drawn()) {
            ids.push_back(id);
            nodes.push_back(build(id, *board));
        }
    }
}
```

- [ ] **Step 6: Placement and paint order**

```cpp
void GuiLayer::placeBillboards(const std::vector<engine_core::VisualBillboard>& rows, const BillboardView& view) {
    for (Placement& placement : placements_) {
        placement.placed = false;
        const engine_core::VisualBillboard* row = nullptr;
        for (const engine_core::VisualBillboard& candidate : rows) {
            if (candidate.id == placement.id) {
                row = &candidate;
                break;
            }
        }
        if (row != nullptr) {
            const BillboardPlacement where =
                PlaceBillboard(view.view, view.fovYDegrees, static_cast<float>(view.paneWidth),
                               static_cast<float>(view.paneHeight), row->anchor);
            placement.placed = where.visible;
            placement.alwaysOnTop = row->always_on_top;
            placement.x = view.paneX + where.x;
            placement.y = view.paneY + where.y;
            placement.pixelsPerUnit = where.pixelsPerUnit;
            placement.distance = where.distance;
            placement.drawn = PlacedBillboard{where.depth, row->always_on_top};
        }
        const auto found = entries_.find(placement.id);
        const bool shown = found != entries_.end() && found->second->visible;
        const bool userTransparent = found != entries_.end() && found->second->mouseTransparent;
        // The scene under the cursor is nearer: the mouse goes past this billboard.
        const bool behindScene = !placement.alwaysOnTop && cursorDepth_ && *cursorDepth_ < placement.drawn.depth;
        placement.node->setVisible(shown && placement.placed);
        placement.node->setMouseTransparent(!placement.placed || behindScene || userTransparent);
    }
    restack();
    requestLayout();
}

void GuiLayer::setCursorDepth(std::optional<float> depth) { cursorDepth_ = depth; }

void GuiLayer::restack() {
    std::vector<const Placement*> sorted;
    sorted.reserve(placements_.size());
    for (const Placement& placement : placements_) {
        sorted.push_back(&placement);
    }
    // Depth tested first, then on top; each far to near, ties by id so the order holds still.
    std::sort(sorted.begin(), sorted.end(), [](const Placement* a, const Placement* b) {
        if (a->alwaysOnTop != b->alwaysOnTop) {
            return !a->alwaysOnTop;
        }
        if (a->distance != b->distance) {
            return a->distance > b->distance;
        }
        return a->id < b->id;
    });
    std::vector<std::shared_ptr<jadefx::Node>> children;
    std::vector<jadefx::Node*> order;
    children.reserve(sorted.size() + screens_.size());
    for (const Placement* placement : sorted) {
        children.push_back(placement->node);
        order.push_back(placement->node.get());
    }
    for (const auto& screen : screens_) {
        children.push_back(screen);
        order.push_back(screen.get());
    }
    if (order != order_) {
        getChildren().setAll(std::move(children));
        order_ = std::move(order);
    }
}

const GuiLayer::PlacedBillboard* GuiLayer::placedFor(const jadefx::Node* node) const {
    for (const Placement& placement : placements_) {
        if (placement.node.get() == node && placement.placed) {
            return &placement.drawn;
        }
    }
    return nullptr;
}
```

and in the header, `std::vector<jadefx::Node*> paintOrder() const { return order_; }`.

- [ ] **Step 7: Layout** — replace `GuiLayer::layoutChildren`:

```cpp
void GuiLayer::layoutChildren() {
    for (const auto& screen : screens_) {
        screen->performLayout(contentLeft(), contentTop(), contentWidth(), contentHeight());
    }
    // A billboard's available size is one world unit at its distance, so a
    // percentage on it is world units; its content and Size work as anywhere.
    const double left = getAbsoluteX();
    const double top = getAbsoluteY();
    for (const Placement& placement : placements_) {
        if (!placement.placed) {
            continue;
        }
        const double unit = std::min(placement.pixelsPerUnit, 1.0e6);
        const double width = placement.node->measuredWidth(unit);
        const double height = placement.node->measuredHeight(width, unit);
        placement.node->performLayout(placement.x - left - width / 2.0, placement.y - top - height / 2.0, width,
                                      height);
    }
}
```

If `performLayout`'s x and y turn out to be absolute rather than parent-relative (check how `GameView::layoutChildren` passes `contentLeft()` to the SubScene and how JadeFX computes `getAbsoluteX`), drop the `- left` / `- top`.

- [ ] **Step 8: GameView: one held snapshot per frame** — in `GameView.hpp` add `#include <memory>`, the public test hook:

```cpp
    // Draws and places billboards from this snapshot instead of the feed's,
    // until set to null. For tests.
    void setSnapshotForTest(std::shared_ptr<const engine_core::VisualSnapshot> snapshot);
```

and private members:

```cpp
    // The snapshot this frame lays out and paints from, held from layout to paint.
    std::shared_ptr<const engine_core::VisualSnapshot> frameSnapshot_;
    std::shared_ptr<const engine_core::VisualSnapshot> testSnapshot_;
```

In `GameView.cpp`:
  - `void GameView::setSnapshotForTest(std::shared_ptr<const engine_core::VisualSnapshot> snapshot) { testSnapshot_ = std::move(snapshot); }`
  - At the top of `layoutChildren()`, before `refreshWorkspace();`:
    ```cpp
    // One snapshot for this frame's layout and paint, so billboards sit where
    // the 3D draw puts what they float over, however fast the camera turns.
    frameSnapshot_ = testSnapshot_ ? testSnapshot_ : feed_->hold();
    followCamera(*frameSnapshot_);
    ```
  - After `guiLayer_->sync();` in `layoutChildren()`:
    ```cpp
    BillboardView billboards;
    billboards.view = renderer_.view();
    billboards.fovYDegrees = renderer_.fovYDegrees();
    billboards.paneX = getAbsoluteX();
    billboards.paneY = getAbsoluteY();
    billboards.paneWidth = getWidth();
    billboards.paneHeight = getHeight();
    guiLayer_->placeBillboards(frameSnapshot_->billboards, billboards);
    ```
    `getAbsoluteX()` here is this view's position from the last layout pass, the same values `renderContent` hands to `renderer_.draw`.
  - In `collectMeshes()`, replace `const engine_core::VisualSnapshot& snapshot = feed_->latest();` with:
    ```cpp
    if (!frameSnapshot_) {
        frameSnapshot_ = testSnapshot_ ? testSnapshot_ : feed_->hold();
    }
    const engine_core::VisualSnapshot& snapshot = *frameSnapshot_;
    ```
    and delete its `followCamera(snapshot);` call (layout now does it).
  - Track the cursor, in absolute window points, for Task 8: add `double cursorX_ = -1; double cursorY_ = -1;` members; set them in `handleMouseMoved`, `handleMouseDragged`, and in the `input.dragged` lambda (which `input.moved` copies), from `event.x` / `event.y`.

- [ ] **Step 9: Run the tests**

Run: `cmake --build build --parallel --target studio-tests && ./build/studio-tests`
Expected: "billboard layer tests passed" and "gui style tests passed"; no `FAIL` lines.

- [ ] **Step 10: Run everything**

Run: `cmake --build build --parallel && cd build && ctest --output-on-failure; cd ..`
Expected: all suites pass.

- [ ] **Step 11: Commit**

```bash
git add src/runner/GuiLayer.hpp src/runner/GuiLayer.cpp src/runner/GameView.hpp src/runner/GameView.cpp tests/BillboardLayerTest.cpp tests/StudioLayoutTest.cpp CMakeLists.txt
git commit -m "Draw BillboardGuis in the Scene View, placed and sized from the frame's own snapshot

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Occlusion: the depth test in the draw, and the cursor's depth

**Files:**
- Create: `src/runner/SceneDepth.hpp`
- Modify: `src/runner/gl.hpp`, `src/runner/gl.cpp`, `src/runner/Renderer.hpp`, `src/runner/Renderer.cpp`, `src/runner/GuiLayer.hpp`, `src/runner/GuiLayer.cpp`, `src/runner/GameView.cpp`

**Interfaces:**
- Consumes: `jadefx::Painter::setOccluder/clearOccluder` (Task 1), `GuiLayer::placedFor`, `setCursorDepth` (Task 7).
- Produces:
  ```cpp
  // runner, SceneDepth.hpp (no GL include, so GuiLayer can use it)
  struct SceneDepth {
      unsigned texture = 0;              // 0: this frame drew nothing a billboard hides behind
      int x = 0, y = 0, width = 0, height = 0;  // framebuffer pixels, origin bottom left
  };
  // runner, Renderer.hpp (includes SceneDepth.hpp)
  SceneDepth sceneDepth() const;         // valid after a draw, until the next
  // The window point (in points, as draw's x and y) to read the scene depth under
  // at the next draw; negative for none. The value arrives one draw later.
  void setDepthProbe(double x, double y);
  std::optional<float> probedDepth() const;
  // runner, GuiLayer.hpp
  void GuiLayer::setSceneDepth(const SceneDepth& depth);
  ```

- [ ] **Step 1: GL entry points** — in `gl.hpp` add constants beside their neighbours:

```cpp
constexpr GLenum RT_GL_READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum RT_GL_PIXEL_PACK_BUFFER = 0x88EB;
constexpr GLenum RT_GL_STREAM_READ = 0x88E1;
constexpr GLbitfield RT_GL_MAP_READ_BIT = 0x0001;
```

and entry points declared, defined in `gl.cpp`, loaded with `LOAD(...)`, and `#define`d, exactly as `rt_glReadPixels` is:

```cpp
extern void* (*rt_glMapBufferRange)(GLenum target, GLsizeiptr offset, GLsizeiptr length, GLbitfield access);
extern GLboolean (*rt_glUnmapBuffer)(GLenum target);
```

- [ ] **Step 2: Renderer** — create `src/runner/SceneDepth.hpp`:

```cpp
#pragma once

namespace runner {

// The depth a Scene View's last draw left, for UI drawn inside the pane to
// hide behind: the renderer's depth texture, 0 near to 1 far, and the
// framebuffer rectangle it covers, in pixels from the bottom left. texture is
// 0 when that draw drew no meshes or sky, or failed: nothing to hide behind.
struct SceneDepth {
    unsigned texture = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

}  // namespace runner
```

In `Renderer.hpp` add `#include "SceneDepth.hpp"` and `#include <optional>`, and the three public methods with these comments:

```cpp
    // The depth this frame's surfaces left, and the framebuffer rectangle it
    // covers, for UI drawn inside the pane to hide behind (as the grid does).
    // texture is 0 when the last draw drew no meshes or sky, or failed.
    SceneDepth sceneDepth() const { return sceneDepth_; }
    // Where the next draw reads one depth value back, in window points as
    // draw's x and y take them; negative x or y for none. Read without
    // stalling: probedDepth has it one draw later.
    void setDepthProbe(double x, double y) {
        probeX_ = x;
        probeY_ = y;
    }
    // The scene depth under the probe point as of the draw before last, 0 near
    // to 1 far; none when the point was outside the pane or nothing was drawn.
    std::optional<float> probedDepth() const { return probedDepth_; }
```

Private:

```cpp
    void readProbe(int paneX, int paneY, int paneWidth, int paneHeight, double sceneWidth, double sceneHeight,
                   const int viewport[4]);
    SceneDepth sceneDepth_;
    double probeX_ = -1;
    double probeY_ = -1;
    std::optional<float> probedDepth_;
    // Two one-float buffers in turn: one is read back while the other fills.
    unsigned probeBuffers_[2] = {0, 0};
    bool probeFilled_[2] = {false, false};
    int probeNext_ = 0;
```

In `Renderer::draw`: at the start (after the early returns), `sceneDepth_ = SceneDepth{};`. After the passes, inside `if (drawn && (hasMeshes || hasSky)) { ... }` (the tone map block), after the tone map draw:

```cpp
        sceneDepth_ = SceneDepth{depthTexture_, pane.x, pane.y, pane.width, pane.height};
        readProbe(pane.x, pane.y, pane.width, pane.height, sceneWidth, sceneHeight, viewport);
```

and when that block does not run, `probedDepth_.reset();`.

```cpp
void Renderer::readProbe(int paneX, int paneY, int paneWidth, int paneHeight, double sceneWidth,
                         double sceneHeight, const int viewport[4]) {
    if (probeBuffers_[0] == 0) {
        glGenBuffers(2, probeBuffers_);
        for (unsigned buffer : probeBuffers_) {
            glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, buffer);
            glBufferData(RT_GL_PIXEL_PACK_BUFFER, sizeof(float), nullptr, RT_GL_STREAM_READ);
        }
        glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, 0);
    }
    // Last draw's read, which the GPU has long finished.
    const int previous = 1 - probeNext_;
    probedDepth_.reset();
    if (probeFilled_[previous]) {
        glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, probeBuffers_[previous]);
        if (const void* mapped = glMapBufferRange(RT_GL_PIXEL_PACK_BUFFER, 0, sizeof(float), RT_GL_MAP_READ_BIT)) {
            probedDepth_ = *static_cast<const float*>(mapped);
            glUnmapBuffer(RT_GL_PIXEL_PACK_BUFFER);
        }
        probeFilled_[previous] = false;
    }
    probeFilled_[probeNext_] = false;
    if (probeX_ >= 0.0 && probeY_ >= 0.0) {
        const double scaleX = static_cast<double>(viewport[2]) / sceneWidth;
        const double scaleY = static_cast<double>(viewport[3]) / sceneHeight;
        const int px = viewport[0] + static_cast<int>(std::floor(probeX_ * scaleX)) - paneX;
        const int py = viewport[1] + viewport[3] - 1 - static_cast<int>(std::floor(probeY_ * scaleY)) - paneY;
        if (px >= 0 && py >= 0 && px < paneWidth && py < paneHeight) {
            glBindFramebuffer(RT_GL_READ_FRAMEBUFFER, gbufferFbo_);
            glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, probeBuffers_[probeNext_]);
            glReadPixels(px, py, 1, 1, RT_GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            probeFilled_[probeNext_] = true;
        }
    }
    glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, 0);
    probeNext_ = previous;
}
```

The G-buffer is `targetWidth_` × `targetHeight_`, which `ensureTargets(pane.width, pane.height)` makes equal to the pane, so pane-relative pixels are G-buffer texels. `saved.restore(viewport)` rebinds the framebuffer afterwards; it binds `RT_GL_FRAMEBUFFER`, which resets both read and draw bindings. In `shutdown()`, delete `probeBuffers_` if non-zero and zero them and `probeFilled_`.

- [ ] **Step 3: GuiLayer draws billboards behind the scene** — in `GuiLayer.hpp` add `#include "SceneDepth.hpp"`, the public `void setSceneDepth(const SceneDepth& depth) { sceneDepth_ = depth; }`, the protected override `void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;`, and the private `SceneDepth sceneDepth_;`.

```cpp
void GuiLayer::renderChildren(jadefx::UiRenderer& renderer, float opacity) {
    jadefx::Painter painter(renderer);
    for (jadefx::Node* child : paintOrder()) {
        const PlacedBillboard* board = placedFor(child);
        const bool hide = board != nullptr && !board->alwaysOnTop && sceneDepth_.texture != 0;
        if (hide) {
            painter.setOccluder(sceneDepth_.texture, sceneDepth_.x, sceneDepth_.y, sceneDepth_.width,
                                sceneDepth_.height, board->depth);
        }
        child->render(renderer, opacity);
        if (hide) {
            painter.clearOccluder();
        }
    }
}
```

- [ ] **Step 4: GameView paint wiring** — in `GameView::renderContent`:
  - Before `renderer_.draw(...)`: `renderer_.setDepthProbe(cursorX_, cursorY_);`
  - After `renderer_.draw(...)` returns (whatever it returned): `guiLayer_->setSceneDepth(renderer_.sceneDepth()); guiLayer_->setCursorDepth(renderer_.probedDepth());`
  - When the pane is not drawn at all (the `ensureGraphics()` branch is skipped): `guiLayer_->setSceneDepth(SceneDepth{});`
  - At the end of `renderContent`, release the frame: `frameSnapshot_.reset();` so the feed can reuse the buffer.
  - In `handleMouseExited` (add the override if GameView lacks one, calling `IdePane::handleMouseExited`), set `cursorX_ = cursorY_ = -1`.

- [ ] **Step 5: Build and run the suites**

Run: `cmake --build build --parallel && cd build && ctest --output-on-failure; cd ..`
Expected: all pass (the headless studio tests never paint, so they keep the cursor depth they set).

- [ ] **Step 6: Commit**

```bash
git add src/runner/SceneDepth.hpp src/runner/gl.hpp src/runner/gl.cpp src/runner/Renderer.hpp src/runner/Renderer.cpp src/runner/GuiLayer.hpp src/runner/GuiLayer.cpp src/runner/GameView.cpp
git commit -m "Hide a BillboardGui behind nearer surfaces, and pass the mouse where they hide it

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: GL check in a real window, and by hand in the studio

**Files:**
- Create: `tests/BillboardDemo.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: The demo** — `tests/BillboardDemo.cpp`, modelled on `tests/ProfilerDemo.cpp` (read it first: it builds a `Runner`, a `GameView` in a real JadeFX window, and drives frames). It:
  1. Builds a place: a Camera at (0, 0, 10) looking at the origin, linked to the view; a 4×4×1 GameObject cube with a visible Prefab at (−2, 0, 0) (copy how `SceneRenderCheck.cpp` or `ProfilerDemo.cpp` give a GameObject a drawable Prefab); a BillboardGui at (0, 0, −2) with a red Pane child styled `pane { width: 400%; height: 200%; background-color: red; }` (so its left half is behind the cube); and an AlwaysOnTop BillboardGui at the same place, 3 units up, with a blue Pane the same size.
  2. Lets 10 frames settle, then saves the window as `<out-dir>/billboards.png` with `runner::EncodePng`, exactly as `AssetsDemo::save` does.
  3. Exits with status 0.
  Register it in `CMakeLists.txt` beside `assets-demo`: `# By hand, not ctest: billboard-demo <out-dir> saves a depth-tested and an always-on-top BillboardGui beside a cube.` / `add_executable(billboard-demo tests/BillboardDemo.cpp)` / `target_link_libraries(billboard-demo PRIVATE studio)`, and add `billboard-demo` to the list of by-hand targets on the `add_custom_target` line near line 866.

- [ ] **Step 2: Run it**

Run: `cmake --build build --parallel --target billboard-demo assets-demo && ./build/billboard-demo /tmp/claude-billboards && ./build/assets-demo /tmp/claude-billboards dark`
Expected: both exit 0 with no "OpenGL error during UI frame". Open `/tmp/claude-billboards/billboards.png`: the red billboard's left part is hidden where the cube is in front, its right part shows; the blue one shows whole, over the cube.

- [ ] **Step 3: By hand in the studio** — build `AnarchyStudio` (per the bundle note, build the `bundle-resources` target too), open a place, add a GameObject with a Prefab, put a BillboardGui with a Label (`label { font-size: 24px; } billboardgui { width: 300%; }`) under it, press Play with a script that spins the part in a circle, and turn the camera fast with the right mouse button. Expected: the label stays exactly on the part (no trailing); walking behind a wall hides the hidden part of it; a Button on a billboard clicks when visible and does not click through a wall; AlwaysOnTop shows it through walls. Open a second Scene View (Window > New Scene View) and repeat the camera turn in both.

- [ ] **Step 4: Commit**

```bash
git add tests/BillboardDemo.cpp CMakeLists.txt
git commit -m "Add billboard-demo, a real-window check of BillboardGui occlusion

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Report** — summarise both branches (`git log --oneline master..billboard-occluder` in JadeFX, `git log --oneline main..billboard-gui` in the engine), the demo's PNG path, and anything from the by-hand check that did not behave as expected. Do not merge either branch; the JadeFX branch must land first, and that is the user's call.
