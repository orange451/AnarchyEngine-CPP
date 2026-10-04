#pragma once

#include "jadefx/jadefx.hpp"

#include <functional>
#include <memory>

namespace ide {

// The popover the status bar's zoom opens: the zoom as a percent, a slider
// from the least to the most zoom in steps of 10%, and a button back to 100%.
// The studio zooms as the slider moves, a step at each 10% the drag passes,
// so the zoom is seen while it is chosen. The drag is measured in window
// points, which the zoom does not rescale. Left and Right while it has the
// keyboard zoom a step. Escape, a click outside, or another click on the zoom
// closes it.
class ZoomPopover : public jadefx::VBox {
public:
    // zoom runs with each new zoom the slider or the button picks, such as 1.1.
    // It should take effect at the end of the frame, not at once: a frame's
    // input is converted to scene points with one zoom before any of it is handled.
    static std::shared_ptr<ZoomPopover> create(std::function<void(double zoom)> zoom);

    // Shows it over anchor, at the zoom now, with the slider taking the keyboard.
    void open(jadefx::Node& anchor);
    void dismiss();
    bool showing() const;
    // Puts it back against its anchor, which a new zoom moves.
    void follow();
    // Shows zoom without running the callback, as when a shortcut zoomed.
    void sync(double zoom);

    jadefx::Slider& slider() const;
    jadefx::Button& resetButton() const { return *reset_; }

protected:
    explicit ZoomPopover(std::function<void(double zoom)> zoom);

private:
    class ZoomSlider;

    // The slider moved, or the pointer let go of it: shows its value, and
    // zooms unless the pointer still holds it.
    void slid();
    void show_value(double zoom);

    std::weak_ptr<ZoomPopover> self_;
    std::function<void(double)> zoom_;
    std::shared_ptr<jadefx::Label> value_;
    std::shared_ptr<ZoomSlider> slider_;
    std::shared_ptr<jadefx::Button> reset_;
    jadefx::Node* anchor_ = nullptr;
    // Set while sync moves the slider, so moving it does not zoom again.
    bool syncing_ = false;
};

}  // namespace ide
