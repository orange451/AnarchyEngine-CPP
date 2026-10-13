# Particle Leftovers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the minor issues the ParticleEmitter's final review deferred, then finish the merge cleanup (the `particles` worktree and branch).

**Architecture:** Six small, independent fixes. Each one gets a test that fails first. Sim fixes are tested in `sandbox` (Catch2), the renderer in `scene-render-check`, and the studio in `properties-tests`. One deferred item is closed with no code change (see Task 4), and one is checked before it is fixed (Task 3).

**Tech Stack:** C++17 (MSVC 14.23), Catch2 (`sandbox`), hand-rolled harnesses (`properties-tests`, `scene-render-check`), OpenGL 3.3.

**Spec:** `docs/superpowers/specs/2026-10-12-particle-emitter-design.md` (the "As built" section lists these leftovers).

## Global Constraints

- Work in a worktree, `.claude/worktrees/particle-leftovers`, on branch `particle-leftovers`. Never commit to `main` directly, and never push.
- Set the worktree up as `worktree-build-setup` memory says: copy main's `build/_deps/*-src` and configure with `FULLY_DISCONNECTED` and `JADEFX_CPP_DIR`.
- Build commands:
  - Build: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target <target> --parallel`.
  - Run from the worktree root: `build/Release/sandbox.exe "[tag]"`, `build/Release/properties-tests.exe`, `build/Release/scene-render-check.exe`.
- Distances are "units", never "studs".
- Every comment and test name matches the surrounding style: short, plain sentences.

## Review Focus

1. **Many Emit calls in one frame** (a script calling `Emit(1e7)` in a loop): the counter saturates and never goes negative. Task 1 pins this.
2. **A particle sitting still on the ground** (Stick response, zero motion): it spends no ray, so other emitters keep their share. Task 1 pins this.
3. **Moving an emitter to a far parent mid-play**: no particles appear along the line between the old and new spots. Task 2 pins this.
4. **The snapshot replaced before the renderer paints** (a GameView repaint without a new collect): the renderer still draws valid data. Task 3 pins this.
5. **A window size the depth copy cannot be made at** (0×0 while minimised): soft edges stay off on every later frame at that size, not only the first. Task 3 pins this.

---

### Task 1: The Emit counter saturates, and still particles spend no rays

**Files:**
- Modify: `src/engine_instances/ParticleEmitter.cpp:468` (`request_emit`)
- Modify: `src/engine_core/particles/ParticleColliders.cpp:114-124` (`WorldCollider::test`)
- Test: `sandbox/particle_emitter_tests.cpp`, `sandbox/particle_world_tests.cpp`

**Interfaces:** none shared. `ParticleEmitter::request_emit(int)` and `take_emit_requests()` keep their signatures.

- [ ] **Step 1: Write the failing tests**

Append to `sandbox/particle_emitter_tests.cpp`:

```cpp
TEST_CASE("PE7 Emit requests saturate rather than overflow", "[particles]") {
    SimRole role;
    Game game;
    ParticleEmitter& e = add_emitter(game, workspace_of(game));
    e.request_emit(INT_MAX);
    e.request_emit(INT_MAX);
    REQUIRE(e.take_emit_requests() == INT_MAX);
}
```

Add `#include <climits>` to that file if it is missing.

Append to `sandbox/particle_world_tests.cpp`:

```cpp
TEST_CASE("PW19 a particle that does not move spends no ray", "[particle-world]") {
    ParticleRig rig;
    Attachment& above = rig.attachment_at(0.f, 8.f, 0.f);
    ParticleEmitter& e = rig.emitter(above.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(10.f)).has_value());
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_collision(static_cast<int>(ParticleCollision::World)).has_value());
    e.request_emit(20);
    rig.steps(3);
    REQUIRE(e.particle_count() == 20);
    REQUIRE(rig.world.rays_last_step() == 0);   // no speed, no acceleration: nothing moved
}
```

