#include "PropertiesPanel.hpp"
#include "AssetPicker.hpp"
#include "ChangeFlag.hpp"
#include "IdeIcons.hpp"
#include "IdeResources.hpp"
#include "LockWaits.hpp"
#include "MaterialBall.hpp"
#include "MaterialPreviews.hpp"
#include "ThumbnailLoader.hpp"

#include "AssetInstances.hpp"

#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Enum.hpp"
#include "PropertyReflection.hpp"
#include "SelectionService.hpp"
#include "SoundEmitter.hpp"
#include "SoundPreview.hpp"
#include "TextUndoStack.hpp"

#include "jadefx/scene/Painter.hpp"
#include "jadefx/scene/controls/ComboBox.hpp"
#include "jadefx/scene/controls/ScrollPane.hpp"
#include "jadefx/scene/image/ImageView.hpp"
#include "jadefx/scene/layout/Pane.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

namespace ide {
namespace {

using engine_core::InstanceId;

// Same wait as the explorer: a busy simulation step must not freeze the shell.

constexpr double kPad = 6;
constexpr double kRowHeight = 24;
constexpr double kRowGap = 2;
constexpr double kIndent = 10;
constexpr double kAxisGap = 4;
constexpr double kClearWidth = 24;
// The widest a slider row's field gets. The track takes the rest.
constexpr double kSliderFieldWidth = 72;
// The fold arrow before a Transform's name sits in the name's indent.
constexpr double kDisclosureWidth = 12;
// The widest side of a Texture's or Material's preview, in points.
constexpr double kPreviewSide = 192;
// Its image's longer side, in pixels: the preview at twice its points, for HiDPI.
constexpr int kPreviewPixels = 384;
// A Material's ball is drawn this big, then shrunk to kPreviewPixels.
constexpr int kBallRenderSize = 768;
// A Sound's transport: its buttons, in order, each with its toolbar icon.
constexpr int kSoundButtons = 4;
enum SoundButton { kSoundPlay, kSoundPause, kSoundResume, kSoundStop };
constexpr const char* kSoundButtonText[kSoundButtons] = {"Play", "Pause", "Resume", "Stop"};
constexpr const char* kSoundButtonIcon[kSoundButtons] = {"Play.png", "Pause.png", "Resume.png", "Stop.png"};
constexpr double kSoundButtonWidth = 84;
// The time beside the track, as "1:05 / 2:30".
constexpr double kSoundTimeWidth = 84;
// The categories, each under a header that folds it: a row's PropertyGroup,
// then the Preview section.
constexpr int kGroups = 3;
constexpr int kPreviewGroup = 2;
constexpr const char* kGroupTitles[kGroups] = {"Instance", "Data", "Preview"};
// A Transform's two lines, under its name when it is open.
constexpr const char* kTransformLines[2] = {"Position", "Orientation"};

constexpr const char* kFieldStyle =
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-field-border-color); background-color: var(--ide-field-color);";
constexpr const char* kReadOnlyStyle =
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-properties-readonly-border-color); "
    "background-color: var(--ide-properties-readonly-color); color: var(--ide-muted-text-color);";
// Position's X, Y, and Z fields are tinted red, green, and blue, and so are a
// Transform's Position and Orientation.
constexpr const char* kAxisStyles[3] = {
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-field-border-color); background-color: var(--ide-properties-x-color);",
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-field-border-color); background-color: var(--ide-properties-y-color);",
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-field-border-color); background-color: var(--ide-properties-z-color);",
};
constexpr const char* kButtonStyle = "padding: 0 4px; border-radius: 3px;";
// The line under the rows: a refused edit, or what to click while picking a reference.
constexpr const char* kErrorStyle = "color: var(--ide-properties-error-color);";
constexpr const char* kHintStyle = "color: var(--ide-properties-hint-color);";
// A reference's value is a button that reads like a field. It turns blue while it waits for a pick.
constexpr const char* kPickingStyle =
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-properties-picking-border-color); background-color: var(--ide-properties-picking-color);";

// A value field. dirty is set by typing and cleared by show, a commit, or undo
// back to where the field started, so a blank mixed field that was only
// focused commits nothing.
//
// Focus selects the whole value, so typing replaces it. A click that focuses
// the field selects it when the button comes up, unless the press dragged out a
// selection of its own. Focus coming back with the window keeps the caret.
class PropertyField : public jadefx::TextField {
public:
    PropertyField() {
        getClassList().add("properties-field");
        setPrefColumnCount(1);
        setStyle(kFieldStyle);
    }

    std::function<void()> on_cancel;
    // Tab, or Shift+Tab when back is true.
    std::function<void(bool back)> on_tab;
    TextUndoStack stack;
    bool dirty = false;
    bool was_focused = false;
    // The style while the field is editable.
    const char* editable_style = kFieldStyle;

    // Shows a value from the world. Typing so far is dropped. A field selected
    // whole stays selected whole.
    void show(const std::string& text) {
        mute_ = true;
        if (getText() != text) {
            const bool whole = isFocused() && selected_whole();
            setText(text);
            if (whole) {
                selectAll();
            }
        }
        mute_ = false;
        stack.reset(text);
        dirty = false;
    }

    void set_read_only(bool read_only) {
        setEditable(!read_only);
        setStyle(read_only ? kReadOnlyStyle : editable_style);
    }

    // The focused field's own undo. False when its stack has nothing that way.
    bool undo_step(bool redo) {
        if (!isEditable() || !(redo ? stack.can_redo() : stack.can_undo())) {
            return false;
        }
        if (redo) {
            stack.redo();
        } else {
            stack.undo();
        }
        mute_ = true;
        setText(stack.text());
        positionCaret(stack.caret());
        mute_ = false;
        dirty = stack.can_undo();
        return true;
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape && isEditable()) {
            event.consume();
            if (on_cancel) {
                on_cancel();
            }
            return;
        }
        if (event.pressed && event.key == jadefx::Key::Tab && !event.shortcut() && !event.alt && on_tab) {
            event.consume();
            on_tab(event.shift);
            return;
        }
        jadefx::TextField::handleKey(event);
        note();
    }

    void handleText(jadefx::TextEvent& event) override {
        jadefx::TextField::handleText(event);
        note();
    }

    void handleFocusGained() override {
        jadefx::TextField::handleFocusGained();
        if (window_away_) {
            window_away_ = false;
            return;
        }
        selectAll();
        // The scene marks the pressed node before it moves the focus.
        select_on_release_ = isPressed();
    }

    void handleFocusLost() override {
        jadefx::Scene* scene = getScene();
        window_away_ = scene != nullptr && !scene->isWindowFocused();
        select_on_release_ = false;
        jadefx::TextField::handleFocusLost();
    }

    // The press that focused the field put the caret under the pointer.
    void handleMouseReleased(const jadefx::MouseEvent& event) override {
        jadefx::TextField::handleMouseReleased(event);
        if (select_on_release_ && event.stillSincePress) {
            selectAll();
        }
        select_on_release_ = false;
    }

private:
    void note() {
        if (mute_ || getText() == stack.text()) {
            return;
        }
        stack.record_text(getText());
        dirty = true;
    }

    bool selected_whole() const {
        return std::min(getAnchor(), getCaretPosition()) == 0 && std::max(getAnchor(), getCaretPosition()) == getLength();
    }

    bool mute_ = false;
    bool window_away_ = false;
    bool select_on_release_ = false;
};

// A Color3 value. on_live runs at each change while the chooser is open, so
// the instances follow the drag. on_done runs once when it closes after any
// change: with true on a pick, so the drag is one undo step, and with false
// when Escape closed it or it closed on the color it opened with, so the
// instances go back. A mixed row draws no color, as a mixed field shows no
// text, and any pick in it counts, even of the color it keeps out of sight.
class PropertyColor : public jadefx::ColorPicker {
public:
    PropertyColor() {
        getClassList().add("properties-color");
        getColorChooser().setShowAlpha(false);
        // While the chooser is open, only the chooser changes the value.
        setOnValueChanged([this] {
            if (!isShowing()) {
                return;
            }
            touched_ = true;
            if (on_live) {
                on_live();
            }
        });
    }

    bool mixed = false;
    std::function<void()> on_live;
    std::function<void(bool)> on_done;

protected:
    void popupShowing() override {
        if (!isShowing()) {
            before_ = getValue().toHex();
            touched_ = false;
            escaped_ = false;
        }
        jadefx::ColorPicker::popupShowing();
    }

    bool handlePopupKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape) {
            escaped_ = true;
        }
        return jadefx::ColorPicker::handlePopupKey(event);
    }

    void popupHidden() override {
        jadefx::ColorPicker::popupHidden();
        const bool touched = touched_;
        const bool picked = touched_ && !escaped_ && (mixed || getValue().toHex() != before_);
        touched_ = false;
        if (touched && on_done) {
            on_done(picked);
        }
    }

    void renderValue(jadefx::UiRenderer& renderer, float opacity, float x, float y, float width,
                     float height) override {
        if (!mixed || isShowing()) {
            jadefx::ColorPicker::renderValue(renderer, opacity, x, y, width, height);
        }
    }

private:
    std::string before_;
    bool touched_ = false;
    bool escaped_ = false;
};

// The slider of a Number row with a range. on_move runs as the user moves it,
// so the row's field and, while the button is held, the instances follow.
// on_done runs when the button comes up after a press: true when it moved, so
// the drag is one undo step, and false when it came back to where it began.
// on_commit runs at each key that moves it. A value from the world, shown
// through show, is none of these. The thumb stops at the ends; the field
// beside it does not.
class PropertySlider : public jadefx::Slider {
public:
    PropertySlider(double min, double max) : jadefx::Slider(min, max, min) {
        getClassList().add("properties-slider");
        setBlockIncrement((max - min) / 20);
        setOnValueChanged([this] {
            if (!mute_ && on_move) {
                on_move();
            }
        });
    }

