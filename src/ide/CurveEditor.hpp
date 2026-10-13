#pragma once

#include "CurveEditModel.hpp"
#include "jadefx/jadefx.hpp"

#include <functional>
#include <optional>

namespace ide {

// A NumberCurve row's editor, open under the row in Properties: mode tabs
// along the top, the curve or curves on a graph, and the selected key's
// interpolation along the bottom. Double-click adds a key, dragging moves
// one, right-click removes one. The row's own fields type exact numbers.
class CurveEditor : public jadefx::Region {
public:
    static constexpr double kHeight = 140;

    CurveEditor();
    const char* getElementType() const override { return "curve-editor"; }

    // The curve the row holds now; ignored mid-drag, which shows its own.
    void show(const engine_core::NumberCurve& curve);
    CurveEditModel& model() { return model_; }
    const CurveEditModel& model() const { return model_; }
    // The selected key, which = -1 for none.
    int selected_which() const { return which_; }
    int selected_key() const { return key_; }
    bool dragging() const { return dragging_; }
    // The row was given model()'s curve from outside (a typed key): show treats it as sent.
    void sent() { sent_ = model_.curve(); }
    // An edit landed or was refused: the next show takes the row's value as it is.
    void settle() {
        sent_.reset();
        shown_.reset();
    }

    // After each change to model(): done false while a drag goes on, true when it ends.
    std::function<void(bool done)> on_edit;
    // The selection changed.
    std::function<void()> on_select;

    // The parts, in scene points, for hit tests and tests.
    struct Rect {
        double x = 0, y = 0, w = 0, h = 0;
        bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };
    Rect mode_tab(int mode) const;
    Rect interp_tab(int interp) const;
    Rect graph() const;
    // Graph space to scene points and back.
    double x_of(float t) const;
    double y_of(float v) const;
    float t_of(double x) const;
    float v_of(double y) const;

    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent& event) override;

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;

private:
    void select(int which, int key);
    bool keyed() const;
    // Tells the row: on_edit, after noting the curve sent (see show).
    void emit(bool done);

    CurveEditModel model_;
    int which_ = 0;
    int key_ = -1;
    // The row's value last shown, and the curve last sent. Until the row
    // shows what was sent, a row still showing the old value is stale.
    std::optional<engine_core::NumberCurve> shown_;
    std::optional<engine_core::NumberCurve> sent_;
    bool dragging_ = false;
    // The vertical range drawn, fixed while a drag goes on.
    float low_ = 0.f;
    float high_ = 1.f;
};

// A ColorCurve row's editor: mode tabs, then one bar for each color or
// gradient with its stops under it. Double-click a bar adds a stop,
// dragging moves one, right-click removes one. The row's color and
// transparency controls edit the selected stop.
class GradientEditor : public jadefx::Region {
public:
    static constexpr double kHeight = 76;

    GradientEditor();
    const char* getElementType() const override { return "gradient-editor"; }

    void show(const engine_core::ColorCurve& curve);
    GradientEditModel& model() { return model_; }
    const GradientEditModel& model() const { return model_; }
    int selected_which() const { return which_; }
    int selected_stop() const { return stop_; }
    bool dragging() const { return dragging_; }
    void sent() { sent_ = model_.curve(); }
    void settle() {
        sent_.reset();
        shown_.reset();
    }

    std::function<void(bool done)> on_edit;
    std::function<void()> on_select;

    using Rect = CurveEditor::Rect;
    Rect mode_tab(int mode) const;
    // Bar which (0 or 1), with room under it for the stops.
    Rect bar(int which) const;
    int bars() const;

    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent& event) override;

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;

private:
    void select(int which, int stop);
    void emit(bool done);

    GradientEditModel model_;
    std::optional<engine_core::ColorCurve> shown_;
    std::optional<engine_core::ColorCurve> sent_;
    int which_ = 0;
    int stop_ = 0;
    bool dragging_ = false;
};

}  // namespace ide