- [ ] **Step 2: Run the tests and watch them fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "PE7*,PW19*"`

Expected:
- PE7 fails: the count wraps negative, and `take_emit_requests` returns something other than INT_MAX.
- PW19 fails: `rays_last_step()` is greater than 0.

(The default Acceleration must be zero for PW19. If it is not, also add `e.set_acceleration(Vec3{0.f, 0.f, 0.f})`.)

- [ ] **Step 3: Fix both**

In `ParticleEmitter.cpp`:

```cpp
void ParticleEmitter::request_emit(int count) {
    const long long total = static_cast<long long>(emit_requests_) + std::max(0, count);
    emit_requests_ = static_cast<int>(std::min<long long>(total, INT_MAX));
}
```

In `WorldCollider::test`, move the zero-length check ahead of the budget, so a still particle never takes a ray:

```cpp
    const Vec3 along = sub(to, from);
    if (!(dot(along, along) > 0.f)) {
        return std::nullopt;   // nothing moved: nothing to hit, and no ray spent
    }
    // In this step's window, and rays left: cast one.
    const std::size_t place = count_ > 0 ? (slot + count_ - cursor_) % count_ : 0;
    PhysicsWorld* physics = game_->physics();
    if (physics != nullptr && *budget_ > 0 && place < share_) {
        --*budget_;
        ++*cast_;
        RayFilter filter;
        filter.collision_group = group_;
        // Looks ahead to the particle's next turn, so its plane is known before it gets there.
        const std::optional<RayHit> hit = physics->raycast(*game_, from, scale(along, reach_), filter);
```

Delete the old `along` declaration and the old zero-length check inside the `if`. The rest of the block is unchanged. The terrain test above it stays first.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `build/Release/sandbox.exe "[particles],[particle-world],[particle-sim]"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_instances/ParticleEmitter.cpp src/engine_core/particles/ParticleColliders.cpp sandbox/particle_emitter_tests.cpp sandbox/particle_world_tests.cpp
git commit -m "Particles: Emit requests saturate; a still particle spends no ray"
```

---

### Task 2: A reparented emitter does not smear its spawns

**Files:**
- Modify: `src/engine_core/ParticleWorld.cpp`:
  - `Entry` (around line 28): add `last_parent`.
  - The step (around line 291): use it.
  - Around line 355: record it.
- Test: `sandbox/particle_world_tests.cpp`

**Interfaces:** none shared.

Today, a spawn mid-step lerps between `entry.last_frame` (from the old parent) and the new parent's frame. A move to a far parent therefore spawns particles all along the line between the two.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("PW20 an emitter moved to a far parent spawns only at the new one", "[particle-world]") {
    ParticleRig rig;
    Attachment& near_one = rig.attachment_at(0.f, 0.f, 0.f);
    Attachment& far_one = rig.attachment_at(100.f, 0.f, 0.f);
    ParticleEmitter& e = rig.emitter(near_one.id());
    REQUIRE_FALSE(e.set_emit_speed(NumberCurve::constant(0.f)).has_value());
    REQUIRE_FALSE(e.set_lifetime(NumberCurve::constant(10.f)).has_value());
    REQUIRE_FALSE(e.set_rate(NumberCurve::constant(600.f)).has_value());
    rig.steps(5);
    rig.game.set_parent(e.id(), far_one.id());
    rig.steps(1);
    for (const auto& p : *rig.world.draws().front().particles) {
        const float x = p.position[0];
        REQUIRE((x < 1.f || x > 99.f));   // at one parent or the other, never between
    }
}
```

The Point shape is assumed to be the default. If it is not, set the shape to Point with `e.set_shape(static_cast<int>(ParticleShape::Point))`.

- [ ] **Step 2: Run it and watch it fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target sandbox --parallel && build/Release/sandbox.exe "PW20*"`
Expected: FAIL, with x values between 1 and 99.

- [ ] **Step 3: Fix**

In `struct ParticleWorld::Entry`, after `bool seen = false;`:

```cpp
    InstanceId last_parent = 0;   // a new parent starts the shape's motion over
```

In the step, replace the line that sets `p.frame.previous`:

```cpp
        p.frame.previous = entry.seen && entry.last_parent == parent ? entry.last_frame : shape_frame;
```

Where `entry.last_frame = p.frame.current;` is set (around line 355), add:

```cpp
        entry.last_parent = p.parent;
```

`p.parent` does not exist yet. Add `InstanceId parent = 0;` to `struct ParticleWorld::Pending`, and set `p.parent = parent;` beside `p.id = id;`.

- [ ] **Step 4: Run and watch it pass**

Run: `build/Release/sandbox.exe "[particle-world],[particle-sim]"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine_core/ParticleWorld.cpp sandbox/particle_world_tests.cpp
git commit -m "Particles: a new parent starts the shape's motion over"
```

---

### Task 3: Renderer batches own their draws; an incomplete depth copy stays off

**Files:**
- Modify: `src/runner/ParticleRenderer.hpp`:
  - `ParticleBatch` holds a `ParticleDraw` by value.
  - Add `depthComplete_`.
  - Add `lastDrawSoft()`.
- Modify: `src/runner/ParticleRenderer.cpp`: `ensureDepthCopy`, `draw` (`batch.draw.` instead of `batch.draw->`), and `destroy`.
- Modify: `src/runner/GameView.cpp:977-991` (`collectParticles`): copy the draw.
- Modify: `tests/SceneRenderCheck.cpp`: change the existing `ParticleBatch{&x, 0}` uses to `ParticleBatch{x, 0}`, and add P-cases.

**Interfaces:**
- Produces: `struct ParticleBatch { engine_core::ParticleDraw draw; unsigned texture = 0; };` and `bool ParticleRenderer::lastDrawSoft() const`.
- The copy is cheap: the particle list is a `shared_ptr`, and the only other heap member is the texture path string.

**Batches:** `GameView` builds batches that point into its snapshot, and `Renderer` keeps a copy of them until the next `setParticles`. If the snapshot is replaced before the paint, those pointers dangle.

**Depth copy:** `ensureDepthCopy` saves the size even when the FBO is incomplete. At that same size, the next frame's cache hit then returns true and blits into the incomplete FBO.

- [ ] **Step 1: Write the failing checks**

In `CheckParticles` (`tests/SceneRenderCheck.cpp`, after the P1 case), add:

```cpp
    // P8: the renderer keeps what it was given, whatever happens to the caller's copy.
    {
        engine_core::ParticleDraw kept = OneParticle(0.f, 0.f, 2.f, 2.f, white);
        runner::ParticleBatch held{kept, 0};
        renderer.setParticles(&held, 1);
        kept = engine_core::ParticleDraw{};   // the snapshot moved on
        held = runner::ParticleBatch{};
        Render(renderer, size);
        Expect(!IsClear(ReadPixel(midX, midY)), "a batch draws after its source is gone");
        renderer.setParticles(nullptr, 0);
    }
```

Use whatever render call the P1 case uses in place of `Render(renderer, size)`. Copy it from P1.

Then add a direct check on the `ParticleRenderer`, after the other P-cases:

```cpp
// P9: a depth copy that cannot be made leaves soft edges off on every frame, not only the first.
void CheckParticleDepthCopy() {
    runner::ParticleRenderer pass;
    Expect(pass.create(), "the particle pass builds");
    const float white[4] = {1.f, 1.f, 1.f, 1.f};
    engine_core::ParticleDraw draw = OneParticle(0.f, 0.f, 2.f, 2.f, white);
    runner::ParticleBatch batch{draw, 0};
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    runner::ParticleFrame frame;
    frame.view = frame.projection = frame.inverseProjection = identity;
    frame.targetWidth = 0;   // no depth copy can be made at 0 x 0
    frame.targetHeight = 0;
    pass.draw(&batch, 1, frame, 0);
    Expect(!pass.lastDrawSoft(), "soft edges are off when the copy cannot be made");
    pass.draw(&batch, 1, frame, 0);
    Expect(!pass.lastDrawSoft(), "and stay off on the next frame at that size");
    pass.destroy();
}
```

Call `CheckParticleDepthCopy()` from the same place that calls `CheckParticles`. Before writing the fix, give `ParticleRenderer` a temporary `lastDrawSoft()` that returns the `copied` flag kept from `draw`, so the check compiles and runs.

- [ ] **Step 2: Build and watch the checks fail**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target scene-render-check --parallel && build/Release/scene-render-check.exe`

Expected:
- P8 fails to compile until `ParticleBatch` takes a draw by value. That is its RED. To see a runtime RED instead, write P8 with `&kept` first: it reports "a batch draws after its source is gone".
- P9 reports "and stay off on the next frame at that size".

- [ ] **Step 3: Fix**

`ParticleRenderer.hpp`:

```cpp
// One emitter to draw: its particles, and its texture as a GL name (0 draws
// the built-in round dot). A copy, so it outlives the snapshot it came from;
// the particle list itself is shared.
struct ParticleBatch {
    engine_core::ParticleDraw draw;
    unsigned texture = 0;
};
```

Add these members to the class:

```cpp
public:
    // Whether the last draw had the depth copy its soft edges read.
    bool lastDrawSoft() const { return lastSoft_; }
private:
    bool depthComplete_ = false;
    bool lastSoft_ = false;
```

In `ensureDepthCopy`:
- Make the cache hit `return depthComplete_;`.
- Before `return complete;`, add `depthComplete_ = complete;`.
- `destroy()` also resets `depthComplete_ = false;`.

In `draw`:
- After `const bool copied = ...`, add `lastSoft_ = copied;`.
- Change every `*batches[i].draw` and `*batch.draw` to `batches[i].draw` and `batch.draw`.

In `GameView::collectParticles`, change the push to `particleBatches_.push_back(ParticleBatch{draw, texture});`.

Update every other `ParticleBatch{&x, ...}` in `tests/SceneRenderCheck.cpp` to `ParticleBatch{x, ...}`. Find them with `grep -n "ParticleBatch" tests/SceneRenderCheck.cpp`.

- [ ] **Step 4: Run and watch them pass**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target scene-render-check AnarchyStudio --parallel && build/Release/scene-render-check.exe`
Expected: every check passes, including P1–P9.

- [ ] **Step 5: Commit**

```bash
git add src/runner/ParticleRenderer.hpp src/runner/ParticleRenderer.cpp src/runner/GameView.cpp tests/SceneRenderCheck.cpp
git commit -m "Particles: batches own their draws; an incomplete depth copy stays off"
```

---

### Task 4: Gradient stop color cancel — check first, then fix only if it fails

**Files:**
- Test: `tests/PropertiesTest.cpp` (a new `TestGradientColorCancel`, called from `main` beside `TestCurveRows`).
- Modify, only if the test fails: `src/ide/PropertiesPanel.cpp:1918-1938` (`stop_color`).

**Interfaces:** none shared.

On a cancel, `stop_color(view, true, false)` reads `view.color->getValue()` and writes it into the editor's model. Then `editor.sent()` keeps that curve on screen while the row reverts. Whether the cancelled colour actually stays visible depends on what the picker holds after Escape. This task finds out.

- [ ] **Step 1: Write the test**

```cpp
// Escape on a gradient stop's color puts the stop back, in the emitter and on screen.
void TestGradientColorCancel() {
    Rig rig;
    const InstanceId id = rig.add<engine_core::ParticleEmitter>("Emitter");
    auto* emitter = dynamic_cast<engine_core::ParticleEmitter*>(rig.game.instance(id));
    REQUIRE_OK(emitter->set_color(engine_core::ColorCurve::gradient(
        {{0.f, engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}}, {1.f, engine_core::ColorRgb{0.f, 0.f, 1.f, 1.f}}})));
    rig.game.history().reset_waypoints();
    rig.select({id});
    rig.frame();
    rig.panel.scroll_pane()->setVvalue(rig.panel.scroll_pane()->getVmax());
    rig.frame();
    rig.click(rig.panel.editor("Color", 4));
    rig.frame();
    rig.frame();
    auto* gradient = dynamic_cast<ide::GradientEditor*>(rig.panel.editor("Color", 1));
    auto* picker = dynamic_cast<jadefx::ColorPicker*>(rig.panel.editor("Color", 2));
    Expect(gradient != nullptr && picker != nullptr, "the gradient editor and its stop color show");
    if (gradient == nullptr || picker == nullptr) {
        return;
    }
    gradient->select(0, 0);
    rig.frame();
    rig.click(picker);
    rig.frame();
    Pick(*picker, 0, 255, 0);
    rig.frame();
    Expect(emitter->color().keys_a()[0].color.g == 1.f, "the stop takes the color while the chooser is open");
    rig.key(jadefx::Key::Escape);
    for (int i = 0; i < 4; ++i) {
        rig.frame();
    }
    Expect(SameColor(emitter->color().keys_a()[0].color, 1.f, 0.f, 0.f), "Escape puts the stop back");
    Expect(SameColor(gradient->model().curve().keys_a()[0].color, 1.f, 0.f, 0.f), "and the editor shows it again");
    Expect(rig.undoDepth() == 0, "and records nothing");
}
```

Notes:
- Use the real `ColorCurve::gradient` factory signature from `src/engine_datatypes/Curve.hpp`. Adjust the initializer if it differs.
- `REQUIRE_OK` stands for whatever the file uses to assert a setter succeeded (`Expect(!x.has_value(), ...)`).
- If `GradientEditor::select` is private, select the stop with a click on `gradient->stop_rect(0, 0)` or a similar helper, as `TestCurveRows` clicks `mode_tab`.

- [ ] **Step 2: Run it**

Run: `MSYS_NO_PATHCONV=1 cmake --build build --config Release --target properties-tests --parallel && build/Release/properties-tests.exe`

- **If every expect passes:** the leftover is not real. Keep the test as a guard, commit it ("Particles: a test that Escape puts a gradient stop back"), and mark the item closed. Skip Steps 3–4.
- **If "and the editor shows it again" fails:** go on to Step 3.

- [ ] **Step 3: Fix (only on a failure)**

In `stop_color`, a cancel puts the row's own curve back in the editor rather than the picker's colour:

```cpp
    void stop_color(RowView& view, bool done, bool commit) {
        if (!view.row.writable) {
            return;
        }
        GradientEditor& editor = *view.gradient;
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::ColorCurve;
        if (done && !commit) {
            // Cancelled: the row's value, from before the chooser opened, goes back on screen.
            finish_live(view.ids, edit, false, [&view] {
                if (view.row.value.color_curve) {
                    view.gradient->show(*view.row.value.color_curve);
                }
            });
            return;
        }
        const jadefx::Color c = view.color->getValue();
        editor.model().set_color(editor.selected_which(), editor.selected_stop(), engine_core::ColorRgb{c.r, c.g, c.b, 1.f});
        editor.sent();
        edit.value.color_curve = std::make_shared<const engine_core::ColorCurve>(editor.model().curve());
        if (done) {
            finish_live(view.ids, edit, commit, settle_after(view.gradient));
        } else {
            preview(view.ids, edit);
        }
    }
