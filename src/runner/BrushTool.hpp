#pragma once

// The Scene View's brush editor (docs/superpowers/specs/2026-10-09-brush-ux-research.md).
// B turns it on and off. One 3D view, so every gesture picks its plane from
// what is under the pointer and locks it for the drag:
//
//   Drag on empty space or an unselected surface   draw a box on that plane,
//                                                  then move for its height and
//                                                  click (Alt while dragging:
//                                                  height at once; Shift: square)
//   Click a brush / Ctrl+click                     select / toggle
//   Drag a selected brush                          move it on the grid (Alt:
//                                                  vertical; Shift: one axis;
//                                                  Ctrl at the start: a copy)
//   Shift+drag a selected brush's face             push or pull that face
//                                                  (Ctrl at the start: extrude
//                                                  a new brush from it)
//   Alt+click a face / Alt+double-click            paint the current Material
//                                                  on the face / the brush
//   V  vertex mode    C  clip mode    Esc  cancel / clear / deselect
//   [ ] or - +  grid size    1-9  grid 1..256    Del  delete
//   Ctrl+D duplicate    arrows, PgUp/PgDn  nudge by the grid (Ctrl: copy)
//   Alt+arrows  turn 90 degrees    Ctrl+F  flip
//
// Each gesture is one undo step. UI thread; every read and write of the
// place goes through Engine::on_simulation.

#include "DataModel.hpp"
#include "DraggerMath.hpp"
#include "brush/BrushGeometry.hpp"

#include <jadefx/scene/layout/VBox.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {
class Engine;
class Brush;
}  // namespace engine_core

namespace jadefx {
class Button;
class Label;
class ToggleButton;
struct KeyEvent;
}  // namespace jadefx

namespace runner {

enum class BrushMode { Draw, Vertex, Clip, Loop };

struct BrushModifiers {
    bool shift = false;
    bool control = false;
    bool alt = false;
};

class BrushTool;

class BrushToolPalette : public jadefx::VBox {
public:
    explicit BrushToolPalette(BrushTool& tool);
    void refresh();

private:
    BrushTool& tool_;
    std::vector<jadefx::ToggleButton*> modes_;
    jadefx::Label* grid_ = nullptr;
    jadefx::Label* hint_ = nullptr;
    jadefx::Button* material_ = nullptr;
};

class BrushTool {
public:
    explicit BrushTool(engine_core::Engine& engine);

    std::shared_ptr<BrushToolPalette> makePalette();

    bool active() const { return active_; }
    void toggle();
    void turnOff();

    // The view the pointer is in, before each pointer call.
    void setView(const engine_core::DraggerView& view) { view_ = view; }
    void hover(const std::optional<engine_core::DraggerRay>& ray, BrushModifiers mods);
    void press(const engine_core::DraggerRay& ray, BrushModifiers mods, int clicks);
    void release(BrushModifiers mods);
    // True when the tool took the key.
    bool key(const jadefx::KeyEvent& event);

    // Colored world-space segments: x, y, z, r, g, b, a per point.
    void appendLines(std::vector<float>& out) const;
    // What to show beside the pointer while something is measured; empty for nothing.
    const std::string& readout() const { return readout_; }
    engine_core::Vec2 readoutAt() const { return readoutAt_; }

    BrushMode mode() const { return mode_; }
    void setMode(BrushMode mode);
    double grid() const { return grid_; }
    void stepGrid(int steps);
    void setGrid(double grid);
    std::string materialName() const { return materialName_; }
    void nextMaterial();
    void applyMaterialToSelection();
    // Replaces each selected brush with a shape filling its bounds.
    void reshapeSelection(int kind, int sides);
    const std::string& hint() const { return hint_; }

    void setOnUsed(std::function<void()> used) { onUsed_ = std::move(used); }
    void used() const {
        if (onUsed_) {
            onUsed_();
        }
    }

private:
    enum class Drag { None, Pending, Drawing, Height, Moving, Resizing, Vertex };

    struct Hit {
        engine_core::InstanceId instance = 0;
        bool brush = false;
        int face = -1;
        engine_core::Vec3 position{};
        engine_core::Vec3 normal{};
    };

