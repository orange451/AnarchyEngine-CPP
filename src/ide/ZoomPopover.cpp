#include "ZoomPopover.hpp"

#include "Preferences.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ide {

namespace {

constexpr double kPopoverWidth = 240;

// The popover is a popup, outside any page, so it carries its own rules.
constexpr const char* kZoomRules = R"CSS(
.zoom-popover {
    background-color: var(--ide-popup-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--ide-popup-border-color);
    border-radius: 10px;
    box-shadow: 0 10px 28px var(--ide-popup-shadow-color);
    padding: 10px 12px;
    spacing: 8px;
}
.zoom-popover-title {
    color: var(--ide-popup-text-color);
    font-size: 13px;
}
.zoom-popover-value {
    color: var(--ide-popup-detail-text-color);
    font-size: 13px;
}
.zoom-popover-bounds {
    color: var(--ide-popup-detail-text-color);
    font-size: 11px;
}
)CSS";

std::string Percent(double zoom) { return std::to_string(static_cast<int>(std::lround(zoom * 100.0))) + "%"; }

std::shared_ptr<jadefx::Label> Text(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style_class);
    label->setMouseTransparent(true);
    return label;
}

// A row of left and right with the gap between them taking the slack.
std::shared_ptr<jadefx::HBox> Ends(std::shared_ptr<jadefx::Node> left, std::shared_ptr<jadefx::Node> right) {
    auto row = jadefx::make<jadefx::HBox>();
    row->setAlignment(jadefx::Pos::CenterLeft);
    auto gap = jadefx::make<jadefx::Pane>();
    gap->setStyle("width: 100%;");
    gap->setMouseTransparent(true);
    row->getChildren().add(std::move(left));
    row->getChildren().add(std::move(gap));
    row->getChildren().add(std::move(right));
    return row;
}

}  // namespace

// A slider whose drag is measured in window points, which a zoom does not
// change. The zoom it sets rescales the scene under the pointer and moves the
// popover with its chip, so the scene points a plain Slider measures in
// would put a pointer that has not moved somewhere else on the track, which
// sets another zoom, and so on. The press records where the track was in the
// window then, and the drag reads the pointer against that.
class ZoomPopover::ZoomSlider : public jadefx::Slider {
public:
    ZoomSlider(double min, double max, double value, std::function<void()> released)
        : jadefx::Slider(min, max, value), released_(std::move(released)) {}

    bool held() const { return held_; }

protected:
    void handleMousePressed(const jadefx::MouseEvent& event) override {
        // The press itself: the thumb is taken, or the track jumps to the pointer.
        jadefx::Slider::handleMousePressed(event);
        if (getMax() <= getMin()) {
            return;
        }
        held_ = true;
        // The thumb's center travels the content less one thumb, as Slider lays it out.
        constexpr double kThumb = 16;
        const double zoom = jadefx::Stage::getZoom();
        press_window_x_ = event.x * zoom;
        travel_window_ = std::max(1.0, (contentWidth() - kThumb) * zoom);
        press_fraction_ = (getValue() - getMin()) / (getMax() - getMin());
    }
    void handleMouseDragged(const jadefx::MouseEvent& event) override {
        if (!held_) {
            jadefx::Slider::handleMouseDragged(event);
            return;
        }
        // Events reach the scene divided by the zoom they were read at, which
        // holds for a whole frame of them: the popover's zoom lands at the frame's end.
        const double moved = (event.x * jadefx::Stage::getZoom() - press_window_x_) / travel_window_;
        const double fraction = std::clamp(press_fraction_ + moved, 0.0, 1.0);
        adjustValue(getMin() + fraction * (getMax() - getMin()));
    }
    void handleMouseReleased(const jadefx::MouseEvent& event) override {
        jadefx::Slider::handleMouseReleased(event);
        held_ = false;
        if (released_) {
            released_();
        }
    }

private:
    std::function<void()> released_;
    bool held_ = false;
    double press_window_x_ = 0;
    double travel_window_ = 1;
    double press_fraction_ = 0;
};

