#include "IdePrefabEditor.hpp"
#include "LockWaits.hpp"

#include "AssetInstances.hpp"
#include "AssetPicker.hpp"
#include "BillboardBaker.hpp"
#include "DataModelLock.hpp"
#include "IdeIcons.hpp"
#include "PropertySheet.hpp"
#include "SelectionService.hpp"
#include "StretchRow.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace ide {

namespace {

// Each slot's part, addressed by the slot's drop handler.
constexpr ModelPart kParts[] = {ModelPart::Mesh, ModelPart::Material};

constexpr double kCardWidth = 272;
// A slot's thumb, which holds the asset's detailed icon when it has one.
constexpr double kThumbSize = 30;
constexpr double kGap = 16;
// About a card's height, so the New Model tile lines up with the cards beside it.
constexpr double kTileHeight = 228;

// Frames a picker waits for its card to be laid out and scrolled into view.
constexpr int kPickerWaitFrames = 30;
// Paints a bake waits for the Prefab's textures to load before it gives up.
constexpr int kBakeWaits = 600;

constexpr const char* kEditorRules = R"CSS(
.prefab-editor {
    background-color: var(--background-color);
}
.pe-header {
    background-color: var(--surface-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 14px 20px;
    spacing: 12px;
}
.pe-badge {
    background-color: var(--selection-color);
    border-radius: 9px;
}
.pe-title {
    color: var(--ide-text-color);
    font-size: 17px;
    font-weight: bold;
}
.pe-subtitle {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.pe-primary {
    background-color: var(--accent-color);
    border-width: 0;
    border-radius: 7px;
    color: var(--surface-color);
    font-weight: bold;
    padding: 7px 14px 7px 10px;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.15);
    transition: box-shadow 120ms;
}
.pe-primary:hover {
    box-shadow: 0 3px 10px rgba(0, 0, 0, 0.25);
}
.pe-primary image-view {
    image-color: currentColor;
}
.pe-scroll, .pe-canvas {
    background-color: var(--background-color);
}
.pe-grid {
    padding: 20px;
}
.pe-card {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 12px;
    padding: 14px;
    spacing: 12px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
    transition: border-color 120ms, box-shadow 160ms;
}
.pe-card:hover {
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.14);
}
.pe-card.selected {
    border-color: var(--accent-color);
    box-shadow: 0 0 0 1px var(--accent-color);
}
.pe-card-icon {
    background-color: var(--selection-color);
    border-radius: 8px;
}
.pe-name {
    color: var(--ide-text-color);
    font-size: 14px;
    font-weight: bold;
}
.pe-rename {
    font-size: 14px;
    padding: 1px 4px;
    border-radius: 4px;
}
.pe-status {
    spacing: 5px;
}
.pe-status-text {
    font-size: 11px;
    color: var(--ide-muted-text-color);
}
.pe-dot {
    border-radius: 4px;
    background-color: var(--warning-color);
}
.pe-status.ready .pe-dot {
    background-color: var(--success-color);
}
.pe-status.missing .pe-dot {
    background-color: var(--error-color);
}
.pe-icon-button {
    border-radius: 6px;
    cursor: pointer;
    opacity: 0;
    transition: background-color 120ms;
}
.pe-icon-button:hover {
    background-color: var(--row-hover-color);
}
.pe-icon-button image-view {
    image-color: var(--ide-muted-text-color);
}
.pe-icon-button:hover image-view {
    image-color: var(--ide-text-color);
}
.pe-card:hover .pe-card-delete, .pe-card.selected .pe-card-delete, .pe-slot:hover .pe-slot-clear {
    opacity: 1;
}
.pe-section {
    spacing: 6px;
}
.pe-slot-label {
    color: var(--ide-muted-text-color);
    font-size: 10px;
    font-weight: bold;
}
.pe-slot {
    background-color: var(--background-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 9px;
    padding: 7px 8px;
    spacing: 10px;
    cursor: pointer;
    transition: border-color 120ms, background-color 120ms, box-shadow 120ms;
}
.pe-slot:hover {
    border-color: var(--accent-color);
}
.pe-slot.empty {
    background-color: var(--wash-color);
}
.pe-slot.missing {
    border-color: var(--error-color);
}
.pe-thumb {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 7px;
}
.pe-slot.empty .pe-thumb {
    background-color: rgba(0, 0, 0, 0);
}
.pe-slot.empty .pe-thumb image-view {
    image-color: var(--accent-color);
}
.pe-slot-text {
    color: var(--ide-text-color);
    font-size: 13px;
}
.pe-slot.empty .pe-slot-text {
    color: var(--accent-color);
}
.pe-slot.missing .pe-slot-text {
    color: var(--error-color);
}
.pe-slot-sub {
    color: var(--ide-muted-text-color);
    font-size: 11px;
}
.pe-new {
    background-color: var(--wash-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 12px;
    spacing: 6px;
    cursor: pointer;
    transition: border-color 120ms, background-color 120ms, box-shadow 120ms;
}
.pe-new:hover {
    border-color: var(--accent-color);
    background-color: var(--surface-color);
}
.pe-new image-view {
    image-color: var(--accent-color);
}
.pe-new-title {
    color: var(--accent-color);
    font-size: 13px;
    font-weight: bold;
}
.pe-new-hint, .pe-empty-text {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.pe-empty {
    spacing: 10px;
    padding: 40px;
}
.pe-empty-art {
    background-color: var(--selection-color);
    border-radius: 36px;
}
.pe-empty-title {
    color: var(--ide-text-color);
    font-size: 17px;
    font-weight: bold;
}
.pe-card.drop, .pe-slot.drop, .pe-new.drop {
    border-color: var(--accent-color);
    box-shadow: 0 0 0 2px var(--accent-color);
}
.pe-canvas.drop {
    background-color: var(--selection-color);
}
.pe-secondary {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 7px;
    color: var(--ide-text-color);
    font-weight: bold;
    padding: 6px 13px 6px 9px;
    transition: border-color 120ms;
}
.pe-secondary:hover {
    border-color: var(--accent-color);
}
.pe-secondary image-view {
    image-color: var(--accent-color);
}
.pe-band {
    padding: 20px 20px 0 20px;
    spacing: 10px;
}
.pe-band-title {
    color: var(--ide-text-color);
    font-size: 11px;
    font-weight: bold;
}
.pe-band-hint {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.pe-lods {
    padding: 14px 20px 24px 20px;
    spacing: 14px;
}
.pe-lod {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 12px;
    padding: 14px;
    spacing: 14px;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.08);
    transition: border-color 120ms, box-shadow 160ms;
}
.pe-lod:hover {
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.14);
}
.pe-lod.selected {
    border-color: var(--accent-color);
    box-shadow: 0 0 0 1px var(--accent-color);
}
.pe-lod:hover .pe-lod-delete, .pe-lod.selected .pe-lod-delete {
    opacity: 1;
}
.pe-lod-range {
    color: var(--ide-muted-text-color);
    font-size: 11px;
}
.pe-lod-range.covered {
    color: var(--warning-color);
}
.pe-lod-word {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.pe-number {
    font-size: 13px;
    padding: 3px 6px;
    border-radius: 6px;
}
.pe-kinds {
    background-color: var(--background-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 8px;
    padding: 2px;
    spacing: 2px;
}
.pe-kind {
    background-color: rgba(0, 0, 0, 0);
    border-width: 0;
    border-radius: 6px;
    color: var(--ide-muted-text-color);
    font-size: 12px;
    padding: 4px 11px;
    transition: background-color 120ms;
}
.pe-kind:hover {
    background-color: var(--row-hover-color);
    color: var(--ide-text-color);
}
.pe-kind.selected {
    background-color: var(--accent-color);
    color: var(--surface-color);
    font-weight: bold;
}
.pe-lod-note {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.pe-lod-billboard {
    spacing: 24px;
}
)CSS";
std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style_class);
    label->setMouseTransparent(true);
    return label;
}

bool has_class(const jadefx::Node& node, const std::string& name) {
    const auto& items = node.getClassList().items();
    return std::find(items.begin(), items.end(), name) != items.end();
}

void set_class(jadefx::Node& node, const std::string& name, bool on) {
    if (on == has_class(node, name)) {
        return;
    }
    if (on) {
        node.getClassList().add(name);
    } else {
        node.getClassList().removeIf([&name](const std::string& item) { return item == name; });
    }
}

// Takes the room left in a line, which pushes what follows it to the right edge.
std::shared_ptr<jadefx::Pane> spacer() {
    auto pane = jadefx::make<jadefx::Pane>();
    pane->setStyle("width: 100%;");
    pane->setMouseTransparent(true);
    return pane;
}

// A file's icon at size, centered in a box of box points, with the box's class.
std::shared_ptr<jadefx::StackPane> icon_box(const std::string& file, double size, double box, const char* style_class) {
    auto pane = jadefx::make<jadefx::StackPane>();
    pane->getClassList().add(style_class);
    pane->setAlignment(jadefx::Pos::Center);
    pane->setPrefSize(box, box);
    pane->setMinSize(box, box);
    pane->setMaxSize(box, box);
    pane->setMouseTransparent(true);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_file(file)) {
        icon->setPrefSize(size, size);
        icon->setMinSize(size, size);
        icon->setMaxSize(size, size);
        icon->setMouseTransparent(true);
        pane->getChildren().add(std::move(icon));
    }
    return pane;
}

// A small icon that acts on a click: a card's delete, a slot's clear. It shows
// while the pointer is over what it belongs to. The click still bubbles on.
std::shared_ptr<jadefx::StackPane> icon_button(const std::string& file, const std::string& tip,
                                               const char* style_class, std::function<void()> run) {
    auto button = jadefx::make<jadefx::StackPane>();
    button->getClassList().add("pe-icon-button");
    button->getClassList().add(style_class);
    button->setAlignment(jadefx::Pos::Center);
    button->setPrefSize(24, 24);
    button->setMinSize(24, 24);
    button->setMaxSize(24, 24);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(file)) {
        button->getChildren().add(std::move(icon));
    }
    button->setOnMouseClicked([run = std::move(run)](const jadefx::MouseEvent& event) {
        if (event.button == 0) {
            run();
        }
    });
    jadefx::Tooltip::install(button.get(), jadefx::make<jadefx::Tooltip>(tip));
    return button;
}

const char* part_icon(ModelPart part) { return part == ModelPart::Mesh ? "Mesh.png" : "Material.png"; }
const char* part_word(ModelPart part) { return part == ModelPart::Mesh ? "mesh" : "material"; }

// A rename field: Escape drops the typing.
class NameField : public jadefx::TextField {
public:
    explicit NameField(std::function<void()> cancel) : cancel_(std::move(cancel)) {
        getClassList().add("pe-rename");
        setCapturesKeys(true);
        setVisible(false);
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape) {
            event.consume();
            cancel_();
            return;
        }
        TextField::handleKey(event);
    }

private:
    std::function<void()> cancel_;
};