    std::function<void()> on_move;
    std::function<void(bool)> on_done;
    std::function<void()> on_commit;

    // Shows a value from the world, clamped to the track.
    void show(double value) {
        mute_ = true;
        setValue(value);
        mute_ = false;
    }

    // Pressed, and not yet let go. The world does not move the thumb meanwhile.
    bool held() const { return held_; }

protected:
    void handleMousePressed(const jadefx::MouseEvent& event) override {
        held_ = !isDisabled();
        start_ = getValue();
        jadefx::Slider::handleMousePressed(event);
    }

    void handleMouseReleased(const jadefx::MouseEvent& event) override {
        jadefx::Slider::handleMouseReleased(event);
        if (held_) {
            held_ = false;
            if (on_done) {
                on_done(getValue() != start_);
            }
        }
    }

    void handleKey(jadefx::KeyEvent& event) override {
        const double before = getValue();
        jadefx::Slider::handleKey(event);
        if (!held_) {
            finish(before);
        }
    }

private:
    void finish(double before) {
        if (getValue() != before && on_commit) {
            on_commit();
        }
    }

    bool mute_ = false;
    bool held_ = false;
    double start_ = 0;
};

// A slider's value rounded to a step that suits its range: two digits below
// the span's own, so 0..1 moves by hundredths and 1..120 by whole degrees.
double slider_round(double value, double min, double max) {
    const double span = max - min;
    if (!(span > 0) || !std::isfinite(value)) {
        return value;
    }
    const int digits = 2 - static_cast<int>(std::floor(std::log10(span)));
    if (digits <= 0) {
        const double step = std::pow(10.0, -digits);
        return std::round(value / step) * step;
    }
    // Divided by a whole power of ten, so 0.35 reads back as 0.35.
    const double scale = std::pow(10.0, digits);
    return std::round(value * scale) / scale;
}

// The arrow that folds a Transform row: it points right while the row is
// folded and down while it is open, as the explorer's does.
class PropertyDisclosure : public jadefx::Region {
public:
    PropertyDisclosure() {
        getClassList().add("properties-disclosure");
        setDefaultCursor(jadefx::Cursor::Pointer);
        setStyle("color: var(--ide-muted-text-color);");
    }

    const char* getElementType() const override { return "properties-disclosure"; }

    bool open = true;

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override {
        jadefx::Color color = computedStyle().color;
        color.a *= opacity;
        if (color.a <= 0.f || getWidth() <= 1 || getHeight() <= 1) {
            return;
        }
        jadefx::Painter painter(renderer);
        // One device pixel per slice, so the slanted edges stay smooth.
        const float step = 1.f / std::max(1.f, painter.pixelsPerPoint());
        const float cx = static_cast<float>(getAbsoluteX() + getWidth() * 0.5);
        const float cy = static_cast<float>(getAbsoluteY() + getHeight() * 0.5);
        constexpr float kLong = 8.f;
        constexpr float kShort = 5.f;
        if (open) {
            const float top = std::round(cy - kShort * 0.5f);
            for (float t = 0.f; t < kShort; t += step) {
                const float width = kLong * (1.f - (t + step * 0.5f) / kShort);
                painter.fillRect(cx - width * 0.5f, top + t, width, step, color);
            }
        } else {
            const float left = std::round(cx - kShort * 0.5f);
            const float top = std::round(cy - kLong * 0.5f);
            for (float t = 0.f; t < kLong; t += step) {
                const float width = kShort * (1.f - std::fabs(t + step * 0.5f - kLong * 0.5f) / (kLong * 0.5f));
                painter.fillRect(left, top + t, width, step, color);
            }
        }
    }
};

// A write handed to the simulation thread. done is set after result.
struct PendingEdit {
    EditResult result;
    std::atomic<bool> done{false};
};

}  // namespace

// The docked page: a ScrollPane over the rows. Its layout pass is also the
// moment the panel reads the world.
class PropertiesPane : public IdePane {
public:
    PropertiesPane() : IdePane("Properties", true) {
        setIconFile("Properties.png");
        setPrefWidth(9999999);
        setMinSize(150, 80);
        setStyle("background-color: var(--ide-panel-color);");
        getClassList().add("properties-pane");
    }

    std::weak_ptr<PropertiesPanel::Impl> owner;

    // The padded box the scroll pane fills, in this pane's coordinates.
    double inner_left() const { return contentLeft(); }
    double inner_top() const { return contentTop(); }
    double inner_width() const { return contentWidth(); }
    double inner_height() const { return contentHeight(); }

protected:
    void layoutChildren() override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;
};

// What the scroll pane scrolls: every row's widgets, placed by hand in its
// layout pass. It is as tall as the rows' last layout made it.
class PropertiesBody : public jadefx::Pane {
public:
    PropertiesBody() { getClassList().add("properties-body"); }

    std::weak_ptr<PropertiesPanel::Impl> owner;

protected:
    void layoutChildren() override;
    double preferredContentWidth(double) const override { return 0; }
    double preferredContentHeight(double) const override;
};

// What the Preview section under the rows is for: a single selected Texture,
// Material, Sound, or SoundEmitter. A SoundEmitter's is its Sound's, played
// at its Volume and Pitch, looping when it is Looped.
struct AssetPreview {
    enum class Kind { None, Texture, Material, Sound };
    Kind kind = Kind::None;
    InstanceId id = 0;
    // A Texture's or Sound's file; empty when its Path is, or a SoundEmitter has no Sound.
    std::filesystem::path file;
    MaterialLook look;
    double volume = 1;
    double pitch = 1;
    bool looped = false;

    bool operator==(const AssetPreview& other) const {
        return kind == other.kind && id == other.id && file == other.file && look == other.look &&
               volume == other.volume && pitch == other.pitch && looped == other.looped;
    }
    bool operator!=(const AssetPreview& other) const { return !(*this == other); }
};

// One row's widgets. Which editors exist depends on the row's kind.
struct RowView {
    PropertyRow row;
    // The instances a commit from this row writes: the selection it was shown for.
    std::vector<InstanceId> ids;
    std::shared_ptr<jadefx::Label> name;
    std::shared_ptr<PropertyField> field;
    // A Vector3's X, Y, and Z. A Transform's Position X, Y, Z, then Orientation's.
    std::shared_ptr<PropertyField> axes[kTransformParts];
    std::shared_ptr<jadefx::CheckBox> check;
    std::shared_ptr<PropertyColor> color;
    // A Number row with a range, beside its field.
    std::shared_ptr<PropertySlider> slider;
    // An Enum row's items, in value order.
    std::shared_ptr<jadefx::ComboBox> choice;
    std::shared_ptr<jadefx::Button> pick;
    std::shared_ptr<jadefx::Button> clear;
    std::shared_ptr<jadefx::Tooltip> tip;
    // A reference to an asset, such as "Mesh": its Name opens the asset picker.
    // Empty for any other reference, which picks from the selection.
    std::string asset_class;
    // A Transform's fold arrow, and the names of its Position and Orientation lines.
    std::shared_ptr<PropertyDisclosure> disclosure;
    std::shared_ptr<jadefx::Label> lines[2];
    bool tip_installed = false;
    // Where the last layout put the row in body, which scrolls.
    double top = 0;

    std::vector<jadefx::Node*> nodes() const {
        std::vector<jadefx::Node*> out;
        for (jadefx::Node* node : {static_cast<jadefx::Node*>(name.get()), static_cast<jadefx::Node*>(field.get()),
                                   static_cast<jadefx::Node*>(check.get()), static_cast<jadefx::Node*>(color.get()),
                                   static_cast<jadefx::Node*>(slider.get()), static_cast<jadefx::Node*>(pick.get()), static_cast<jadefx::Node*>(clear.get()),
                                   static_cast<jadefx::Node*>(choice.get()),
                                   static_cast<jadefx::Node*>(disclosure.get()),
                                   static_cast<jadefx::Node*>(lines[0].get()),
                                   static_cast<jadefx::Node*>(lines[1].get())}) {
            if (node != nullptr) {
                out.push_back(node);
            }
        }
        for (const auto& axis : axes) {
            if (axis) {
                out.push_back(axis.get());
            }
        }
        return out;
    }

    std::vector<PropertyField*> fields() const {
        std::vector<PropertyField*> out;
        if (field) {
            out.push_back(field.get());
        }
        for (const auto& axis : axes) {
            if (axis) {
                out.push_back(axis.get());
            }
        }
        return out;
    }
};

struct PropertiesPanel::Impl : std::enable_shared_from_this<PropertiesPanel::Impl> {
    engine_core::DataModel* world = nullptr;
    engine_core::SelectionService* selection = nullptr;
    engine_core::ChangeHistoryService* history = nullptr;
    PropertiesRun run;

    std::shared_ptr<PropertiesPane> pane;
    std::shared_ptr<jadefx::Label> empty;
    // Each category's header, Instance, Data, and Preview, and the arrow that
    // folds it. A category is open until folded, and stays folded across
    // selections for the session.
    std::shared_ptr<jadefx::Label> headers[kGroups];
    std::shared_ptr<PropertyDisclosure> header_arrows[kGroups];
    bool group_folded[kGroups] = {};
    std::shared_ptr<jadefx::Label> status_label;
    std::vector<std::shared_ptr<RowView>> rows;
    PropertySheet sheet;
    // The drag that shows on the instances as it goes, or null. Only the
    // writes posted to the simulation thread read or change what it holds;
    // live_ids and live_edit, its instances and its last value, are this thread's.
    std::shared_ptr<LiveEdit> live;
    std::vector<InstanceId> live_ids;
    PropertyEdit live_edit;
    bool force = true;
    bool polling = false;
    // Set by the world when a property of a shown instance changes. The sheet
    // is read again only then, or when the selection or the tree moved.
    ChangeFlag changed;
    std::uint64_t watch = 0;
    std::vector<InstanceId> watched;
    std::uint64_t seen_selection = ~std::uint64_t{0};
    std::uint64_t seen_tree = ~std::uint64_t{0};
    std::string status;
    std::vector<std::shared_ptr<PendingEdit>> pending;