std::shared_ptr<ZoomPopover> ZoomPopover::create(std::function<void(double zoom)> zoom) {
    std::shared_ptr<ZoomPopover> popover(new ZoomPopover(std::move(zoom)));
    popover->self_ = popover;
    return popover;
}

ZoomPopover::ZoomPopover(std::function<void(double zoom)> zoom) : zoom_(std::move(zoom)) {
    getClassList().add("zoom-popover");
    setElementId("zoom-popover");
    setStylesheet(kZoomRules);
    setPrefWidth(kPopoverWidth);

    value_ = Text("100%", "zoom-popover-value");
    value_->setElementId("zoom-popover-value");
    getChildren().add(Ends(Text("Zoom", "zoom-popover-title"), value_));

    // Majors every 50%, with a minor at each 10% between, which is what it snaps to.
    slider_ = jadefx::make<ZoomSlider>(Preferences::kMinZoom, Preferences::kMaxZoom, 1.0, [this] { slid(); });
    slider_->setElementId("zoom-slider");
    slider_->setMajorTickUnit(0.5);
    slider_->setMinorTickCount(4);
    slider_->setShowTickMarks(true);
    slider_->setSnapToTicks(true);
    slider_->setBlockIncrement(0.1);
    slider_->setStyle("width: 100%;");
    slider_->setOnValueChanged([this] { slid(); });
    getChildren().add(slider_);

    reset_ = jadefx::make<jadefx::Button>("Reset to 100%");
    reset_->setElementId("zoom-reset");
    reset_->setOnAction([this](jadefx::ActionEvent&) {
        sync(1.0);
        if (zoom_) {
            zoom_(1.0);
        }
    });
    getChildren().add(Ends(Text(Percent(Preferences::kMinZoom) + " to " + Percent(Preferences::kMaxZoom),
                                "zoom-popover-bounds"),
                           reset_));
}

void ZoomPopover::open(jadefx::Node& anchor) {
    jadefx::Scene* scene = anchor.getScene();
    if (scene == nullptr) {
        return;
    }
    anchor_ = &anchor;
    sync(jadefx::Stage::getZoom());
    jadefx::PopupOptions options;
    options.autoHide = true;
    // A press on the zoom itself is left to it, so a second click closes this.
    options.owner = &anchor;
    scene->showPopupNear(self_.lock(), &anchor, jadefx::Side::Top, options);
    slider_->requestFocus();
}

void ZoomPopover::dismiss() {
    jadefx::Scene* scene = getScene();
    if (scene != nullptr && !scene->isTearingDown() && scene->isPopupShowing(this)) {
        scene->hidePopup(this);
    }
}

bool ZoomPopover::showing() const {
    const jadefx::Scene* scene = getScene();
    return scene != nullptr && !scene->isTearingDown() && scene->isPopupShowing(this);
}

void ZoomPopover::follow() {
    if (!showing() || anchor_ == nullptr || anchor_->getScene() == nullptr) {
        return;
    }
    jadefx::PopupOptions options;
    options.autoHide = true;
    options.owner = anchor_;
    // On a popup already showing, this only moves it.
    anchor_->getScene()->showPopupNear(self_.lock(), anchor_, jadefx::Side::Top, options);
}

jadefx::Slider& ZoomPopover::slider() const { return *slider_; }

void ZoomPopover::slid() {
    // The drag snaps to 10% steps as it goes, so each step it passes is a zoom.
    const double zoom = std::round(slider_->getValue() * 10.0) / 10.0;
    show_value(zoom);
    if (!syncing_ && zoom_) {
        zoom_(zoom);
    }
}

void ZoomPopover::sync(double zoom) {
    if (slider_->held()) {
        // The pointer has the slider; what it shows is where it would go.
        return;
    }
    syncing_ = true;
    slider_->setValue(zoom);
    syncing_ = false;
    show_value(zoom);
}

void ZoomPopover::show_value(double zoom) {
    const std::string text = Percent(zoom);
    if (value_->getText() != text) {
        value_->setText(text);
    }
}

}  // namespace ide