// A number typed in place: Enter, or leaving the field, applies it, and
// Escape puts back what it showed. A number below min, or at it when above,
// is refused: the field shows its value again.
class NumberField : public jadefx::TextField {
public:
    NumberField(double min, bool above, std::function<void(double)> apply)
        : min_(min), above_(above), apply_(std::move(apply)) {
        getClassList().add("pe-number");
        setCapturesKeys(true);
        setOnAction([this](jadefx::ActionEvent&) { commit(); });
    }

    // Shows value, unless the person is typing in the field.
    void show(double value) {
        const std::string text = distance_text(value);
        if (text != shown_) {
            shown_ = text;
            sent_.clear();
        }
        if (!isFocused() && getText() != shown_) {
            setText(shown_);
        }
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape) {
            event.consume();
            setText(shown_);
            return;
        }
        TextField::handleKey(event);
    }

    void handleFocusLost() override {
        TextField::handleFocusLost();
        commit();
    }

private:
    void commit() {
        std::string text = getText();
        const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
        text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), space));
        text.erase(std::find_if_not(text.rbegin(), text.rend(), space).base(), text.end());
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        const bool number = !text.empty() && end != nullptr && *end == '\0' && std::isfinite(value);
        if (!number || value < min_ || (above_ && value <= min_)) {
            setText(shown_);
            return;
        }
        const std::string tidy = distance_text(value);
        if (getText() != tidy) {
            setText(tidy);
        }
        // Sent once: the place catches up on the simulation thread.
        if (tidy == shown_ || tidy == sent_) {
            return;
        }
        sent_ = tidy;
        apply_(value);
    }

    double min_ = 0.0;
    bool above_ = false;
    std::function<void(double)> apply_;
    std::string shown_;
    std::string sent_;
};

// A slot's widgets: a thumb, the asset's name over its folder, and clear.
struct SlotWidgets {
    std::shared_ptr<jadefx::HBox> root;
    std::shared_ptr<jadefx::StackPane> thumb;
    std::shared_ptr<jadefx::Label> text;
    std::shared_ptr<jadefx::Label> sub;
    std::shared_ptr<jadefx::StackPane> clear;
    PartView shown;
    bool filled = false;
};

// Makes slot's widgets: icon in the thumb until it is filled, and clear on its × button.
void build_slot(SlotWidgets& slot, const char* icon, const std::string& element_id, const std::string& clear_tip,
                std::function<void()> clear) {
    // The text takes what is left, so a long name ends in an ellipsis.
    slot.root = std::make_shared<StretchRow>(1);
    slot.root->getClassList().add("pe-slot");
    slot.root->setElementId(element_id);
    slot.thumb = icon_box(icon, 18, kThumbSize, "pe-thumb");
    slot.root->getChildren().add(slot.thumb);
    auto text = jadefx::make<jadefx::VBox>();
    text->setMouseTransparent(true);
    text->setAlignment(jadefx::Pos::CenterLeft);
    text->setStyle("width: 100%;");
    text->setMinSize(0, 0);
    slot.text = text_label("", "pe-slot-text");
    slot.sub = text_label("", "pe-slot-sub");
    // A VBox lays each out at its text's width; a full width lets it end in an ellipsis instead.
    slot.text->setMinSize(0, 0);
    slot.sub->setMinSize(0, 0);
    slot.text->setStyle("width: 100%;");
    slot.sub->setStyle("width: 100%;");
    text->getChildren().add(slot.text);
    text->getChildren().add(slot.sub);
    slot.root->getChildren().add(text);
    slot.clear = icon_button("Cross.png", clear_tip, "pe-slot-clear", std::move(clear));
    slot.root->getChildren().add(slot.clear);
}

// slot shows view: the asset's detailed icon, else icon, with its name and
// folder; a missing asset; or what the slot asks for. word names what it
// takes ("mesh"), property the property that holds it ("Mesh").
void show_slot(SlotWidgets& slot, const char* icon, const char* word, const char* property, const PartView& view) {
    const bool filled = view.id != 0;
    if (slot.shown == view && slot.text->getText().size() > 0) {
        return;
    }
    slot.shown = view;
    set_class(*slot.root, "empty", !filled && !view.missing);
    set_class(*slot.root, "missing", view.missing);
    // The thumb shows the asset's detailed icon, else the slot's icon when
    // filled, and a plus when it asks for one.
    slot.thumb->getChildren().clear();
    const std::string file = filled ? icon : view.missing ? "Warning.png" : "Plus.png";
    if (std::shared_ptr<jadefx::Node> detailed = filled ? AssetPicker::detailedIcon(view.id, kThumbSize - 2) : nullptr) {
        slot.thumb->getChildren().add(std::move(detailed));
    } else if (std::shared_ptr<jadefx::ImageView> image = icon_file(file)) {
        image->setPrefSize(18, 18);
        image->setMinSize(18, 18);
        image->setMaxSize(18, 18);
        image->setMouseTransparent(true);
        slot.thumb->getChildren().add(std::move(image));
    }
    if (filled) {
        slot.text->setText(view.name);
        slot.sub->setText(view.where.empty() ? std::string(property) : view.where);
    } else if (view.missing) {
        slot.text->setText(std::string("Missing ") + word);
        slot.sub->setText("Its asset was deleted. Choose another.");
    } else {
        slot.text->setText(std::string("Choose a ") + word);
        slot.sub->setText("Click, or drop one from Assets");
    }
    slot.clear->setVisible(filled || view.missing);
    slot.filled = filled;
}

constexpr engine_core::LodKind kLodKinds[] = {engine_core::LodKind::Model, engine_core::LodKind::Billboard,
                                              engine_core::LodKind::None};

const char* lod_kind_tip(engine_core::LodKind kind) {
    switch (kind) {
    case engine_core::LodKind::Model:
        return "Swap in other Models from this distance";
    case engine_core::LodKind::Billboard:
        return "Draw a card with a Material, turned to face the camera, from this distance";
    case engine_core::LodKind::None:
        return "Draw nothing from this distance";
    }
    return "";
}

// The icon a LOD card shows for its kind.
const char* lod_kind_icon(engine_core::LodKind kind) {
    switch (kind) {
    case engine_core::LodKind::Model:
        return "Model.png";
    case engine_core::LodKind::Billboard:
        return "billboard.png";
    case engine_core::LodKind::None:
        return "EyeClosed.png";
    }
    return "Model.png";
}

// box shows file's icon at size, in place of what it showed.
void set_icon(jadefx::StackPane& box, const std::string& file, double size) {
    box.getChildren().clear();
    if (std::shared_ptr<jadefx::ImageView> icon = icon_file(file)) {
        icon->setPrefSize(size, size);
        icon->setMinSize(size, size);
        icon->setMaxSize(size, size);
        icon->setMouseTransparent(true);
        box.getChildren().add(std::move(icon));
    }
}

// A word beside a field, such as "units".
std::shared_ptr<jadefx::Label> field_word(const std::string& text) { return text_label(text, "pe-lod-word"); }

}  // namespace

// One Model's card and the widgets that change with it.
struct IdePrefabEditor::Card {
    engine_core::InstanceId id = 0;
    std::shared_ptr<jadefx::VBox> root;
    std::shared_ptr<jadefx::Label> name;
    std::shared_ptr<NameField> rename;
    std::shared_ptr<jadefx::HBox> status;
    std::shared_ptr<jadefx::Label> status_text;
    using Slot = SlotWidgets;
    Slot slots[2];
    // Set by a button inside the card, so the click it bubbles to the card or slot next does nothing more.
    bool swallow = false;

    Slot& slot(ModelPart part) { return slots[part == ModelPart::Mesh ? 0 : 1]; }
};