    // Per selected brush at a drag's start.
    struct Held {
        engine_core::InstanceId id = 0;
        engine_core::Matrix4 transform;
        std::vector<engine_core::brush::Face> faces;
        int face = -1;
    };

    std::optional<Hit> pick(const engine_core::DraggerRay& ray, bool brushesOnly) const;
    std::vector<engine_core::InstanceId> selection() const;
    void select(std::vector<engine_core::InstanceId> ids) const;
    bool selected(engine_core::InstanceId id) const;

    void beginStep(const char* name);
    void endStep(bool commit = true);

    void startDrag(BrushModifiers mods);
    void updateDraw(const engine_core::DraggerRay& ray, BrushModifiers mods);
    void updateHeight(const engine_core::DraggerRay& ray, BrushModifiers mods);
    void commitDraw();
    void updateMove(const engine_core::DraggerRay& ray, BrushModifiers mods);
    void updateResize(const engine_core::DraggerRay& ray);
    void updateVertex(const engine_core::DraggerRay& ray, BrushModifiers mods);
    void captureHeld(int face);
    void paint(const Hit& hit, bool wholeBrush);
    void extrude();
    void clickClip(const engine_core::DraggerRay& ray);
    void applyClip();
    void applyLoop(const engine_core::DraggerRay& ray, BrushModifiers mods);
    void nudge(engine_core::Vec3 delta, bool copy);
    void turn(int axis, int quarterTurns);
    void flip(int axis);
    void duplicate();
    void deleteSelection();
    void refreshHover(const engine_core::DraggerRay& ray, BrushModifiers mods);
    void refreshVertices();

    double snap(double value) const;
    engine_core::Vec3 snapVec(engine_core::Vec3 p) const;
    // The world axis (0 x, 1 y, 2 z) nearest the camera's right / forward on the ground.
    int cameraAxis(bool right, int& sign) const;

    engine_core::Engine& engine_;
    engine_core::DraggerView view_;
    bool active_ = false;
    BrushMode mode_ = BrushMode::Draw;
    double grid_ = 1.0;
    double lastHeight_ = 4.0;

    std::vector<engine_core::InstanceId> materials_;
    std::size_t material_ = 0;
    std::string materialName_ = "Default";
    std::string materialGuid_;

    // Pointer state.
    std::optional<engine_core::DraggerRay> ray_;
    BrushModifiers mods_;
    std::optional<Hit> hover_;
    Drag drag_ = Drag::None;
    engine_core::Vec2 pressAt_{};
    engine_core::DraggerRay pressRay_{};
    std::optional<Hit> pressHit_;
    BrushModifiers pressMods_;
    int pressClicks_ = 0;

    // Drawing: a box on a plane through planePoint_, normal along planeAxis_.
    int planeAxis_ = 1;
    double planeSign_ = 1.0;
    engine_core::Vec3 planePoint_{};
    engine_core::Vec3 drawStart_{};
    engine_core::Vec3 drawEnd_{};
    double height_ = 4.0;

    // Moving, resizing, vertex editing.
    std::vector<Held> held_;
    engine_core::Vec3 dragOrigin_{};
    engine_core::Vec3 dragDelta_{};
    engine_core::Vec3 dragNormal_{};
    double dragAmount_ = 0.0;
    bool dragInvalid_ = false;

    // Vertex mode: the selected brushes' corners in world space.
    struct Corner {
        engine_core::InstanceId brush = 0;
        engine_core::Vec3 world{};
    };
    std::vector<Corner> corners_;
    int hoverCorner_ = -1;
    int dragCorner_ = -1;

    // Clip mode.
    std::vector<engine_core::Vec3> clipPoints_;
    engine_core::Vec3 clipNormal_{0.f, 1.f, 0.f};
    int clipSide_ = 0;  // 0 keep back, 1 keep front, 2 split

    // Lines to draw, rebuilt by every pointer event.
    std::vector<float> lines_;
    std::string readout_;
    engine_core::Vec2 readoutAt_{};
    std::string hint_;
    std::string recording_;
    std::function<void()> onUsed_;
};

}  // namespace runner