    bool picking = false;
    std::string pick_name;
    std::vector<InstanceId> pick_ids;
    std::uint64_t pick_seen = 0;

    // The popover an asset reference opens, and the row it is open for.
    std::shared_ptr<AssetPicker> asset_picker;
    std::weak_ptr<RowView> asset_pick_view;

    // Transform rows folded by name. A row is open until folded, and stays
    // folded across selections for the session.
    std::set<std::string> folded;

    // The page holds scroller, and scroller body, which holds every widget.
    std::shared_ptr<jadefx::ScrollPane> scroller;
    std::shared_ptr<PropertiesBody> body;
    // How tall the rows came out in the last layout.
    double content = 0;

    // The Preview section: a Texture's image or a Material's ball in a frame,
    // or a Sound's transport: Play, Pause, Resume, and Stop, then a track to
    // seek on with the time beside it. The ball is drawn in the pane's paint,
    // where GL is current; the texture loads on the loader's thread.
    AssetPreview asset_preview;
    std::shared_ptr<jadefx::Label> preview_header;
    std::shared_ptr<jadefx::StackPane> preview_frame;
    std::shared_ptr<jadefx::ImageView> preview_image;
    std::shared_ptr<jadefx::Label> preview_note;
    std::shared_ptr<jadefx::Button> sound_buttons[kSoundButtons];
    std::shared_ptr<PropertySlider> sound_track;
    std::shared_ptr<jadefx::Label> sound_time;
    // The shown Sound's length, read when it is selected, so the track and
    // time read right before it plays.
    double sound_length = 0;
    std::unique_ptr<ThumbnailLoader> thumbnails;
    std::unique_ptr<MaterialBall> ball;
    std::unique_ptr<MaterialPreviews> balls;
    // Opened on the first Play, so a studio that never plays one opens no device.
    std::unique_ptr<engine_core::SoundPreview> sound;

    void build() {
        pane = jadefx::make<PropertiesPane>();
        body = jadefx::make<PropertiesBody>();
        scroller = jadefx::make<jadefx::ScrollPane>(body);
        scroller->getClassList().add("properties-scroll");
        scroller->setFitToWidth(true);
        scroller->setHbarPolicy(jadefx::ScrollBarPolicy::Never);
        pane->getChildren().add(scroller);
        empty = jadefx::make<jadefx::Label>("No selection");
        empty->getClassList().add("properties-empty");
        empty->setStyle("color: var(--ide-muted-text-color);");
        body->getChildren().add(empty);
        std::weak_ptr<Impl> weak_self = weak_from_this();
        for (int index = 0; index < kGroups; ++index) {
            // The title and its arrow both fold it.
            auto fold = [weak_self, index](const jadefx::MouseEvent&) {
                if (const auto self = weak_self.lock()) {
                    self->toggle_group(index);
                }
            };
            headers[index] = jadefx::make<jadefx::Label>(kGroupTitles[index]);
            headers[index]->getClassList().add("properties-group");
            headers[index]->setStyle(
                "padding: 0 6px 0 20px; background-color: var(--ide-properties-group-color); "
                "color: var(--ide-properties-group-text-color);");
            headers[index]->setCursor(jadefx::Cursor::Pointer);
            headers[index]->setOnMouseClicked(fold);
            headers[index]->setVisible(false);
            body->getChildren().add(headers[index]);
            // After the title, so it paints over the title's background.
            header_arrows[index] = jadefx::make<PropertyDisclosure>();
            header_arrows[index]->setStyle("color: var(--ide-properties-group-text-color);");
            header_arrows[index]->setOnMouseClicked(fold);
            header_arrows[index]->setVisible(false);
            body->getChildren().add(header_arrows[index]);
        }
        preview_header = headers[kPreviewGroup];
        status_label = jadefx::make<jadefx::Label>("");
        status_label->getClassList().add("properties-status");
        status_label->setStyle(kErrorStyle);
        status_label->setVisible(false);
        body->getChildren().add(status_label);
        build_preview();
    }

    void build_preview() {
        // Added before the image, so it paints behind it.
        preview_frame = jadefx::make<jadefx::StackPane>();
        preview_frame->getClassList().add("properties-preview-frame");
        preview_frame->setStyle(
            "border-width: 1px; border-style: solid; border-radius: 3px; "
            "border-color: var(--ide-field-border-color); background-color: var(--ide-field-color);");
        preview_frame->setMouseTransparent(true);
        preview_frame->setVisible(false);
        body->getChildren().add(preview_frame);
        preview_image = jadefx::make<jadefx::ImageView>();
        preview_image->getClassList().add("properties-preview-image");
        preview_image->setMouseTransparent(true);
        preview_image->setVisible(false);
        body->getChildren().add(preview_image);
        preview_note = jadefx::make<jadefx::Label>("");
        preview_note->getClassList().add("properties-preview-note");
        preview_note->setStyle("color: var(--ide-muted-text-color);");
        preview_note->setAlignment(jadefx::Pos::Center);
        preview_note->setMouseTransparent(true);
        preview_note->setVisible(false);
        body->getChildren().add(preview_note);
        std::weak_ptr<Impl> weak_self = weak_from_this();
        for (int part = 0; part < kSoundButtons; ++part) {
            auto button = jadefx::make<jadefx::Button>(kSoundButtonText[part]);
            button->getClassList().add("properties-sound-button");
            button->setStyle(kButtonStyle);
            button->setVisible(false);
            if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(kSoundButtonIcon[part])) {
                button->setGraphic(std::move(icon));
                button->setGraphicTextGap(4);
            }
            button->setOnAction([weak_self, part](jadefx::ActionEvent&) {
                if (const auto self = weak_self.lock()) {
                    self->sound_action(static_cast<SoundButton>(part));
                }
            });
            body->getChildren().add(button);
            sound_buttons[part] = std::move(button);
        }
        // Dragging the thumb seeks as it goes; an arrow key seeks a step.
        sound_track = jadefx::make<PropertySlider>(0.0, 1.0);
        sound_track->getClassList().add("properties-sound-track");
        sound_track->setVisible(false);
        sound_track->on_move = [weak_self] {
            const auto self = weak_self.lock();
            if (self && self->sound_track->held()) {
                self->seek_sound();
            }
        };
        sound_track->on_commit = [weak_self] {
            if (const auto self = weak_self.lock()) {
                self->seek_sound();
            }
        };
        body->getChildren().add(sound_track);
        sound_time = jadefx::make<jadefx::Label>("");
        sound_time->getClassList().add("properties-sound-time");
        sound_time->setStyle("color: var(--ide-muted-text-color);");
        sound_time->setAlignment(jadefx::Pos::CenterRight);
        sound_time->setVisible(false);
        body->getChildren().add(sound_time);