// One LodLayer's card: where it takes over, its kind, and what it draws.
struct IdePrefabEditor::LodCard {
    engine_core::InstanceId id = 0;
    std::shared_ptr<jadefx::VBox> root;
    // Its kind's icon.
    std::shared_ptr<jadefx::StackPane> icon;
    engine_core::LodKind icon_kind = engine_core::LodKind::Model;
    std::shared_ptr<jadefx::Label> name;
    std::shared_ptr<jadefx::Label> range;
    std::shared_ptr<NumberField> distance;
    // In kLodKinds order.
    std::shared_ptr<jadefx::Button> kinds[3];
    // Holds the one of these its kind shows.
    std::shared_ptr<jadefx::VBox> body;
    // Model: its Models' cards, and its New Model tile.
    std::shared_ptr<jadefx::FlowPane> grid;
    std::shared_ptr<jadefx::Node> new_tile;
    // Billboard: its Material, its width and height, and Bake card.
    std::shared_ptr<jadefx::HBox> billboard;
    SlotWidgets material;
    std::shared_ptr<NumberField> size[2];
    std::shared_ptr<jadefx::Button> bake;
    // None.
    std::shared_ptr<jadefx::Label> none;
    // Set by a control inside the card, so the click it bubbles to the card does nothing more.
    bool swallow = false;
};

IdePrefabEditor::IdePrefabEditor(engine_core::DataModel& world, engine_core::InstanceId prefab, PrefabEditorHost host)
    : IdePane("Prefab", true), world_(world), prefab_(prefab), host_(std::move(host)) {
    setIconFile("ModelAlt.png");
    getClassList().add("prefab-editor");
    setStylesheet(kEditorRules);
    setMinSize(240, 180);
    setFocusTraversable(true);
    watch_ = world_.watch_changes(edited_.setter());

    // The header: the Prefab, a summary of its Models, and Add Model.
    auto header = jadefx::make<jadefx::HBox>();
    header->getClassList().add("pe-header");
    header->setAlignment(jadefx::Pos::CenterLeft);
    header->setStyle("width: 100%;");
    header->getChildren().add(icon_box("ModelAlt.png", 22, 38, "pe-badge"));
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(1);
    heading->setMouseTransparent(true);
    title_ = text_label("", "pe-title");
    subtitle_ = text_label("", "pe-subtitle");
    heading->getChildren().add(title_);
    heading->getChildren().add(subtitle_);
    header->getChildren().add(heading);
    header->getChildren().add(spacer());
    add_button_ = jadefx::make<jadefx::Button>("Add Model");
    add_button_->getClassList().add("pe-primary");
    if (std::shared_ptr<jadefx::ImageView> plus = icon_graphic("Plus.png")) {
        add_button_->setGraphic(std::move(plus));
    }
    add_button_->setGraphicTextGap(6);
    add_button_->setOnAction([this](jadefx::ActionEvent&) { addModel(); });
    jadefx::Tooltip::install(add_button_.get(),
                             jadefx::make<jadefx::Tooltip>("Add a Model, then pick its Mesh and Material"));
    add_lod_button_ = jadefx::make<jadefx::Button>("Add LOD");
    add_lod_button_->getClassList().add("pe-secondary");
    if (std::shared_ptr<jadefx::ImageView> plus = icon_graphic("Plus.png")) {
        add_lod_button_->setGraphic(std::move(plus));
    }
    add_lod_button_->setGraphicTextGap(6);
    add_lod_button_->setOnAction([this](jadefx::ActionEvent&) { addLod(); });
    jadefx::Tooltip::install(add_lod_button_.get(),
                             jadefx::make<jadefx::Tooltip>("Add what this Prefab draws instead, farther away"));
    header->getChildren().add(add_lod_button_);
    header->getChildren().add(add_button_);

    // The page: the Models, drawn up close, and the New Model tile after
    // them; then, once there are any, the LodLayers that take over farther out,
    // each band under its heading.
    grid_ = jadefx::make<jadefx::FlowPane>(kGap, kGap);
    grid_->getClassList().add("pe-grid");
    grid_->setStyle("width: 100%;");
    new_tile_ = make_new_tile(0);
    grid_->getChildren().add(new_tile_);
    base_heading_ = jadefx::make<jadefx::HBox>();
    base_heading_->getClassList().add("pe-band");
    base_heading_->setAlignment(jadefx::Pos::CenterLeft);
    base_heading_->setStyle("width: 100%;");
    base_heading_->setMouseTransparent(true);
    base_heading_->getChildren().add(text_label("UP CLOSE", "pe-band-title"));
    base_range_ = text_label("", "pe-band-hint");
    base_heading_->getChildren().add(base_range_);
    lod_heading_ = jadefx::make<jadefx::HBox>();
    lod_heading_->getClassList().add("pe-band");
    lod_heading_->setAlignment(jadefx::Pos::CenterLeft);
    lod_heading_->setStyle("width: 100%;");
    lod_heading_->setMouseTransparent(true);
    lod_heading_->getChildren().add(text_label("FARTHER AWAY", "pe-band-title"));
    lod_heading_->getChildren().add(text_label("What it draws instead, nearest first", "pe-band-hint"));
    lod_section_ = jadefx::make<jadefx::VBox>();
    lod_section_->getClassList().add("pe-lods");
    lod_section_->setStyle("width: 100%;");
    page_ = jadefx::make<jadefx::VBox>();
    page_->setStyle("width: 100%;");
    page_->getChildren().add(grid_);

    scroll_ = jadefx::make<jadefx::ScrollPane>(page_);
    scroll_->getClassList().add("pe-scroll");
    scroll_->setFitToWidth(true);
    scroll_->setHbarPolicy(jadefx::ScrollBarPolicy::Never);
    Fill(*scroll_);

    // No Models yet: what a Model is, and the way to make one.
    auto empty = jadefx::make<jadefx::VBox>();
    empty->getClassList().add("pe-empty");
    empty->setAlignment(jadefx::Pos::Center);
    Fill(*empty);
    empty->getChildren().add(icon_box("ModelAlt.png", 34, 72, "pe-empty-art"));
    empty->getChildren().add(text_label("No models yet", "pe-empty-title"));
    empty->getChildren().add(
        text_label("A Model pairs a Mesh with a Material. Add one to start building this Prefab.", "pe-empty-text"));
    auto start = jadefx::make<jadefx::Button>("Add Model");
    start->getClassList().add("pe-primary");
    if (std::shared_ptr<jadefx::ImageView> plus = icon_graphic("Plus.png")) {
        start->setGraphic(std::move(plus));
    }
    start->setGraphicTextGap(6);
    start->setOnAction([this](jadefx::ActionEvent&) { addModel(); });
    empty->getChildren().add(start);
    empty->getChildren().add(text_label("or drop a Mesh or Material from Assets anywhere here", "pe-empty-text"));
    empty->setVisible(false);
    empty_ = empty;

    auto gone = jadefx::make<jadefx::VBox>();
    gone->getClassList().add("pe-empty");
    gone->setAlignment(jadefx::Pos::Center);
    Fill(*gone);
    gone->getChildren().add(icon_box("Warning.png", 30, 72, "pe-empty-art"));
    gone->getChildren().add(text_label("This Prefab no longer exists", "pe-empty-title"));
    gone->getChildren().add(text_label("It was deleted, or the place it was in was closed.", "pe-empty-text"));
    gone->setVisible(false);
    gone_ = gone;

    auto canvas = jadefx::make<jadefx::StackPane>();
    canvas->getClassList().add("pe-canvas");
    canvas->setAlignment(jadefx::Pos::TopLeft);
    Fill(*canvas);
    canvas->getChildren().add(scroll_);
    canvas->getChildren().add(empty);
    canvas->getChildren().add(gone);
    // Empty space takes a drop as a new Model.
    accept_drops(*canvas, 0, nullptr);
    canvas->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        // Clicks bubble here from the cards and the tile, which mark them taken.
        if (took_click_) {
            took_click_ = false;
            return;
        }
        // A click on empty space clears the selection of this Prefab's Models.
        if (event.button == 0 && !event.shortcut() && !selected_models().empty()) {
            world_.selection().set({});
        }
        requestFocus();
    });

    auto root = jadefx::make<jadefx::BorderPane>();
    Fill(*root);
    root->setTop(header);
    root->setCenter(canvas);
    getChildren().add(root);

    picker_ = AssetPicker::create();
}

IdePrefabEditor::~IdePrefabEditor() {
    if (picker_) {
        picker_->dismiss();
    }
    if (menu_) {
        menu_->hide();
    }
    world_.unwatch_changes(watch_);
}

