# ViewportPane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `ViewportPane` GUI class that draws the 3D instances inside it (GameObjects, lights, a Skybox) from its CurrentCamera into an offscreen texture, shows it under its GUI children, and renders again only when something it draws changes.

**Architecture:**
- **Engine.** The class lives beside ImagePane. A new ECS scope tag, `InViewport`, walls a pane's contents off from Workspace and Lighting. The SnapshotPump builds one immutable `VisualViewport` per pane with a revision that moves only when its content changes, and publishes them in `VisualSnapshot::viewports`.
- **Runner.** Each GuiTree host (GameView, PluginWidgetPane) owns a `ViewportRenderer`: a pane-only `Renderer` with transparent output, pooled targets, and a sky cache. It renders dirty, shown panes into their own textures within a per-frame budget. Each pane's `ViewportNode` composites its texture with JadeFX's new `Painter::drawTexture`.
- **Split for testing.** Bookkeeping and scheduling (`ViewportQueue`) are pure and tested in the sandbox. GL is checked in `scene-render-check` and a by-hand demo.

**Tech Stack:** C++17 (MSVC 14.23, CMake 3.16, VS 2019 generator), OpenGL 3.3 core through `runner/gl.hpp`, JadeFX (sibling repo), flecs, Catch2 (`sandbox`), plain `Expect` programs (`studio-tests`, `jadefx-tests`, `scene-render-check`).

**Spec:** `docs/superpowers/specs/2026-10-11-viewport-pane-design.md`. Read it with this plan; "spec §N" below means its numbered sections.

## Global Constraints

- **Words.** Engine distances are "units", never "studs": in code, comments, tests, and messages.
- **Where to work.**
  - JadeFX: worktree `C:/Users/Andrew/Documents/GitHub/JadeFX_CPP/.claude/worktrees/viewport-texture`, branch `viewport-texture`.
  - Engine: worktree `C:/Users/Andrew/Documents/GitHub/AnarchyEngine-CPP/.claude/worktrees/viewport-pane`, branch `viewport-pane`.
  - Never edit, stash, switch, or commit on either repo's main checkout; other sessions work there. Use absolute paths: a Bash `cd` into a worktree moves the session's working directory.
- **Configure the engine worktree** against the JadeFX worktree, offline, as the worktree memory says: copy `../../../build/_deps/*-src` into `build/_deps/` first (one zstd symlink fails to copy; that is harmless), then
  `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" -S . -B build -G "Visual Studio 16 2019" -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DJADEFX_CPP_DIR=C:/Users/Andrew/Documents/GitHub/JadeFX_CPP/.claude/worktrees/viewport-texture`