```

The `after` callback captures `view` by reference. Before using it, check how `settle_after` guards against a row that went away, and copy that guard (a `weak_ptr` to the RowView). If `show()` is ignored while the editor holds a sent curve, call the editor's own "row caught up" reset instead, the way `settle_after` does.

- [ ] **Step 4: Run and watch it pass**

Run: `build/Release/properties-tests.exe`
Expected: all pass, including `TestCurveRows` and `TestColorPicker`.

- [ ] **Step 5: Commit**

```bash
git add tests/PropertiesTest.cpp src/ide/PropertiesPanel.cpp
git commit -m "Particles: Escape on a gradient stop's color puts the editor back"
```

---

### Task 5: Docs, full suites, merge, cleanup

**Files:**
- Modify: `docs/superpowers/specs/2026-10-12-particle-emitter-design.md` ("As built": strike each leftover that is now fixed, and give the reasons for the ones closed without code).

**Not fixed, by ruling:**
- *"The renderer leaks GL state when CanDraw is false."* Every pass in `Renderer.cpp` returns false the same way, and a false ends the frame.
  - `CanDraw` is `glValidateProgram`, which a built program on a working driver does not fail.
  - A fix here alone would make one pass differ from its twenty siblings, with no test able to force the failure.
  - Cost if wrong: one frame drawn with depth writes off after a validation failure.

Steps:

- [ ] **Step 1:** Update the spec's "As built" section, then commit:

```bash
git commit -am "Particles: leftovers fixed or closed in the spec"
```

- [ ] **Step 2: Full suites (Release)**

Run:
- `build/Release/sandbox.exe`
- `build/Release/engine-tests.exe`
- `build/Release/properties-tests.exe`
- `build/Release/scene-render-check.exe`
- `build/Release/studio-tests.exe`
- `build/Release/explorer-tests.exe`
- `build/Release/profiler-tests.exe`

Expected: green. The exceptions are failures that `main` also has: S38 (flaky), and the 6 explorer-tests from 2026-10-12. Name any failure in the report. The profiler-tests passed on main's current build on 2026-10-13, so a profiler failure now is new.

- [ ] **Step 3: Merge to main (after the user says so)**

From the main checkout:

```bash
git merge --ff-only particle-leftovers
```

If `main` moved, first run `git merge main` in the worktree, then rerun Step 2's particle tags.

- [ ] **Step 4: Cleanup**

First ask the user to close any AnarchyStudio running from either worktree. Then, from the main checkout:

```bash
git worktree remove .claude/worktrees/particles
git worktree remove .claude/worktrees/particle-leftovers
git worktree prune
git branch -d particles particle-leftovers
```

- If a removal is refused because of untracked files: show the user `git -C <path> status --porcelain` and ask. Never use `--force` on my own.
  - The build directories are untracked and ignored, so they should not block removal.
- "Permission denied" usually means a running exe. Find it, ask, and retry.

- [ ] **Step 5:** Rebuild main's Release and Debug `AnarchyStudio` (and `AnarchyPlayer`), so the user's studio has the particles.