void IdePrefabEditor::layoutChildren() {
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (lock.owns()) {
            refresh();
            if (world_.selection().revision() != selection_seen_) {
                selected_ = world_.selection().get(selection_seen_);
                show_selection();
            }
            if (pending_add_ && pending_add_->done.load(std::memory_order_acquire)) {
                const engine_core::InstanceId made = pending_add_->id.load(std::memory_order_relaxed);
                if (made != 0) {
                    world_.selection().set({made});
                    // Ask for what it still needs, once its card is in view.
                    const ModelView* view = model_view(made);
                    if (view == nullptr || view->mesh.id == 0) {
                        pending_picker_ = PendingPicker{made, ModelPart::Mesh, true};
                    } else if (view->material.id == 0) {
                        pending_picker_ = PendingPicker{made, ModelPart::Material, false};
                    }
                } else if (host_.notice && !pending_add_->error.empty()) {
                    host_.notice(pending_add_->error);
                }
                pending_add_.reset();
            }
            if (pending_lod_ && pending_lod_->done.load(std::memory_order_acquire)) {
                const engine_core::InstanceId made = pending_lod_->id.load(std::memory_order_relaxed);
                if (made != 0) {
                    world_.selection().set({made});
                    reveal_lod_ = made;
                    reveal_waited_ = 0;
                } else if (host_.notice && !pending_lod_->error.empty()) {
                    host_.notice(pending_lod_->error);
                }
                pending_lod_.reset();
            }
        }
    }
    if (renaming_ != 0) {
        jadefx::TextField* field = renameField(renaming_);
        if (field == nullptr || !field->isFocused()) {
            finish_rename(false);
        }
    }
    IdePane::layoutChildren();
    // A picker waits for its card to be laid out in view.
    if (pending_picker_) {
        jadefx::Node* slot = slotNode(pending_picker_->model, pending_picker_->part);
        const bool placed = slot != nullptr && slot->getWidth() > 0 && slot->getAbsoluteY() >= scroll_->getAbsoluteY() &&
                            slot->getAbsoluteY() + slot->getHeight() <= scroll_->getAbsoluteY() + scroll_->getHeight();
        // Laid out but out of view: scrolled to, and placed next frame.
        if (!placed && slot != nullptr && slot->getWidth() > 0) {
            reveal(*slot);
        }
        if (placed || ++pending_picker_->waited > kPickerWaitFrames) {
            const PendingPicker pending = *pending_picker_;
            pending_picker_.reset();
            if (slot != nullptr) {
                openPicker(pending.model, pending.part, pending.guided);
            }
        }
    }
    // A new layer's card comes into view once it is laid out.
    if (reveal_lod_ != 0) {
        const LodCard* card = lod_card(reveal_lod_);
        if (card != nullptr && card->root->getHeight() > 0) {
            reveal(*card->root);
            reveal_lod_ = 0;
        } else if (++reveal_waited_ > kPickerWaitFrames) {
            reveal_lod_ = 0;
        }
    }
    // The prefab and the selection change on their own, so the editor checks them again next frame.
    markLayoutDirty(LayoutDirt::Arrange);
}

void IdePrefabEditor::refresh() {
    if (!bake_shown_) {
        bake_shown_ = true;
        sync_cards();
    }
    const std::uint64_t tree = world_.tree_revision();
    // Taken before the read, so a change made while reading reads again next frame.
    const bool edited = edited_.take();
    if (tree == seen_tree_ && !edited) {
        return;
    }
    seen_tree_ = tree;
    const engine_core::DataModel* object = world_.instance(prefab_);
    prefab_alive_ = dynamic_cast<const engine_core::Prefab*>(object) != nullptr;
    std::vector<ModelView> models;
    std::vector<LodView> lods;
    if (prefab_alive_) {
        prefab_name_ = world_.name(prefab_);
        setTitle(prefab_name_);
        models = read_models(world_, prefab_);
        lods = read_lods(world_, prefab_);
    }
    // Every Model and layer shown, so an edit to any of them reads them again.
    std::vector<engine_core::InstanceId> watched;
    for (const ModelView& view : models) {
        watched.push_back(view.id);
    }
    for (const LodView& lod : lods) {
        watched.push_back(lod.id);
        for (const ModelView& view : lod.models) {
            watched.push_back(view.id);
        }
    }
    if (watched != watched_) {
        watched_ = watched;
        world_.set_watched(watch_, std::move(watched));
    }
    if (models != models_ || lods != lods_) {
        models_ = std::move(models);
        lods_ = std::move(lods);
        sync_cards();
        show_selection();
    }
    show_header();
}

void IdePrefabEditor::show_header() {
    title_->setText(prefab_alive_ ? prefab_name_ : std::string("Prefab"));
    std::string summary;
    if (!prefab_alive_) {
        summary = "Deleted";
    } else if (models_.empty() && lods_.empty()) {
        summary = "No models yet";
    } else {
        const auto needing = [](const std::vector<ModelView>& views) {
            return static_cast<std::size_t>(std::count_if(views.begin(), views.end(), [](const ModelView& view) {
                return view.status() != ModelStatus::Ready;
            }));
        };
        std::size_t attention = needing(models_);
        for (const LodView& lod : lods_) {
            if (lod.kind == engine_core::LodKind::Model) {
                attention += needing(lod.models);
            }
        }
        summary = std::to_string(models_.size()) + (models_.size() == 1 ? " model" : " models");
        if (!lods_.empty()) {
            summary += "  ·  " + std::to_string(lods_.size()) + (lods_.size() == 1 ? " LOD" : " LODs");
        }
        summary += attention == 0 ? std::string("  ·  All ready")
                                  : "  ·  " + std::to_string(attention) + (attention == 1 ? " needs" : " need") +
                                        " attention";
    }
    subtitle_->setText(summary);
    add_button_->setDisable(!prefab_alive_);
    add_lod_button_->setDisable(!prefab_alive_);
    gone_->setVisible(!prefab_alive_);
    empty_->setVisible(prefab_alive_ && models_.empty() && lods_.empty());
    scroll_->setVisible(prefab_alive_ && (!models_.empty() || !lods_.empty()));
}

void IdePrefabEditor::sync_cards() {
    // Every grid empties first, so a Model moved from one layer to another
    // takes its card along.
    grid_->getChildren().clear();
    for (const std::shared_ptr<LodCard>& card : lod_cards_) {
        card->grid->getChildren().clear();
    }
    // Keeps the card of a Model or layer still here, so a rename, a pick, or typing in it survives.
    std::vector<std::shared_ptr<Card>> kept;
    sync_grid(*grid_, models_, new_tile_, kept);
    std::vector<std::shared_ptr<LodCard>> lods;
    for (const LodView& view : lods_) {
        auto found = std::find_if(lod_cards_.begin(), lod_cards_.end(),
                                  [&view](const std::shared_ptr<LodCard>& card) { return card->id == view.id; });
        std::shared_ptr<LodCard> card = found != lod_cards_.end() ? *found : make_lod_card(view);
        fill_lod_card(*card, view, kept);
        lods.push_back(std::move(card));
    }
    if (renaming_ != 0 && std::none_of(kept.begin(), kept.end(),
                                       [this](const std::shared_ptr<Card>& card) { return card->id == renaming_; })) {
        renaming_ = 0;
    }
    cards_ = std::move(kept);
    lod_cards_ = std::move(lods);
    lod_section_->getChildren().clear();
    for (const std::shared_ptr<LodCard>& card : lod_cards_) {
        lod_section_->getChildren().add(card->root);
    }
    // The bands' headings and the LOD section show while there are LODs.
    page_->getChildren().clear();
    if (!lods_.empty()) {
        const double first = lods_.front().distance;
        base_range_->setText(first > 0.0 ? "Drawn up to " + distance_text(first) + " units"
                                         : "Never drawn: the nearest LOD starts at 0 units");
        page_->getChildren().add(base_heading_);
    }
    page_->getChildren().add(grid_);
    if (!lods_.empty()) {
        page_->getChildren().add(lod_heading_);
        page_->getChildren().add(lod_section_);
    }
}

void IdePrefabEditor::sync_grid(jadefx::FlowPane& grid, const std::vector<ModelView>& models,
                                const std::shared_ptr<jadefx::Node>& tile, std::vector<std::shared_ptr<Card>>& kept) {
    grid.getChildren().clear();
    for (const ModelView& view : models) {
        auto found = std::find_if(cards_.begin(), cards_.end(),
                                  [&view](const std::shared_ptr<Card>& card) { return card->id == view.id; });
        std::shared_ptr<Card> card = found != cards_.end() ? *found : make_card(view);
        fill_card(*card, view);
        grid.getChildren().add(card->root);
        kept.push_back(std::move(card));
    }
    grid.getChildren().add(tile);
}

std::shared_ptr<jadefx::Node> IdePrefabEditor::make_new_tile(engine_core::InstanceId holder) {
    auto tile = jadefx::make<jadefx::VBox>();
    tile->getClassList().add("pe-new");
    tile->setAlignment(jadefx::Pos::Center);
    tile->setPrefSize(kCardWidth, kTileHeight);
    tile->setMinSize(kCardWidth, kTileHeight);
    if (holder != 0) {
        tile->setElementId("pe-new:" + std::to_string(holder));
    }
    if (std::shared_ptr<jadefx::StackPane> plus = icon_box("Plus.png", 22, 30, "pe-new-plus")) {
        tile->getChildren().add(std::move(plus));
    }
    tile->getChildren().add(text_label("New Model", "pe-new-title"));
    tile->getChildren().add(text_label("or drop a Mesh or Material here", "pe-new-hint"));
    tile->setOnMouseClicked([this, holder](const jadefx::MouseEvent& event) {
        took_click_ = true;
        if (event.button == 0) {
            addModel(holder);
        }
    });
    accept_drops(*tile, 0, nullptr, holder);
    return tile;
}