- **Build** from Git Bash: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target <target> --parallel`. Build only the targets a step needs. Run every test program from the worktree root (they find `resources/` there): `build/Debug/sandbox.exe "[tag]"`, `build/Debug/studio-tests.exe`, `build/Debug/scene-render-check.exe`. A Windows fast-fail shows as exit 127 in Bash; re-run from PowerShell for the real code. After restoring a file from a copy, `touch` it.
- **Compiler.** C++17 only. MSVC `/W4` stays warning-free on touched code. Use `std::numeric_limits<float>::max()`, not an FLT_MAX literal. Use `engine_core::environment_variable`, not `getenv`.
- **GL.** Any GL error in a JadeFX frame closes the app ("OpenGL error during UI frame"). No GL in constructors, destructors, layout, or sync. GL objects are released in `sceneChanged` only when `previous != nullptr && !previous->isTearingDown() && getScene() != previous`, as `GameView::sceneChanged` does. Never draw a pane with GameView's own `renderer_`: its depth texture is the billboard occluder.
- **Threads.** GUI and instance setters run on SimulationThread (`on_gameplay_thread()`, else `contract_fail`). UI code posts writes with `Engine::on_simulation`. UI reads of the DataModel take `DataModelLock::Read` with a 1 ms try and skip the frame on failure. Paint never reads the DataModel; it draws from immutable snapshot data.
- **Allocation.** A quiet Prepare (nothing changed) allocates nothing in `ViewportScenes::update`: keep scratch vectors as members and reserve them.
- **Names, exact.** Class `ViewportPane`, base `GuiBasePane`, CSS element type `viewportpane`. Properties `CurrentCamera` (`Camera?`, nil), `Ambient` (`Color3`, 0.5, 0.5, 0.5), `ImageTransparency` (number, 0). Messages: `CurrentCamera must be a Camera`, `A Skybox must be in Lighting or a ViewportPane`, `CurrentCamera cannot be a Camera inside a ViewportPane`.
- **Budget and sizes.**
  - Per host per frame: render up to 8 panes or 1,048,576 pixels, oldest change first, always at least one.
  - Pixel size per side: `ceil(points × pixelsPerPoint)`, rounded up to a multiple of 8, at most 1024.
  - Pooled target sets: at most 64 MiB. Cached sky cube sets: 3. A pane not shown for 60 frames is deleted.
- **Test ids.**
  - Sandbox, tag `[viewport]`: `VP` for the class, `VW` for the scope wall, `VS` for pane scenes, `VQ` for the queue.
  - `VR` for checks in `scene-render-check`, `VN` for studio tests, `TX` for the JadeFX texture tests.
- **Commits.** One commit per task, in the repo's style: a plain sentence subject, often with an `Area:` prefix ("ViewportPane: a pane's contents stay out of the world"). It ends with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Afterwards, `git show --stat HEAD` must list only that task's files.
- **Comments.** Match the surrounding code: full-sentence prose saying what a thing is or does, in the voice of `Gui.hpp` and `SnapshotPump.hpp`. No `TODO`s.

## Review Focus

- **CurrentCamera deleted, then undone, during play.** Expected: the pane falls back to the first Camera inside it, then follows the restored Camera again. No crash, and no linear GUID scan every frame while the reference dangles. Pinned by `VS6` (Task 7) and `VP4` (Task 5).
- **Contents moved between two panes, or a pane moved between ScreenGuis.** Expected: both panes re-render with the right contents; neither keeps a stale row. Pinned by `VS8` (Task 7).
- **A pane whose size changes every frame** (window resize, or a pane in a BillboardGui as the camera moves). Expected: pixel sizes snap to multiples of 8, the Renderer reuses parked targets instead of rebuilding every frame, and memory stays under the 64 MiB pool. Pinned by `VQ2` (Task 8) and `VR4` (Task 3).
- **Sixty icons appear at once while one preview animates every frame.** Expected: 8 renders a frame, oldest first; every icon is drawn within 8 frames; the animated pane still updates. Pinned by `VQ5` (Task 8).
- **Stop after play moved and recolored pane contents.** Expected: the pane shows the restored scene on the next frame, because Stop's resync marks every pane dirty. Pinned by `VS9` (Task 7).

---

## File Structure

**JadeFX (worktree `viewport-texture`):**
- Modify `include/jadefx/scene/Painter.hpp`, `src/scene/Painter.cpp`: `TextureAlpha`, `drawTexture`.
- Modify `src/gl/UiRenderer.hpp`, `src/gl/UiRenderer.cpp`: `drawTexture`, the `uPremultiplied` uniform.
- Modify `res/shaders/image.frag`: premultiplied opacity.
- Create `tests/texture_draw_tests.cpp`; modify `tests/layout_tests.cpp` (runner list) and `CMakeLists.txt` (test sources).

**Engine (worktree `viewport-pane`):**
- **Renderer:** `src/runner/Renderer.hpp/.cpp` (transparent output, target pool, the `EnvironmentMap` cache size passed through), `src/runner/EnvironmentMap.hpp/.cpp` (image cube cache), and `resources/shaders/pipeline/fxaa.frag` (alpha).
- **Shared draw building:** create `src/runner/SceneDraws.hpp/.cpp`, the snapshot-to-draws helpers; GameView calls them. Modify `src/runner/GameView.cpp`.
- **Class:**
  - `src/engine_instances/Gui.hpp/.cpp`: `ViewportPane`.
  - `src/engine_core/DataModel.hpp`: the `viewport_pane()` flag.
  - Registration sites: `ScriptBindings.cpp`, `Project.cpp`, `Containment.cpp`, `src/ide/ClassOrder.hpp`, `src/ide/IdeIcons.cpp`, `src/ide/InsertPopup.cpp`, and `src/engine_core/LuaApi.cpp`.
- **Scope wall:**
  - `src/engine_core/Ecs.hpp/.cpp`, `DataModel.hpp/.cpp`, `DataModelState.hpp`, `DataModelPlace.cpp`, `AnimatorStep.cpp`.
  - `src/engine_core/SnapshotPump.cpp` (`has_row`, `resync`).
  - `src/engine_instances/WireframeAdornment.cpp`.
  - `src/engine_services/SceneService.cpp`.
- **Pane scenes:**
  - Create `src/engine_core/ViewportScenes.hpp/.cpp`.
  - Modify `src/engine_core/SnapshotPump.hpp/.cpp` (`VisualViewport`, the `viewports` field, and the factored `fill_visual_prefab` and `fill_visual_sky`) and `src/runner/SceneFeed.cpp`.
- **Queue:** create `src/runner/ViewportQueue.hpp/.cpp`. It is pure and goes in `STUDIO_CORE_SOURCES`.
- **Drawing:**
  - Create `src/runner/ViewportRenderer.hpp/.cpp` and `src/runner/ViewportNode.hpp/.cpp`, both in `STUDIO_SOURCES`.
  - Modify `src/runner/GuiTree.hpp/.cpp`, `src/runner/GuiLayer.hpp/.cpp`, `src/runner/GameView.hpp/.cpp`, `src/ide/PluginWidgetPane.hpp/.cpp`, and `src/ide/IdeLayoutDocking.cpp`.
- **Tests:**
  - Sandbox: create `sandbox/viewport_pane_tests.cpp`, `sandbox/viewport_scene_tests.cpp`, and `sandbox/viewport_queue_tests.cpp`; modify `sandbox/skybox_tests.cpp` and `sandbox/gui_tests.cpp` (the GUI2 class list).
  - Studio: create `tests/ViewportPaneTest.cpp`; modify `tests/StudioLayoutTest.cpp`, `tests/LuauCompleteTest.cpp` (the Insert golden list), and `tests/ExplorerRenameTest.cpp`.
  - GL: `tests/SceneRenderCheck.cpp`. Create `tests/ViewportDemo.cpp`. Modify `CMakeLists.txt`.
- **Docs:** `README.md` (Screen GUIs), `src/engine_instances/README.md`, `src/engine_core/README.md` (snapshot paragraph, if it lists `VisualSnapshot` fields).

---

### Task 0: Worktrees and a clean baseline

**Files:** none.

- [ ] **Step 1: Create the JadeFX worktree**

```bash
cd /c/Users/Andrew/Documents/GitHub/JadeFX_CPP
git worktree add -q .claude/worktrees/viewport-texture -b viewport-texture HEAD
```

- [ ] **Step 2: Create the engine worktree and copy dependency sources**

```bash
cd /c/Users/Andrew/Documents/GitHub/AnarchyEngine-CPP
git worktree add -q .claude/worktrees/viewport-pane -b viewport-pane main
cd .claude/worktrees/viewport-pane
mkdir -p build/_deps
cp -r ../../../build/_deps/*-src build/_deps/
```

- [ ] **Step 3: Configure and build both baselines**

```bash
cd /c/Users/Andrew/Documents/GitHub/JadeFX_CPP/.claude/worktrees/viewport-texture
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" -S . -B build -G "Visual Studio 16 2019"
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target jadefx-tests --parallel
build/Debug/jadefx-tests.exe
cd /c/Users/Andrew/Documents/GitHub/AnarchyEngine-CPP/.claude/worktrees/viewport-pane
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" -S . -B build -G "Visual Studio 16 2019" -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DJADEFX_CPP_DIR=C:/Users/Andrew/Documents/GitHub/JadeFX_CPP/.claude/worktrees/viewport-texture
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox studio-tests scene-render-check --parallel
build/Debug/sandbox.exe "[gui]"
build/Debug/studio-tests.exe
build/Debug/scene-render-check.exe
```

Expected: all pass. If anything fails on the untouched baseline, stop and report it; do not fix unrelated failures. Configure plus the first build takes about 25 minutes.

---

### Task 1: JadeFX `Painter::drawTexture`

**Files:**
- Modify: `include/jadefx/scene/Painter.hpp`, `src/scene/Painter.cpp`
- Modify: `src/gl/UiRenderer.hpp`, `src/gl/UiRenderer.cpp`
- Modify: `res/shaders/image.frag`
- Test: `tests/texture_draw_tests.cpp` (new), `tests/layout_tests.cpp`, `CMakeLists.txt`

**Interfaces:**
- Produces (public, `jadefx`):
  ```cpp
  enum class TextureAlpha { Straight, Premultiplied };
  // Painter:
  void drawTexture(unsigned texture, float x, float y, float width, float height, float opacity,
                   TextureAlpha alpha, bool flipV);
  ```
  `texture` is an app-owned `GL_TEXTURE_2D`. x, y, width, height are window points, as the other Painter draws take them. `flipV` is true for a texture whose row 0 is its bottom (any framebuffer's). With `Premultiplied`, opacity scales color and alpha alike, and the blend is `ONE, ONE_MINUS_SRC_ALPHA`. The draw is clipped and occluded like any image, and leaves the blend function `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.

- [ ] **Step 1: Write the failing test** — `tests/texture_draw_tests.cpp`

```cpp
#include "jadefx/jadefx.hpp"

#include "gl/gl.hpp"
#include "platform/GlfwHost.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Painter::drawTexture: an app's own GL texture drawn as a node's content,
// straight or premultiplied, flipped, faded, and clipped like an image.
namespace {

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

// A node that makes a 1 by 2 texture on its first paint, the bottom row (GL's
// row 0) blue and the top row red, both premultiplied at alpha a, and draws it
// over its whole box.
class TextureNode : public jadefx::Region {
public:
    TextureNode(unsigned char alpha, float opacity, jadefx::TextureAlpha mode)
        : alpha_(alpha), opacity_(opacity), mode_(mode) {}
    unsigned texture = 0;

protected:
    void renderContent(jadefx::UiRenderer& renderer, float) override {
        if (texture == 0) {
            const unsigned char a = alpha_;
            const unsigned char pixels[] = {0, 0, a, a, a, 0, 0, a};
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        }
        jadefx::Painter painter(renderer);
        painter.drawTexture(texture, static_cast<float>(getAbsoluteX()), static_cast<float>(getAbsoluteY()),
                            static_cast<float>(getWidth()), static_cast<float>(getHeight()), opacity_, mode_, true);
    }

private:
    unsigned char alpha_;
    float opacity_;
    jadefx::TextureAlpha mode_;
};

// Draws root on black in a 40 by 40 window twice and returns the last frame's
// RGB, top row first. Empty when the window, the context, or the dump failed.
std::vector<unsigned char> RenderFrame(const std::shared_ptr<jadefx::Node>& root, bool& ok) {
    ok = false;
    jadefx::GlfwHost host;
    if (!host.create(40, 40, "texture")) {
        Expect(false, "TX window opens");
        return {};
    }
    jadefx::Stage stage;
    if (!stage.initializeGraphics(&jadefx::GlfwHost::proc)) {
        Expect(false, "TX context");
        host.destroy();
        return {};
    }
    root->setStyle("background-color: #000000;");
    stage.setScene(jadefx::make<jadefx::Scene>(root, 40, 40));
    const char* path = "jadefx-texture.ppm";
    _putenv_s("JADEFX_DUMP_PPM", path);
    int pw = 0, ph = 0, fw = 0, fh = 0;
    host.windowSize(pw, ph);
    host.framebufferSize(fw, fh);
    const bool first = stage.frame(pw, ph, fw, fh);
    const bool second = stage.frame(pw, ph, fw, fh);
    _putenv_s("JADEFX_DUMP_PPM", "");
    std::vector<unsigned char> pixels;
    FILE* file = std::fopen(path, "rb");
    int width = 0, height = 0;
    if (file != nullptr && std::fscanf(file, "P6\n%d %d\n255\n", &width, &height) == 2 && width > 0 && height > 0) {
        pixels.resize(static_cast<std::size_t>(width * height * 3));
        if (std::fread(pixels.data(), 1, pixels.size(), file) != pixels.size()) {
            pixels.clear();
        }
    }
    if (file != nullptr) {
        std::fclose(file);
    }
    std::remove(path);
    ok = first && second && stage.graphicsOk();
    stage.shutdownGraphics();
    host.destroy();
    return pixels;
}

// The RGB at x, y (top row first) of a 40-wide frame.
const unsigned char* At(const std::vector<unsigned char>& pixels, int x, int y) {
    return pixels.data() + (static_cast<std::size_t>(y) * 40 + x) * 3;
}

void TestPremultipliedFlipped() {
    auto node = std::make_shared<TextureNode>(255, 1.f, jadefx::TextureAlpha::Premultiplied);
    node->setPrefSize(40, 40);
    bool ok = false;
    const std::vector<unsigned char> pixels = RenderFrame(node, ok);
    Expect(ok, "TX1 drawing a texture is no GL error");
    if (pixels.size() != 40 * 40 * 3) {
        return;
    }
    Expect(At(pixels, 20, 5)[0] > 200 && At(pixels, 20, 5)[2] < 50, "TX1 flipped, the texture's top row is on top");
    Expect(At(pixels, 20, 35)[2] > 200 && At(pixels, 20, 35)[0] < 50, "TX1 and its bottom row below");
}

void TestPremultipliedOpacity() {
    // Premultiplied red at alpha 128, drawn at opacity 0.5 over black: color is
    // scaled too, so red comes out near 64. Straight alpha would leave it near 128.
    auto node = std::make_shared<TextureNode>(128, 0.5f, jadefx::TextureAlpha::Premultiplied);
    node->setPrefSize(40, 40);
    bool ok = false;
    const std::vector<unsigned char> pixels = RenderFrame(node, ok);
    if (pixels.size() != 40 * 40 * 3) {
        Expect(false, "TX2 frame");
        return;
    }
    const int red = At(pixels, 20, 5)[0];
    Expect(red > 50 && red < 80, "TX2 opacity scales a premultiplied texture's color (red " + std::to_string(red) + ")");
}

void TestStraight() {
    // A straight draw is no GL error, and leaves the blend as the next UI draw expects.
    auto node = std::make_shared<TextureNode>(128, 1.f, jadefx::TextureAlpha::Straight);
    node->setPrefSize(40, 40);
    bool ok = false;
    const std::vector<unsigned char> pixels = RenderFrame(node, ok);
    Expect(ok && pixels.size() == 40 * 40 * 3, "TX3 a straight texture draws");
}

}  // namespace

int RunTextureDrawTests() {
    TestPremultipliedFlipped();
    TestPremultipliedOpacity();
    TestStraight();
    if (gFailures == 0) {
        std::printf("texture draw tests passed\n");
    }
    return gFailures;
}
```

Add `tests/texture_draw_tests.cpp` to the `jadefx-tests` sources in `CMakeLists.txt` (next to `tests/occluder_tests.cpp`). In `tests/layout_tests.cpp`, declare `int RunTextureDrawTests();` beside `int RunOccluderTests();` and add `failures += RunTextureDrawTests();` where `RunOccluderTests` is called.

- [ ] **Step 2: Run the test to make sure it fails**

Run: `MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target jadefx-tests --parallel`
Expected: compile error, `drawTexture` is not a member of `jadefx::Painter` and `TextureAlpha` is not declared.

- [ ] **Step 3: Add the API.** In `Painter.hpp`, after `class UiRenderer;`:

```cpp
// How a texture's color relates to its alpha: as drawn, or already multiplied by it.
enum class TextureAlpha { Straight, Premultiplied };
```

and in `class Painter`, after `clearOccluder`:

```cpp
    // Draws an app-owned GL 2D texture over x, y, width, height (window
    // points), clipped and occluded as any image. flipV is for a texture whose
    // row 0 is its bottom, as a framebuffer's is. Premultiplied scales color
    // and alpha by opacity alike. A texture of 0 draws nothing.
    void drawTexture(unsigned texture, float x, float y, float width, float height, float opacity,
                     TextureAlpha alpha, bool flipV);
```

In `Painter.cpp`:

```cpp
void Painter::drawTexture(unsigned texture, float x, float y, float width, float height, float opacity,
                          TextureAlpha alpha, bool flipV) {
    renderer_.drawTexture(texture, x, y, width, height, opacity, alpha == TextureAlpha::Premultiplied, flipV);
}
```

In `UiRenderer.hpp`, after `drawImage`:

```cpp
    // An app's own GL 2D texture, as drawImage draws an image's: x and y are
    // the top left in window points. premultiplied scales color by opacity as
    // well as alpha and blends ONE, ONE_MINUS_SRC_ALPHA; flipV samples row 0 at
    // the bottom. The blend function is SRC_ALPHA, ONE_MINUS_SRC_ALPHA after.
    void drawTexture(unsigned texture, float x, float y, float width, float height, float opacity,
                     bool premultiplied, bool flipV);
```

and `int premultiplied = -1;` in `struct ImageProgram`. In `locateImage`, add `program.premultiplied = Location(id, "uPremultiplied");`. In `drawImage`, after `glUniform1f(program.tinted, ...)`, add `glUniform1f(program.premultiplied, 0.f);`.

- [ ] **Step 4: Implement `UiRenderer::drawTexture`** after `drawImage` in `UiRenderer.cpp`:

```cpp
void UiRenderer::drawTexture(unsigned texture, float x, float y, float width, float height, float opacity,
                             bool premultiplied, bool flipV) {
    if (!ready_ || image_.id == 0 || texture == 0 || opacity <= 0.f || width <= 0.f || height <= 0.f ||
        viewportW_ <= 0 || viewportH_ <= 0) {
        return;
    }
    const float scale = scale_;
    const float left = x * scale;
    const float top = y * scale;
    const float right = (x + width) * scale;
    const float bottom = (y + height) * scale;
    // v 0 is the quad's top; a framebuffer's row 0 is its bottom, so flipV swaps them.
    const float vTop = flipV ? 1.f : 0.f;
    const float vBottom = flipV ? 0.f : 1.f;
    const float quad[] = {
        left, top, 0.f, vTop, right, top,    1.f, vTop,    right, bottom, 1.f, vBottom,
        left, top, 0.f, vTop, right, bottom, 1.f, vBottom, left,  bottom, 0.f, vBottom,
    };
    if (premultiplied) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    ImageProgram& program = pick(image_, imageOccluded_);
    glUseProgram(program.id);
    sendOccluder(program.occluder);
    glUniform2f(program.viewport, static_cast<float>(viewportW_), static_cast<float>(viewportH_));
    glUniform1f(program.opacity, opacity);
    glUniform1i(program.sampler, 0);
    glUniform1f(program.tinted, 0.f);
    glUniform1f(program.premultiplied, premultiplied ? 1.f : 0.f);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(textVao_);
    glBindBuffer(GL_ARRAY_BUFFER, textVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
```

- [ ] **Step 5: Shader.** In `res/shaders/image.frag`, add `uniform float uPremultiplied;` after `uTinted`, update the header comment ("Straight alpha, or premultiplied when uPremultiplied is set, for an app's own texture."), and replace `color.a *= uOpacity;` with:

```glsl
    // A premultiplied texture fades by scaling every channel; a straight one by its alpha.
    if (uPremultiplied > 0.5) {
        color *= uOpacity;
    } else {
        color.a *= uOpacity;
    }
```

- [ ] **Step 6: Run the tests**

Run: build `jadefx-tests`, then `build/Debug/jadefx-tests.exe`
Expected: `texture draw tests passed`, and every other suite still passes.

- [ ] **Step 7: Commit** (in the JadeFX worktree)

```bash
git add include/jadefx/scene/Painter.hpp src/scene/Painter.cpp src/gl/UiRenderer.hpp src/gl/UiRenderer.cpp res/shaders/image.frag tests/texture_draw_tests.cpp tests/layout_tests.cpp CMakeLists.txt
git commit -m "Painter: drawTexture draws an app's own GL texture, straight or premultiplied

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Renderer transparent output

**Files:**
- Modify: `src/runner/Renderer.hpp`, `src/runner/Renderer.cpp`
- Modify: `resources/shaders/pipeline/fxaa.frag`
- Test: `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Produces: `void Renderer::setTransparentOutput(bool on);`. Off by default. When on, `draw` leaves alpha 0 where nothing was drawn and premultiplied coverage elsewhere, through both the FXAA path and the direct tone-map path. A Skybox makes every pixel opaque.

- [ ] **Step 1: Write the failing check** in `tests/SceneRenderCheck.cpp`, as a new block just before the Material ball block (`// A Material's preview: ...`):

```cpp
    {
        // VR1-VR3: transparent output. A white cube at the origin seen from
        // (0, 0, 6) into a 64 by 64 target: alpha 0 in the corners, 255 at the
        // middle, and FXAA keeps an edge pixel between.
        GpuMesh cube;
        cube.upload(Cube());
        runner::Renderer clear;
        Expect(clear.initialize(), "VR the renderer builds for transparent output");
        clear.setTransparentOutput(true);
        clear.setCamera(engine_core::matrix4_translation(0.f, 0.f, 6.f), 40.f);
        OffscreenTarget target(64, 64);
        const std::vector<runner::MeshDraw> draws = {runner::MeshDraw{&cube, engine_core::matrix4_identity()}};
        runner::LightDraw sun;
        sun.kind = runner::LightDraw::Kind::Directional;
        sun.direction[0] = -0.4f;
        sun.direction[1] = -0.6f;
        sun.direction[2] = -0.7f;
        sun.intensity = 2.f;
        runner::ViewPixels pixels;
        Expect(target.complete() && target.shoot(clear, draws, sun, pixels), "VR1 a transparent shot draws");
        if (pixels.rgba.size() == 64 * 64 * 4) {
            const auto alpha = [&pixels](int x, int y) { return pixels.rgba[(static_cast<std::size_t>(y) * 64 + x) * 4 + 3]; };
            Expect(alpha(0, 0) == 0 && alpha(63, 63) == 0, "VR1 nothing drawn is alpha 0");
            Expect(alpha(32, 32) == 255, "VR2 the cube is opaque");
            int between = 0;
            for (int x = 0; x < 64; ++x) {
                const int a = alpha(x, 32);
                between += a > 0 && a < 255 ? 1 : 0;
            }
            Expect(between > 0, "VR3 FXAA leaves the cube's silhouette partly covered");
        }
        // The same renderer with transparent output off clears opaque again.
        clear.setTransparentOutput(false);
        runner::ViewPixels opaque;
        target.shoot(clear, draws, sun, opaque);
        Expect(opaque.rgba.size() == 64 * 64 * 4 && opaque.rgba[3] == 255, "VR1 off, the corner is opaque");
        clear.shutdown();
        cube.destroy();
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "transparent output leaves no GL error");
    }
```

- [ ] **Step 2: Build `scene-render-check` and see it fail**

Expected: compile error, `setTransparentOutput` is not a member of `runner::Renderer`.

- [ ] **Step 3: Implement.** In `Renderer.hpp`, beside `setClearColor`:

```cpp
    // With on, draw clears to transparent and leaves premultiplied coverage in
    // alpha: 0 where nothing was drawn, 1 under an opaque surface or a sky,
    // and between at silhouettes and see-through surfaces. For a pane composited
    // over a UI. Off, as the Scene View draws, the pane is opaque.
    void setTransparentOutput(bool on) { transparentOutput_ = on; }
```

with `bool transparentOutput_ = false;` beside `clear_`, and `int keepAlpha = -1;` in `struct Program`. Locate it beside `program.texel = at("uTexel");`: `program.keepAlpha = at("uKeepAlpha");`.

In `Renderer::draw`:
- The pane clear: `glClearColor(clear_[0], clear_[1], clear_[2], 1.0f);` becomes
  ```cpp
  if (transparentOutput_) {
      glClearColor(0.f, 0.f, 0.f, 0.f);
  } else {
      glClearColor(clear_[0], clear_[1], clear_[2], 1.0f);
  }
  ```
- The LDR clear before the tone map gets the same change.
- `blendOverClear` becomes:
  ```cpp
  // Blended over the clear: where nothing was drawn the pane shows through.
  // A transparent pane keeps the coverage in alpha as well.
  const auto blendOverClear = [this] {
      glEnable(GL_BLEND);
      if (transparentOutput_) {
          glBlendFuncSeparate(RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA);
      } else {
          glBlendFuncSeparate(RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
      }
  };
  ```
- In `fxaaPass`, after the `uTexel` uniform: `glUniform1f(fxaa_.keepAlpha, transparentOutput_ ? 1.f : 0.f);`.

- [ ] **Step 4: FXAA keeps alpha.** In `fxaa.frag`, add `uniform float uKeepAlpha;` after `uTexel`, with the comment `// 1 for a transparent pane: alpha is filtered with the color, and edges are found as the image would look over mid grey, so a dark silhouette on nothing is smoothed too.` Then:

```glsl
// The luma an edge is found by: of the color, or, keeping alpha, of the
// premultiplied color over mid grey, so coverage edges count.
float lumaOf(vec4 color) {
    vec3 seen = uKeepAlpha > 0.5 ? color.rgb + vec3(0.5) * (1.0 - color.a) : color.rgb;
    return luma(seen);
}

float lumaAt(vec2 uv) {
    return lumaOf(textureLod(uScene, uv, 0.0));
}
```

replacing the old `lumaAt`. In `main`, read `vec4 rgbaM = textureLod(uScene, posM, 0.0);` and `float lumaM = lumaOf(rgbaM);`. Make both returns keep alpha when asked:
- the early return: `fragColor = vec4(rgbaM.rgb, uKeepAlpha > 0.5 ? rgbaM.a : 1.0);`
- the final one:
  ```glsl
  vec4 blended = textureLod(uScene, posM, 0.0);
  fragColor = vec4(blended.rgb, uKeepAlpha > 0.5 ? blended.a : 1.0);
  ```

Keep `rgbM` for any later use as `rgbaM.rgb`.

- [ ] **Step 5: Run the check**

Run: `build/Debug/scene-render-check.exe`
Expected: the VR1 to VR3 lines pass, and every existing check still passes. That second part proves the Scene View's opaque path is unchanged.

- [ ] **Step 6: Commit**

```bash
git add src/runner/Renderer.hpp src/runner/Renderer.cpp resources/shaders/pipeline/fxaa.frag tests/SceneRenderCheck.cpp
git commit -m "Renderer: transparent output keeps coverage in alpha, through FXAA too

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Pooled targets and a sky cube cache

**Files:**
- Modify: `src/runner/Renderer.hpp`, `src/runner/Renderer.cpp`
- Modify: `src/runner/EnvironmentMap.hpp`, `src/runner/EnvironmentMap.cpp`
- Test: `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Renderer. 0 (the default) destroys a size's targets when another size draws,
  // as the Scene View wants. Above 0, a size's targets are parked instead, and
  // parked sets are deleted least-recently-used while they total more than bytes.
  void setTargetPool(std::size_t bytes);
  // Renderer. How many Skybox images' cubes stay made at once (1 by default).
  void setSkyCacheSize(int images) { environment_.setImageCacheSize(images); }
  // Renderer, for checks: how many times draw made a size's targets.
  int targetsMade() const { return targetsMade_; }
  // EnvironmentMap.
  void setImageCacheSize(int images);
  int imageBuilds() const { return imageBuilds_; }   // how many times update filtered an image
  ```

- [ ] **Step 1: Write the failing checks.** Add to the Task 2 block in `SceneRenderCheck.cpp`, before `clear.shutdown()`:

```cpp
        // VR4: two sizes in turn make each size's targets once with a pool.
        clear.setTargetPool(std::size_t(64) << 20);
        OffscreenTarget small(32, 32);
        runner::ViewPixels shot;
        const int before = clear.targetsMade();
        for (int round = 0; round < 3; ++round) {
            target.shoot(clear, draws, &sun, 1, 1, shot);
            small.shoot(clear, draws, &sun, 1, 1, shot);
        }
        Expect(clear.targetsMade() - before <= 2,
               "VR4 alternating two sizes makes each size's targets once (" +
                   std::to_string(clear.targetsMade() - before) + " made)");
```

and a sky block right after it, using the existing equirect test image helper the Skybox checks in this file use (search the file for `getEnvironment`). Two different sky images A and B, uploaded through a `runner::TextureCache`:

```cpp
        // VR5: two Skyboxes in turn filter each image once with a cache of 3.
        clear.setSkyCacheSize(3);
        runner::SceneLighting withSky;
        const int builds = clear.skyBuilds();
        for (int round = 0; round < 3; ++round) {
            for (const runner::EnvironmentTexture* sky : {&skyA, &skyB}) {
                withSky.sky.image = sky->texture;
                withSky.sky.imageRevision = sky->revision;
                clear.setLighting(withSky);
                target.shoot(clear, draws, &sun, 1, 1, shot);
            }
        }
        Expect(clear.skyBuilds() - builds == 2, "VR5 each sky's cubes are filtered once (" +
                                                    std::to_string(clear.skyBuilds() - builds) + ")");
```

Here `skyA` and `skyB` are `runner::EnvironmentTexture` values from the Skybox check's helper; if that helper makes one image, make a second by tinting its pixels before upload. `Renderer::skyBuilds()` returns `environment_.imageBuilds()`; add it beside `targetsMade()`.

- [ ] **Step 2: Build and see it fail**

Expected: compile errors: `setTargetPool`, `targetsMade`, `setSkyCacheSize` and `skyBuilds` are not members.

- [ ] **Step 3: Park target sets.** In `Renderer.hpp`, private:

```cpp
    // Every size-dependent buffer, as ensureTargets makes them: what a pool
    // parks while another size draws.
    struct TargetSet {
        int width = 0;
        int height = 0;
        unsigned gbufferFbo = 0, albedo = 0, normal = 0, material = 0, emissive = 0, depth = 0;
        unsigned accumulationFbo = 0, accumulation = 0, transparencyFbo = 0, transparency = 0;
        unsigned mergeFbo = 0, merge = 0, ldrFbo = 0, ldr = 0;
        bool fxaaValid = false;
        // The draw that last used it, for eviction.
        std::uint64_t used = 0;
    };
    // Moves the current buffers into a set and leaves none current.
    TargetSet parkTargets();
    // Makes set the current buffers.
    void unparkTargets(const TargetSet& set);
    static std::size_t TargetBytes(int width, int height) { return std::size_t(48) * width * height; }
    std::vector<TargetSet> parked_;
    std::size_t targetPool_ = 0;
    std::uint64_t drawCount_ = 0;
    int targetsMade_ = 0;
```

Bloom, SSR, and AO buffers are not parked. A pane never uses them, and a pool parks only on a size change. So `parkTargets` calls `destroyBloomChain(); destroyReflectionBuffers(); destroyOcclusionBuffers();` before moving the rest. Implement in `Renderer.cpp`:

```cpp
Renderer::TargetSet Renderer::parkTargets() {
    destroyBloomChain();
    destroyReflectionBuffers();
    destroyOcclusionBuffers();
    TargetSet set;
    set.width = targetWidth_;
    set.height = targetHeight_;
    set.gbufferFbo = std::exchange(gbufferFbo_, 0u);
    set.albedo = std::exchange(albedoTexture_, 0u);
    set.normal = std::exchange(normalTexture_, 0u);
    set.material = std::exchange(materialTexture_, 0u);
    set.emissive = std::exchange(emissiveTexture_, 0u);
    set.depth = std::exchange(depthTexture_, 0u);
    set.accumulationFbo = std::exchange(accumulationFbo_, 0u);
    set.accumulation = std::exchange(accumulationTexture_, 0u);
    set.transparencyFbo = std::exchange(transparencyFbo_, 0u);
    set.transparency = std::exchange(transparencyTexture_, 0u);
    set.mergeFbo = std::exchange(mergeFbo_, 0u);
    set.merge = std::exchange(mergeTexture_, 0u);
    set.ldrFbo = std::exchange(ldrFbo_, 0u);
    set.ldr = std::exchange(ldrTexture_, 0u);
    set.fxaaValid = std::exchange(fxaaValid_, false);
    set.used = drawCount_;
    targetWidth_ = 0;
    targetHeight_ = 0;
    return set;
}

void Renderer::unparkTargets(const TargetSet& set) {
    targetWidth_ = set.width;
    targetHeight_ = set.height;
    gbufferFbo_ = set.gbufferFbo;
    albedoTexture_ = set.albedo;
    normalTexture_ = set.normal;
    materialTexture_ = set.material;
    emissiveTexture_ = set.emissive;
    depthTexture_ = set.depth;
    accumulationFbo_ = set.accumulationFbo;
    accumulationTexture_ = set.accumulation;
    transparencyFbo_ = set.transparencyFbo;
    transparencyTexture_ = set.transparency;
    mergeFbo_ = set.mergeFbo;
    mergeTexture_ = set.merge;
    ldrFbo_ = set.ldrFbo;
    ldrTexture_ = set.ldr;
    fxaaValid_ = set.fxaaValid;
}

void Renderer::setTargetPool(std::size_t bytes) { targetPool_ = bytes; }
```

At the top of `ensureTargets`, replace the early-out and `destroyTargets()` with:

```cpp
    ++drawCount_;
    if (gbufferFbo_ != 0 && width == targetWidth_ && height == targetHeight_) {
        return true;
    }
    if (targetPool_ > 0) {
        if (gbufferFbo_ != 0) {
            parked_.push_back(parkTargets());
        }
        for (std::size_t i = 0; i < parked_.size(); ++i) {
            if (parked_[i].width == width && parked_[i].height == height) {
                unparkTargets(parked_[i]);
                parked_.erase(parked_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        // Least recently used first, while the parked sets and the new one are over the pool.
        std::size_t total = TargetBytes(width, height);
        for (const TargetSet& set : parked_) {
            total += TargetBytes(set.width, set.height);
        }
        while (!parked_.empty() && total > targetPool_) {
            const auto oldest = std::min_element(parked_.begin(), parked_.end(),
                                                 [](const TargetSet& a, const TargetSet& b) { return a.used < b.used; });
            total -= TargetBytes(oldest->width, oldest->height);
            unparkTargets(*oldest);
            parked_.erase(oldest);
            destroyTargets();
        }
    } else {
        destroyTargets();
    }
    ++targetsMade_;
```

then the existing making code. `destroyTargets` stays as it is (it destroys the current set). In `shutdown`, before the existing `destroyTargets()` call, destroy every parked set:

```cpp
    for (const TargetSet& set : parked_) {
        unparkTargets(set);
        destroyTargets();
    }
    parked_.clear();
```

`sceneDepth_` refers to `depthTexture_`. A pane renderer never hands it out, and the Scene View's renderer has no pool, so nothing else changes.

- [ ] **Step 4: Cache image cubes.** In `EnvironmentMap.hpp`, public:

```cpp
    // How many Skybox images' cubes stay made, least recently used going first
    // (1, the default, keeps only the current one). A Renderer that draws
    // several panes with different Skyboxes sets more, so switching between
    // them does not filter again.
    void setImageCacheSize(int images) { imageCacheSize_ = std::max(images, 1); }
    // How many times update filtered an image, for checks.
    int imageBuilds() const { return imageBuilds_; }
```

private:

```cpp
    struct CachedImage {
        std::uint64_t revision = 0;
        unsigned environment = 0;
        unsigned irradiance = 0;
        unsigned prefiltered = 0;
        std::uint64_t used = 0;
    };
    std::vector<CachedImage> cached_;
    int imageCacheSize_ = 1;
    int imageBuilds_ = 0;
    std::uint64_t updates_ = 0;
```

(add `#include <algorithm>` and `<vector>`). In `update(image, imageRevision, emptyVao)`, at the point where it decides the revision differs and before it builds, do this when the cubes in use hold an image (`imageRevision_ != 0 && !procedural_`) and `imageCacheSize_ > 1`:
1. Park the current cubes as a `CachedImage{imageRevision_, environment_, irradiance_, prefiltered_, updates_}`.
2. Zero those three names, `imageRevision_`, `environmentSize_`, and `prefilteredSize_`, so `ensureTextures` makes new cubes.
3. If `cached_` holds `imageRevision`, move that entry's names back in, set `imageRevision_`, the sizes (`kEnvironmentSize`, `kPrefilteredSize`), and `procedural_ = false`, erase it from `cached_`, and return true.
4. Otherwise build as today, and `++imageBuilds_` when the build finishes.

While `cached_` holds more than `imageCacheSize_ - 1` entries, delete the least-recently-used entry's three textures. Increment `updates_` on every call. In `shutdown`, delete every cached entry's textures and clear `cached_`. A procedural redraw (`startProcedural`) parks nothing. Keep the existing early return for a matching revision unchanged, so `imageBuilds_` stays put on a hit.

- [ ] **Step 5: Run the check**

Run: `build/Debug/scene-render-check.exe`
Expected: VR4 and VR5 pass, as does everything else.

- [ ] **Step 6: Commit**

```bash
git add src/runner/Renderer.hpp src/runner/Renderer.cpp src/runner/EnvironmentMap.hpp src/runner/EnvironmentMap.cpp tests/SceneRenderCheck.cpp
git commit -m "Renderer: a target pool parks each size's buffers, and skies keep their cubes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: SceneDraws, the snapshot-to-draws helpers

**Files:**
- Create: `src/runner/SceneDraws.hpp`, `src/runner/SceneDraws.cpp`
- Modify: `src/runner/GameView.cpp` (`collectMeshes`), `CMakeLists.txt` (`STUDIO_SOURCES`)
- Test: existing `scene-render-check`, `studio-tests`, `billboard-demo` (unchanged behaviour)

**Interfaces:**
- Produces (`runner`, `SceneDraws.hpp`):
  ```cpp
  // prefab's Models as MeshDraw templates: each mesh and texture loaded
  // through the caches, Material values copied, slots numbered from nextSlot
  // (which moves on). A Model whose mesh does not load adds nothing.
  void LoadPrefabDraws(const engine_core::VisualPrefab& prefab, MeshCache& meshes, TextureCache& textures,
                       std::uint32_t& nextSlot, std::vector<MeshDraw>& out);
  // row's light, when the row is an enabled light; false otherwise.
  bool LightDrawFor(const engine_core::VisualInstance& row, LightDraw& out);
  // Appends row drawn with templates: scaled by its Scale, tinted by its
  // Color, faded by its Transparency, and posed by its Pose. The row's pose
  // must outlive the draws.
  void AppendRowDraws(const engine_core::VisualInstance& row, const std::vector<MeshDraw>& templates,
                      std::vector<MeshDraw>& out);
  // sky's SceneSky, its image loaded through textures. Leaves out alone when
  // sky is not present.
  void SkyFor(const engine_core::VisualSky& sky, TextureCache& textures, SceneSky& out);
  ```

- [ ] **Step 1: Create `SceneDraws.hpp`** with the declarations above. It includes `Renderer.hpp`, `MeshCache.hpp`, `TextureCache.hpp`, `SnapshotPump.hpp`, `<cstdint>`, and `<vector>`. Precede them with a file comment: "What a snapshot draws, as Renderer takes it: shared by the Scene View and the ViewportPanes, so a GameObject draws the same in either."

- [ ] **Step 2: Move the code.** In `SceneDraws.cpp`:
  - `LoadPrefabDraws` is the inner loop body of `GameView::collectMeshes`' per-prefab loop, from `for (const engine_core::VisualMesh& source : ...)` through `loaded.push_back(draw);`.
  - `LightDrawFor` is the `if (row.light.kind != ... && row.light.enabled) { ... }` block. It fills `out` and returns true there, and returns false otherwise.
  - `AppendRowDraws` is the scaled-matrix and per-template loop. It starts at `const float opacity = 1.f - row.transparency;`, and the caller does the prefab-index check.
  - `SkyFor` is the `if (sky.present) { ... }` block.

  Copy each verbatim. Change only `meshes_`/`textures_` to the parameters, `prefabMeshes_[row.prefab]` to `templates`, and `meshDraws_` to `out`. Then `collectMeshes` becomes:

```cpp
    std::uint32_t nextSlot = 1;
    for (std::size_t index = 0; index < snapshot.prefabs.size(); ++index) {
        std::vector<MeshDraw>& loaded = prefabMeshes_[index];
        loaded.clear();
        LoadPrefabDraws(snapshot.prefabs[index], meshes_, textures_, nextSlot, loaded);
    }
    meshes_.sweepSessions();
    lightDraws_.clear();
    for (const engine_core::VisualInstance& row : snapshot.instances) {
        if (!row.alive) {
            continue;
        }
        LightDraw light;
        if (LightDrawFor(row, light)) {
            lightDraws_.push_back(light);
        }
        if (row.prefab == 0 || row.prefab >= snapshot.prefabs.size()) {
            continue;
        }
        AppendRowDraws(row, prefabMeshes_[row.prefab], meshDraws_);
    }
```

and the sky block becomes `SkyFor(snapshot.sky, textures_, lighting.sky);`. Add `src/runner/SceneDraws.cpp` to `STUDIO_SOURCES` after `src/runner/GameView.cpp`.

- [ ] **Step 3: Build and run what checks the Scene View draws**

Run: build `scene-render-check studio-tests billboard-demo`, then `build/Debug/scene-render-check.exe` and `build/Debug/studio-tests.exe`.
Expected: all pass, unchanged. This is a pure move; any difference is a bug in the move.

- [ ] **Step 4: Commit**

```bash
git add src/runner/SceneDraws.hpp src/runner/SceneDraws.cpp src/runner/GameView.cpp CMakeLists.txt
git commit -m "Scene View: what a snapshot draws is built by SceneDraws, for panes to share

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: The ViewportPane class

**Files:**
- Modify: `src/engine_core/DataModel.hpp` (the `viewport_pane()` flag)
- Modify: `src/engine_instances/Gui.hpp`, `src/engine_instances/Gui.cpp`
- Modify: `src/engine_core/ScriptBindings.cpp`, `src/engine_core/Project.cpp`, `src/engine_core/Containment.cpp`, `src/engine_core/LuaApi.cpp`
- Modify: `src/ide/ClassOrder.hpp`, `src/ide/IdeIcons.cpp`, `src/ide/InsertPopup.cpp`
- Modify: `src/runner/GuiTree.cpp` (a placeholder node, so the Scene View does not crash; Task 9 replaces it)
- Test: `sandbox/viewport_pane_tests.cpp` (new), `sandbox/skybox_tests.cpp`, `sandbox/gui_tests.cpp`, `tests/LuauCompleteTest.cpp`, `tests/ExplorerRenameTest.cpp`, `CMakeLists.txt`

**Interfaces:**
- Produces (`engine_core`):
  ```cpp
  // DataModel: true for ViewportPane. Read once, when its entity is issued.
  virtual bool viewport_pane() const { return false; }

  class ViewportPane : public GuiBasePane {
  public:
      static constexpr ColorRgb kDefaultAmbient{0.5f, 0.5f, 0.5f, 1.f};
      ViewportPane(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
      const char* class_name() const override;
      bool viewport_pane() const override { return true; }
      LuaSlot current_camera() const;
      InstanceId current_camera_id() const;   // live Camera, else 0; a miss is cached until tree_revision moves
      std::optional<std::string> set_current_camera(const LuaSlot& value);
      ColorRgb ambient() const { return ambient_; }
      std::optional<std::string> set_ambient(ColorRgb color);
  };
  ```

- [ ] **Step 1: Write the failing tests** — `sandbox/viewport_pane_tests.cpp`

```cpp
// ViewportPane: a pane that draws the 3D world inside it. Its properties, how
// a project saves them, and where a Skybox may go.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Skybox.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::GuiProperty;
using engine_core::InstanceId;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A ViewportPane in a ScreenGui under Gui.
engine_core::ViewportPane& make_pane(engine_core::Game& game) {
    engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
    game.set_parent(screen.id(), game.scene_service("Gui"));
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), screen.id());
    return pane;
}

}  // namespace

TEST_CASE("VP1 ViewportPane is a pane with CurrentCamera, Ambient, and ImageTransparency", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::lua_class_inherits("ViewportPane", "GuiBasePane"));
    engine_core::ViewportPane& pane = make_pane(game);
    REQUIRE(pane.viewport_pane());
    REQUIRE(pane.vec2(GuiProperty::Size).x == 100.f);
    REQUIRE(pane.current_camera().kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(pane.current_camera_id() == 0);
    REQUIRE(pane.ambient().r == 0.5f);
    REQUIRE(pane.number(GuiProperty::ImageTransparency) == 0);
    engine_core::PropertyBag defaults;
    pane.save_properties(defaults);
    REQUIRE(defaults.empty());
}

TEST_CASE("VP2 CurrentCamera takes a Camera, refuses anything else, and undoes", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(*pane.set_current_camera(instance_slot(folder.id())) == "CurrentCamera must be a Camera");
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), pane.id());
    const std::uint64_t before = pane.revision();
    begin_step(game, "Set CurrentCamera");
    REQUIRE_FALSE(pane.set_current_camera(instance_slot(camera.id())));
    end_step(game);
    REQUIRE(pane.revision() != before);
    REQUIRE(pane.current_camera_id() == camera.id());
    game.history().undo();
    REQUIRE(pane.current_camera_id() == 0);
}

TEST_CASE("VP3 Ambient is a Color3 that records undo and moves the revision", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    const std::uint64_t before = pane.revision();
    begin_step(game, "Set Ambient");
    REQUIRE_FALSE(pane.set_ambient(engine_core::ColorRgb{0.2f, 0.3f, 0.4f, 1.f}));
    end_step(game);
    REQUIRE(pane.revision() != before);
    REQUIRE(pane.ambient().g == 0.3f);
    game.history().undo();
    REQUIRE(pane.ambient().g == 0.5f);
}

TEST_CASE("VP4 a CurrentCamera that names nothing is not looked for again until the tree changes", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::Camera& camera = game.create<engine_core::Camera>();
    game.set_parent(camera.id(), pane.id());
    REQUIRE_FALSE(pane.set_current_camera(instance_slot(camera.id())));
    begin_step(game, "Delete");
    game.destroy_tree(camera.id());
    end_step(game);
    REQUIRE(pane.current_camera_id() == 0);
    REQUIRE(pane.current_camera_id() == 0);
    game.history().undo();
    // The undo relinks the tree, so the Camera is found again under its GUID.
    const InstanceId back = pane.current_camera_id();
    REQUIRE(back != 0);
    REQUIRE(dynamic_cast<engine_core::Camera*>(game.instance(back)) != nullptr);
}

TEST_CASE("VP5 a project saves and loads a ViewportPane with its Camera and Ambient", "[viewport][project]") {
    SimRole role;
    TempDir dir;
    std::string pane_guid;
    std::string camera_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        engine_core::DataModel& game = project.datamodel();
        engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
        game.set_parent(screen.id(), game.scene_service("Gui"));
        engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
        game.set_parent(pane.id(), screen.id());
        engine_core::Camera& camera = game.create<engine_core::Camera>();
        game.set_parent(camera.id(), pane.id());
        REQUIRE_FALSE(pane.set_current_camera(instance_slot(camera.id())));
        REQUIRE_FALSE(pane.set_ambient(engine_core::ColorRgb{0.1f, 0.2f, 0.3f, 1.f}));
        pane_guid = game.guid(pane.id());
        camera_guid = game.guid(camera.id());
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    const auto* back = dynamic_cast<const engine_core::ViewportPane*>(game.instance(*game.find_guid(pane_guid)));
    REQUIRE(back != nullptr);
    REQUIRE(back->ambient().b == 0.3f);
    REQUIRE(back->current_camera_id() == *game.find_guid(camera_guid));
}

TEST_CASE("VP6 a Skybox goes in Lighting or a ViewportPane; DynamicSky only in Lighting", "[viewport]") {
    REQUIRE_FALSE(engine_core::placement_error("ViewportPane", "Skybox", "Sky"));
    REQUIRE(*engine_core::placement_error("Workspace", "Skybox", "Sky") ==
            "A Skybox must be in Lighting or a ViewportPane");
    REQUIRE(*engine_core::placement_error("ViewportPane", "DynamicSky", "Sky") == "A DynamicSky must be in Lighting");
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), pane.id());
    engine_core::Skybox& sky = game.create<engine_core::Skybox>();
    REQUIRE_FALSE(game.parent_error(sky.id(), folder.id()));
}

TEST_CASE("VP7 scripts make a ViewportPane and set its CurrentCamera", "[viewport]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        local pane = Instance.new("ViewportPane", screen)
        local camera = Instance.new("Camera", pane)
        _G.defaults = pane.CurrentCamera == nil and pane:IsA("GuiBasePane") and pane.Size == Vector2.new(100, 100)
        pane.CurrentCamera = camera
        pane.Ambient = Color3.new(1, 0, 0)
        _G.set = pane.CurrentCamera == camera and pane.Ambient.R == 1
        _G.refused = not pcall(function() pane.CurrentCamera = screen end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"defaults", "set", "refused"});
}
```

`require_globals` is local to `sandbox/gui_tests.cpp`; copy it into this file's anonymous namespace. `TempDir`, `ScriptRig`, `add_script`, `begin_step`, and `end_step` come from `support.hpp`, as `gui_tests.cpp` uses them. In VP6, `placement_error(holder, child, name)` is `Containment.hpp`'s function, and `parent_error(child, parent)` is DataModel's. Check both signatures and use them as declared.

Add `sandbox/viewport_pane_tests.cpp` to the `sandbox` sources in `CMakeLists.txt`.

- [ ] **Step 2: Build `sandbox` and see it fail**

Expected: compile errors; `engine_core::ViewportPane` is not declared.

- [ ] **Step 3: The flag and the class.** In `DataModel.hpp`, after `billboard_gui()`:

```cpp
    // True for a ViewportPane, which draws the instances inside it as a world
    // of their own. Read once, when its entity is issued.
    virtual bool viewport_pane() const { return false; }
```

In `Gui.hpp`, after `class ImagePane { ... };`:

```cpp
// A pane that draws the 3D world inside it: the GameObjects, lights, and
// Skybox it holds, at any depth, seen from CurrentCamera and lit only by its
// own lights and Ambient, as runner::ViewportRenderer draws it. Its GUI
// children are drawn over that image. What it holds is never in Workspace or
// Lighting, wherever the pane is (DataModel's InViewport scope).
class ViewportPane : public GuiBasePane {
public:
    static constexpr ColorRgb kDefaultAmbient{0.5f, 0.5f, 0.5f, 1.f};

    ViewportPane(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
    bool viewport_pane() const override { return true; }

    // CurrentCamera as a script reads it.
    LuaSlot current_camera() const;
    // CurrentCamera's live Camera, or 0. A GUID that names nothing is not
    // looked for again until the tree changes, so a deleted Camera costs no
    // scan each frame.
    InstanceId current_camera_id() const;
    // SimulationThread. nil clears it; anything but a live Camera is refused,
    // and returns why.
    std::optional<std::string> set_current_camera(const LuaSlot& value);
    ColorRgb ambient() const { return ambient_; }
    // SimulationThread. A color that is not finite is refused.
    std::optional<std::string> set_ambient(ColorRgb color);

protected:
    void on_reuse() override;

private:
    InstanceRef camera_ref_;
    ColorRgb ambient_ = kDefaultAmbient;
    // The tree_revision at which CurrentCamera last named nothing; 0 for none.
    mutable std::uint64_t camera_miss_ = 0;
};
```

Add `viewportpane` to the element-type list in the header comment, after `imagepane`.

- [ ] **Step 4: Implement in `Gui.cpp`.** In `default_value`, Size: `klass == "Pane" || klass == "ImagePane" || klass == "ViewportPane"`. After the ImagePane functions:

```cpp
ViewportPane::ViewportPane(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : GuiBasePane(tag, state, id) {
    reset_values();
}

const char* ViewportPane::class_name() const { return "ViewportPane"; }

LuaSlot ViewportPane::current_camera() const { return instance_reference_slot(camera_ref_, "Camera"); }

InstanceId ViewportPane::current_camera_id() const {
    if (camera_ref_.guid().empty()) {
        return 0;
    }
    const std::uint64_t tree = tree_revision();
    if (camera_miss_ == tree) {
        return 0;
    }
    const InstanceId target = camera_ref_.resolve(*this);
    const DataModel* object = target != 0 ? instance(target) : nullptr;
    if (object == nullptr || !lua_class_inherits(object->class_name(), "Camera")) {
        camera_miss_ = tree;
        return 0;
    }
    return target;
}

std::optional<std::string> ViewportPane::set_current_camera(const LuaSlot& value) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    const std::string before = camera_ref_.guid();
    std::optional<std::string> error = set_instance_reference("CurrentCamera", "Camera", camera_ref_, value);
    if (camera_ref_.guid() != before) {
        camera_miss_ = 0;
        touch();
    }
    return error;
}

std::optional<std::string> ViewportPane::set_ambient(ColorRgb color) {
    if (!on_gameplay_thread()) {
        contract_fail("Gui setters run on SimulationThread");
    }
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string("Ambient must be finite");
    }
    // A Color3 has no alpha.
    color.a = 1.f;
    if (color.r == ambient_.r && color.g == ambient_.g && color.b == ambient_.b) {
        return std::nullopt;
    }
    const ColorRgb previous = ambient_;
    ambient_ = color;
    touch();
    note_property_change("Ambient", color_slot(previous), color_slot(color));
    return std::nullopt;
}

void ViewportPane::on_reuse() {
    GuiValues::on_reuse();
    camera_ref_.set_guid(std::string());
    ambient_ = kDefaultAmbient;
    camera_miss_ = 0;
}
```

Add read and write functions beside `read_image` and `write_image`:

```cpp
bool read_current_camera(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* pane = dynamic_cast<const ViewportPane*>(&object);
    if (pane == nullptr) {
        return false;
    }
    out = pane->current_camera();
    return true;
}

bool write_current_camera(DataModel&, DataModel& object, LuaSlot& in) {
    auto* pane = dynamic_cast<ViewportPane*>(&object);
    if (pane == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = pane->set_current_camera(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

bool read_ambient(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* pane = dynamic_cast<const ViewportPane*>(&object);
    if (pane == nullptr) {
        return false;
    }
    out = color_slot(pane->ambient());
    return true;
}

bool write_ambient(DataModel&, DataModel& object, LuaSlot& in) {
    auto* pane = dynamic_cast<ViewportPane*>(&object);
    if (pane == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = pane->set_ambient(in.color)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}
```

In `register_gui_lua`, after `add_class("ImagePane", ...)`:

```cpp
    const float ambient[3] = {ViewportPane::kDefaultAmbient.r, ViewportPane::kDefaultAmbient.g,
                              ViewportPane::kDefaultAmbient.b};
    const LuaField viewport[] = {
        lua_group("Layout"),
        gui_field<GuiProperty::Size>("ViewportPane"),
        lua_group("Appearance"),
        lua_saved_property("CurrentCamera", "Camera?", read_current_camera, write_current_camera, "null"),
        lua_saved_property("Ambient", "Color3", read_ambient, write_ambient, kept(write_json(json_floats(ambient, 3)))),
        gui_field<GuiProperty::ImageTransparency>("ViewportPane"),
    };
    add_class("ViewportPane", "GuiBasePane", viewport);
```

and at the end of the registrar:

```cpp
    // What a ViewportPane draws: Insert offers these inside one.
    for (const char* inside : {"GameObject", "Camera", "Light", "DirectionalLight", "Skybox"}) {
        register_suited_parents(inside, {"ViewportPane"});
    }
```

- [ ] **Step 5: Registration sites.**
  - `ScriptBindings.cpp`: `DataModel& create_viewport_pane(DataModel& world) { return world.create<ViewportPane>(); }` after `create_image_pane`, and `register_lua_creatable("ViewportPane", create_viewport_pane);` after ImagePane's.
  - `Project.cpp`: after the ImagePane line, `out.push_back({"ViewportPane", [](DataModel& world) -> DataModel& { return world.create<ViewportPane>(); }});`.
  - `ClassOrder.hpp`: `{"ViewportPane", 12},` after `{"ImagePane", 12},`.
  - `IdeIcons.cpp`: after the ImagePane branch, `if (class_name == "ViewportPane") { return "monitor-window-3d.png"; }`.
  - `InsertPopup.cpp`: add `{"ViewportPane", "Shows the 3D objects inside it, seen from its CurrentCamera."},` to `kRows`.
  - `LuaApi.cpp`, after ImagePane's members:
    ```cpp
    add("ViewportPane", "CurrentCamera",
        "The Camera it draws from, anywhere in the place. Nil uses the first Camera inside it; with none it draws only "
        "its background and children.",
        "Camera?", false, {});
    add("ViewportPane", "Ambient",
        "The light every surface inside it gets, from no direction. Only its own lights and Skybox add more.",
        "Color3", false, {});
    add("ViewportPane", "ImageTransparency",
        "0 is the 3D image as drawn, 1 is no image. Its background and children stay.", "number", false, {});
    ```

- [ ] **Step 6: Containment.** In `Containment.cpp`, replace the "These affect the whole place" block with:

```cpp
    // These affect the whole place, so they live where the place's lighting does;
    // a Skybox may also be a ViewportPane's own.
    if ((child_class == "Skybox" || child_class == "DynamicSky" || child_class == "BloomEffect" ||
         child_class == "ScreenSpaceReflections" || child_class == "AmbientOcclusionEffect") &&
        holder_class != "Lighting") {
        if (child_class == "Skybox") {
            if (holder_class == "ViewportPane") {
                return std::nullopt;
            }
            return std::string("A Skybox must be in Lighting or a ViewportPane");
        }
        const bool vowel = child_class.front() == 'A';
        return std::string(vowel ? "An " : "A ") + std::string(child_class) + " must be in Lighting";
    }
```

The `return std::nullopt` must not skip a later rule that applies to Skybox. Skybox is not an asset, so the `asset_home` check after this block does not apply to it.

- [ ] **Step 7: A placeholder node.** `GuiTree::makeNode` returns null for an unknown class, and `build` dereferences it. Add a branch after ImagePane: `} else if (className == "ViewportPane") { node = std::make_shared<GuiNode<jadefx::StackPane>>("viewportpane", input, false);`. Add `|| name == "ViewportPane"` to `IsPaneClass`. Task 9 replaces the node type.

- [ ] **Step 8: Update the existing tests that pin lists and messages.**
  - `sandbox/skybox_tests.cpp`: the message asserted for a Skybox outside Lighting becomes `A Skybox must be in Lighting or a ViewportPane`.
  - `sandbox/gui_tests.cpp`: add `"ViewportPane"` to the GUI2 class list.
  - `tests/LuauCompleteTest.cpp`: add `"ViewportPane"` after `"VBox"` in the pane cluster of the Insert golden list.
  - `tests/ExplorerRenameTest.cpp`: next to the existing `insert_suits` expectations, add:
    ```cpp
    Expect(insert_suits("ViewportPane", "GameObject"), "Insert offers a GameObject in a ViewportPane");
    Expect(insert_suits("ViewportPane", "Camera"), "Insert offers a Camera in a ViewportPane");
    Expect(insert_suits("ViewportPane", "Skybox"), "Insert offers a Skybox in a ViewportPane");
    Expect(insert_suits("ViewportPane", "Label"), "Insert still offers GUI in a ViewportPane");
    ```
    Use that file's own `insert_suits` call form.

- [ ] **Step 9: Run the tests**

Run: build `sandbox studio-tests explorer-tests`, then:
- `build/Debug/sandbox.exe "[viewport]"`
- `build/Debug/sandbox.exe "[gui]"`
- `build/Debug/sandbox.exe "[skybox]"`
- `build/Debug/studio-tests.exe`
- `build/Debug/explorer-tests.exe`

If `LuauCompleteTest` is in another program, run that too; find it in `CMakeLists.txt`.
Expected: all pass.

- [ ] **Step 10: Commit**

```bash
git add src/engine_core/DataModel.hpp src/engine_instances/Gui.hpp src/engine_instances/Gui.cpp src/engine_core/ScriptBindings.cpp src/engine_core/Project.cpp src/engine_core/Containment.cpp src/engine_core/LuaApi.cpp src/ide/ClassOrder.hpp src/ide/IdeIcons.cpp src/ide/InsertPopup.cpp src/runner/GuiTree.cpp sandbox/viewport_pane_tests.cpp sandbox/skybox_tests.cpp sandbox/gui_tests.cpp tests/LuauCompleteTest.cpp tests/ExplorerRenameTest.cpp CMakeLists.txt
git commit -m "ViewportPane: a pane with a CurrentCamera and an Ambient that may hold a Skybox

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: The scope wall

**Files:**
- Modify: `src/engine_core/Ecs.hpp`, `src/engine_core/Ecs.cpp`
- Modify: `src/engine_core/DataModel.hpp`, `src/engine_core/DataModel.cpp`, `src/engine_core/DataModelState.hpp`, `src/engine_core/DataModelPlace.cpp`
- Modify: `src/engine_core/AnimatorStep.cpp`, `src/engine_core/SnapshotPump.cpp` (`has_row`, `resync`)
- Modify: `src/engine_instances/WireframeAdornment.cpp`, `src/engine_services/SceneService.cpp`
- Test: `sandbox/viewport_pane_tests.cpp`

**Interfaces:**
- Produces (`DataModel`):
  ```cpp
  bool in_viewport(InstanceId id) const;                       // below a ViewportPane, at any depth
  void viewport_panes(std::vector<InstanceId>& out) const;     // every ViewportPane under game, no set order
  void viewport_animators(std::vector<InstanceId>& out) const; // every Animator below a ViewportPane
  ```
  ECS: `ecs::InViewport`, `ecs::ViewportTag`; `EcsIds::in_viewport`, `EcsIds::viewport`.

- [ ] **Step 1: Write the failing tests** (append to `sandbox/viewport_pane_tests.cpp`; add includes `Animator.hpp`, `GameObject.hpp`, `Light.hpp`, `PhysicsObject.hpp`, `SceneService.hpp`, `SnapshotPump.hpp`, `WireframeAdornment.hpp`, `SoundEmitter.hpp`, and `<algorithm>`):

```cpp
namespace {

bool listed(const std::vector<InstanceId>& ids, InstanceId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST_CASE("VW1 what is in a ViewportPane is InViewport, never in Workspace or Lighting", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    // A pane under a BillboardGui in Workspace: the case that leaked.
    engine_core::GameObject& anchor = game.create_game_object();
    game.set_parent(anchor.id(), workspace);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), anchor.id());
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), board.id());
    engine_core::GameObject& inside = game.create_game_object();
    game.set_parent(inside.id(), pane.id());
    REQUIRE(game.in_workspace(pane.id()));
    REQUIRE_FALSE(game.in_viewport(pane.id()));
    REQUIRE(game.in_viewport(inside.id()));
    REQUIRE_FALSE(game.in_workspace(inside.id()));
    REQUIRE(game.in_game(inside.id()));
    std::vector<InstanceId> panes;
    game.viewport_panes(panes);
    REQUIRE(listed(panes, pane.id()));
    // Out of the pane, back in Workspace.
    game.set_parent(inside.id(), workspace);
    REQUIRE_FALSE(game.in_viewport(inside.id()));
    REQUIRE(game.in_workspace(inside.id()));
}

TEST_CASE("VW2 a pane in Core keeps InCore below it but draws nothing in the Scene View", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), game.core());
    engine_core::GameObject& inside = game.create_game_object();
    game.set_parent(inside.id(), pane.id());
    engine_core::PointLight& light = game.create<engine_core::PointLight>();
    game.set_parent(light.id(), pane.id());
    REQUIRE(game.in_core(inside.id()));
    REQUIRE(game.in_viewport(inside.id()));
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(inside.id()) == nullptr);
    REQUIRE(pump.find(light.id()) == nullptr);
}

TEST_CASE("VW3 a light in a pane under Lighting does not light the place", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), game.scene_service("Lighting"));
    engine_core::DirectionalLight& sun = game.create<engine_core::DirectionalLight>();
    game.set_parent(sun.id(), pane.id());
    REQUIRE_FALSE(game.in_lighting(sun.id()));
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(sun.id()) == nullptr);
    // Stop asks for a resync, which walks Lighting and must stop at the pane too.
    game.capture_place();
    game.start_simulation();
    game.stop_simulation();
    pump.prepare_copy(game);
    pump.publish();
    REQUIRE(pump.find(sun.id()) == nullptr);
}

TEST_CASE("VW4 physics, sound, and Draggers skip a pane's contents; Animators there have their own list", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::GameObject& anchor = game.create_game_object();
    game.set_parent(anchor.id(), workspace);
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), anchor.id());
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), board.id());
    engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
    game.set_parent(body.id(), pane.id());
    engine_core::SoundEmitter& sound = game.create<engine_core::SoundEmitter>();
    game.set_parent(sound.id(), pane.id());
    engine_core::GameObject& rig = game.create_game_object();
    game.set_parent(rig.id(), pane.id());
    engine_core::Animator& animator = game.create<engine_core::Animator>();
    game.set_parent(animator.id(), rig.id());
    std::vector<InstanceId> ids;
    game.physics_bodies(ids);
    REQUIRE_FALSE(listed(ids, body.id()));
    game.sound_sources(ids);
    REQUIRE_FALSE(listed(ids, sound.id()));
    game.animators(ids);
    REQUIRE_FALSE(listed(ids, animator.id()));
    game.viewport_animators(ids);
    REQUIRE(listed(ids, animator.id()));
}

TEST_CASE("VW5 a Simulated GameObject with Velocity does not move inside a pane", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::ViewportPane& pane = make_pane(game);
    engine_core::GameObject& thing = game.create_game_object();
    game.set_parent(thing.id(), pane.id());
    game.set_simulated(thing.id(), true);
    thing.set_linear_velocity(1.f, 0.f, 0.f);
    game.integrate_simulated(1.0);
    REQUIRE(thing.transform().m[12] == 0.f);
}

TEST_CASE("VW6 Workspace.CurrentCamera refuses a Camera inside a ViewportPane", "[viewport]") {
    ScriptRig rig;
    add_script(rig.game, "Ui", R"(
        local screen = Instance.new("ScreenGui", game.Gui)
        local pane = Instance.new("ViewportPane", screen)
        local camera = Instance.new("Camera", pane)
        local ok, why = pcall(function() workspace.CurrentCamera = camera end)
        _G.refused = not ok and string.find(tostring(why), "CurrentCamera cannot be a Camera inside a ViewportPane", 1, true) ~= nil
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"refused"});
}

TEST_CASE("VW7 a WireframeAdornment in a pane under Workspace is not drawn", "[viewport]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& anchor = game.create_game_object();
    game.set_parent(anchor.id(), game.scene_service("Workspace"));
    engine_core::BillboardGui& board = game.create<engine_core::BillboardGui>();
    game.set_parent(board.id(), anchor.id());
    engine_core::ViewportPane& pane = game.create<engine_core::ViewportPane>();
    game.set_parent(pane.id(), board.id());
    engine_core::WireframeAdornment& wire = game.create<engine_core::WireframeAdornment>();
    game.set_parent(wire.id(), pane.id());
    REQUIRE_FALSE(wire.drawn());
}
```

`set_simulated`, `physics_bodies`, `sound_sources`, `capture_place`, `start_simulation`, and `stop_simulation` are DataModel's. `set_linear_velocity` is SpatialObject's. The point light class is the one `Light.hpp` declares for a PointLight: use its C++ name.

- [ ] **Step 2: Build `sandbox` and see it fail**

Expected: compile errors; `in_viewport`, `viewport_panes` and `viewport_animators` are not members.

- [ ] **Step 3: ECS.** In `Ecs.hpp`, after `struct InCore {};`:

```cpp
// Below a ViewportPane, at any depth: a world of its own, never in Workspace
// or Lighting, wherever the pane is.
struct InViewport {};
```

and after `BillboardTag`:

```cpp
// A ViewportPane: its class's viewport_pane() is true.
struct ViewportTag {};
```

Add `ecs_id_t in_viewport = 0;` and `ecs_id_t viewport = 0;` to `EcsIds`, and register both in `register_ecs`.

- [ ] **Step 4: Queries.** In `DataModelState.hpp`, add `flecs::query<> viewport_query;` and `flecs::query<> viewport_animator_query;` beside `billboard_query`. In the `DataModel` constructor:
  - Add `.without<ecs::InViewport>()` to `physics_query`, `core_render_query`, `source_query`, and `dragger_query`.
  - Add:

```cpp
    world.viewport_query = world.ecs.query_builder<>()
                                .with<ecs::Instance>()
                                .in()
                                .with<ecs::ViewportTag>()
                                .with<ecs::InGame>()
                                .cached()
                                .build();
    world.viewport_animator_query = world.ecs.query_builder<>()
                                         .with<ecs::Instance>()
                                         .in()
                                         .with<ecs::AnimatorTag>()
                                         .with<ecs::InViewport>()
                                         .cached()
                                         .build();
```

In `tag_entity`, add `if (object.viewport_pane()) { ecs_add_id(world, entity, ids.viewport); }`. Add `viewport_panes` and `viewport_animators` listers, copying `billboards` with the new queries, and declare them in `DataModel.hpp` beside `billboards` with one-line comments. Declare `bool in_viewport(InstanceId id) const;` beside `in_core` and implement it like `in_core`, with `ids.in_viewport`.

- [ ] **Step 5: Scope.** Change `apply_scope`'s signature to add `bool in_viewport_now` last (in `DataModel.hpp` too). Add a private helper:

```cpp
// Whether id is a ViewportPane, whose children are a world of their own.
bool DataModel::walls_scope(InstanceId id) const {
    const Slot* part = slot(id);
    return part != nullptr && part->instance != nullptr && part->instance->viewport_pane();
}
```

In `refresh_scope`, declare `bool viewport = false;`, and in the parented branch:

```cpp
        game = in_game(part->parent);
        viewport = in_viewport(part->parent) || walls_scope(part->parent);
        // A ViewportPane's contents are a world of their own: never in Workspace or Lighting.
        workspace = !viewport && (in_workspace(part->parent) || part->parent == scene_service("Workspace"));
        lighting = !viewport && (in_lighting(part->parent) || part->parent == scene_service("Lighting"));
        in_core_now = in_core(part->parent) || part->parent == core();
```

The early return also compares `in_viewport(id) == viewport`. The call becomes `apply_scope(id, game, workspace, lighting, in_core_now, viewport);`.

In `apply_scope`:
- Per node, `bool viewport = in_viewport_now;`. For `i > 0`, derive it as `refresh_scope` does: compute `viewport` first, then `workspace` and `lighting` with `!viewport &&`.
- Read `had_viewport = has_tag(world, part->entity, ids.in_viewport)` and include it in the unchanged-tags prune comparison.
- When it changed, `set_tag(world, part->entity, ids.in_viewport, viewport)`.
- Add `had_viewport != viewport` to the Ancestry note condition.

Update the comment above the Ancestry note: "The render snapshot keeps rows for Workspace and Core, and for lights under Lighting, never for a ViewportPane's contents."

In `DataModelPlace.cpp` `clear_hierarchy`, add `set_tag(world, part.entity, state_->ecs_ids.in_viewport, false);`.

- [ ] **Step 6: The remaining exclusions.**
  - `SnapshotPump.cpp` `has_row`: `return game.in_workspace(id) || (game.in_core(id) && !game.in_viewport(id)) || (game.in_lighting(id) && is_light(game.instance(id)));`. Update its comment to say a ViewportPane's contents never have a row.
  - In `resync`'s walk, after the row push and before pushing children: `if (instance != nullptr && instance->viewport_pane()) { continue; }`, with the comment `// A ViewportPane's contents are its own world, not the place's.`
  - `WireframeAdornment::drawn`: `return visible_ && (in_workspace(id()) || in_core(id())) && !in_viewport(id());`
  - `AnimatorStep.cpp`: after the existing loop, step the pane animators the same way:
    ```cpp
    // Animators inside a ViewportPane step in play too, so a preview animates.
    game.viewport_animators(ids);
    for (const InstanceId id : ids) {
        auto* animator = dynamic_cast<Animator*>(game.instance(id));
        if (animator != nullptr && animator->auto_step()) {
            animator->step(dt);
        }
    }
    ```
  - `SceneService.cpp` `write_current_camera`, before the existing check:
    ```cpp
    if (in.kind == LuaSlot::Kind::Instance && workspace->in_viewport(in.id)) {
        in.error = "CurrentCamera cannot be a Camera inside a ViewportPane";
        return false;
    }
    ```
    In `Workspace::set_current_camera`, also refuse such an id (`return false`), so C++ callers cannot set one either.

- [ ] **Step 7: Run the tests**

Run: build `sandbox`, then:
- `build/Debug/sandbox.exe "[viewport]"`
- `build/Debug/sandbox.exe "[gui]"`
- `build/Debug/sandbox.exe "[physics]"`
- `build/Debug/sandbox.exe "[assembly]"`
- `build/Debug/sandbox.exe "[animator]"`
- `build/Debug/sandbox.exe "[wireframe]"`
- `build/Debug/sandbox.exe "[billboard]"`
- `build/Debug/sandbox.exe "[scene]"`

Then run the whole sandbox with `build/Debug/sandbox.exe`.
Expected: all pass. The whole run matters: scope tags touch everything.

- [ ] **Step 8: Commit**

```bash
git add src/engine_core/Ecs.hpp src/engine_core/Ecs.cpp src/engine_core/DataModel.hpp src/engine_core/DataModel.cpp src/engine_core/DataModelState.hpp src/engine_core/DataModelPlace.cpp src/engine_core/AnimatorStep.cpp src/engine_core/SnapshotPump.cpp src/engine_instances/WireframeAdornment.cpp src/engine_services/SceneService.cpp sandbox/viewport_pane_tests.cpp
git commit -m "ViewportPane: a pane's contents are a world of their own, never the place's

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Pane scenes in the snapshot

**Files:**
- Create: `src/engine_core/ViewportScenes.hpp`, `src/engine_core/ViewportScenes.cpp`
- Modify: `src/engine_core/SnapshotPump.hpp`, `src/engine_core/SnapshotPump.cpp`, `src/runner/SceneFeed.cpp`, `CMakeLists.txt`
- Test: `sandbox/viewport_scene_tests.cpp` (new)

**Interfaces:**
- Produces (`SnapshotPump.hpp`):
  ```cpp
  // One ViewportPane's world as drawn. Immutable once published, and the same
  // pointer frame after frame until something it draws changes, when revision
  // moves with it.
  struct VisualViewport {
      InstanceId pane = 0;
      std::uint64_t revision = 0;
      // The Camera drawn from: CurrentCamera, else the first Camera inside, else none.
      InstanceId camera = 0;
      bool has_camera = false;
      // Whether that Camera is inside the pane, so the pane may report its ViewportSize.
      bool camera_inside = false;
      Matrix4 camera_world = matrix4_identity();
      float field_of_view = 70.f;
      ColorRgb ambient{0.5f, 0.5f, 0.5f, 1.f};
      // GameObjects and lights, in tree order. prefab indexes prefabs; 0 is none.
      std::vector<VisualInstance> rows;
      // Entry 0 is always null.
      std::vector<std::shared_ptr<const VisualPrefab>> prefabs;
      // The first Skybox inside, in tree order; present false with none.
      VisualSky sky;
  };
  // VisualSnapshot:
  std::vector<std::shared_ptr<const VisualViewport>> viewports;   // by pane id, ascending
  // Free functions, factored out of resolve_prefabs and resolve_lighting:
  void fill_visual_prefab(const DataModel& game, const std::string& guid, InstanceId& cached, VisualPrefab& out);
  void fill_visual_sky(const DataModel& game, const Skybox& skybox, VisualSky& out);
  VisualLight light_of(const SpatialObject& object);       // moved out of the anonymous namespace
  VisualLight light_of(const DirectionalLight& sun);
  ```
- Produces (`ViewportScenes.hpp`):
  ```cpp
  class ViewportScenes {
  public:
      ViewportScenes();
      void note(const DataModel& game, InstanceId id);   // from the pump's drain
      void mark_all();                                   // a resync or Stop
      void update(DataModel& game, std::vector<std::shared_ptr<const VisualViewport>>& out);
  };
  ```

- [ ] **Step 1: Write the failing tests** — `sandbox/viewport_scene_tests.cpp`

```cpp
// ViewportPane scenes in the render snapshot: what a pane draws, and that its
// revision moves exactly when that changes.

#include "skinning_support.hpp"

#include "AssetInstances.hpp"
#include "Bone.hpp"
#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "Light.hpp"
#include "Skybox.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using engine_core::InstanceId;

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A game with a pump, a ScreenGui under Gui holding a pane, and a Prefab of one Model on a Mesh.
struct PaneScene {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    engine_core::ViewportPane* pane = nullptr;
    engine_core::Mesh* mesh = nullptr;
    engine_core::Material* material = nullptr;
    engine_core::Prefab* prefab = nullptr;

    PaneScene() {
        pump.reserve(engine_core::DataModel::kMaxInstances);
        engine_core::ScreenGui& screen = game.create<engine_core::ScreenGui>();
        game.set_parent(screen.id(), game.scene_service("Gui"));
        pane = &game.create<engine_core::ViewportPane>();
        game.set_parent(pane->id(), screen.id());
        mesh = &game.create<engine_core::Mesh>();
        game.set_parent(mesh->id(), game.service("Meshes"));
        REQUIRE_FALSE(mesh->set_path("meshes/box.amesh"));
        material = &game.create<engine_core::Material>();
        game.set_parent(material->id(), game.service("Materials"));
        prefab = &game.create<engine_core::Prefab>();
        game.set_parent(prefab->id(), game.service("Prefabs"));
        engine_core::Model& model = game.create<engine_core::Model>();
        game.set_parent(model.id(), prefab->id());
        REQUIRE_FALSE(model.set_reference(engine_core::Model::kMeshReference, instance_slot(mesh->id())));
        REQUIRE_FALSE(model.set_reference(engine_core::Model::kMaterialReference, instance_slot(material->id())));
    }

    void frame() {
        pump.prepare_copy(game);
        pump.publish();
    }

    engine_core::GameObject& object(InstanceId parent) {
        engine_core::GameObject& made = game.create_game_object();
        game.set_parent(made.id(), parent);
        REQUIRE_FALSE(made.set_prefab(instance_slot(prefab->id())));
        return made;
    }

    // The published scene of a pane, or null.
    std::shared_ptr<const engine_core::VisualViewport> scene(InstanceId id) {
        for (const auto& found : pump.front().viewports) {
            if (found != nullptr && found->pane == id) {
                return found;
            }
        }
        return nullptr;
    }

    std::uint64_t revision() {
        const auto found = scene(pane->id());
        REQUIRE(found != nullptr);
        return found->revision;
    }
};

}  // namespace

TEST_CASE("VS1 a pane's scene holds its GameObjects, lights, and Skybox, and the place's snapshot does not", "[viewport]") {
    PaneScene s;
    engine_core::GameObject& thing = s.object(s.pane->id());
    REQUIRE_FALSE(thing.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f)));
    engine_core::PointLight& light = s.game.create<engine_core::PointLight>();
    s.game.set_parent(light.id(), s.pane->id());
    engine_core::Skybox& sky = s.game.create<engine_core::Skybox>();
    s.game.set_parent(sky.id(), s.pane->id());
    s.frame();
    const auto scene = s.scene(s.pane->id());
    REQUIRE(scene != nullptr);
    REQUIRE(scene->rows.size() == 2);
    REQUIRE(scene->rows[0].id == thing.id());
    REQUIRE(scene->rows[0].world.m[13] == 2.f);
    REQUIRE(scene->rows[0].prefab != 0);
    REQUIRE(scene->prefabs[scene->rows[0].prefab]->meshes.size() == 1);
    REQUIRE(scene->prefabs[scene->rows[0].prefab]->meshes[0].path == "meshes/box.amesh");
    REQUIRE(scene->rows[1].light.kind == engine_core::VisualLight::Kind::Point);
    REQUIRE(scene->sky.present);
    REQUIRE(s.pump.find(thing.id()) == nullptr);
    REQUIRE_FALSE(s.pump.front().sky.present);
}

TEST_CASE("VS2 nothing changed, the same scene pointer and revision", "[viewport]") {
    PaneScene s;
    s.object(s.pane->id());
    s.frame();
    const auto first = s.scene(s.pane->id());
    s.frame();
    s.frame();
    REQUIRE(s.scene(s.pane->id()) == first);
}

TEST_CASE("VS3 a Transform, a Color, and a pane property each move the revision", "[viewport]") {
    PaneScene s;
    engine_core::GameObject& thing = s.object(s.pane->id());
    s.frame();
    std::uint64_t last = s.revision();
    REQUIRE_FALSE(thing.set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f)));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(thing.set_color(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(s.pane->set_ambient(engine_core::ColorRgb{0.1f, 0.1f, 0.1f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->ambient.r == 0.1f);
}

TEST_CASE("VS4 a Material, a Texture path, and a Prefab's Model each move the revision", "[viewport]") {
    PaneScene s;
    s.object(s.pane->id());
    s.frame();
    std::uint64_t last = s.revision();
    REQUIRE_FALSE(s.material->set_color(engine_core::ColorRgb{0.f, 1.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.revision() != last);
    last = s.revision();
    REQUIRE_FALSE(s.mesh->set_path("meshes/other.amesh"));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->prefabs[1]->meshes[0].path == "meshes/other.amesh");
    last = s.revision();
    engine_core::Model& second = s.game.create<engine_core::Model>();
    s.game.set_parent(second.id(), s.prefab->id());
    REQUIRE_FALSE(second.set_reference(engine_core::Model::kMeshReference, instance_slot(s.mesh->id())));
    s.frame();
    REQUIRE(s.revision() != last);
}

TEST_CASE("VS5 the camera: CurrentCamera, else the first inside, else none; one outside the pane counts", "[viewport]") {
    PaneScene s;
    s.frame();
    REQUIRE_FALSE(s.scene(s.pane->id())->has_camera);
    engine_core::Camera& inside = s.game.create<engine_core::Camera>();
    s.game.set_parent(inside.id(), s.pane->id());
    s.frame();
    auto scene = s.scene(s.pane->id());
    REQUIRE(scene->has_camera);
    REQUIRE(scene->camera == inside.id());
    REQUIRE(scene->camera_inside);
    engine_core::Camera& outside = s.game.create<engine_core::Camera>();
    s.game.set_parent(outside.id(), s.game.scene_service("Workspace"));
    REQUIRE_FALSE(s.pane->set_current_camera(instance_slot(outside.id())));
    s.frame();
    scene = s.scene(s.pane->id());
    REQUIRE(scene->camera == outside.id());
    REQUIRE_FALSE(scene->camera_inside);
    const std::uint64_t last = scene->revision;
    REQUIRE_FALSE(outside.set_transform(engine_core::matrix4_translation(0.f, 0.f, 9.f)));
    s.frame();
    REQUIRE(s.revision() != last);
    REQUIRE(s.scene(s.pane->id())->camera_world.m[14] == 9.f);
}

TEST_CASE("VS6 CurrentCamera deleted and undone: the first Camera inside, then CurrentCamera again", "[viewport]") {
    PaneScene s;
    engine_core::Camera& fallback = s.game.create<engine_core::Camera>();
    s.game.set_parent(fallback.id(), s.pane->id());
    engine_core::Camera& chosen = s.game.create<engine_core::Camera>();
    s.game.set_parent(chosen.id(), s.pane->id());
    REQUIRE_FALSE(s.pane->set_current_camera(instance_slot(chosen.id())));
    const std::string guid = s.game.guid(chosen.id());
    s.frame();
    REQUIRE(s.scene(s.pane->id())->camera == chosen.id());
    begin_step(s.game, "Delete");
    s.game.destroy_tree(chosen.id());
    end_step(s.game);
    s.frame();
    REQUIRE(s.scene(s.pane->id())->camera == fallback.id());
    s.game.history().undo();
    s.frame();
    REQUIRE(s.scene(s.pane->id())->camera == *s.game.find_guid(guid));
}

TEST_CASE("VS7 a skinned GameObject's new pose moves the revision", "[viewport]") {
    skinning_test::Rig rig;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::ScreenGui& screen = rig.game.create<engine_core::ScreenGui>();
    rig.game.set_parent(screen.id(), rig.game.scene_service("Gui"));
    engine_core::ViewportPane& pane = rig.game.create<engine_core::ViewportPane>();
    rig.game.set_parent(pane.id(), screen.id());
    engine_core::GameObject& arm = rig.arm_object();
    rig.game.set_parent(arm.id(), pane.id());
    engine_core::Bone& hand = rig.game.create<engine_core::Bone>();
    rig.game.set_name(hand.id(), "Hand");
    rig.game.set_parent(hand.id(), arm.id());
    const auto published = [&] {
        pump.prepare_copy(rig.game);
        pump.publish();
        for (const auto& found : pump.front().viewports) {
            if (found != nullptr && found->pane == pane.id()) {
                return found;
            }
        }
        return std::shared_ptr<const engine_core::VisualViewport>();
    };
    const auto first = published();
    REQUIRE(first != nullptr);
    REQUIRE(first->rows.size() == 1);
    REQUIRE(first->rows[0].pose != nullptr);
    REQUIRE(published() == first);
    REQUIRE_FALSE(hand.set_offset(skinning_test::spin_z()));
    const auto second = published();
    REQUIRE(second->revision != first->revision);
    REQUIRE(second->rows[0].pose != first->rows[0].pose);
}

TEST_CASE("VS8 contents moved between two panes leave neither stale", "[viewport]") {
    PaneScene s;
    engine_core::ViewportPane& other = s.game.create<engine_core::ViewportPane>();
    s.game.set_parent(other.id(), s.game.parent(s.pane->id()));
    engine_core::GameObject& thing = s.object(s.pane->id());
    s.frame();
    REQUIRE(s.scene(s.pane->id())->rows.size() == 1);
    REQUIRE(s.scene(other.id())->rows.empty());
    s.game.set_parent(thing.id(), other.id());
    s.frame();
    REQUIRE(s.scene(s.pane->id())->rows.empty());
    REQUIRE(s.scene(other.id())->rows.size() == 1);
    // A pane moved to another ScreenGui keeps its scene.
    engine_core::ScreenGui& second = s.game.create<engine_core::ScreenGui>();
    s.game.set_parent(second.id(), s.game.scene_service("Gui"));
    s.game.set_parent(other.id(), second.id());
    s.frame();
    REQUIRE(s.scene(other.id())->rows.size() == 1);
}

TEST_CASE("VS9 Stop shows the restored scene on the next frame", "[viewport]") {
    PaneScene s;
    engine_core::GameObject& thing = s.object(s.pane->id());
    REQUIRE_FALSE(thing.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f)));
    s.frame();
    s.game.capture_place();
    s.game.start_simulation();
    REQUIRE_FALSE(thing.set_transform(engine_core::matrix4_translation(5.f, 0.f, 0.f)));
    REQUIRE_FALSE(thing.set_color(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f}));
    s.frame();
    REQUIRE(s.scene(s.pane->id())->rows[0].world.m[12] == 5.f);
    const std::uint64_t playing = s.revision();
    // Stop relinks the tree and asks for a resync; the pane is built again.
    s.game.stop_simulation();
    s.frame();
    const auto restored = s.scene(s.pane->id());
    REQUIRE(restored->revision != playing);
    REQUIRE(restored->rows.size() == 1);
    REQUIRE(restored->rows[0].world.m[12] == 1.f);
    REQUIRE(restored->rows[0].color.g == 1.f);
}

TEST_CASE("VS10 a pane destroyed leaves the list", "[viewport]") {
    PaneScene s;
    s.frame();
    REQUIRE(s.scene(s.pane->id()) != nullptr);
    const InstanceId gone = s.pane->id();
    s.game.destroy_tree(gone);
    s.frame();
    REQUIRE(s.scene(gone) == nullptr);
}
```

**VS7** uses `skinning_test::Rig` from `sandbox/skinning_support.hpp` (which includes `support.hpp`): its `arm_object()` is a GameObject on a two-bone arm mesh written to a temp resources folder, and `spin_z()` is a quarter turn, as `BONE6` uses them. `arm_object()` puts the GameObject in Workspace; the test moves it into the pane.

**API names.** `set_color` is GameObject's and Material's, `set_path` is Mesh's, and `destroy_tree` is DataModel's. Use them as the existing sandbox tests do.

Add the file to the sandbox sources.

- [ ] **Step 2: Build `sandbox` and see it fail**

Expected: compile errors; `VisualViewport` and `VisualSnapshot::viewports` are not declared.

- [ ] **Step 3: Snapshot types and factored fills.** In `SnapshotPump.hpp`, add `VisualViewport` (from the Interfaces block) after `VisualPrefab`, and `std::vector<std::shared_ptr<const VisualViewport>> viewports;` at the end of `VisualSnapshot`, with the comment `// Every ViewportPane under game, by pane id, ascending. Pointers only: each is immutable and shared until its pane changes.`

Declare `fill_visual_prefab`, `fill_visual_sky`, and both `light_of` overloads after `fill_visual_material`, forward-declaring `class Skybox; class SpatialObject; class DirectionalLight;`.

In `SnapshotPump.cpp`:
- Move the two `light_of` functions out of the anonymous namespace, unchanged.
- Factor `fill_visual_prefab` from `resolve_prefabs`' per-entry body. Everything from the `find_guid` re-resolve through `meshes.resize(used);` moves into it, with `entry.cached` and `entry.guid` as parameters, and `out.meshes` and `out.skeleton` replacing `meshes` and `skeleton`. `resolve_prefabs` calls it for `entry.rows != 0` and clears the prefab otherwise.
- Factor `fill_visual_sky` from `resolve_lighting`'s `if (skybox != nullptr) { ... }` block, including the `texture_path` lambda it uses. `resolve_lighting` calls it and sets `sky.present = true`.

`SnapshotPump` gets:
- a constructor and destructor defined in the .cpp;
- `std::unique_ptr<ViewportScenes> viewports_;` (forward-declare `class ViewportScenes;`).

In `take_changes`:

```cpp
    if (queue.take_overflow() || game.consume_resync()) {
        resync(game);
        viewports_->mark_all();
    } else {
        queue.drain([&](const Invalidation& change) {
            apply_live(game, change);
            viewports_->note(game, change.id);
        });
    }
    resolve_prefabs(game);
    resolve_poses(game);
    viewports_->update(game, base_.viewports);
```

In `blit`, add `dst.viewports = base_.viewports;`. In `SceneFeed::perform`, add `out->viewports = front.viewports;`.

- [ ] **Step 4: `ViewportScenes`.** Create `src/engine_core/ViewportScenes.hpp`:

```cpp
#pragma once

#include "SnapshotPump.hpp"
#include "types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModel;
class ViewportPane;

// Each ViewportPane's VisualViewport, kept between Prepares and built again
// only when the pane may have changed. A pane is looked at again when one of
// these happens:
// - an invalidation names something inside it;
// - the tree changed;
// - its own revision moved;
// - its camera, its Skybox, one of its Prefabs, or one of its poses reads differently;
// - mark_all was called.
// A pane that is built again publishes a new VisualViewport, with the next
// revision, only when the result differs from the last one. RenderThread,
// under the Prepare lock, as SnapshotPump.
class ViewportScenes {
public:
    ViewportScenes();
    // An invalidation's id: marks the ViewportPane it is inside, if any.
    void note(const DataModel& game, InstanceId id);
    // Every pane is built again at the next update, as after a resync or Stop.
    void mark_all() { all_ = true; }
    // Every pane's scene, as the DataModel is now, into out, by pane id.
    void update(DataModel& game, std::vector<std::shared_ptr<const VisualViewport>>& out);

private:
    // A Prefab some pane draws, resolved each update and shared by every pane that names it.
    struct Prefab {
        std::string guid;
        InstanceId cached = 0;
        std::shared_ptr<const VisualPrefab> current;
        std::uint64_t revision = 0;
        std::uint32_t panes = 0;
    };
    struct Pane {
        std::shared_ptr<const VisualViewport> published;
        std::uint64_t pane_revision = 0;
        // What the last build read from outside the pane's own values.
        InstanceId first_camera = 0;
        InstanceId sky = 0;
        std::vector<std::uint32_t> prefabs;            // entries in prefabs_
        std::vector<std::uint64_t> prefab_revisions;   // as built
        std::vector<std::uint32_t> skinned_rows;       // indexes into published->rows
        bool seen = false;
    };

    void refresh_prefabs(const DataModel& game);
    std::uint32_t acquire_prefab(const std::string& guid);
    void release_prefabs(Pane& pane);
    // Whether something the pane read from outside its subtree reads differently now.
    bool outside_changed(const DataModel& game, const ViewportPane& pane, const Pane& entry);
    // Builds pane's scene into scratch_ and records what it read into entry.
    void build(const DataModel& game, const ViewportPane& pane, Pane& entry);

    std::unordered_map<InstanceId, Pane> panes_;
    std::vector<Prefab> prefabs_;
    std::unordered_map<std::string, std::uint32_t> prefab_by_guid_;
    std::vector<InstanceId> pane_ids_;
    std::vector<InstanceId> dirty_;
    std::vector<InstanceId> walk_;
    VisualViewport scratch_;
    VisualPrefab prefab_scratch_;
    VisualSky sky_scratch_;
    std::uint64_t tree_seen_ = 0;
    std::uint64_t next_revision_ = 1;
    bool all_ = true;
};

}  // namespace engine_core
```

`ViewportScenes.cpp` implements it as follows.
- **Equality helpers (anonymous namespace).** `same_mesh` compares every `VisualMesh` field, with `session` by pointer. `same_prefab` compares meshes and skeleton by pointer. `same_light` compares every `VisualLight` field. `same_row` compares id, the world's 16 floats, prefab, color, transparency, scale, the pose pointer, and the light. `same_sky` compares present, image, image_flip_y, exposure, light_scale, rotation, and tint. `same_scene` compares camera, has_camera, camera_inside, camera_world, field_of_view, ambient, rows, the prefab pointers, and the sky.
- **`note`:** if `!game.in_viewport(id)`, return. Walk `game.parent(...)` up to the nearest instance whose `viewport_pane()` is true and push it onto `dirty_`.
- **`update`:**
  1. `game.viewport_panes(pane_ids_)`, then sort.
  2. `tree_moved = game.tree_revision() != tree_seen_`; remember it.
  3. `refresh_prefabs(game)`: for each entry with `panes > 0`, `fill_visual_prefab(game, entry.guid, entry.cached, prefab_scratch_)`. When it differs from `*entry.current` (or `current` is null), `entry.current = std::make_shared<const VisualPrefab>(prefab_scratch_)` and `++entry.revision`.
  4. Sort and unique `dirty_`.
  5. For each pane id: get its `ViewportPane` (skip a non-pane). `rebuild = all_ || tree_moved || published == nullptr || pane.revision() != entry.pane_revision || binary_search(dirty_, id) || outside_changed(game, pane, entry)`. If rebuilding, `build(...)`, then compare `scratch_` with `*published`. When different, or with nothing published, set `scratch_.revision = next_revision_++` and `entry.published = std::make_shared<const VisualViewport>(scratch_)`. Then `entry.pane_revision = pane.revision()` and `entry.seen = true`. Push `entry.published` to `out`.
  6. Erase entries not seen this update, releasing their prefabs, and clear `seen` on the rest. Clear `dirty_` and set `all_ = false`. Erase prefab entries with `panes == 0` from `prefab_by_guid_`, leaving the slot free (`guid.clear()`, `current.reset()`).
- **`outside_changed`:**
  1. **Camera.** Resolve `pane.current_camera_id()`, falling back to `entry.first_camera`. If that id differs from `published->camera`, it changed. If there is a camera, its `transform()` (16 floats) and `field_of_view()` must equal `published->camera_world` and `published->field_of_view`.
  2. **Sky.** When `entry.sky != 0`, `fill_visual_sky` into `sky_scratch_` from the live Skybox and compare. A dead Skybox counts as changed.
  3. **Prefabs.** Each `prefabs_[entry.prefabs[k]].revision != entry.prefab_revisions[k]` counts as changed.
  4. **Poses.** For each `skinned_rows` index, the GameObject's `pose()` pointer must equal the published row's `pose`.
- **`build`:**
  1. Clear `scratch_.rows` and `scratch_.prefabs`, then set `scratch_.prefabs = {nullptr}`, `pane = pane.id()`, `ambient = pane.ambient()`, and `sky = VisualSky{}`.
  2. Call `release_prefabs(entry)`, then clear `entry.prefabs`, `prefab_revisions`, and `skinned_rows`, and set `first_camera = 0` and `sky = 0`.
  3. Walk the subtree depth-first in child order with `walk_`, pushing children last first as `find_first_of` does. Do not descend into a nested ViewportPane (its contents are its own), but walk every other child, GUI ones included.
  4. **Camera:** the first one becomes `first_camera`.
  5. **Skybox:** the first one is `entry.sky`; fill it into `scratch_.sky` with `present = true`.
  6. **GameObject:** a row with `world = transform()`, color, `transparency` clamped to 0..1, scale, and its prefab. For the prefab, `acquire_prefab(prefab_guid())` maps to a local index: push `prefabs_[entry].current` (refresh it now if null) and record the entry and its revision in `entry.prefabs` and `entry.prefab_revisions`; dedupe within the pane so two rows share an index. When that prefab has a skeleton, set `row.pose = pose()` and push the row index to `skinned_rows`.
  7. **Light or DirectionalLight:** a row as `apply_live` and `sun_row` make them. Make `sun_row` public too, or rebuild it inline from `light_of`.
  8. **Camera fields:** `camera` is `current_camera_id()`, else `first_camera`. `has_camera = camera != 0`. `camera_inside` is true when the camera is `in_viewport` with the same owning pane. Read `camera_world` and `field_of_view` from the live Camera.
- **Allocation.** Every vector member is reserved for 64 in the constructor. `walk_` is reused. Allocation happens only when a pane is rebuilt and its result differs.

Add `src/engine_core/ViewportScenes.cpp` to the engine_core sources in `CMakeLists.txt`, next to `SnapshotPump.cpp`.

- [ ] **Step 5: Run the tests**

Run: build `sandbox`, then `build/Debug/sandbox.exe "[viewport]"`, then the whole `build/Debug/sandbox.exe`.
Expected: all pass. The prefab-render, billboard, and skybox suites prove the factored fills changed nothing for the place.

- [ ] **Step 6: Commit**

```bash
git add src/engine_core/ViewportScenes.hpp src/engine_core/ViewportScenes.cpp src/engine_core/SnapshotPump.hpp src/engine_core/SnapshotPump.cpp src/runner/SceneFeed.cpp sandbox/viewport_scene_tests.cpp CMakeLists.txt
git commit -m "ViewportPane: each pane's world is in the snapshot, rebuilt only when it changes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: `ViewportQueue`, which panes render when

**Files:**
- Create: `src/runner/ViewportQueue.hpp`, `src/runner/ViewportQueue.cpp` (in `STUDIO_CORE_SOURCES`, no GL)
- Test: `sandbox/viewport_queue_tests.cpp` (new), `CMakeLists.txt`

**Interfaces:**
- Produces (`runner`):
  ```cpp
  constexpr int kViewportPanesPerFrame = 8;
  constexpr long long kViewportPixelsPerFrame = 1 << 20;
  constexpr int kViewportMaxSide = 1024;
  constexpr int kViewportForgetFrames = 60;
  // points across at pixelsPerPoint, in device pixels: up to a multiple of 8, at most 1024; 0 for none.
  int ViewportPixels(double points, float pixelsPerPoint);

  class ViewportQueue {
  public:
      struct Job {
          engine_core::InstanceId pane = 0;
          std::shared_ptr<const engine_core::VisualViewport> scene;
          int width = 0;
          int height = 0;
      };
      enum class Result { Complete, Incomplete, NotReady };
      using Draw = std::function<Result(const Job&)>;
      void setScene(engine_core::InstanceId pane, std::shared_ptr<const engine_core::VisualViewport> scene);
      void shown(engine_core::InstanceId pane, int width, int height);
      // One frame: draws, with draw, the panes shown since the last call that need it, by budget, oldest first.
      // Returns how many it drew.
      int renderPending(const Draw& draw);
      // Panes not shown for kViewportForgetFrames frames, removed here, for their owner to delete.
      void takeForgotten(std::vector<engine_core::InstanceId>& out);
      // Whether pane has an image to show, and its size.
      bool image(engine_core::InstanceId pane, int& width, int& height) const;
      void clear();   // forget everything, as when GL is released
  };
  ```

- [ ] **Step 1: Write the failing tests** — `sandbox/viewport_queue_tests.cpp`

```cpp
// ViewportQueue: which ViewportPanes a host renders each frame, without GL.

#include "runner/ViewportQueue.hpp"

#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

namespace {

using engine_core::InstanceId;
using runner::ViewportQueue;

std::shared_ptr<const engine_core::VisualViewport> scene(InstanceId pane, std::uint64_t revision) {
    auto made = std::make_shared<engine_core::VisualViewport>();
    made->pane = pane;
    made->revision = revision;
    return made;
}

// A draw that records what it drew and answers result.
struct Recorder {
    std::vector<InstanceId> drawn;
    ViewportQueue::Result result = ViewportQueue::Result::Complete;
    ViewportQueue::Draw draw() {
        return [this](const ViewportQueue::Job& job) {
            drawn.push_back(job.pane);
            return result;
        };
    }
};

}  // namespace

TEST_CASE("VQ1 pixel sizes round up to a multiple of 8 and stop at 1024", "[viewport]") {
    REQUIRE(runner::ViewportPixels(64, 1.f) == 64);
    REQUIRE(runner::ViewportPixels(100, 1.f) == 104);
    REQUIRE(runner::ViewportPixels(100.2, 1.f) == 104);
    REQUIRE(runner::ViewportPixels(70, 2.f) == 144);
    REQUIRE(runner::ViewportPixels(5000, 1.f) == 1024);
    REQUIRE(runner::ViewportPixels(0, 1.f) == 0);
    REQUIRE(runner::ViewportPixels(-3, 1.f) == 0);
}

TEST_CASE("VQ2 a pane resized within its multiple of 8 is not drawn again", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(7, scene(7, 1));
    queue.shown(7, runner::ViewportPixels(97, 1.f), runner::ViewportPixels(97, 1.f));
    REQUIRE(queue.renderPending(record.draw()) == 1);
    queue.shown(7, runner::ViewportPixels(99, 1.f), runner::ViewportPixels(98, 1.f));
    REQUIRE(queue.renderPending(record.draw()) == 0);
    queue.shown(7, runner::ViewportPixels(105, 1.f), runner::ViewportPixels(98, 1.f));
    REQUIRE(queue.renderPending(record.draw()) == 1);
}

TEST_CASE("VQ3 only shown panes draw, and a new revision draws again", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    REQUIRE(queue.renderPending(record.draw()) == 0);
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 0);
    queue.setScene(1, scene(1, 2));
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    int width = 0;
    int height = 0;
    REQUIRE(queue.image(1, width, height));
    REQUIRE(width == 64);
}

TEST_CASE("VQ4 incomplete draws are tried again; not ready draws leave no image", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    record.result = ViewportQueue::Result::NotReady;
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    int width = 0;
    int height = 0;
    REQUIRE_FALSE(queue.image(1, width, height));
    record.result = ViewportQueue::Result::Incomplete;
    queue.shown(1, 64, 64);
    queue.renderPending(record.draw());
    REQUIRE(queue.image(1, width, height));
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    record.result = ViewportQueue::Result::Complete;
    queue.shown(1, 64, 64);
    queue.renderPending(record.draw());
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 0);
}

TEST_CASE("VQ5 sixty icons and one animated preview: 8 a frame, oldest first, none starved", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    for (InstanceId pane = 1; pane <= 60; ++pane) {
        queue.setScene(pane, scene(pane, 1));
    }
    queue.setScene(100, scene(100, 1));
    std::uint64_t spin = 1;
    int framesToAll = 0;
    std::vector<bool> done(61, false);
    for (int frame = 0; frame < 20; ++frame) {
        queue.setScene(100, scene(100, ++spin));
        for (InstanceId pane = 1; pane <= 60; ++pane) {
            queue.shown(pane, 64, 64);
        }
        queue.shown(100, 512, 512);
        record.drawn.clear();
        REQUIRE(queue.renderPending(record.draw()) <= runner::kViewportPanesPerFrame);
        for (const InstanceId drawn : record.drawn) {
            if (drawn <= 60) {
                done[drawn] = true;
            }
        }
        if (framesToAll == 0 && std::count(done.begin() + 1, done.end(), true) == 60) {
            framesToAll = frame + 1;
        }
    }
    REQUIRE(framesToAll != 0);
    REQUIRE(framesToAll <= 9);
    // The animated preview keeps getting drawn once the icons are done.
    record.drawn.clear();
    queue.setScene(100, scene(100, ++spin));
    queue.shown(100, 512, 512);
    queue.renderPending(record.draw());
    REQUIRE(record.drawn == std::vector<InstanceId>{100});
}

TEST_CASE("VQ6 the pixel budget stops a frame early, but always draws one", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    queue.setScene(2, scene(2, 1));
    queue.shown(1, 1024, 1024);
    queue.shown(2, 1024, 1024);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    queue.shown(1, 1024, 1024);
    queue.shown(2, 1024, 1024);
    REQUIRE(queue.renderPending(record.draw()) == 1);
}

TEST_CASE("VQ7 a pane not shown for 60 frames is forgotten", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    queue.shown(1, 64, 64);
    queue.renderPending(record.draw());
    std::vector<InstanceId> gone;
    for (int frame = 0; frame < runner::kViewportForgetFrames; ++frame) {
        queue.renderPending(record.draw());
        queue.takeForgotten(gone);
        REQUIRE(gone.empty());
    }
    queue.renderPending(record.draw());
    queue.takeForgotten(gone);
    REQUIRE(gone == std::vector<InstanceId>{1});
    int width = 0;
    int height = 0;
    REQUIRE_FALSE(queue.image(1, width, height));
}
```

Add `#include <algorithm>` for `std::count`, and the file to the sandbox sources.

- [ ] **Step 2: Build `sandbox` and see it fail**

Expected: `runner/ViewportQueue.hpp` not found.

- [ ] **Step 3: Implement.** `ViewportQueue.hpp` holds the declarations from the Interfaces block, with a class comment:

```cpp
// Which ViewportPanes a host renders each frame, kept without GL so it can be
// tested: the newest scene of each pane (setScene, from layout), whether it was
// painted and at what pixel size (shown, from paint), and what was last drawn.
// renderPending runs once a frame, before the panes paint. Among the panes shown
// since the last call whose scene, size, or incomplete last draw needs a render,
// it draws the oldest change first, until kViewportPanesPerFrame panes or
// kViewportPixelsPerFrame pixels, and always at least one.
```

Private state:

```cpp
    struct Entry {
        std::shared_ptr<const engine_core::VisualViewport> scene;
        int shownWidth = 0;
        int shownHeight = 0;
        bool shown = false;
        std::uint64_t lastShown = 0;
        // What was last drawn, and whether that draw was complete.
        std::uint64_t drawnRevision = 0;
        int drawnWidth = 0;
        int drawnHeight = 0;
        bool hasImage = false;
        bool complete = false;
        // The frame it first needed a render since its last complete one; 0 when it needs none.
        std::uint64_t dirtySince = 0;
    };
    std::unordered_map<engine_core::InstanceId, Entry> entries_;
    std::vector<engine_core::InstanceId> forgotten_;
    std::vector<std::pair<std::uint64_t, engine_core::InstanceId>> due_;
    std::uint64_t frame_ = 1;
```

`ViewportQueue.cpp`:

```cpp
int ViewportPixels(double points, float pixelsPerPoint) {
    const double pixels = std::ceil(points * static_cast<double>(pixelsPerPoint) - 1e-6);
    if (!(pixels > 0.0)) {
        return 0;
    }
    const int rounded = (static_cast<int>(std::min(pixels, 1.0e6)) + 7) / 8 * 8;
    return std::min(rounded, kViewportMaxSide);
}

void ViewportQueue::setScene(engine_core::InstanceId pane,
                             std::shared_ptr<const engine_core::VisualViewport> scene) {
    entries_[pane].scene = std::move(scene);
}

void ViewportQueue::shown(engine_core::InstanceId pane, int width, int height) {
    Entry& entry = entries_[pane];
    entry.shown = true;
    entry.shownWidth = width;
    entry.shownHeight = height;
    entry.lastShown = frame_;
}

int ViewportQueue::renderPending(const Draw& draw) {
    due_.clear();
    for (auto& [pane, entry] : entries_) {
        if (!entry.shown || entry.scene == nullptr || entry.shownWidth <= 0 || entry.shownHeight <= 0) {
            continue;
        }
        const bool needed = !entry.complete || entry.scene->revision != entry.drawnRevision ||
                            entry.shownWidth != entry.drawnWidth || entry.shownHeight != entry.drawnHeight;
        if (!needed) {
            continue;
        }
        if (entry.dirtySince == 0) {
            entry.dirtySince = frame_;
        }
        due_.emplace_back(entry.dirtySince, pane);
    }
    std::sort(due_.begin(), due_.end());
    int drew = 0;
    long long pixels = 0;
    for (const auto& [since, pane] : due_) {
        Entry& entry = entries_[pane];
        const long long cost = static_cast<long long>(entry.shownWidth) * entry.shownHeight;
        if (drew > 0 && (drew >= kViewportPanesPerFrame || pixels + cost > kViewportPixelsPerFrame)) {
            break;
        }
        const Job job{pane, entry.scene, entry.shownWidth, entry.shownHeight};
        const Result result = draw(job);
        ++drew;
        pixels += cost;
        if (result == Result::NotReady) {
            continue;
        }
        entry.hasImage = true;
        entry.drawnWidth = job.width;
        entry.drawnHeight = job.height;
        entry.drawnRevision = job.scene->revision;
        entry.complete = result == Result::Complete;
        if (entry.complete) {
            entry.dirtySince = 0;
        }
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        it->second.shown = false;
        if (frame_ - it->second.lastShown > static_cast<std::uint64_t>(kViewportForgetFrames)) {
            forgotten_.push_back(it->first);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    ++frame_;
    return drew;
}

void ViewportQueue::takeForgotten(std::vector<engine_core::InstanceId>& out) {
    out.swap(forgotten_);
    forgotten_.clear();
}

bool ViewportQueue::image(engine_core::InstanceId pane, int& width, int& height) const {
    const auto found = entries_.find(pane);
    if (found == entries_.end() || !found->second.hasImage) {
        return false;
    }
    width = found->second.drawnWidth;
    height = found->second.drawnHeight;
    return true;
}

void ViewportQueue::clear() {
    for (const auto& [pane, entry] : entries_) {
        forgotten_.push_back(pane);
    }
    entries_.clear();
}
```

**Check VQ7's off-by-one against this code.** A pane last shown at frame f is forgotten in the first `renderPending` whose `frame_ - f > 60`. If that lands one frame off from the test, fix the test's loop count, not the constant. The intent is "not shown for 60 frames".

**Entry created by `shown()` alone.** It has no scene and stays put until it has one. That is fine.

Add `src/runner/ViewportQueue.cpp` to `STUDIO_CORE_SOURCES` after `src/runner/BillboardMath.cpp`.

- [ ] **Step 4: Run the tests**

Run: build `sandbox`, then `build/Debug/sandbox.exe "[viewport]"`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/runner/ViewportQueue.hpp src/runner/ViewportQueue.cpp sandbox/viewport_queue_tests.cpp CMakeLists.txt
git commit -m "ViewportPane: a queue picks which panes render each frame, oldest first, within a budget

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: `ViewportRenderer` and `ViewportNode`, wired into the hosts

**Files:**
- Create: `src/runner/ViewportRenderer.hpp`, `src/runner/ViewportRenderer.cpp`, `src/runner/ViewportNode.hpp`, `src/runner/ViewportNode.cpp`
- Modify: `src/runner/GuiTree.hpp`, `src/runner/GuiTree.cpp`, `src/runner/GuiLayer.hpp`, `src/runner/GuiLayer.cpp`
- Modify: `src/runner/GameView.hpp`, `src/runner/GameView.cpp`
- Modify: `src/ide/PluginWidgetPane.hpp`, `src/ide/PluginWidgetPane.cpp`, `src/ide/IdeLayoutDocking.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/ViewportPaneTest.cpp` (new), `tests/StudioLayoutTest.cpp`, `tests/SceneRenderCheck.cpp`

**Interfaces:**
- Consumes: `Painter::drawTexture` (Task 1); `Renderer::setTransparentOutput`, `setTargetPool`, `setSkyCacheSize` (Tasks 2-3); `LoadPrefabDraws`, `LightDrawFor`, `AppendRowDraws`, `SkyFor` (Task 4); `VisualViewport` (Task 7); `ViewportQueue`, `ViewportPixels` (Task 8).
- Produces:
  ```cpp
  class ViewportRenderer {
  public:
      // meshes and textures: the host's caches, or null to own a pair.
      ViewportRenderer(MeshCache* meshes, TextureCache* textures);
      ~ViewportRenderer();                                 // touches no GL
      void setScene(engine_core::InstanceId pane, std::shared_ptr<const engine_core::VisualViewport> scene);
      void setResourcesRoot(const std::filesystem::path& root);   // owned caches only
      // Paint: pane's last image over x, y, width, height (window points), and notes it shown.
      void composite(jadefx::UiRenderer& ui, engine_core::InstanceId pane, double x, double y, double width,
                     double height, float opacity);
      void renderPending();   // host paint, before the GUI, GL current
      void release();         // host sceneChanged
      // For checks: render one pane now, whatever the budget, and read its texture name.
      ViewportQueue::Result renderNow(const ViewportQueue::Job& job);
      unsigned texture(engine_core::InstanceId pane) const;
  };
  class ViewportNode : public jadefx::StackPane {
  public:
      ViewportNode(engine_core::InstanceId pane, ViewportRenderer* renderer);
      void setScene(std::shared_ptr<const engine_core::VisualViewport> scene);
      const std::shared_ptr<const engine_core::VisualViewport>& scene() const;
      void setImageOpacity(float opacity);
      float imageOpacity() const;
  };
  // GuiTree:
  void setViewportRenderer(ViewportRenderer* renderer);
  void setViewports(const std::vector<std::shared_ptr<const engine_core::VisualViewport>>* viewports);
  // GuiLayer:
  void setViewportRenderer(ViewportRenderer* renderer);
  void sync(const std::vector<std::shared_ptr<const engine_core::VisualViewport>>* viewports = nullptr);
  ```

- [ ] **Step 1: Write the failing studio test** — `tests/ViewportPaneTest.cpp`, on `GuiImageTest.cpp`'s layout setup:

```cpp
#include "ide/IdeLayout.hpp"
#include "runner/GameView.hpp"
#include "runner/ViewportNode.hpp"

#include "Camera.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <string>

// A ViewportPane in the Scene View's GUI layer: a viewportpane node holding its
// pane's scene from the snapshot, GUI children as nodes over it, 3D children not.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

runner::GameView* FindGameView(jadefx::Scene& scene) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* view = dynamic_cast<runner::GameView*>(node)) {
            return view;
        }
    }
    return nullptr;
}

}  // namespace

int RunViewportPaneTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    double time = scene.timeSeconds() + 0.01;
    auto frame = [&] {
        for (int i = 0; i < 4; ++i) {
            scene.layout(1280, 800, time);
            time += 0.02;
        }
    };
    frame();
    runner::GameView* view = FindGameView(scene);
    Expect(view != nullptr, "VN the studio has a Scene View");
    if (view == nullptr) {
        return gFailures;
    }
    engine_core::Engine& engine = layout.simulation();
    engine_core::InstanceId screen = 0, pane = 0, label = 0, thing = 0, camera = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        screen = engine_core::lua_create_instance(game, "ScreenGui")->id();
        game.set_parent(screen, game.scene_service("Gui"));
        pane = engine_core::lua_create_instance(game, "ViewportPane")->id();
        game.set_parent(pane, screen);
        label = engine_core::lua_create_instance(game, "Label")->id();
        game.set_parent(label, pane);
        thing = engine_core::lua_create_instance(game, "GameObject")->id();
        game.set_parent(thing, pane);
        camera = engine_core::lua_create_instance(game, "Camera")->id();
        game.set_parent(camera, pane);
    });
    frame();
    jadefx::Node* node = view->guiLayer().nodeFor(pane);
    Expect(node != nullptr, "VN1 a ViewportPane in a ScreenGui is drawn");
    if (node == nullptr) {
        return gFailures;
    }
    Expect(std::string(node->getElementType()) == "viewportpane", "VN1 its element type is viewportpane");
    auto* viewport = dynamic_cast<runner::ViewportNode*>(node);
    Expect(viewport != nullptr, "VN2 its node is a ViewportNode");
    Expect(view->guiLayer().nodeFor(label) != nullptr, "VN3 a GUI child is a node over it");
    Expect(view->guiLayer().nodeFor(thing) == nullptr && view->guiLayer().nodeFor(camera) == nullptr,
           "VN3 its 3D children are not nodes");
    if (viewport != nullptr) {
        Expect(viewport->scene() != nullptr && viewport->scene()->pane == pane, "VN4 the node holds its pane's scene");
        Expect(viewport->scene() != nullptr && viewport->scene()->camera == camera,
               "VN4 the scene draws from the Camera inside");
        const std::uint64_t before = viewport->scene() != nullptr ? viewport->scene()->revision : 0;
        engine.on_simulation([&](engine_core::DataModel& game) {
            dynamic_cast<engine_core::GameObject*>(game.instance(thing))
                ->set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
        });
        frame();
        Expect(viewport->scene() != nullptr && viewport->scene()->revision != before,
               "VN5 moving a GameObject inside gives the node a new scene");
        engine.on_simulation([&](engine_core::DataModel& game) {
            engine_core::LuaSlot value;
            value.kind = engine_core::LuaSlot::Kind::Number;
            value.number = 0.25;
            dynamic_cast<engine_core::ViewportPane*>(game.instance(pane))
                ->set_value(engine_core::GuiProperty::ImageTransparency, value);
        });
        frame();
        Expect(std::abs(viewport->imageOpacity() - 0.75f) < 1e-4f, "VN6 ImageTransparency 0.25 draws the image at 0.75");
    }
    engine.on_simulation([&](engine_core::DataModel& game) { game.destroy_tree(screen); });
    frame();
    if (gFailures == 0) {
        std::printf("viewport pane tests passed\n");
    }
    return gFailures;
}
```

(`#include <cmath>` for `std::abs`.) In `tests/StudioLayoutTest.cpp`, declare `int RunViewportPaneTests(ide::IdeLayout& layout, jadefx::Scene& scene);` beside `RunGuiImageTests`, and call it after `RunGuiImageTests`. Add the file to `studio-tests` in `CMakeLists.txt`.

- [ ] **Step 2: Build `studio-tests` and see it fail**

Expected: `runner/ViewportNode.hpp` not found.

- [ ] **Step 3: `ViewportNode`.** `ViewportNode.hpp`:

```cpp
#pragma once

#include "SnapshotPump.hpp"
#include "types.hpp"

#include "jadefx/jadefx.hpp"

#include <memory>

namespace runner {

class ViewportRenderer;

// A ViewportPane's node: a StackPane, so GUI children lay out over it, that
// draws its pane's last rendered image between its background and its
// children. It holds the pane's scene for its host's ViewportRenderer, which
// owns every GL object; the node owns none. With no renderer, as in a
// headless test, it draws only its background and children.
class ViewportNode : public jadefx::StackPane {
public:
    ViewportNode(engine_core::InstanceId pane, ViewportRenderer* renderer);

    void setScene(std::shared_ptr<const engine_core::VisualViewport> scene);
    const std::shared_ptr<const engine_core::VisualViewport>& scene() const { return scene_; }
    // 1 - ImageTransparency.
    void setImageOpacity(float opacity) { imageOpacity_ = opacity; }
    float imageOpacity() const { return imageOpacity_; }

protected:
    void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;

private:
    engine_core::InstanceId pane_;
    ViewportRenderer* renderer_;
    std::shared_ptr<const engine_core::VisualViewport> scene_;
    float imageOpacity_ = 1.f;
};

}  // namespace runner
```

`ViewportNode.cpp`:

```cpp
#include "ViewportNode.hpp"

#include "ViewportRenderer.hpp"

namespace runner {

ViewportNode::ViewportNode(engine_core::InstanceId pane, ViewportRenderer* renderer)
    : pane_(pane), renderer_(renderer) {}

void ViewportNode::setScene(std::shared_ptr<const engine_core::VisualViewport> scene) {
    scene_ = std::move(scene);
    if (renderer_ != nullptr) {
        renderer_->setScene(pane_, scene_);
    }
}

void ViewportNode::renderChildren(jadefx::UiRenderer& renderer, float opacity) {
    // Node::render draws the background, then the children, then the content;
    // the image goes over the background and under the children.
    if (renderer_ != nullptr) {
        renderer_->composite(renderer, pane_, getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(),
                             opacity * imageOpacity_);
    }
    jadefx::StackPane::renderChildren(renderer, opacity);
}

}  // namespace runner
```

- [ ] **Step 4: `ViewportRenderer`.** `ViewportRenderer.hpp` declares the class from the Interfaces block, with this comment:

```cpp
// Draws a host's ViewportPanes, each into a texture of its own, and composites
// them as their nodes paint. A host is a Scene View or a plugin pane, one
// GL context each.
// - A Renderer of its own, never the host's, draws them. Output is
//   transparent, shadows are off, targets are pooled, and up to three skies
//   keep their cubes.
// - The host's MeshCache and TextureCache are used when it has them, so an
//   asset in the world and in a pane is uploaded once.
// - ViewportQueue decides which panes draw each frame. A pane is drawn from
//   its own Camera, lit by its own lights and Ambient, with a new Lighting's
//   tone settings.
// - renderPending runs in the host's paint before its GUI. composite runs as
//   each pane's node paints.
// - Every call but the constructor and destructor needs the host's GL
//   context current. release deletes everything in it.
```

Private members:
- `Renderer renderer_;`
- `std::unique_ptr<MeshCache> ownMeshes_;` and `std::unique_ptr<TextureCache> ownTextures_;`
- `MeshCache* meshes_;` and `TextureCache* textures_;`
- `ViewportQueue queue_;`
- `struct Target { unsigned framebuffer = 0; unsigned color = 0; int width = 0; int height = 0; }; std::unordered_map<engine_core::InstanceId, Target> targets_;`
- `std::vector<engine_core::InstanceId> forgotten_;`
- `std::vector<MeshDraw> templates_, draws_; std::vector<std::vector<MeshDraw>> prefabDraws_; std::vector<LightDraw> lights_;`
- `bool attempted_ = false, ready_ = false;`

`ViewportRenderer.cpp`:

```cpp
ViewportRenderer::ViewportRenderer(MeshCache* meshes, TextureCache* textures)
    : ownMeshes_(meshes == nullptr ? std::make_unique<MeshCache>() : nullptr),
      ownTextures_(textures == nullptr ? std::make_unique<TextureCache>() : nullptr),
      meshes_(meshes != nullptr ? meshes : ownMeshes_.get()),
      textures_(textures != nullptr ? textures : ownTextures_.get()) {}

// Needs no GL: one still holding names is one whose context is gone.
ViewportRenderer::~ViewportRenderer() = default;

void ViewportRenderer::setScene(engine_core::InstanceId pane,
                                std::shared_ptr<const engine_core::VisualViewport> scene) {
    queue_.setScene(pane, std::move(scene));
}

void ViewportRenderer::setResourcesRoot(const std::filesystem::path& root) {
    if (ownMeshes_) {
        ownMeshes_->setRoot(root);
        ownTextures_->setRoot(root);
    }
}

bool ViewportRenderer::ensureGraphics() {
    if (attempted_) {
        return ready_;
    }
    attempted_ = true;
    ready_ = LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); }) &&
             renderer_.initialize();
    if (ready_) {
        renderer_.setTransparentOutput(true);
        ShadowSettings noShadows;
        noShadows.enabled = false;
        renderer_.setShadowSettings(noShadows);
        renderer_.setTargetPool(std::size_t(64) << 20);
        renderer_.setSkyCacheSize(3);
    }
    return ready_;
}
```

Use the same GL loader call MaterialBall's `ensureGraphics` uses: copy its include and lambda.

```cpp
void ViewportRenderer::composite(jadefx::UiRenderer& ui, engine_core::InstanceId pane, double x, double y,
                                 double width, double height, float opacity) {
    jadefx::Painter painter(ui);
    const float scale = painter.pixelsPerPoint();
    queue_.shown(pane, ViewportPixels(width, scale), ViewportPixels(height, scale));
    int drawnWidth = 0;
    int drawnHeight = 0;
    const auto found = targets_.find(pane);
    if (found == targets_.end() || !queue_.image(pane, drawnWidth, drawnHeight)) {
        return;
    }
    painter.drawTexture(found->second.color, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                        static_cast<float>(height), opacity, jadefx::TextureAlpha::Premultiplied, true);
}

void ViewportRenderer::renderPending() {
    PROFILE_SCOPE("Viewport panes", profiler::Group::Render);
    if (!ensureGraphics()) {
        return;
    }
    if (ownTextures_) {
        ownTextures_->pump();
    }
    queue_.renderPending([this](const ViewportQueue::Job& job) { return renderNow(job); });
    queue_.takeForgotten(forgotten_);
    for (const engine_core::InstanceId pane : forgotten_) {
        deleteTarget(pane);
    }
    forgotten_.clear();
}
```

`renderNow(job)`:
1. Return `NotReady` unless `ensureGraphics()`.
2. Make or resize the pane's `Target`: an RGBA8 texture, `GL_LINEAR` min and mag, `CLAMP_TO_EDGE`, attached to a new FBO, made exactly as `MaterialBall::ensureGraphics` makes its FBO. On a size change, delete and remake both. If incomplete, return `NotReady`.
3. Save `RT_GL_FRAMEBUFFER_BINDING`, `GL_VIEWPORT`, and the scissor box and enable state as `MaterialBall::draw` does. Bind the FBO, `glViewport(0, 0, w, h)`, `glDisable(GL_SCISSOR_TEST)`.
4. **No camera** (`!job.scene->has_camera`): `glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);`, restore, return `Complete`.
5. **With a camera,** build the draws:
   - `prefabDraws_` is resized to `scene.prefabs.size()`. Entry 0 stays empty; for each nonzero entry, call `LoadPrefabDraws(*scene.prefabs[i], *meshes_, *textures_, nextSlot, prefabDraws_[i])`.
   - For each row: `LightDrawFor` into `lights_`, then `AppendRowDraws(row, prefabDraws_[row.prefab], draws_)` when `row.prefab != 0`.
   - `SceneLighting lighting;` takes `ambient` from `scene.ambient`, and `SkyFor(scene.sky, *textures_, lighting.sky)`.
   - `renderer_.setLighting(lighting); renderer_.setCamera(scene.camera_world, scene.field_of_view);`
   - `const bool drawn = renderer_.draw(0, 0, w, h, w, h, draws_.data(), int(draws_.size()), lights_.data(), int(lights_.size()));`
6. Restore everything saved. Return:
   - `NotReady` when `!drawn`;
   - `Incomplete` when `textures_->loading()`, or `scene.sky.present && !scene.sky.image.empty() && lighting.sky.image == 0`;
   - `Complete` otherwise.

`texture(pane)` returns the target's color name, or 0. `deleteTarget(pane)` deletes its FBO and texture. `release()` does five things:
- calls `queue_.clear()` and discards the forgotten list;
- deletes every target;
- calls `renderer_.shutdown()`;
- clears `ownMeshes_` and `ownTextures_` if owned;
- sets `attempted_ = ready_ = false`.

Add both new `.cpp` files to `STUDIO_SOURCES` after `src/runner/SceneDraws.cpp`.

- [ ] **Step 5: GuiTree and GuiLayer.**
  - **GuiTree members:** add `ViewportRenderer* viewportRenderer_ = nullptr;` and `const std::vector<std::shared_ptr<const engine_core::VisualViewport>>* viewports_ = nullptr;`, with the two setters from Interfaces. The class comment gets one sentence: "A ViewportPane's node is a ViewportNode holding its pane's scene from setViewports, the frame's snapshot list, found by pane id."
  - **makeNode:** the Task 5 placeholder becomes `node = std::make_shared<GuiNode<ViewportNode>>("viewportpane", input, false, id, viewportRenderer_);`.
  - **build:** after the ImagePane block:
    ```cpp
    // Read at every sync: the pane's scene moves with its contents, not its revision.
    if (dynamic_cast<const engine_core::ViewportPane*>(&gui) != nullptr) {
        if (auto* viewport = dynamic_cast<ViewportNode*>(entry.node.get())) {
            viewport->setScene(findViewport(id));
            viewport->setImageOpacity(1.f - static_cast<float>(gui.number(GuiProperty::ImageTransparency)));
        }
    }
    ```
  - **findViewport(id):** a private member. It binary-searches `*viewports_`, which is sorted by pane id, and returns null when `viewports_` is null or holds no entry for that pane.
  - **GuiLayer:** `sync` takes the list and calls `tree_->setViewports(viewports)` before `syncTree()`, then `tree_->setViewports(nullptr)` after. `setViewportRenderer` forwards to the tree. Update the GuiLayer and GuiTree element-type comments to include `viewportpane`.

- [ ] **Step 6: GameView.**
  - Add the member `std::unique_ptr<ViewportRenderer> viewports_;`. In the constructor, `viewports_ = std::make_unique<ViewportRenderer>(&meshes_, &textures_);` and `guiLayer_->setViewportRenderer(viewports_.get());`.
  - `layoutChildren`: `guiLayer_->sync(&frameSnapshot_->viewports);`.
  - `renderContent`: inside the `"GUIs and overlays"` scope, before `Node::renderChildren(renderer, opacity);`:
    ```cpp
        // The ViewportPanes due this frame, each into its own texture, before the GUI paints them.
        if (graphicsReady_) {
            viewports_->renderPending();
        }
    ```
    Use the flag that `ensureGraphics()` sets; it is `graphicsReady_` in `sceneChanged`.
  - `sceneChanged`: before `renderer_.shutdown();`, add `viewports_->release();`.

- [ ] **Step 7: PluginWidgetPane.**
  - The constructor takes `runner::SceneFeed& feed` after `engine` and stores it as `feed_`, and adds the member `runner::ViewportRenderer viewports_{nullptr, nullptr};`.
  - In the constructor, call `tree_->setViewportRenderer(&viewports_);`.
  - In `sync`, before taking the lock: `const std::shared_ptr<const engine_core::VisualSnapshot> held = feed_.hold();`. Under the lock, before `beginPass`: `tree_->setViewports(held ? &held->viewports : nullptr);` and `viewports_.setResourcesRoot(engine_.datamodel().resources_root());`. After `endPass`: `tree_->setViewports(nullptr);`.
  - Override `renderChildren(jadefx::UiRenderer& renderer, float opacity)` to call `viewports_.renderPending();` then `IdePane::renderChildren(renderer, opacity);`.
  - Override `sceneChanged(jadefx::Scene* previous)`. Call the base, then `viewports_.release()` under the same guard GameView uses.
  - In `IdeLayoutDocking.cpp:2101`, pass `runner_.feed()`.
  - Add `#include "runner/SceneFeed.hpp"` and `#include "runner/ViewportRenderer.hpp"` to the header.

- [ ] **Step 8: A GL check of `renderNow`.** In `SceneRenderCheck.cpp`, add this after the Task 2 and 3 block:

```cpp
    {
        // VR6-VR8: ViewportRenderer draws a pane's scene into its texture: a
        // white box in front of its Camera, transparent around it, and a scene
        // with no Camera clears to nothing.
        runner::MeshCache meshes;
        runner::TextureCache textures;
        meshes.setRoot(regressionRoot);
        textures.setRoot(regressionRoot);
        runner::ViewportRenderer panes(&meshes, &textures);
        auto scene = std::make_shared<engine_core::VisualViewport>();
        scene->pane = 1;
        scene->revision = 1;
        scene->has_camera = true;
        scene->camera_world = engine_core::matrix4_translation(0.f, 0.f, 6.f);
        scene->field_of_view = 40.f;
        auto prefab = std::make_shared<engine_core::VisualPrefab>();
        prefab->meshes.emplace_back();
        prefab->meshes[0].path = kCubeMeshPath;
        prefab->meshes[0].color = engine_core::ColorRgb{1.f, 1.f, 1.f, 1.f};
        scene->prefabs = {nullptr, prefab};
        engine_core::VisualInstance row;
        row.id = 2;
        row.prefab = 1;
        scene->rows.push_back(row);
        engine_core::VisualInstance sun;
        sun.id = 3;
        sun.light.kind = engine_core::VisualLight::Kind::Directional;
        sun.light.enabled = true;
        sun.light.intensity = 2.f;
        sun.light.direction[0] = 0.4f;
        sun.light.direction[1] = 0.6f;
        sun.light.direction[2] = 0.7f;
        scene->rows.push_back(sun);
        runner::ViewportQueue::Job job{1, scene, 64, 64};
        runner::ViewportQueue::Result result = runner::ViewportQueue::Result::NotReady;
        for (int attempt = 0; attempt < 4 && result != runner::ViewportQueue::Result::Complete; ++attempt) {
            result = panes.renderNow(job);
        }
        Expect(result == runner::ViewportQueue::Result::Complete, "VR6 a pane's scene renders completely");
        std::vector<unsigned char> rgba(64 * 64 * 4);
        glBindTexture(runner::GL_TEXTURE_2D, panes.texture(1));
        glGetTexImage(runner::GL_TEXTURE_2D, 0, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, rgba.data());
        Expect(rgba[3] == 0 && rgba[(32 * 64 + 32) * 4 + 3] == 255 && rgba[(32 * 64 + 32) * 4] > 100,
               "VR7 the box is drawn, opaque, with nothing around it");
        auto empty = std::make_shared<engine_core::VisualViewport>(*scene);
        empty->has_camera = false;
        empty->revision = 2;
        panes.renderNow(runner::ViewportQueue::Job{1, empty, 64, 64});
        glGetTexImage(runner::GL_TEXTURE_2D, 0, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, rgba.data());
        Expect(rgba[(32 * 64 + 32) * 4 + 3] == 0, "VR8 a pane with no Camera clears to nothing");
        glBindTexture(runner::GL_TEXTURE_2D, 0);
        panes.release();
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "VR the pane renderer leaves no GL error");
    }
```

`regressionRoot` and `kCubeMeshPath` stand for the resources folder and cube mesh file that the file's other file-backed checks use. Search for `meshes.setRoot` or `.amesh` in `SceneRenderCheck.cpp` and use those names. If `glGetTexImage` is not in `runner/gl.hpp`'s loader, read back through a framebuffer with `glReadPixels`, as `OffscreenTarget` does.

- [ ] **Step 9: Run everything that draws**

Run: build `studio-tests scene-render-check AnarchyStudio`, then `build/Debug/studio-tests.exe` and `build/Debug/scene-render-check.exe`.
Expected: `viewport pane tests passed`, VR6 to VR8 pass, and nothing else changes.

- [ ] **Step 10: Commit**

```bash
git add src/runner/ViewportRenderer.hpp src/runner/ViewportRenderer.cpp src/runner/ViewportNode.hpp src/runner/ViewportNode.cpp src/runner/GuiTree.hpp src/runner/GuiTree.cpp src/runner/GuiLayer.hpp src/runner/GuiLayer.cpp src/runner/GameView.hpp src/runner/GameView.cpp src/ide/PluginWidgetPane.hpp src/ide/PluginWidgetPane.cpp src/ide/IdeLayoutDocking.cpp tests/ViewportPaneTest.cpp tests/StudioLayoutTest.cpp tests/SceneRenderCheck.cpp CMakeLists.txt
git commit -m "ViewportPane: panes render into their own textures and show under their children

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Studio touches: the camera list, F, and ViewportSize

**Files:**
- Modify: `src/runner/GameView.cpp` (camera list walk, `frameSelection`)
- Modify: `src/runner/GuiTree.cpp` (ViewportSize posting)
- Test: `tests/ViewportPaneTest.cpp`

**Interfaces:**
- Consumes: `DataModel::in_viewport` (Task 6), `VisualViewport::camera`, `camera_inside` (Task 7), `ViewportNode` (Task 9).

- [ ] **Step 1: Write the failing checks.** Append to `RunViewportPaneTests`, before the cleanup:

```cpp
    // VN7: a Camera in a pane under Workspace is not offered by the Scene View.
    engine_core::InstanceId hostCamera = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        const engine_core::InstanceId anchor = engine_core::lua_create_instance(game, "GameObject")->id();
        game.set_parent(anchor, game.scene_service("Workspace"));
        const engine_core::InstanceId board = engine_core::lua_create_instance(game, "BillboardGui")->id();
        game.set_parent(board, anchor);
        const engine_core::InstanceId inner = engine_core::lua_create_instance(game, "ViewportPane")->id();
        game.set_parent(inner, board);
        hostCamera = engine_core::lua_create_instance(game, "Camera")->id();
        game.set_parent(hostCamera, inner);
        game.set_name(hostCamera, "PaneCamera");
    });
    frame();
    bool offered = false;
    for (const std::string& item : view->cameraList().getItems()) {
        offered = offered || item == "PaneCamera";
    }
    Expect(!offered, "VN7 a Camera inside a ViewportPane is not in the Scene View's camera list");

    // VN8: the pane reports its size, in points, to the Camera inside it.
    frame();
    frame();
    float reportedWidth = 0.f;
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (auto* own = dynamic_cast<engine_core::Camera*>(game.instance(camera))) {
            reportedWidth = own->viewport_size().x;
        }
    });
    frame();
    Expect(reportedWidth > 0.f && std::abs(reportedWidth - static_cast<float>(node->getWidth())) < 0.5f,
           "VN8 a pane's Camera gets the pane's size as its ViewportSize");
```

`view->cameraList()` is the Scene View's camera `ComboBox`, as `StudioLayoutTest` reads it. Its items are the Cameras' names. `Camera::viewport_size()` is the accessor behind `Camera.ViewportSize`.

The `engine.on_simulation` read in VN8 runs on the next frame's step. That is why a `frame()` follows it before the `Expect`. `reportedWidth` is written on the simulation thread and read after the frame. That is the same pattern `GuiImageTest` uses for its setup lambdas.

- [ ] **Step 2: Build `studio-tests` and see VN7 and VN8 fail**

Expected: both FAIL lines print.

- [ ] **Step 3: Implement.**
  - **GameView camera walk** (`readWorkspace`), first thing in the loop after popping `id`: `if (game_->in_viewport(id)) { continue; }`, with the comment `// A ViewportPane's Cameras look into its own world, not the place's.`
  - **`frameSelection`:** `if (id == camera || game.in_viewport(id)) { continue; }`, with the comment `// A ViewportPane's contents are in its own world's coordinates, not the view's.`
  - **GuiTree build**, in the ViewportPane block after `setScene`:
    ```cpp
    // The Camera inside a pane learns the pane's size, so ViewportPointToRay works in it.
    const std::shared_ptr<const engine_core::VisualViewport>& scene = viewport->scene();
    if (scene != nullptr && scene->camera_inside && entry.node->getWidth() > 0.0 &&
        (scene->camera != entry.sizedCamera || entry.node->getWidth() != entry.sizedWidth ||
         entry.node->getHeight() != entry.sizedHeight)) {
        entry.sizedCamera = scene->camera;
        entry.sizedWidth = entry.node->getWidth();
        entry.sizedHeight = entry.node->getHeight();
        const engine_core::Vec2 size{static_cast<float>(entry.sizedWidth), static_cast<float>(entry.sizedHeight)};
        engine_.on_simulation([camera = scene->camera, size](engine_core::DataModel& game) {
            if (auto* target = dynamic_cast<engine_core::Camera*>(game.instance(camera))) {
                target->set_viewport_size(size);
            }
        });
    }
    ```
    Add `engine_core::InstanceId sizedCamera = 0; double sizedWidth = 0, sizedHeight = 0;` to `GuiTree::Entry`, and include `Camera.hpp`.

- [ ] **Step 4: Run**

Run: build `studio-tests`, then `build/Debug/studio-tests.exe`
Expected: all pass, including the existing camera-list tests in `StudioLayoutTest.cpp`.

- [ ] **Step 5: Commit**

```bash
git add src/runner/GameView.cpp src/runner/GuiTree.cpp tests/ViewportPaneTest.cpp
git commit -m "ViewportPane: its Cameras stay out of the Scene View's list, and learn the pane's size

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: `viewport-demo`, docs, and the full run

**Files:**
- Create: `tests/ViewportDemo.cpp`
- Modify: `CMakeLists.txt` (target and warnings list), `README.md`, `src/engine_instances/README.md`, `src/engine_core/README.md` (if it lists `VisualSnapshot` fields)

- [ ] **Step 1: The demo.** `tests/ViewportDemo.cpp` copies `tests/BillboardDemo.cpp`'s shape: the Application, a temp resources folder with `meshes/cube.amesh`, `start`, `frame`, and `save`. Change these parts:
  - **Header comment.** "By hand, not ctest: viewport-demo out-dir shows two ViewportPanes in a ScreenGui over an empty place and saves the window. The left pane, 128 points square with BackgroundTransparency 1, holds a gray cube, a DirectionalLight, and a Camera at (0, 0, 6) looking at it: an icon on nothing, so the dark view shows around the cube. The right pane, 320 points square, holds a Skybox, the same cube, and its own Camera, and a script spins the cube a little each frame. It saves <out-dir>/viewports.png after 20 frames and viewports-later.png 30 frames after that; the cube in the right pane must have turned between the two, and the left must not have changed. It exits 1 when either is not so."
  - **`build`.** Make the cube Prefab as BillboardDemo does. Then:
    1. A `ScreenGui` under Gui, an `HBox` in it, and two `ViewportPane`s in the HBox, each with a `Camera` (FieldOfView 40, Transform `matrix4_translation(0, 0, 6)`) set as its `CurrentCamera`.
    2. A `GameObject` on the Prefab, and in the left pane a `DirectionalLight` with its default Direction.
    3. Sizes by CSS on the ScreenGui: `#Icon { width: 128px; height: 128px; } #Preview { width: 320px; height: 320px; }`. Name the panes `Icon` and `Preview`.
    4. The left pane's `BackgroundTransparency` set to 1.
    5. A Skybox in the right pane only when a sky image ships in `resources/`. Search for an `.hdr` used by the Skybox demo or tests. With none, skip it and the right pane uses Ambient.
    6. The spin is a `Script` in the right pane: `game:GetService("RunService").RenderStepped:Connect(function(dt) cube.Transform = cube.Transform * CFrame.Angles(0, dt, 0) end)`. Use the engine's real API for rotating a Transform; check `sandbox` script tests for `Transform` and `Matrix4` usage.
  - **`frame`.** Save at frame 20 and at frame 50. Compare the two PNGs' pixel buffers, which are kept in memory: the Preview pane's rectangle must differ and the Icon pane's rectangle must not. Then close the stage.

  The pane rectangles come from `view_->guiLayer().nodeFor(id)` (`getAbsoluteX`, `getAbsoluteY`, `getWidth`, `getHeight`), in points times the framebuffer scale.

  In `CMakeLists.txt`, beside `billboard-demo`:

```cmake
# By hand, not ctest: viewport-demo <out-dir> saves two ViewportPanes, an icon and a spinning preview.
add_executable(viewport-demo tests/ViewportDemo.cpp)
target_link_libraries(viewport-demo PRIVATE studio)
```

and add `viewport-demo` to the `anarchy_warnings` `foreach` list next to `billboard-demo`, and to the list at line ~1212 that holds `billboard-demo`.

- [ ] **Step 2: Run the demo and look at the pictures**

```bash
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target viewport-demo --parallel
build/Debug/viewport-demo.exe "C:/Users/Andrew/AppData/Local/Temp/viewport-demo"
```

Expected: exit 0. Open both PNGs with `Start-Process` from PowerShell, as the memory says, and send them with `SendUserFile`. Check:
- the left pane shows a lit gray cube on the view's dark background, with no box around it;
- the right pane shows the sky (or Ambient) behind a cube turned differently in the two pictures;
- there is no seam at the panes' edges.

- [ ] **Step 3: Docs.**
  - **`README.md`, Screen GUIs section:** add `ViewportPane` to the class list and `viewportpane` to the element types, plus one paragraph. A ViewportPane draws the GameObjects, lights, and Skybox inside it from its CurrentCamera (or the first Camera inside), lit only by its own lights and Ambient, with GUI children over it. What it holds never joins the place's world. Animators inside it play; physics, Terrain, constraints, and sounds do nothing there. It renders again only when something it shows changes. Use "units" for any distance.
  - **`src/engine_instances/README.md`:** add `ViewportPane` to the GUI classes sentence and a short paragraph like the BillboardGui one. Cover `InViewport` (contents never in Workspace or Lighting; InGame and InCore pass through), CurrentCamera as a saved `Camera?` with the cached miss, and Ambient.
  - **`src/engine_core/README.md`:** if it describes `VisualSnapshot`'s fields, add `viewports` and `ViewportScenes` in a sentence.

- [ ] **Step 4: The full run**

```bash
MSYS_NO_PATHCONV=1 "C:/Program Files/CMake/bin/cmake.exe" --build build --config Debug --target sandbox studio-tests explorer-tests properties-tests scene-render-check AnarchyStudio AnarchyPlayer --parallel
build/Debug/sandbox.exe
build/Debug/studio-tests.exe
build/Debug/explorer-tests.exe
build/Debug/properties-tests.exe
build/Debug/scene-render-check.exe
```

Also run `jadefx-tests` in the JadeFX worktree. Expected: all pass. Then build Release, the user's "rebuild": `--config Release --target AnarchyStudio AnarchyPlayer`. Open a place, insert a ScreenGui, a ViewportPane, a GameObject on a Prefab, a DirectionalLight, and a Camera, and set CurrentCamera. The pane should show the object.

To test an export, hide `../JadeFX_CPP/res/shaders` first (rename it, and put it back after) so its fallback does not mask a missing `image.frag` change.

- [ ] **Step 5: Commit**

```bash
git add tests/ViewportDemo.cpp CMakeLists.txt README.md src/engine_instances/README.md src/engine_core/README.md
git commit -m "ViewportPane: a demo window, and the docs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Self-review notes (for the executor)

- **Spec coverage, section by section:**
  - §1 class: Task 5.
  - §2 containment: Task 5.
  - §3 scope wall: Task 6, plus Task 10 for the camera list and F.
  - §4 pane scenes: Task 7.
  - §5 JadeFX: Task 1.
  - §6 renderer: Tasks 2-4.
  - §7 drawing and ViewportSize: Tasks 8-10.
  - §8 studio notes: Task 10 and the docs in Task 11.
  - Tests: every task. Build order: Tasks 1-11.
- **Names to look up while working.** Two things are named by where to find them rather than spelled out:
  - **Task 3:** the existing Skybox check's sky image helper in `SceneRenderCheck.cpp`.
  - **Task 9:** the cube mesh file and resources folder its file-backed checks use.
  - Use those. Do not invent new helpers when an existing one does the job.
- **Task 1's TX3** only proves a straight draw is no GL error. Its pixels are not compared; TX1 and TX2 carry the color checks.