        thumbnails = std::make_unique<ThumbnailLoader>(kPreviewPixels, std::function<void()>());
        ball = std::make_unique<MaterialBall>(kBallRenderSize, kPreviewPixels);
        balls = std::make_unique<MaterialPreviews>(
            [this](const MaterialLook& look, std::shared_ptr<jadefx::Image>& image) {
                runner::ViewPixels pixels;
                if (!ball->draw(look, pixels)) {
                    return false;
                }
                if (!pixels.empty()) {
                    image = jadefx::Image::fromRgba(pixels.width, pixels.height, std::move(pixels.rgba));
                }
                return true;
            },
            1);
    }

    // ---- Preview ---------------------------------------------------------

    // The preview for ids. Callers hold the world's read lock.
    AssetPreview read_preview(const std::vector<InstanceId>& ids) const {
        AssetPreview next;
        if (ids.size() != 1) {
            return next;
        }
        const engine_core::DataModel* object = world->instance(ids.front());
        auto file_of = [this](const std::string& path) {
            const std::filesystem::path root = world->resources_root();
            return path.empty() || root.empty() ? std::filesystem::path() : root / path_from_utf8(path);
        };
        if (const auto* texture = dynamic_cast<const engine_core::Texture*>(object)) {
            next.kind = AssetPreview::Kind::Texture;
            next.file = file_of(texture->path());
        } else if (const auto* sound_asset = dynamic_cast<const engine_core::Sound*>(object)) {
            next.kind = AssetPreview::Kind::Sound;
            next.file = file_of(sound_asset->path());
        } else if (const auto* emitter = dynamic_cast<const engine_core::SoundEmitter*>(object)) {
            next.kind = AssetPreview::Kind::Sound;
            const auto* played = dynamic_cast<const engine_core::Sound*>(world->instance(emitter->sound_id()));
            next.file = played != nullptr ? file_of(played->path()) : std::filesystem::path();
            next.volume = emitter->volume();
            next.pitch = emitter->pitch();
            next.looped = emitter->looped();
        } else if (const std::optional<MaterialLook> look = material_look(*world, ids.front())) {
            next.kind = AssetPreview::Kind::Material;
            next.look = *look;
        } else {
            return next;
        }
        next.id = ids.front();
        return next;
    }

    void set_preview(AssetPreview next) {
        if (next == asset_preview) {
            return;
        }
        // A sound plays only while its Sound is the one shown.
        if (sound && (next.kind != AssetPreview::Kind::Sound || next.file != sound->file())) {
            sound->stop();
        }
        const bool new_sound = next.kind == AssetPreview::Kind::Sound &&
                               (asset_preview.kind != AssetPreview::Kind::Sound || next.file != asset_preview.file);
        asset_preview = std::move(next);
        // A SoundEmitter's Volume, Pitch, and Looped are heard at once, as they are edited.
        if (sound) {
            sound->set_mix(asset_preview.volume, asset_preview.pitch, asset_preview.looped);
        }
        if (asset_preview.kind != AssetPreview::Kind::Sound) {
            sound_length = 0;
        } else if (new_sound) {
            sound_length = asset_preview.file.empty() ? 0.0 : engine_core::SoundPreview::length_of(asset_preview.file);
        }
        if (asset_preview.kind == AssetPreview::Kind::Texture && !asset_preview.file.empty()) {
            thumbnails->retain({asset_preview.file});
        } else {
            thumbnails->retain({});
        }
        balls->retain(asset_preview.kind == AssetPreview::Kind::Material
                          ? std::vector<InstanceId>{asset_preview.id}
                          : std::vector<InstanceId>{});
    }

    engine_core::SoundPreview::State sound_state() const {
        return sound ? sound->state() : engine_core::SoundPreview::State::Stopped;
    }

    void sound_action(SoundButton action) {
        if (asset_preview.kind != AssetPreview::Kind::Sound) {
            return;
        }
        switch (action) {
        case kSoundPlay: {
            if (asset_preview.file.empty()) {
                status = "This Sound has no Path to play";
                return;
            }
            if (!sound) {
                sound = std::make_unique<engine_core::SoundPreview>();
                sound->set_mix(asset_preview.volume, asset_preview.pitch, asset_preview.looped);
            }
            std::string error;
            if (!sound->play(asset_preview.file, error)) {
                status = "Could not play " + utf8_path(asset_preview.file) + " (" + error + ")";
                return;
            }
            status.clear();
            if (sound->length() > 0) {
                sound_length = sound->length();
            }
            break;
        }
        case kSoundPause:
            if (sound) {
                sound->pause();
            }
            break;
        case kSoundResume:
            if (sound) {
                sound->resume();
            }
            break;
        case kSoundStop:
            if (sound) {
                sound->stop();
            }
            break;
        }
    }

    void seek_sound() {
        if (sound && sound_state() != engine_core::SoundPreview::State::Stopped) {
            sound->seek(sound_track->getValue());
        }
    }

    // Seconds as "m:ss".
    static std::string clock_text(double seconds) {
        const long whole = std::lround(std::max(0.0, std::floor(seconds)));
        char text[32];
        std::snprintf(text, sizeof text, "%ld:%02ld", whole / 60, whole % 60);
        return text;
    }

    // Draws a queued Material ball. GL is current: the pane is painting.
    void draw_previews() {
        if (balls && !balls->idle() && world != nullptr) {
            ball->setRoot(world->resources_root());
            balls->draw_pending();
        }
    }

    // ---- Reading ---------------------------------------------------------

    void poll() {
        if (world == nullptr || selection == nullptr || polling) {
            return;
        }
        polling = true;
        collect_pending();
        check_focus();
        if (picking) {
            check_pick();
        }
        read();
        polling = false;
    }

    void read() {
        const std::uint64_t tree = world->tree_revision();
        const bool moved = selection->revision() != seen_selection || tree != seen_tree;
        std::uint64_t revision = 0;
        const std::vector<InstanceId> ids = selection->get(revision);
        // Watched before the read, so a write that lands during it is heard.
        if (ids != watched) {
            watched = ids;
            world->set_watched(watch, ids);
        }
        // Taken before the read, so a change made while reading reads again next frame.
        if (!changed.take() && !moved && !force) {
            return;
        }
        PropertySheet next;
        AssetPreview next_preview;
        {
            engine_core::DataModelLock lock(*world, engine_core::DataModelLock::Read, kFrameLockWait);
            // Busy, or undo is writing the world back under this lock: try next frame.
            if (!lock.owns() || (history != nullptr && history->applying_undo_redo())) {
                changed.set();
                return;
            }
            next = read_sheet(*world, ids);
            next_preview = read_preview(ids);
        }
        seen_selection = revision;
        seen_tree = tree;
        set_preview(std::move(next_preview));
        if (next == sheet && !force) {
            return;
        }
        force = false;
        apply(std::move(next));
    }

    void collect_pending() {
        for (auto it = pending.begin(); it != pending.end();) {
            if (!(*it)->done.load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            if ((*it)->result.rejected) {
                status = (*it)->result.error;
            }
            it = pending.erase(it);
            force = true;
        }
    }

    // ---- Rows ------------------------------------------------------------

    static bool same_layout(const PropertySheet& a, const PropertySheet& b) {
        if (a.rows.size() != b.rows.size()) {
            return false;
        }
        for (std::size_t index = 0; index < a.rows.size(); ++index) {
            if (!a.rows[index].same_slot(b.rows[index]) || a.rows[index].writable != b.rows[index].writable) {
                return false;
            }
        }
        return true;
    }

    void apply(PropertySheet next) {
        if (next.ids != sheet.ids) {
            // Typing aimed at the old selection lands there, as if focus had left.
            for (const auto& view : rows) {
                finish_typing(*view);
            }
            // A drag still under way, as a held slider, keeps its value for the instances it was dragged on.
            if (live) {
                finish_live(live_ids, live_edit, true);
            }
        }
        if (!same_layout(next, sheet) || rows.size() != next.rows.size()) {
            rebuild_rows(next);
        }
        sheet = std::move(next);
        for (std::size_t index = 0; index < rows.size() && index < sheet.rows.size(); ++index) {
            rows[index]->ids = sheet.ids;
            show_row(*rows[index], sheet.rows[index]);
        }
        empty->setVisible(sheet.ids.empty());
    }

    void finish_typing(RowView& view) {
        for (PropertyField* field : view.fields()) {
            if (field->dirty) {
                commit_field(view, *field);
            }
        }
        // Closing keeps the color picked so far, for the instances it was picked for.
        if (view.color && view.color->isShowing()) {
            view.color->hide();
        }
    }

    void rebuild_rows(const PropertySheet& next) {
        std::vector<std::shared_ptr<RowView>> kept;
        std::vector<bool> used(rows.size(), false);
        for (const PropertyRow& row : next.rows) {
            std::shared_ptr<RowView> reuse;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (!used[index] && rows[index]->row.same_slot(row) && rows[index]->row.writable == row.writable) {
                    used[index] = true;
                    reuse = rows[index];
                    break;
                }
            }
            kept.push_back(reuse ? reuse : make_view(row));
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (used[index]) {
                continue;
            }
            RowView& gone = *rows[index];
            finish_typing(gone);
            if (picking && pick_name == gone.row.name) {
                cancel_pick();
            }
            if (asset_pick_view.lock().get() == &gone) {
                close_asset_picker();
            }
            for (jadefx::Node* node : gone.nodes()) {
                if (jadefx::Scene* scene = node->getScene()) {
                    scene->releaseFocus(node);
                }
                body->getChildren().removeIf(
                    [node](const std::shared_ptr<jadefx::Node>& child) { return child.get() == node; });
            }
        }
        rows = std::move(kept);
    }

    std::shared_ptr<PropertyField> make_field(const std::shared_ptr<RowView>& view, bool read_only,
                                              const char* style = kFieldStyle) {
        auto field = jadefx::make<PropertyField>();
        field->editable_style = style;
        field->set_read_only(read_only);
        std::weak_ptr<Impl> weak_self = shared_from_this();
        std::weak_ptr<RowView> weak_view = view;
        PropertyField* raw = field.get();
        field->setOnAction([weak_self, weak_view, raw](jadefx::ActionEvent&) {
            const auto self = weak_self.lock();
            const auto row = weak_view.lock();
            if (self && row) {
                self->commit_field(*row, *raw);
                self->release(*raw);
            }
        });
        field->on_cancel = [weak_self, weak_view, raw]() {
            const auto self = weak_self.lock();
            const auto row = weak_view.lock();
            if (self && row) {
                self->cancel_field(*row, *raw);
            }
        };
        field->on_tab = [weak_self, weak_view, raw](bool back) {
            const auto self = weak_self.lock();
            const auto row = weak_view.lock();
            if (self && row) {
                self->tab_from(*row, *raw, back);
            }
        };
        body->getChildren().add(field);
        return field;
    }

    std::shared_ptr<RowView> make_view(const PropertyRow& row) {
        auto view = std::make_shared<RowView>();
        view->row = row;
        view->name = jadefx::make<jadefx::Label>(row.name);
        view->name->getClassList().add("properties-name");
        view->name->setStyle(row.writable ? "color: var(--ide-text-color);"
                                          : "color: var(--ide-properties-readonly-name-color);");
        body->getChildren().add(view->name);
        std::weak_ptr<Impl> weak_self = shared_from_this();
        std::weak_ptr<RowView> weak_view = view;
        switch (row.kind) {
        case PropertyKind::String:
        case PropertyKind::Number:
        case PropertyKind::ReadOnlyText:
            view->field = make_field(view, !row.writable);
            if (row.slider()) {
                view->slider = jadefx::make<PropertySlider>(row.slider_min, row.slider_max);
                view->slider->setDisable(!row.writable);
                view->slider->on_move = [weak_self, weak_view]() {
                    const auto self = weak_self.lock();
                    const auto row_view = weak_view.lock();
                    if (self && row_view) {
                        self->follow_slider(*row_view);
                    }
                };
                view->slider->on_done = [weak_self, weak_view](bool moved) {
                    const auto self = weak_self.lock();
                    const auto row_view = weak_view.lock();
                    if (self && row_view) {
                        self->finish_slider(*row_view, moved);
                    }
                };
                view->slider->on_commit = [weak_self, weak_view]() {
                    const auto self = weak_self.lock();
                    const auto row_view = weak_view.lock();
                    if (self && row_view) {
                        self->commit_slider(*row_view);
                    }
                };
                body->getChildren().add(view->slider);
            }
            break;
        case PropertyKind::Vector2:
        case PropertyKind::Vector3:
            for (int axis = 0; axis < vector_axes(row.kind); ++axis) {
                view->axes[axis] =
                    make_field(view, !row.writable, row.name == "Position" ? kAxisStyles[axis] : kFieldStyle);
            }
            break;
        case PropertyKind::Transform: {
            // The arrow and the name both fold it.
            auto fold = [weak_self, weak_view](const jadefx::MouseEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->toggle_fold(*row_view);
                }
            };
            view->disclosure = jadefx::make<PropertyDisclosure>();
            view->disclosure->open = !folded.count(row.name);
            view->disclosure->setOnMouseClicked(fold);
            body->getChildren().add(view->disclosure);
            view->name->setCursor(jadefx::Cursor::Pointer);
            view->name->setOnMouseClicked(fold);
            for (int line = 0; line < 2; ++line) {
                view->lines[line] = jadefx::make<jadefx::Label>(kTransformLines[line]);
                view->lines[line]->getClassList().add("properties-name");
                view->lines[line]->setStyle(row.writable ? "color: var(--ide-text-color);"
                                                         : "color: var(--ide-properties-readonly-name-color);");
                body->getChildren().add(view->lines[line]);
            }
            for (int axis = 0; axis < kTransformParts; ++axis) {
                view->axes[axis] = make_field(view, !row.writable, kAxisStyles[axis % 3]);
            }
            break;
        }
        case PropertyKind::Enum: {
            view->choice = jadefx::make<jadefx::ComboBox>();
            view->choice->getClassList().add("properties-choice");
            view->choice->setDisable(!row.writable);
            std::vector<std::string> names;
            for (int index = 0; row.enum_type != nullptr && index < row.enum_type->count; ++index) {
                names.emplace_back(row.enum_type->items[index].name);
            }
            view->choice->getItems().setAll(std::move(names));
            view->choice->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->commit_choice(*row_view);
                }
            });
            body->getChildren().add(view->choice);
            break;
        }
        case PropertyKind::Bool:
            view->check = jadefx::make<jadefx::CheckBox>();
            view->check->getClassList().add("properties-check");
            view->check->setDisable(!row.writable);
            view->check->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->commit_check(*row_view);
                }
            });
            body->getChildren().add(view->check);
            break;
        case PropertyKind::Color3:
            view->color = jadefx::make<PropertyColor>();
            view->color->setDisable(!row.writable);
            view->color->on_live = [weak_self, weak_view]() {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->live_color(*row_view);
                }
            };
            view->color->on_done = [weak_self, weak_view](bool picked) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->finish_color(*row_view, picked);
                }
            };
            body->getChildren().add(view->color);
            break;
        case PropertyKind::Ref: {
            // The value is never typed in. Clicking it picks, and Clear sets nil.
            // An asset reference picks from a list of that asset's class.
            const std::string referred = engine_core::reference_class(row.type_name);
            if (engine_core::is_asset_class(referred)) {
                view->asset_class = referred;
            }
            view->tip = jadefx::make<jadefx::Tooltip>("");
            view->pick = jadefx::make<jadefx::Button>("");
            view->pick->getClassList().add("properties-pick");
            view->pick->setStyle(row.writable ? kFieldStyle : kReadOnlyStyle);
            view->pick->setAlignment(jadefx::Pos::CenterLeft);
            view->pick->setDisable(!row.writable);
            if (!view->asset_class.empty()) {
                if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(icon_filename(view->asset_class))) {
                    view->pick->setGraphic(std::move(icon));
                    view->pick->setGraphicTextGap(4);
                }
            }
            view->pick->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (!self || !row_view) {
                    return;
                }
                if (row_view->asset_class.empty()) {
                    self->toggle_pick(*row_view);
                } else {
                    self->open_asset_picker(row_view);
                }
            });
            // Parent already picks by clicking the Explorer; every other
            // reference also takes a drag from the Assets pane.
            if (row.name != "Parent") {
                view->pick->setOnDragOver([](jadefx::DragEvent& event) {
                    if (event.dragboard != nullptr && event.dragboard->has(kInstanceDragFormat)) {
                        event.acceptTransferModes(jadefx::TransferMode::Link);
                        event.consume();
                    }
                });
                view->pick->setOnDragDropped([weak_self, weak_view](jadefx::DragEvent& event) {
                    const auto self = weak_self.lock();
                    const auto row_view = weak_view.lock();
                    if (!self || !row_view || event.dragboard == nullptr) {
                        return;
                    }
                    const std::vector<engine_core::InstanceId> ids =
                        instance_drag_ids(event.dragboard->get(kInstanceDragFormat));
                    if (ids.empty()) {
                        return;
                    }
                    PropertyEdit edit;
                    edit.property = row_view->row.name;
                    edit.kind = PropertyKind::Ref;
                    edit.value.ref = ids.front();
                    self->submit(row_view->ids, edit);
                    event.setDropCompleted(true);
                    event.consume();
                });
            }
            view->clear = jadefx::make<jadefx::Button>("x");
            view->clear->getClassList().add("properties-clear");
            view->clear->setStyle(kButtonStyle);
            view->clear->setDisable(!row.writable);
            view->clear->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->clear_ref(*row_view);
                }
            });
            body->getChildren().add(view->pick);
            body->getChildren().add(view->clear);
            break;
        }
        }
        return view;
    }

    static std::string shown_text(const PropertyRow& row) {
        if (row.mixed) {
            return {};
        }
        switch (row.kind) {
        case PropertyKind::Number:
            return format_number(row.value.number);
        case PropertyKind::Ref:
            return row.label;
        default:
            return row.value.text;
        }
    }

    // The text of one part of a Vector3 or Transform: blank when mixed.
    static std::string part_text(const PropertyRow& row, int axis) {
        if (axis < 0 || axis >= kTransformParts || row.axis_mixed[axis]) {
            return {};
        }
        const engine_core::Vec3& vec = axis < 3 ? row.value.vec : row.value.orientation;
        const int part = axis % 3;
        return format_float(part == 0 ? vec.x : part == 1 ? vec.y : vec.z);
    }

    // Whether a row's fields show: its category is open, and a Transform is unfolded.
    bool open(const RowView& view) const {
        return !group_folded[group_index(view.row.group)] &&
               (view.row.kind != PropertyKind::Transform || !folded.count(view.row.name));
    }

    static int group_index(PropertyGroup group) { return group == PropertyGroup::Instance ? 0 : 1; }

    // Folding a category leaves any field in it, keeping what was typed, and
    // closes a color chooser open in it.
    void toggle_group(int index) {
        group_folded[index] = !group_folded[index];
        if (!group_folded[index] || index == kPreviewGroup) {
            return;
        }
        for (const auto& view : rows) {
            if (group_index(view->row.group) != index) {
                continue;
            }
            finish_typing(*view);
            for (PropertyField* field : view->fields()) {
                if (field->isFocused()) {
                    release(*field);
                }
            }
            if (picking && pick_name == view->row.name) {
                cancel_pick();
            }
            if (asset_pick_view.lock() == view) {
                close_asset_picker();
            }
        }
    }

    // Puts a category's header, with its arrow, at y. True while it is open.
    template <typename Place>
    bool place_header(Place& place, int index, double left, double width) {
        place(*headers[index], left, width, kRowHeight - 2);
        place(*header_arrows[index], left + 4, kDisclosureWidth, kRowHeight - 2);
        header_arrows[index]->open = !group_folded[index];
        return !group_folded[index];
    }

    // Folding a row leaves any field in it, keeping what was typed.
    void toggle_fold(RowView& view) {
        if (folded.erase(view.row.name) == 0) {
            folded.insert(view.row.name);
            for (PropertyField* field : view.fields()) {
                if (field->dirty) {
                    commit_field(view, *field);
                }
                if (field->isFocused()) {
                    release(*field);
                }
            }
        }
        view.disclosure->open = open(view);
    }

    // Puts the row's value in its widgets. A field being typed in keeps its text.
    void show_row(RowView& view, const PropertyRow& row) {
        view.row = row;
        auto put = [](PropertyField& field, const std::string& text) {
            if (field.isFocused() && field.dirty) {
                return;
            }
            field.show(text);
        };
        switch (row.kind) {
        case PropertyKind::String:
        case PropertyKind::Number:
        case PropertyKind::ReadOnlyText:
            put(*view.field, shown_text(row));
            // A mixed row rests the thumb at the start. A held one keeps the drag.
            if (view.slider && !view.slider->held()) {
                view.slider->show(row.mixed ? row.slider_min : row.value.number);
            }
            break;
        case PropertyKind::Vector2:
        case PropertyKind::Vector3:
            for (int axis = 0; axis < vector_axes(row.kind); ++axis) {
                put(*view.axes[axis], part_text(row, axis));
            }
            break;
        case PropertyKind::Transform:
            for (int axis = 0; axis < kTransformParts; ++axis) {
                put(*view.axes[axis], part_text(row, axis));
            }
            break;
        case PropertyKind::Enum: {
            // Mixed shows no item.
            int selected = -1;
            for (int index = 0; !row.mixed && row.enum_type != nullptr && index < row.enum_type->count; ++index) {
                if (row.enum_type->items[index].value == static_cast<int>(row.value.number)) {
                    selected = index;
                    break;
                }
            }
            view.choice->select(selected);
            break;
        }
        case PropertyKind::Bool:
            // Mixed is an indeterminate box, never an unchecked "false".
            view.check->setSelected(!row.mixed && row.value.flag);
            view.check->setIndeterminate(row.mixed);
            break;
        case PropertyKind::Color3:
            // An open chooser keeps the color being picked.
            if (view.color->isShowing()) {
                break;
            }
            view.color->mixed = row.mixed;
            view.color->setValue(jadefx::Color::rgba(row.value.color.r, row.value.color.g, row.value.color.b, 1.f));
            break;
        case PropertyKind::Ref:
            view.pick->setText(shown_text(row));
            view.tip->setText(row.mixed ? std::string() : row.path);
            if (!row.mixed && !row.path.empty()) {
                if (!view.tip_installed) {
                    jadefx::Tooltip::install(view.pick.get(), view.tip);
                    view.tip_installed = true;
                }
            } else if (view.tip_installed) {
                jadefx::Tooltip::uninstall(view.pick.get());
                view.tip_installed = false;
            }
            view.pick->setStyle(picking && pick_name == row.name ? kPickingStyle
                                : row.writable                  ? kFieldStyle
                                                                : kReadOnlyStyle);
            break;
        }
    }

    // ---- Writing ---------------------------------------------------------

    // Runs write where the world may be written, and reads the rows again after.
    void post(std::function<void(engine_core::DataModel&)> write) {
        if (run) {
            run(std::move(write));
        } else {
            write(*world);
        }
        force = true;
    }

    // Runs edit_fn there, keeping its result so a refusal shows under the rows.
    void post_recorded(std::function<EditResult(engine_core::DataModel&)> edit_fn) {
        status.clear();
        auto result = std::make_shared<PendingEdit>();
        pending.push_back(result);
        post([edit_fn = std::move(edit_fn), result](engine_core::DataModel& game) {
            result->result = edit_fn(game);
            result->done.store(true, std::memory_order_release);
        });
    }

    void submit(const std::vector<InstanceId>& ids, const PropertyEdit& edit) {
        if (world == nullptr || ids.empty()) {
            return;
        }
        post_recorded([ids, edit](engine_core::DataModel& game) { return apply_edit(game, ids, edit); });
    }

    // One step of a drag: the instances show edit now, and the history does
    // not hear of it until finish_live.
    void preview(const std::vector<InstanceId>& ids, const PropertyEdit& edit) {
        if (world == nullptr || ids.empty()) {
            return;
        }
        // A drag whose end never came, as when its row went away, keeps its last value.
        if (live && (live_edit.property != edit.property || live_ids != ids)) {
            finish_live(live_ids, live_edit, true);
        }
        if (!live) {
            live = std::make_shared<LiveEdit>();
            live_ids = ids;
        }
        live_edit = edit;
        post([ids, edit, target = live](engine_core::DataModel& game) { preview_edit(game, ids, edit, *target); });
    }

    // The end of a drag. commit records edit as one undo step from the value
    // before the drag; otherwise the instances go back to that value. With no
    // drag under way, a commit is an ordinary edit.
    void finish_live(const std::vector<InstanceId>& ids, const PropertyEdit& edit, bool commit) {
        if (!live) {
            if (commit) {
                submit(ids, edit);
            }
            return;
        }
        std::shared_ptr<LiveEdit> target = std::move(live);
        live.reset();
        const std::vector<InstanceId> drag_ids = std::move(live_ids);
        live_ids.clear();
        if (world == nullptr) {
            return;
        }
        post_recorded([drag_ids, edit, target, commit](engine_core::DataModel& game) {
            return finish_live_edit(game, drag_ids, edit, *target, commit);
        });
    }

    int axis_of(const RowView& view, const PropertyField& field) const {
        for (int axis = 0; axis < kTransformParts; ++axis) {
            if (view.axes[axis].get() == &field) {
                return axis;
            }
        }
        return -1;
    }

    // Nothing typed is nothing to write: a blank mixed field stays mixed.
    void commit_field(RowView& view, PropertyField& field) {
        if (!field.dirty || !field.isEditable() || !view.row.writable) {
            field.dirty = false;
            return;
        }
        field.dirty = false;
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = view.row.kind;
        const std::string text = field.getText();
        switch (view.row.kind) {
        case PropertyKind::String:
            edit.value.text = text;
            break;
        case PropertyKind::Number:
            if (!parse_number(text, edit.value.number)) {
                status = view.row.name + " must be a number";
                field.show(shown_text(view.row));
                return;
            }
            break;
        case PropertyKind::Vector2:
        case PropertyKind::Vector3: {
            double component = 0;
            const int axis = axis_of(view, field);
            if (axis < 0 || axis >= vector_axes(view.row.kind) || !parse_number(text, component)) {
                status = view.row.name + " must be a number";
                field.show(part_text(view.row, axis));
                return;
            }
            edit.axis = axis;
            const float value = static_cast<float>(component);
            edit.value.vec = {axis == 0 ? value : 0.f, axis == 1 ? value : 0.f, axis == 2 ? value : 0.f};
            break;
        }
        case PropertyKind::Transform: {
            double component = 0;
            const int axis = axis_of(view, field);
            if (axis < 0 || !parse_number(text, component)) {
                status = std::string(kTransformLines[axis >= 3 ? 1 : 0]) + " must be a number";
                field.show(part_text(view.row, axis));
                return;
            }
            edit.axis = axis;
            const float value = static_cast<float>(component);
            const int part = axis % 3;
            engine_core::Vec3& target = axis < 3 ? edit.value.vec : edit.value.orientation;
            target = {part == 0 ? value : 0.f, part == 1 ? value : 0.f, part == 2 ? value : 0.f};
            break;
        }
        default:
            return;
        }
        field.stack.reset(text);
        submit(view.ids, edit);
    }

    void cancel_field(RowView& view, PropertyField& field) {
        field.dirty = false;
        show_row(view, view.row);
        release(field);
    }

    void release(PropertyField& field) {
        if (jadefx::Scene* scene = field.getScene()) {
            scene->releaseFocus(&field);
        }
        field.was_focused = false;
    }

    void commit_choice(RowView& view) {
        const int index = view.choice->getSelectionIndex();
        if (!view.row.writable || view.row.enum_type == nullptr || index < 0 || index >= view.row.enum_type->count) {
            return;
        }
        const int value = view.row.enum_type->items[index].value;
        if (!view.row.mixed && static_cast<int>(view.row.value.number) == value) {
            return;
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Enum;
        edit.value.number = value;
        submit(view.ids, edit);
    }

    void commit_check(RowView& view) {
        if (!view.row.writable) {
            return;
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Bool;
        // From mixed, a click checks every one.
        edit.value.flag = view.check->isSelected();
        view.check->setIndeterminate(false);
        submit(view.ids, edit);
    }

    PropertyEdit color_edit(const RowView& view) const {
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Color3;
        const jadefx::Color picked = view.color->getValue();
        edit.value.color = engine_core::Color3{picked.r, picked.g, picked.b};
        return edit;
    }

    // The chooser moved: the instances take its color now.
    void live_color(RowView& view) {
        if (view.row.writable) {
            preview(view.ids, color_edit(view));
        }
    }

    // The chooser closed: a pick is one undo step; anything else puts the color back.
    void finish_color(RowView& view, bool picked) {
        if (!view.row.writable) {
            return;
        }
        if (picked) {
            view.color->mixed = false;
        }
        finish_live(view.ids, color_edit(view), picked);
    }

    double slider_value(const RowView& view) const {
        return slider_round(view.slider->getValue(), view.row.slider_min, view.row.slider_max);
    }

    PropertyEdit slider_edit(const RowView& view) const {
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Number;
        edit.value.number = slider_value(view);
        return edit;
    }

    // The field shows where the thumb is. Anything typed in it gives way.
    // While the button is held, the instances take the value too.
    void follow_slider(RowView& view) {
        view.field->show(format_number(slider_value(view)));
        if (view.slider->held() && view.row.writable) {
            preview(view.ids, slider_edit(view));
        }
    }

    // The button came up: a move is one undo step, and a press that came back
    // to where it began puts the value back.
    void finish_slider(RowView& view, bool moved) {
        if (!view.row.writable) {
            return;
        }
        const PropertyEdit edit = slider_edit(view);
        view.field->show(format_number(edit.value.number));
        finish_live(view.ids, edit, moved);
    }

    void commit_slider(RowView& view) {
        if (!view.row.writable) {
            return;
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Number;
        edit.value.number = slider_value(view);
        view.field->show(format_number(edit.value.number));
        // A nudge that rounds back to the value held is no edit.
        if (!view.row.mixed && edit.value.number == view.row.value.number) {
            return;
        }
        submit(view.ids, edit);
    }

    void clear_ref(RowView& view) {
        if (picking) {
            cancel_pick();
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Ref;
        edit.value.ref = engine_core::DataModel::kNoParent;
        submit(view.ids, edit);
    }

    // Blur is a commit. Focus arriving starts that field's own undo fresh.
    void check_focus() {
        for (const auto& view : rows) {
            for (PropertyField* field : view->fields()) {
                const bool focused = field->isFocused();
                if (field->was_focused && !focused && field->dirty) {
                    commit_field(*view, *field);
                }
                if (focused && !field->was_focused && !field->dirty) {
                    field->stack.reset(field->getText());
                }
                field->was_focused = focused;
            }
        }
    }

    // ---- Tab -------------------------------------------------------------

    // Tab and Shift+Tab walk the editable fields top to bottom, a Vector3's
    // X, Y, and Z in turn, and wrap around. The field left commits first. A
    // folded Transform's fields are passed over.
    void tab_from(RowView& view, PropertyField& from, bool back) {
        std::vector<std::pair<RowView*, PropertyField*>> order;
        std::size_t at = 0;
        for (const auto& row : rows) {
            if (!open(*row)) {
                continue;
            }
            for (PropertyField* field : row->fields()) {
                if (field == &from) {
                    at = order.size();
                }
                order.emplace_back(row.get(), field);
            }
        }
        commit_field(view, from);
        for (std::size_t step = 1; step < order.size(); ++step) {
            const std::size_t index = (at + (back ? order.size() - step : step)) % order.size();
            const auto [row, field] = order[index];
            if (field->isEditable()) {
                field->requestFocus();
                reveal(*row, *field);
                return;
            }
        }
        // The only field to go to is this one.
        if (from.isEditable()) {
            from.selectAll();
        }
    }

    // Scrolls just enough to show the line the field is on.
    void reveal(const RowView& view, const PropertyField& field) {
        const double height = scroller->getViewportBounds().height;
        const double range = content - height;
        if (!(range > 0)) {
            return;
        }
        double top = view.top;
        if (view.row.kind == PropertyKind::Transform) {
            top += (axis_of(view, field) >= 3 ? 2 : 1) * (kRowHeight + kRowGap);
        }
        // vvalue runs from vmin at the top to vmax at the bottom.
        const double span = scroller->getVmax() - scroller->getVmin();
        const double offset = (scroller->getVvalue() - scroller->getVmin()) / span * range;
        double next = offset;
        if (top - kPad < offset) {
            next = std::max(0.0, top - kPad);
        } else if (top + kRowHeight + kPad > offset + height) {
            next = top + kRowHeight + kPad - height;
        }
        if (next != offset) {
            scroller->setVvalue(scroller->getVmin() + std::min(next, range) / range * span);
        }
    }

    // ---- Pick ------------------------------------------------------------

    void toggle_pick(RowView& view) {
        if (picking && pick_name == view.row.name) {
            cancel_pick();
            return;
        }
        picking = true;
        pick_name = view.row.name;
        pick_ids = view.ids;
        pick_seen = selection->revision();
        status = "Click an instance in an explorer or the Assets pane to set " + pick_name + ". Click " + pick_name + " again to cancel.";
        force = true;
    }

    void cancel_pick() {
        picking = false;
        pick_name.clear();
        pick_ids.clear();
        status.clear();
        force = true;
    }

    // An asset reference lists every asset of its class under its Name. A
    // pick writes it to the instances the row was showing; None sets nil.
    void open_asset_picker(const std::shared_ptr<RowView>& view) {
        if (world == nullptr || !view->pick || !view->row.writable) {
            return;
        }
        if (picking) {
            cancel_pick();
        }
        std::vector<AssetChoice> choices;
        {
            engine_core::DataModelLock lock(*world, engine_core::DataModelLock::Read, kActionLockWait);
            if (!lock.owns()) {
                return;
            }
            choices = asset_choices(*world, view->asset_class);
        }
        if (!asset_picker) {
            asset_picker = AssetPicker::create();
        }
        asset_pick_view = view;
        const InstanceId current = view->row.mixed || view->row.value.nil_ref() ? 0 : view->row.value.ref;
        std::weak_ptr<Impl> weak_self = shared_from_this();
        std::weak_ptr<RowView> weak_view = view;
        asset_picker->open(*view->pick, view->asset_class, std::move(choices), current,
                           [weak_self, weak_view](InstanceId id) {
                               const auto self = weak_self.lock();
                               const auto row_view = weak_view.lock();
                               if (self && row_view) {
                                   self->picked_asset(*row_view, id);
                               }
                           });
    }

    void close_asset_picker() {
        if (asset_picker) {
            asset_picker->dismiss();
        }
        asset_pick_view.reset();
    }

    void picked_asset(RowView& view, InstanceId id) {
        asset_pick_view.reset();
        if (id == 0) {
            clear_ref(view);
            return;
        }
        // Picking what every instance already holds is no edit.
        if (!view.row.mixed && view.row.value.ref == id) {
            return;
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Ref;
        edit.value.ref = id;
        submit(view.ids, edit);
    }

    // The explorer click lands in the selection. The last instance picked is
    // the value; then the selection goes back to what was being edited.
    void check_pick() {
        if (selection->revision() == pick_seen) {
            return;
        }
        std::uint64_t revision = 0;
        const std::vector<InstanceId> picked = selection->get(revision);
        pick_seen = revision;
        if (picked == pick_ids) {
            return;
        }
        const std::string name = pick_name;
        const std::vector<InstanceId> targets = pick_ids;
        cancel_pick();
        selection->set(targets);
        if (picked.empty()) {
            return;
        }
        PropertyEdit edit;
        edit.property = name;
        edit.kind = PropertyKind::Ref;
        edit.value.ref = picked.back();
        submit(targets, edit);
    }

    // ---- Layout ----------------------------------------------------------

    // Lays the scroll pane over the page. The rows' height is known only once
    // they are placed, so a change lays it out again with the new height.
    void layout_page() {
        const double before = content;
        auto lay = [this] {
            scroller->performLayout(pane->inner_left(), pane->inner_top(), std::max(0.0, pane->inner_width()),
                                    std::max(0.0, pane->inner_height()));
        };
        lay();
        if (content != before) {
            lay();
        }
    }

    // Places every widget in body, top down, from body's top-left.
    void layout() {
        if (!body) {
            return;
        }
        const double left = 0;
        const double top = 0;
        const double width = std::max(0.0, body->getWidth());
        const double inner = std::max(0.0, width - 2 * kPad);
        const double name_width = std::clamp(inner * 0.38, 64.0, 180.0);
        const double editor_x = left + kPad + name_width;
        const double editor_width = std::max(24.0, inner - name_width);

        double y = kPad;
        auto place = [&](jadefx::Node& node, double x, double w, double h) {
            node.setVisible(true);
            node.performLayout(x, top + y, std::max(0.0, w), h);
        };

        if (sheet.ids.empty()) {
            place(*empty, left + kPad, inner, kRowHeight);
            y += kRowHeight + kRowGap;
        } else {
            empty->setVisible(false);
        }
        for (int index = 0; index < kGroups; ++index) {
            headers[index]->setVisible(false);
            header_arrows[index]->setVisible(false);
        }
        PropertyGroup group = PropertyGroup::Instance;
        bool any = false;
        bool group_open = true;
        for (const auto& view : rows) {
            if (!any || view->row.group != group) {
                group = view->row.group;
                any = true;
                group_open = place_header(place, group_index(group), left, width);
                y += kRowHeight - 2 + kRowGap;
            }
            view->top = y;
            if (!group_open) {
                for (jadefx::Node* node : view->nodes()) {
                    node->setVisible(false);
                }
                continue;
            }
            place(*view->name, left + kPad + kIndent, name_width - kIndent - 4, kRowHeight);
            const double each = (editor_width - 2 * kAxisGap) / 3;
            switch (view->row.kind) {
            case PropertyKind::Vector2: {
                const double half = (editor_width - kAxisGap) / 2;
                for (int axis = 0; axis < 2; ++axis) {
                    place(*view->axes[axis], editor_x + axis * (half + kAxisGap), half, kRowHeight);
                }
                break;
            }
            case PropertyKind::Vector3:
                for (int axis = 0; axis < 3; ++axis) {
                    place(*view->axes[axis], editor_x + axis * (each + kAxisGap), each, kRowHeight);
                }
                break;
            case PropertyKind::Transform: {
                // The name's line holds only the arrow. Open, Position and
                // Orientation follow, a step further in, each an X, Y, Z line.
                place(*view->disclosure, left + kPad + kIndent - kDisclosureWidth - 1, kDisclosureWidth, kRowHeight);
                const bool shown = open(*view);
                for (int line = 0; line < 2; ++line) {
                    if (!shown) {
                        view->lines[line]->setVisible(false);
                        for (int part = 0; part < 3; ++part) {
                            view->axes[line * 3 + part]->setVisible(false);
                        }
                        continue;
                    }
                    y += kRowHeight + kRowGap;
                    place(*view->lines[line], left + kPad + 2 * kIndent, name_width - 2 * kIndent - 4, kRowHeight);
                    for (int part = 0; part < 3; ++part) {
                        place(*view->axes[line * 3 + part], editor_x + part * (each + kAxisGap), each, kRowHeight);
                    }
                }
                break;
            }
            case PropertyKind::Bool:
                place(*view->check, editor_x, std::min(editor_width, kRowHeight), kRowHeight);
                break;
            case PropertyKind::Enum:
                place(*view->choice, editor_x, editor_width, kRowHeight);
                break;
            case PropertyKind::Color3:
                place(*view->color, editor_x, editor_width, kRowHeight);
                break;
            case PropertyKind::Ref: {
                const double display = std::max(24.0, editor_width - kClearWidth - kAxisGap);
                place(*view->pick, editor_x, display, kRowHeight);
                place(*view->clear, editor_x + display + kAxisGap, kClearWidth, kRowHeight);
                break;
            }
            default:
                if (view->slider) {
                    // The track first, then the field that takes any number.
                    const double typed = std::clamp(editor_width * 0.35, 40.0, kSliderFieldWidth);
                    const double track = std::max(0.0, editor_width - typed - kAxisGap);
                    place(*view->slider, editor_x, track, kRowHeight);
                    place(*view->field, editor_x + track + kAxisGap, typed, kRowHeight);
                } else {
                    place(*view->field, editor_x, editor_width, kRowHeight);
                }
                break;
            }
            y += kRowHeight + kRowGap;
        }
        layout_preview(place, y, left, width, inner);
        status_label->setText(status);
        // Pick instructions are a hint. Anything else is a refused edit.
        status_label->setStyle(picking ? kHintStyle : kErrorStyle);
        if (!status.empty()) {
            y += kRowGap;
            place(*status_label, left + kPad, inner, kRowHeight);
            y += kRowHeight;
        } else {
            status_label->setVisible(false);
        }
        content = y + kPad;
    }

    // The Preview section, from y down, which it moves past it.
    template <typename Place>
    void layout_preview(Place& place, double& y, double left, double width, double inner) {
        preview_frame->setVisible(false);
        preview_image->setVisible(false);
        preview_note->setVisible(false);
        for (const auto& button : sound_buttons) {
            button->setVisible(false);
        }
        sound_track->setVisible(false);
        sound_time->setVisible(false);
        if (asset_preview.kind == AssetPreview::Kind::None) {
            return;
        }
        const bool shown = place_header(place, kPreviewGroup, left, width);
        y += kRowHeight - 2 + kRowGap;
        if (!shown) {
            return;
        }
        y += kPad;
        if (asset_preview.kind == AssetPreview::Kind::Sound) {
            layout_sound(place, y, left, inner);
            return;
        }
        // A square frame, as wide as the page allows, with the image fitted in it.
        const double side = std::min(inner, kPreviewSide);
        const double frame_x = left + kPad + (inner - side) / 2;
        place(*preview_frame, frame_x, side, side);
        std::shared_ptr<jadefx::Image> image;
        if (asset_preview.kind == AssetPreview::Kind::Material) {
            image = balls->get(asset_preview.id, asset_preview.look);
        } else if (!asset_preview.file.empty()) {
            image = thumbnails->get(asset_preview.file);
        }
        if (image && image->getWidth() > 0 && image->getHeight() > 0) {
            if (preview_image->getImage() != image) {
                preview_image->setImage(image);
            }
            const double fit = side - 8;
            const double scale = fit / std::max(image->getWidth(), image->getHeight());
            const double w = std::max(1.0, image->getWidth() * scale);
            const double h = std::max(1.0, image->getHeight() * scale);
            const double frame_y = y;
            y = frame_y + (side - h) / 2;
            place(*preview_image, frame_x + (side - w) / 2, w, h);
            y = frame_y;
        } else {
            const bool no_path = asset_preview.kind == AssetPreview::Kind::Texture && asset_preview.file.empty();
            preview_note->setText(no_path ? "No Path" : "No preview");
            const double frame_y = y;
            y = frame_y + (side - kRowHeight) / 2;
            place(*preview_note, frame_x, side, kRowHeight);
            y = frame_y;
        }
        y += side + kRowGap;
    }

    // A Sound's transport, from y down. Each button is enabled only when it
    // does something: Pause while playing, Resume while paused, Stop while
    // either. The track follows the sound unless its thumb is held.
    template <typename Place>
    void layout_sound(Place& place, double& y, double left, double inner) {
        using State = engine_core::SoundPreview::State;
        const State state = sound_state();
        const bool enabled[kSoundButtons] = {!asset_preview.file.empty(), state == State::Playing,
                                             state == State::Paused, state != State::Stopped};
        const double each =
            std::min(kSoundButtonWidth, (inner - (kSoundButtons - 1) * kAxisGap) / kSoundButtons);
        for (int part = 0; part < kSoundButtons; ++part) {
            sound_buttons[part]->setDisable(!enabled[part]);
            place(*sound_buttons[part], left + kPad + part * (each + kAxisGap), each, kRowHeight);
        }
        y += kRowHeight + kRowGap;

        const double length = state != State::Stopped && sound->length() > 0 ? sound->length() : sound_length;
        const double position = state != State::Stopped ? std::min(sound->position(), length) : 0.0;
        sound_track->setDisable(state == State::Stopped || !(length > 0));
        if (sound_track->getMax() != std::max(length, 0.001)) {
            sound_track->setMax(std::max(length, 0.001));
            sound_track->setBlockIncrement(std::max(length, 0.001) / 20);
        }
        if (!sound_track->held()) {
            sound_track->show(position);
        }
        const double shown = sound_track->held() ? sound_track->getValue() : position;
        sound_time->setText(clock_text(shown) + " / " + clock_text(length));
        const double time_width = std::min(kSoundTimeWidth, inner / 2);
        const double track = std::max(0.0, inner - time_width - kAxisGap);
        place(*sound_track, left + kPad, track, kRowHeight);
        place(*sound_time, left + kPad + track + kAxisGap, time_width, kRowHeight);
        y += kRowHeight + kRowGap;
    }
};

void PropertiesPane::layoutChildren() {
    if (const auto impl = owner.lock()) {
        impl->poll();
        impl->layout_page();
    }
}

void PropertiesBody::layoutChildren() {
    if (const auto impl = owner.lock()) {
        impl->layout();
    }
}

double PropertiesBody::preferredContentHeight(double) const {
    const auto impl = owner.lock();
    return impl ? impl->content : 0.0;
}

void PropertiesPane::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    IdePane::renderContent(renderer, opacity);
    if (const auto impl = owner.lock()) {
        impl->draw_previews();
    }
}

void PropertiesPane::sceneChanged(jadefx::Scene* previous) {
    IdePane::sceneChanged(previous);
    // As the Assets pane does: release the ball's GL objects while the context
    // that made them is current. Scene teardown runs after the context is gone.
    if (previous == nullptr || previous->isTearingDown() || getScene() == previous) {
        return;
    }
    if (const auto impl = owner.lock()) {
        impl->ball->release();
    }
}

PropertiesPanel::PropertiesPanel() : impl_(std::make_shared<Impl>()) {
    impl_->build();
    impl_->pane->owner = impl_;
    impl_->body->owner = impl_;
}

PropertiesPanel::~PropertiesPanel() {
    if (impl_) {
        impl_->close_asset_picker();
    }
    if (impl_ && impl_->pane) {
        impl_->pane->owner.reset();
    }
    if (impl_ && impl_->world != nullptr && impl_->watch != 0) {
        impl_->world->unwatch_changes(impl_->watch);
    }
}

void PropertiesPanel::bind(engine_core::DataModel& world, engine_core::SelectionService& selection,
                           engine_core::ChangeHistoryService& history) {
    if (impl_->world != nullptr && impl_->watch != 0) {
        impl_->world->unwatch_changes(impl_->watch);
    }
    impl_->world = &world;
    impl_->selection = &selection;
    impl_->history = &history;
    impl_->watch = world.watch_changes(impl_->changed.setter());
    impl_->watched.clear();
    impl_->force = true;
    impl_->poll();
}

void PropertiesPanel::set_runner(PropertiesRun run) { impl_->run = std::move(run); }

void PropertiesPanel::rebuild() {
    impl_->force = true;
    impl_->poll();
}

std::shared_ptr<IdePane> PropertiesPanel::dock_widget() const { return impl_->pane; }

bool PropertiesPanel::owns(const jadefx::Node* node) const {
    for (const jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (cursor == impl_->pane.get()) {
            return true;
        }
    }
    return false;
}

bool PropertiesPanel::field_undo(bool redo) {
    jadefx::Scene* scene = impl_->pane ? impl_->pane->getScene() : nullptr;
    jadefx::Node* focused = scene != nullptr ? scene->focusedNode() : nullptr;
    if (focused == nullptr) {
        return false;
    }
    for (const auto& view : impl_->rows) {
        for (PropertyField* field : view->fields()) {
            if (field == focused || field->isAncestorOf(focused)) {
                return field->undo_step(redo);
            }
        }
    }
    return false;
}

jadefx::Node* PropertiesPanel::editor(const std::string& property, int part) const {
    for (const auto& view : impl_->rows) {
        if (view->row.name != property) {
            continue;
        }
        switch (view->row.kind) {
        case PropertyKind::Vector3:
            return part >= 0 && part < 3 ? view->axes[part].get() : nullptr;
        case PropertyKind::Vector2:
            return part >= 0 && part < 2 ? view->axes[part].get() : nullptr;
        case PropertyKind::Transform:
            return part >= 0 && part < kTransformParts ? static_cast<jadefx::Node*>(view->axes[part].get())
                   : part == kTransformParts           ? static_cast<jadefx::Node*>(view->disclosure.get())
                                                       : nullptr;
        case PropertyKind::Bool:
            return part == 0 ? view->check.get() : nullptr;
        case PropertyKind::Enum:
            return part == 0 ? view->choice.get() : nullptr;
        case PropertyKind::Color3:
            return part == 0 ? view->color.get() : nullptr;
        case PropertyKind::Ref:
            return part == 0 ? static_cast<jadefx::Node*>(view->pick.get())
                   : part == 1 ? static_cast<jadefx::Node*>(view->clear.get())
                               : nullptr;
        default:
            return part == 0 ? static_cast<jadefx::Node*>(view->field.get())
                   : part == 1 ? static_cast<jadefx::Node*>(view->slider.get())
                               : nullptr;
        }
    }
    return nullptr;
}

const PropertySheet& PropertiesPanel::sheet() const { return impl_->sheet; }

bool PropertiesPanel::picking() const { return impl_->picking; }

const std::string& PropertiesPanel::pick_property() const { return impl_->pick_name; }

const std::string& PropertiesPanel::status() const { return impl_->status; }

bool PropertiesPanel::asset_picking() const { return impl_->asset_picker && impl_->asset_picker->showing(); }

jadefx::TextField* PropertiesPanel::asset_pick_field() const {
    return asset_picking() ? impl_->asset_picker->field() : nullptr;
}

jadefx::Node* PropertiesPanel::asset_pick_row(engine_core::InstanceId asset) const {
    return asset_picking() ? impl_->asset_picker->row(asset) : nullptr;
}

std::string PropertiesPanel::preview_class() const {
    switch (impl_->asset_preview.kind) {
    case AssetPreview::Kind::Texture:
        return "Texture";
    case AssetPreview::Kind::Material:
        return "Material";
    case AssetPreview::Kind::Sound:
        return "Sound";
    case AssetPreview::Kind::None:
        break;
    }
    return {};
}

jadefx::ScrollPane* PropertiesPanel::scroll_pane() const { return impl_->scroller.get(); }

jadefx::Node* PropertiesPanel::group_header(const std::string& title) const {
    for (int index = 0; index < kGroups; ++index) {
        if (title == kGroupTitles[index]) {
            return impl_->headers[index].get();
        }
    }
    return nullptr;
}

void PropertiesPanel::stop_sound() {
    if (impl_->sound) {
        impl_->sound->stop();
    }
}

bool PropertiesPanel::sound_live() const {
    return impl_->sound_state() != engine_core::SoundPreview::State::Stopped;
}

jadefx::Node* PropertiesPanel::sound_control(int part) const {
    if (impl_->asset_preview.kind != AssetPreview::Kind::Sound || part < 0) {
        return nullptr;
    }
    return part < kSoundButtons      ? static_cast<jadefx::Node*>(impl_->sound_buttons[part].get())
           : part == kSoundButtons   ? static_cast<jadefx::Node*>(impl_->sound_track.get())
           : part == kSoundButtons + 1 ? static_cast<jadefx::Node*>(impl_->sound_time.get())
                                     : nullptr;
}

}  // namespace ide