std::shared_ptr<IdePrefabEditor::LodCard> IdePrefabEditor::make_lod_card(const LodView& view) {
    auto card = std::make_shared<LodCard>();
    card->id = view.id;
    const engine_core::InstanceId id = view.id;
    LodCard* raw = card.get();

    card->root = jadefx::make<jadefx::VBox>();
    card->root->getClassList().add("pe-lod");
    card->root->setStyle("width: 100%;");
    card->root->setElementId("pe-lod:" + std::to_string(id));

    // Icon, name over its range; the distance it takes over from; its kind; delete.
    // The heading takes what is left, so a long name ends in an ellipsis.
    auto top = std::make_shared<StretchRow>(1);
    top->setSpacing(12);
    card->icon = icon_box(lod_kind_icon(view.kind), 18, 32, "pe-card-icon");
    card->icon_kind = view.kind;
    top->getChildren().add(card->icon);
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(2);
    heading->setStyle("width: 100%;");
    heading->setMinSize(0, 0);
    heading->setMouseTransparent(true);
    card->name = text_label(view.name, "pe-name");
    card->range = text_label("", "pe-lod-range");
    for (const std::shared_ptr<jadefx::Label>& label : {card->name, card->range}) {
        label->setMinSize(0, 0);
        label->setStyle("width: 100%;");
        heading->getChildren().add(label);
    }
    top->getChildren().add(heading);

    auto from = jadefx::make<jadefx::HBox>();
    from->setSpacing(6);
    from->setAlignment(jadefx::Pos::CenterLeft);
    from->getChildren().add(field_word("From"));
    card->distance = std::make_shared<NumberField>(0.0, false, [this, id](double value) {
        if (host_.set_lod_distance) {
            host_.set_lod_distance(id, value);
        }
    });
    card->distance->setElementId("pe-lod-distance:" + std::to_string(id));
    card->distance->setPrefWidth(70);
    card->distance->setMinSize(70, 0);
    jadefx::Tooltip::install(card->distance.get(),
                             jadefx::make<jadefx::Tooltip>("How far from the camera this LOD takes over, in units"));
    from->getChildren().add(card->distance);
    from->getChildren().add(field_word("units"));
    top->getChildren().add(from);

    auto kinds = jadefx::make<jadefx::HBox>();
    kinds->getClassList().add("pe-kinds");
    kinds->setAlignment(jadefx::Pos::CenterLeft);
    for (std::size_t index = 0; index < std::size(kLodKinds); ++index) {
        const engine_core::LodKind kind = kLodKinds[index];
        auto button = jadefx::make<jadefx::Button>(lod_kind_text(kind));
        button->getClassList().add("pe-kind");
        button->setElementId("pe-lod-kind:" + std::to_string(id) + ":" + lod_kind_text(kind));
        button->setOnAction([this, id, kind](jadefx::ActionEvent&) {
            const LodView* shown = lod_view(id);
            if (shown != nullptr && shown->kind != kind && host_.set_lod_kind) {
                host_.set_lod_kind(id, kind);
            }
        });
        jadefx::Tooltip::install(button.get(), jadefx::make<jadefx::Tooltip>(lod_kind_tip(kind)));
        kinds->getChildren().add(button);
        card->kinds[index] = std::move(button);
    }
    top->getChildren().add(kinds);
    top->getChildren().add(icon_button("Cross.png", "Delete LOD", "pe-lod-delete", [this, raw, id] {
        raw->swallow = true;
        if (host_.remove) {
            host_.remove({id});
        }
    }));
    card->root->getChildren().add(top);

    card->body = jadefx::make<jadefx::VBox>();
    card->body->setStyle("width: 100%;");
    card->root->getChildren().add(card->body);

    // Model: its Models' cards, then its New Model tile. Drops on the grid add to it.
    card->grid = jadefx::make<jadefx::FlowPane>(kGap, kGap);
    card->grid->setStyle("width: 100%;");
    card->new_tile = make_new_tile(id);
    accept_drops(*card->grid, 0, nullptr, id);

    // Billboard: its Texture and its size. A Texture dropped anywhere on it fills the slot.
    card->billboard = jadefx::make<jadefx::HBox>();
    card->billboard->getClassList().add("pe-lod-billboard");
    card->billboard->setAlignment(jadefx::Pos::TopLeft);
    auto material = jadefx::make<jadefx::VBox>();
    material->getClassList().add("pe-section");
    material->setPrefWidth(kCardWidth);
    material->setMinSize(kCardWidth, 0);
    material->getChildren().add(text_label("MATERIAL", "pe-slot-label"));
    build_slot(card->material, part_icon(ModelPart::Material), "pe-lod-material:" + std::to_string(id),
               "Clear Material", [this, raw, id] {
                   raw->swallow = true;
                   if (host_.set_lod_material) {
                       host_.set_lod_material(id, 0);
                   }
               });
    card->material.root->setOnMouseClicked([this, raw, id](const jadefx::MouseEvent& event) {
        if (event.button != 0 || raw->swallow) {
            return;
        }
        raw->swallow = true;
        world_.selection().set({id});
        requestFocus();
        openMaterialPicker(id);
    });
    material->getChildren().add(card->material.root);
    card->billboard->getChildren().add(material);
    auto size = jadefx::make<jadefx::VBox>();
    size->getClassList().add("pe-section");
    size->getChildren().add(text_label("SIZE", "pe-slot-label"));
    auto size_row = jadefx::make<jadefx::HBox>();
    size_row->setSpacing(6);
    size_row->setAlignment(jadefx::Pos::CenterLeft);
    for (int axis = 0; axis < 2; ++axis) {
        card->size[axis] = std::make_shared<NumberField>(0.0, true, [this, id, axis](double value) {
            const LodView* shown = lod_view(id);
            if (shown == nullptr || !host_.set_lod_size) {
                return;
            }
            host_.set_lod_size(id, axis == 0 ? static_cast<float>(value) : shown->width,
                               axis == 1 ? static_cast<float>(value) : shown->height);
        });
        card->size[axis]->setElementId("pe-lod-size:" + std::to_string(id) + ":" + std::to_string(axis));
        card->size[axis]->setPrefWidth(56);
        card->size[axis]->setMinSize(56, 0);
        jadefx::Tooltip::install(card->size[axis].get(),
                                 jadefx::make<jadefx::Tooltip>(axis == 0 ? "The card's width, in units"
                                                                         : "The card's height, in units"));
        size_row->getChildren().add(card->size[axis]);
        if (axis == 0) {
            size_row->getChildren().add(field_word("by"));
        }
    }
    size_row->getChildren().add(field_word("units"));
    size->getChildren().add(size_row);
    card->billboard->getChildren().add(size);
    // Bake card: the Prefab's own Models drawn onto the card, its texture and size.
    auto bake = jadefx::make<jadefx::VBox>();
    bake->getClassList().add("pe-section");
    bake->getChildren().add(text_label("FROM ITS MODELS", "pe-slot-label"));
    card->bake = jadefx::make<jadefx::Button>("Bake card");
    card->bake->getClassList().add("pe-secondary");
    card->bake->setElementId("pe-lod-bake:" + std::to_string(id));
    card->bake->setOnAction([this, id](jadefx::ActionEvent&) { bakeCard(id); });
    jadefx::Tooltip::install(card->bake.get(),
                             jadefx::make<jadefx::Tooltip>("Draw this Prefab's Models onto a card, and use it as this "
                                                           "LOD's Material and size"));
    bake->getChildren().add(card->bake);
    card->billboard->getChildren().add(bake);
    accept_material_drops(*card->billboard, id);

    // None: what it saves.
    card->none = text_label("", "pe-lod-note");

    card->root->setOnMouseClicked([this, raw, id](const jadefx::MouseEvent& event) {
        // A Model card, a tile, or a control inside took this click already.
        if (raw->swallow || took_click_) {
            raw->swallow = false;
            return;
        }
        took_click_ = true;
        if (event.button == 0) {
            clicked_lod(id, event);
        }
    });
    return card;
}

void IdePrefabEditor::fill_lod_card(LodCard& card, const LodView& view, std::vector<std::shared_ptr<Card>>& kept) {
    if (card.name->getText() != view.name) {
        card.name->setText(view.name);
    }
    if (card.icon_kind != view.kind) {
        card.icon_kind = view.kind;
        set_icon(*card.icon, lod_kind_icon(view.kind), 18);
    }
    const std::string from = distance_text(view.distance);
    std::string range;
    if (view.covered) {
        range = "Never drawn: the next LOD starts at " + from + " units too";
    } else if (view.until) {
        range = from + " to " + distance_text(*view.until) + " units";
    } else {
        range = from + " units and beyond";
    }
    card.range->setText(range);
    set_class(*card.range, "covered", view.covered);
    card.distance->show(view.distance);
    for (std::size_t index = 0; index < std::size(kLodKinds); ++index) {
        set_class(*card.kinds[index], "selected", kLodKinds[index] == view.kind);
    }
    // The body shows what its kind draws.
    std::shared_ptr<jadefx::Node> body;
    switch (view.kind) {
    case engine_core::LodKind::Model:
        sync_grid(*card.grid, view.models, card.new_tile, kept);
        body = card.grid;
        break;
    case engine_core::LodKind::Billboard:
        show_slot(card.material, part_icon(ModelPart::Material), "material", "Material", view.material);
        card.size[0]->show(view.width);
        card.size[1]->show(view.height);
        card.bake->setText(baking_ == view.id ? "Baking..." : "Bake card");
        card.bake->setDisable(baking_ != 0 || !host_.set_lod_card);
        body = card.billboard;
        break;
    case engine_core::LodKind::None:
        card.none->setText("Draws nothing from " + from + " units on, so far-off copies cost nothing to draw.");
        body = card.none;
        break;
    }
    const auto& shown = card.body->getChildren().items();
    if (shown.size() != 1 || shown.front() != body) {
        card.body->getChildren().clear();
        card.body->getChildren().add(body);
    }
}

void IdePrefabEditor::clicked_lod(engine_core::InstanceId layer, const jadefx::MouseEvent& event) {
    // Typing in one of its fields keeps the focus there.
    const jadefx::Scene* scene = getScene();
    if (scene == nullptr || dynamic_cast<const jadefx::TextField*>(scene->focusedNode()) == nullptr) {
        requestFocus();
    }
    std::vector<engine_core::InstanceId> ids = selected_;
    if (event.shortcut()) {
        const auto found = std::find(ids.begin(), ids.end(), layer);
        if (found != ids.end()) {
            ids.erase(found);
        } else {
            ids.push_back(layer);
        }
    } else {
        ids = {layer};
    }
    world_.selection().set(ids);
}

void IdePrefabEditor::reveal(const jadefx::Node& node) {
    const double viewport = scroll_->getHeight();
    const double range = page_->getHeight() - viewport;
    const double span = scroll_->getVmax() - scroll_->getVmin();
    if (range <= 0.0 || span <= 0.0) {
        return;
    }
    const double offset = (scroll_->getVvalue() - scroll_->getVmin()) / span * range;
    const double top = node.getAbsoluteY() - page_->getAbsoluteY();
    const double bottom = top + node.getHeight();
    double wanted = offset;
    if (top < offset) {
        wanted = top - kGap;
    } else if (bottom > offset + viewport) {
        wanted = std::min(top - kGap, bottom - viewport + kGap);
    }
    wanted = std::clamp(wanted, 0.0, range);
    if (wanted != offset) {
        scroll_->setVvalue(scroll_->getVmin() + span * wanted / range);
    }
}

std::shared_ptr<IdePrefabEditor::Card> IdePrefabEditor::make_card(const ModelView& view) {
    auto card = std::make_shared<Card>();
    card->id = view.id;
    const engine_core::InstanceId id = view.id;
    Card* raw = card.get();

    card->root = jadefx::make<jadefx::VBox>();
    card->root->getClassList().add("pe-card");
    card->root->setPrefWidth(kCardWidth);
    card->root->setMinSize(kCardWidth, 0);
    card->root->setMaxSize(kCardWidth, 100000);
    card->root->setElementId("pe-card:" + std::to_string(id));

    // Icon, name over status, and delete.
    // The heading takes what is left, so a long name ends in an ellipsis.
    auto top = std::make_shared<StretchRow>(1);
    top->setSpacing(10);
    top->getChildren().add(icon_box("Model.png", 18, 32, "pe-card-icon"));
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(2);
    heading->setStyle("width: 100%;");
    heading->setMinSize(0, 0);
    auto name_stack = jadefx::make<jadefx::StackPane>();
    name_stack->setMinSize(0, 0);
    name_stack->setStyle("width: 100%;");
    name_stack->setAlignment(jadefx::Pos::CenterLeft);
    card->name = jadefx::make<jadefx::Label>(view.name);
    card->name->getClassList().add("pe-name");
    card->name->setMinSize(0, 0);
    card->name->setStyle("width: 100%;");
    card->name->setOnMouseClicked([this, id](const jadefx::MouseEvent& event) {
        if (event.button == 0 && event.clickCount == 2) {
            beginRename(id);
        }
    });
    card->rename = std::make_shared<NameField>([this] { finish_rename(false); });
    card->rename->setOnAction([this](jadefx::ActionEvent&) { finish_rename(true); });
    name_stack->getChildren().add(card->name);
    name_stack->getChildren().add(card->rename);
    heading->getChildren().add(name_stack);
    card->status = jadefx::make<jadefx::HBox>();
    card->status->getClassList().add("pe-status");
    card->status->setAlignment(jadefx::Pos::CenterLeft);
    card->status->setMouseTransparent(true);
    auto dot = jadefx::make<jadefx::Pane>();
    dot->getClassList().add("pe-dot");
    dot->setPrefSize(7, 7);
    dot->setMinSize(7, 7);
    dot->setMaxSize(7, 7);
    card->status->getChildren().add(dot);
    card->status_text = text_label("", "pe-status-text");
    card->status->getChildren().add(card->status_text);
    heading->getChildren().add(card->status);
    top->getChildren().add(heading);
    top->getChildren().add(icon_button("Cross.png", "Delete Model", "pe-card-delete", [this, raw, id] {
        raw->swallow = true;
        if (host_.remove) {
            host_.remove({id});
        }
    }));
    card->root->getChildren().add(top);

    // A slot for each part.
    for (const ModelPart part : {ModelPart::Mesh, ModelPart::Material}) {
        Card::Slot& slot = card->slot(part);
        auto section = jadefx::make<jadefx::VBox>();
        section->getClassList().add("pe-section");
        section->getChildren().add(text_label(part == ModelPart::Mesh ? "MESH" : "MATERIAL", "pe-slot-label"));
        build_slot(slot, part_icon(part), std::string("pe-slot:") + std::to_string(id) + ":" + model_part_name(part),
                   std::string("Clear ") + model_part_name(part), [this, raw, id, part] {
                       raw->swallow = true;
                       if (host_.set_part) {
                           host_.set_part(id, part, 0);
                       }
                   });
        slot.root->setOnMouseClicked([this, raw, id, part](const jadefx::MouseEvent& event) {
            if (event.button != 0 || raw->swallow) {
                return;
            }
            raw->swallow = true;
            world_.selection().set({id});
            requestFocus();
            openPicker(id, part);
        });
        const ModelPart* which = part == ModelPart::Mesh ? &kParts[0] : &kParts[1];
        accept_drops(*slot.root, id, which);
        section->getChildren().add(slot.root);
        card->root->getChildren().add(section);
    }

    card->root->setOnMouseClicked([this, raw, id](const jadefx::MouseEvent& event) {
        took_click_ = true;
        // A button or slot inside took this click already.
        if (raw->swallow) {
            raw->swallow = false;
            return;
        }
        if (event.button == 0) {
            clicked_card(id, event);
        }
    });
    card->root->setOnContextMenuRequested(
        [this, id](const jadefx::MouseEvent& event) { show_card_menu(id, event.x, event.y); });
    accept_drops(*card->root, id, nullptr);
    return card;
}

void IdePrefabEditor::fill_card(Card& card, const ModelView& view) {
    if (card.name->getText() != view.name) {
        card.name->setText(view.name);
    }
    const ModelStatus status = view.status();
    card.status_text->setText(model_status_text(status));
    set_class(*card.status, "ready", status == ModelStatus::Ready);
    set_class(*card.status, "missing", status == ModelStatus::Missing);
    fill_slot(card, ModelPart::Mesh, view.mesh);
    fill_slot(card, ModelPart::Material, view.material);
}

void IdePrefabEditor::fill_slot(Card& card, ModelPart part, const PartView& view) {
    show_slot(card.slot(part), part_icon(part), part_word(part), model_part_name(part), view);
}

void IdePrefabEditor::show_selection() {
    const auto on = [this](engine_core::InstanceId id) {
        return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
    };
    for (const std::shared_ptr<Card>& card : cards_) {
        set_class(*card->root, "selected", on(card->id));
    }
    for (const std::shared_ptr<LodCard>& card : lod_cards_) {
        set_class(*card->root, "selected", on(card->id));
    }
}

void IdePrefabEditor::clicked_card(engine_core::InstanceId model, const jadefx::MouseEvent& event) {
    requestFocus();
    std::vector<engine_core::InstanceId> ids = selected_;
    if (event.shortcut()) {
        const auto found = std::find(ids.begin(), ids.end(), model);
        if (found != ids.end()) {
            ids.erase(found);
        } else {
            ids.push_back(model);
        }
    } else if (event.shift() && anchor_ != 0) {
        // Every card from the anchor to this one, in the order shown.
        auto from = std::find_if(models_.begin(), models_.end(), [this](const ModelView& v) { return v.id == anchor_; });
        auto to = std::find_if(models_.begin(), models_.end(), [model](const ModelView& v) { return v.id == model; });
        ids.clear();
        if (from != models_.end() && to != models_.end()) {
            if (from > to) {
                std::swap(from, to);
            }
            for (auto it = from; it <= to; ++it) {
                ids.push_back(it->id);
            }
        } else {
            ids.push_back(model);
        }
    } else {
        ids = {model};
    }
    if (!event.shift()) {
        anchor_ = model;
    }
    world_.selection().set(ids);
}

void IdePrefabEditor::show_card_menu(engine_core::InstanceId model, double x, double y) {
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        return;
    }
    // A right-click on a selected card keeps the rest of the selection.
    if (std::find(selected_.begin(), selected_.end(), model) == selected_.end()) {
        world_.selection().set({model});
    }
    std::vector<engine_core::InstanceId> ids = selected_models();
    if (std::find(ids.begin(), ids.end(), model) == ids.end()) {
        ids = {model};
    }
    if (menu_) {
        menu_->hide();
    }
    menu_ = jadefx::make<jadefx::Menu>();
    auto add = [this](const std::string& text, const char* icon, std::function<void()> run) {
        auto item = jadefx::make<jadefx::MenuItem>(text);
        if (std::shared_ptr<jadefx::ImageView> image = icon_graphic(icon)) {
            item->setGraphic(std::move(image));
        }
        item->setOnAction([run = std::move(run)](jadefx::ActionEvent&) { run(); });
        menu_->getItems().add(item);
        return item;
    };
    add("Choose Mesh", part_icon(ModelPart::Mesh), [this, model] { openPicker(model, ModelPart::Mesh); });
    add("Choose Material", part_icon(ModelPart::Material), [this, model] { openPicker(model, ModelPart::Material); });
    menu_->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    add("Rename", "Rename.png", [this, model] { beginRename(model); })->setDisable(ids.size() != 1);
    add(ids.size() == 1 ? "Delete" : "Delete " + std::to_string(ids.size()) + " Models", "Cross.png", [this, ids] {
        if (host_.remove) {
            host_.remove(ids);
        }
    });
    menu_->show(*scene, x, y);
}

void IdePrefabEditor::addModel(engine_core::InstanceId holder) {
    if (!prefab_alive_ || !host_.add_model || pending_add_) {
        return;
    }
    closePicker();
    pending_add_ = std::make_shared<InsertResult>();
    host_.add_model(holder != 0 ? holder : prefab_, 0, 0, pending_add_);
}

void IdePrefabEditor::addLod() {
    if (!prefab_alive_ || !host_.add_lod || pending_lod_) {
        return;
    }
    closePicker();
    pending_lod_ = std::make_shared<InsertResult>();
    host_.add_lod(prefab_, pending_lod_);
}

void IdePrefabEditor::bakeCard(engine_core::InstanceId layer) {
    const LodView* view = lod_view(layer);
    if (view == nullptr || view->kind != engine_core::LodKind::Billboard || baking_ != 0 || !host_.set_lod_card) {
        return;
    }
    baking_ = layer;
    bake_waits_ = 0;
    // The button says it is baking; the next paint draws it.
    sync_cards();
}

void IdePrefabEditor::end_bake(const std::string& why) {
    baking_ = 0;
    bake_waits_ = 0;
    if (!why.empty() && host_.notice) {
        host_.notice(why);
    }
    // Called while painting: the cards show it at the next layout.
    bake_shown_ = false;
    markLayoutDirty(LayoutDirt::Arrange);
}

void IdePrefabEditor::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    IdePane::renderContent(renderer, opacity);
    if (baking_ != 0) {
        bake_pending();
    }
}

void IdePrefabEditor::sceneChanged(jadefx::Scene* previous) {
    IdePane::sceneChanged(previous);
    // Leaving a live scene releases the baker's GL objects while the context
    // that made them is current; the next bake makes them again. Scene
    // teardown runs after JadeFX destroyed the context.
    if (previous == nullptr || previous->isTearingDown() || getScene() == previous || !baker_) {
        return;
    }
    baker_->release();
}

void IdePrefabEditor::bake_pending() {
    engine_core::VisualPrefab prefab;
    engine_core::Vec3 low{};
    engine_core::Vec3 high{};
    bool boxed = false;
    std::filesystem::path root;
    std::string name;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (!lock.owns()) {
            return;
        }
        const engine_core::InstanceId owner = world_.alive(baking_) ? world_.parent(baking_) : 0;
        const auto* holder = dynamic_cast<const engine_core::Prefab*>(world_.instance(owner));
        if (holder == nullptr) {
            end_bake("That LOD no longer exists");
            return;
        }
        engine_core::InstanceId cached = owner;
        engine_core::fill_visual_prefab(world_, world_.guid(owner), cached, prefab);
        boxed = holder->bounds(low, high);
        root = world_.resources_root();
        name = world_.name(owner);
    }
    if (!boxed || prefab.meshes.empty() || !(card_frame(low, high).height > 0.f)) {
        end_bake("A card is drawn from the Prefab's Models: give it one with a Mesh first");
        return;
    }
    if (root.empty()) {
        end_bake("Save the place first: its card is kept in the resources folder");
        return;
    }
    if (!baker_) {
        baker_ = std::make_unique<BillboardBaker>();
    }
    baker_->setRoot(root);
    CardMaps card;
    if (!baker_->bake(prefab, low, high, card)) {
        // A texture still loads: drawn whole on a later paint.
        if (++bake_waits_ > kBakeWaits) {
            end_bake("No card was baked: the Prefab's textures did not load, or the GPU would not draw it");
        }
        markLayoutDirty(LayoutDirt::Arrange);
        return;
    }
    if (card.color.empty()) {
        end_bake("The card could not be drawn");
        return;
    }
    CardFiles files;
    std::string why;
    if (!save_card(card, root, name, files, why)) {
        end_bake(why);
        return;
    }
    // The card as drawn: its height, and as wide as its pixels make it.
    files.height = card_frame(low, high).height;
    files.width = files.height * static_cast<float>(card.color.width) / static_cast<float>(card.color.height);
    const engine_core::InstanceId layer = baking_;
    end_bake("");
    host_.set_lod_card(layer, std::move(files));
}

void IdePrefabEditor::openMaterialPicker(engine_core::InstanceId layer) {
    const LodCard* card = lod_card(layer);
    const LodView* view = lod_view(layer);
    if (card == nullptr || view == nullptr || !picker_ || view->kind != engine_core::LodKind::Billboard) {
        return;
    }
    std::vector<AssetChoice> choices;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (!lock.owns()) {
            return;
        }
        choices = part_choices(world_, ModelPart::Material);
    }
    picker_->open(*card->material.root, model_part_name(ModelPart::Material), std::move(choices), view->material.id,
                  [this, layer](engine_core::InstanceId asset) {
                      const LodView* now = lod_view(layer);
                      if (now != nullptr && now->material.id != asset && host_.set_lod_material) {
                          host_.set_lod_material(layer, asset);
                      }
                  });
}

void IdePrefabEditor::openPicker(engine_core::InstanceId model, ModelPart part, bool guided) {
    jadefx::Node* anchor = slotNode(model, part);
    const ModelView* view = model_view(model);
    if (anchor == nullptr || view == nullptr || !picker_) {
        return;
    }
    std::vector<AssetChoice> choices;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (!lock.owns()) {
            return;
        }
        choices = part_choices(world_, part);
    }
    picker_->open(*anchor, model_part_name(part), std::move(choices), view->part(part).id,
                  [this, model, part, guided](engine_core::InstanceId asset) { picked(model, part, asset, guided); });
}

void IdePrefabEditor::closePicker() {
    if (picker_) {
        picker_->dismiss();
    }
}

bool IdePrefabEditor::pickerOpen() const { return picker_ && picker_->showing(); }

void IdePrefabEditor::picked(engine_core::InstanceId model, ModelPart part, engine_core::InstanceId asset, bool guided) {
    const ModelView* view = model_view(model);
    if (view != nullptr && view->part(part).id != asset && host_.set_part) {
        host_.set_part(model, part, asset);
    }
    // In a guided run, a Mesh leads on to the Material when that is still empty.
    if (guided && part == ModelPart::Mesh && asset != 0 && view != nullptr && view->material.id == 0) {
        pending_picker_ = PendingPicker{model, ModelPart::Material, false};
    }
}

void IdePrefabEditor::beginRename(engine_core::InstanceId model) {
    if (renaming_ != 0) {
        finish_rename(false);
    }
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id != model) {
            continue;
        }
        card->rename->setText(card->name->getText());
        card->rename->selectAll();
        card->rename->setVisible(true);
        card->name->setVisible(false);
        card->rename->requestFocus();
        renaming_ = model;
        return;
    }
}

void IdePrefabEditor::finish_rename(bool apply) {
    const engine_core::InstanceId model = renaming_;
    renaming_ = 0;
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id != model) {
            continue;
        }
        const bool had_focus = card->rename->isFocused();
        const std::string name = card->rename->getText();
        card->rename->setVisible(false);
        card->name->setVisible(true);
        if (apply && !name.empty() && name != card->name->getText() && host_.rename) {
            // Shown at once; the place catches up on the simulation thread.
            card->name->setText(name);
            host_.rename(model, name);
        }
        if (had_focus) {
            requestFocus();
        }
    }
}

std::vector<engine_core::InstanceId> IdePrefabEditor::selected_models() const {
    // The selected Models and layers this Prefab has, as the page shows them.
    std::vector<engine_core::InstanceId> ids;
    const auto take = [this, &ids](engine_core::InstanceId id) {
        if (std::find(selected_.begin(), selected_.end(), id) != selected_.end()) {
            ids.push_back(id);
        }
    };
    for (const ModelView& view : models_) {
        take(view.id);
    }
    for (const LodView& lod : lods_) {
        take(lod.id);
        for (const ModelView& view : lod.models) {
            take(view.id);
        }
    }
    return ids;
}

void IdePrefabEditor::handleKey(jadefx::KeyEvent& event) {
    const jadefx::Scene* scene = getScene();
    const jadefx::Node* focus = scene != nullptr ? scene->focusedNode() : nullptr;
    if (!event.pressed || dynamic_cast<const jadefx::TextField*>(focus) != nullptr) {
        IdePane::handleKey(event);
        return;
    }
    const std::vector<engine_core::InstanceId> ids = selected_models();
    if ((event.key == jadefx::Key::Delete || event.key == jadefx::Key::Backspace) && !ids.empty()) {
        event.consume();
        if (host_.remove) {
            host_.remove(ids);
        }
        return;
    }
    if ((event.key == jadefx::Key::F2 || event.key == jadefx::Key::Enter) && ids.size() == 1) {
        event.consume();
        beginRename(ids.front());
        return;
    }
    IdePane::handleKey(event);
}

std::vector<engine_core::InstanceId> IdePrefabEditor::dragged(const jadefx::DragEvent& event) const {
    if (event.dragboard == nullptr || !event.dragboard->has(kInstanceDragFormat)) {
        return {};
    }
    return instance_drag_ids(event.dragboard->get(kInstanceDragFormat));
}

void IdePrefabEditor::accept_drops(jadefx::Node& node, engine_core::InstanceId model, const ModelPart* part,
                                   engine_core::InstanceId holder) {
    jadefx::Node* raw = &node;
    node.setOnDragOver([this, raw, model, part](jadefx::DragEvent& event) {
        const std::vector<engine_core::InstanceId> ids = dragged(event);
        if (ids.empty() || !prefab_alive_) {
            return;
        }
        DraggedParts parts;
        {
            // A busy place refuses this over; the next one asks again.
            engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
            if (!lock.owns()) {
                return;
            }
            parts = dragged_parts(world_, ids);
        }
        const bool fits = part != nullptr ? (*part == ModelPart::Mesh ? parts.mesh : parts.material) != 0 : parts.any();
        if (!fits) {
            return;
        }
        // Filling a slot links to the asset; a drop on empty space makes a Model, so the pointer shows a plus.
        event.acceptTransferModes(model != 0 ? jadefx::TransferMode::Link : jadefx::TransferMode::Copy);
        event.consume();
        mark_drop(raw);
    });
    node.setOnDragExited([this, raw](jadefx::DragEvent&) {
        if (drop_mark_ == raw) {
            mark_drop(nullptr);
        }
    });
    node.setOnDragDropped([this, model, part, holder](jadefx::DragEvent& event) {
        const std::vector<engine_core::InstanceId> ids = dragged(event);
        if (ids.empty()) {
            return;
        }
        mark_drop(nullptr);
        event.setDropCompleted(dropOnto(ids, model, part, holder));
        event.consume();
    });
}

void IdePrefabEditor::accept_material_drops(jadefx::Node& node, engine_core::InstanceId layer) {
    jadefx::Node* raw = &node;
    node.setOnDragOver([this, raw](jadefx::DragEvent& event) {
        const std::vector<engine_core::InstanceId> ids = dragged(event);
        if (ids.empty() || !prefab_alive_) {
            return;
        }
        engine_core::InstanceId material = 0;
        {
            // A busy place refuses this over; the next one asks again.
            engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
            if (!lock.owns()) {
                return;
            }
            material = dragged_parts(world_, ids).material;
        }
        if (material == 0) {
            return;
        }
        event.acceptTransferModes(jadefx::TransferMode::Link);
        event.consume();
        mark_drop(raw);
    });
    node.setOnDragExited([this, raw](jadefx::DragEvent&) {
        if (drop_mark_ == raw) {
            mark_drop(nullptr);
        }
    });
    node.setOnDragDropped([this, layer](jadefx::DragEvent& event) {
        const std::vector<engine_core::InstanceId> ids = dragged(event);
        if (ids.empty()) {
            return;
        }
        mark_drop(nullptr);
        event.setDropCompleted(dropMaterial(ids, layer));
        event.consume();
    });
}

void IdePrefabEditor::mark_drop(jadefx::Node* node) {
    if (drop_mark_ == node) {
        return;
    }
    if (drop_mark_ != nullptr) {
        set_class(*drop_mark_, "drop", false);
    }
    drop_mark_ = node;
    if (node != nullptr) {
        set_class(*node, "drop", true);
    }
}

bool IdePrefabEditor::dropMaterial(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId layer) {
    engine_core::InstanceId material = 0;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kDropLockWait);
        if (!lock.owns()) {
            return false;
        }
        material = dragged_parts(world_, ids).material;
    }
    if (material == 0) {
        if (host_.notice) {
            host_.notice("Only a Material goes on a billboard");
        }
        return false;
    }
    if (!host_.set_lod_material) {
        return false;
    }
    host_.set_lod_material(layer, material);
    return true;
}

bool IdePrefabEditor::dropOnto(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId model,
                               const ModelPart* part, engine_core::InstanceId holder) {
    DraggedParts parts;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kDropLockWait);
        if (!lock.owns()) {
            return false;
        }
        parts = dragged_parts(world_, ids);
    }
    auto refuse = [this](std::string text) {
        if (host_.notice) {
            host_.notice(std::move(text));
        }
        return false;
    };
    if (model == 0) {
        if (!parts.any()) {
            return refuse("Drop a Mesh or a Material to make a Model");
        }
        if (!host_.add_model || pending_add_) {
            return false;
        }
        pending_add_ = std::make_shared<InsertResult>();
        host_.add_model(holder != 0 ? holder : prefab_, parts.mesh, parts.material, pending_add_);
        return true;
    }
    if (!host_.set_part) {
        return false;
    }
    if (part != nullptr) {
        const engine_core::InstanceId target = *part == ModelPart::Mesh ? parts.mesh : parts.material;
        if (target == 0) {
            return refuse(std::string("Only a ") + model_part_name(*part) + " goes in the " + model_part_name(*part) +
                          " slot");
        }
        host_.set_part(model, *part, target);
        return true;
    }
    if (!parts.any()) {
        return refuse("Drop a Mesh or a Material onto a Model");
    }
    if (parts.mesh != 0) {
        host_.set_part(model, ModelPart::Mesh, parts.mesh);
    }
    if (parts.material != 0) {
        host_.set_part(model, ModelPart::Material, parts.material);
    }
    return true;
}

const ModelView* IdePrefabEditor::model_view(engine_core::InstanceId model) const {
    for (const ModelView& view : models_) {
        if (view.id == model) {
            return &view;
        }
    }
    for (const LodView& lod : lods_) {
        for (const ModelView& view : lod.models) {
            if (view.id == model) {
                return &view;
            }
        }
    }
    return nullptr;
}

const LodView* IdePrefabEditor::lod_view(engine_core::InstanceId layer) const {
    for (const LodView& view : lods_) {
        if (view.id == layer) {
            return &view;
        }
    }
    return nullptr;
}

const IdePrefabEditor::LodCard* IdePrefabEditor::lod_card(engine_core::InstanceId layer) const {
    for (const std::shared_ptr<LodCard>& card : lod_cards_) {
        if (card->id == layer) {
            return card.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdePrefabEditor::cardNode(engine_core::InstanceId model) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == model) {
            return card->root.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdePrefabEditor::slotNode(engine_core::InstanceId model, ModelPart part) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == model) {
            return card->slot(part).root.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdePrefabEditor::slotClearNode(engine_core::InstanceId model, ModelPart part) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == model && card->slot(part).clear->isVisible()) {
            return card->slot(part).clear.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdePrefabEditor::addButton() const { return add_button_.get(); }
jadefx::Node* IdePrefabEditor::addLodButton() const { return add_lod_button_.get(); }

jadefx::Node* IdePrefabEditor::lodCard(engine_core::InstanceId layer) const {
    const LodCard* card = lod_card(layer);
    return card != nullptr ? card->root.get() : nullptr;
}

jadefx::TextField* IdePrefabEditor::lodDistanceField(engine_core::InstanceId layer) const {
    const LodCard* card = lod_card(layer);
    return card != nullptr ? card->distance.get() : nullptr;
}

jadefx::Node* IdePrefabEditor::lodKindButton(engine_core::InstanceId layer, engine_core::LodKind kind) const {
    const LodCard* card = lod_card(layer);
    for (std::size_t index = 0; card != nullptr && index < std::size(kLodKinds); ++index) {
        if (kLodKinds[index] == kind) {
            return card->kinds[index].get();
        }
    }
    return nullptr;
}

jadefx::Node* IdePrefabEditor::lodMaterialSlot(engine_core::InstanceId layer) const {
    const LodCard* card = lod_card(layer);
    const LodView* view = lod_view(layer);
    return card != nullptr && view != nullptr && view->kind == engine_core::LodKind::Billboard
               ? card->material.root.get()
               : nullptr;
}

jadefx::TextField* IdePrefabEditor::lodSizeField(engine_core::InstanceId layer, int axis) const {
    const LodCard* card = lod_card(layer);
    return card != nullptr && (axis == 0 || axis == 1) ? card->size[axis].get() : nullptr;
}

jadefx::Node* IdePrefabEditor::lodBakeButton(engine_core::InstanceId layer) const {
    const LodCard* card = lod_card(layer);
    const LodView* view = lod_view(layer);
    return card != nullptr && view != nullptr && view->kind == engine_core::LodKind::Billboard ? card->bake.get()
                                                                                               : nullptr;
}

jadefx::Node* IdePrefabEditor::lodNewModelTile(engine_core::InstanceId layer) const {
    const LodCard* card = lod_card(layer);
    const LodView* view = lod_view(layer);
    return card != nullptr && view != nullptr && view->kind == engine_core::LodKind::Model ? card->new_tile.get()
                                                                                           : nullptr;
}
jadefx::Node* IdePrefabEditor::emptyState() const { return empty_->isVisible() ? empty_.get() : nullptr; }

jadefx::TextField* IdePrefabEditor::pickerField() const { return pickerOpen() ? picker_->field() : nullptr; }

jadefx::Node* IdePrefabEditor::pickerRow(engine_core::InstanceId asset) const {
    return pickerOpen() ? picker_->row(asset) : nullptr;
}

jadefx::TextField* IdePrefabEditor::renameField(engine_core::InstanceId model) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == model) {
            return card->rename.get();
        }
    }
    return nullptr;
}

}  // namespace ide
