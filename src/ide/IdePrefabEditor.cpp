#include "IdePrefabEditor.hpp"
#include "LockWaits.hpp"

#include "AssetInstances.hpp"
#include "AssetPicker.hpp"
#include "DataModelLock.hpp"
#include "IdeIcons.hpp"
#include "PropertySheet.hpp"
#include "SelectionService.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace

// One Model's card and the widgets that change with it.
struct IdePrefabEditor::Card {
    engine_core::InstanceId id = 0;
    std::shared_ptr<jadefx::VBox> root;
    std::shared_ptr<jadefx::Label> name;
    std::shared_ptr<NameField> rename;
    std::shared_ptr<jadefx::HBox> status;
    std::shared_ptr<jadefx::Label> status_text;
    struct Slot {
        std::shared_ptr<jadefx::HBox> root;
        std::shared_ptr<jadefx::StackPane> thumb;
        std::shared_ptr<jadefx::Label> text;
        std::shared_ptr<jadefx::Label> sub;
        std::shared_ptr<jadefx::StackPane> clear;
        PartView shown;
        bool filled = false;
    };
    Slot slots[2];
    // Set by a button inside the card, so the click it bubbles to the card or slot next does nothing more.
    bool swallow = false;

    Slot& slot(ModelPart part) { return slots[part == ModelPart::Mesh ? 0 : 1]; }
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
    header->getChildren().add(add_button_);

    // The cards, and the New Model tile after them.
    grid_ = jadefx::make<jadefx::FlowPane>(kGap, kGap);
    grid_->getClassList().add("pe-grid");
    auto tile = jadefx::make<jadefx::VBox>();
    tile->getClassList().add("pe-new");
    tile->setAlignment(jadefx::Pos::Center);
    tile->setPrefSize(kCardWidth, kTileHeight);
    tile->setMinSize(kCardWidth, kTileHeight);
    if (std::shared_ptr<jadefx::StackPane> plus = icon_box("Plus.png", 22, 30, "pe-new-plus")) {
        tile->getChildren().add(std::move(plus));
    }
    tile->getChildren().add(text_label("New Model", "pe-new-title"));
    tile->getChildren().add(text_label("or drop a Mesh or Material here", "pe-new-hint"));
    tile->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        took_click_ = true;
        if (event.button == 0) {
            addModel();
        }
    });
    new_tile_ = tile;
    accept_drops(*tile, 0, nullptr);
    grid_->getChildren().add(tile);

    scroll_ = jadefx::make<jadefx::ScrollPane>(grid_);
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
                    // The new card is last: bring it into view, and ask for what it still needs.
                    scroll_->setVvalue(scroll_->getVmax());
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
        if (placed || ++pending_picker_->waited > kPickerWaitFrames) {
            const PendingPicker pending = *pending_picker_;
            pending_picker_.reset();
            if (slot != nullptr) {
                openPicker(pending.model, pending.part, pending.guided);
            }
        }
    }
    // The prefab and the selection change on their own, so the editor checks them again next frame.
    markLayoutDirty(LayoutDirt::Arrange);
}

void IdePrefabEditor::refresh() {
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
    if (prefab_alive_) {
        prefab_name_ = world_.name(prefab_);
        setTitle(prefab_name_);
        models = read_models(world_, prefab_);
    }
    std::vector<engine_core::InstanceId> watched;
    for (const ModelView& view : models) {
        watched.push_back(view.id);
    }
    if (watched != watched_) {
        watched_ = watched;
        world_.set_watched(watch_, std::move(watched));
    }
    if (models != models_) {
        models_ = std::move(models);
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
    } else if (models_.empty()) {
        summary = "No models yet";
    } else {
        const std::size_t attention = static_cast<std::size_t>(std::count_if(
            models_.begin(), models_.end(), [](const ModelView& view) { return view.status() != ModelStatus::Ready; }));
        summary = std::to_string(models_.size()) + (models_.size() == 1 ? " model" : " models");
        summary += attention == 0 ? std::string("  ·  All ready")
                                  : "  ·  " + std::to_string(attention) + (attention == 1 ? " needs" : " need") +
                                        " attention";
    }
    subtitle_->setText(summary);
    add_button_->setDisable(!prefab_alive_);
    gone_->setVisible(!prefab_alive_);
    empty_->setVisible(prefab_alive_ && models_.empty());
    scroll_->setVisible(prefab_alive_ && !models_.empty());
}

void IdePrefabEditor::sync_cards() {
    // Keeps the card of a Model still here, so a rename or a pick in it survives.
    std::vector<std::shared_ptr<Card>> kept;
    for (const ModelView& view : models_) {
        auto found = std::find_if(cards_.begin(), cards_.end(),
                                  [&view](const std::shared_ptr<Card>& card) { return card->id == view.id; });
        std::shared_ptr<Card> card = found != cards_.end() ? *found : make_card(view);
        fill_card(*card, view);
        kept.push_back(std::move(card));
    }
    if (renaming_ != 0 && std::none_of(kept.begin(), kept.end(),
                                       [this](const std::shared_ptr<Card>& card) { return card->id == renaming_; })) {
        renaming_ = 0;
    }
    cards_ = std::move(kept);
    grid_->getChildren().clear();
    for (const std::shared_ptr<Card>& card : cards_) {
        grid_->getChildren().add(card->root);
    }
    grid_->getChildren().add(new_tile_);
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
    auto top = jadefx::make<jadefx::HBox>();
    top->setSpacing(10);
    top->setAlignment(jadefx::Pos::CenterLeft);
    top->getChildren().add(icon_box("Model.png", 18, 32, "pe-card-icon"));
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(2);
    heading->setStyle("width: 100%;");
    heading->setMinSize(0, 0);
    auto name_stack = jadefx::make<jadefx::StackPane>();
    name_stack->setMinSize(0, 0);
    name_stack->setAlignment(jadefx::Pos::CenterLeft);
    card->name = jadefx::make<jadefx::Label>(view.name);
    card->name->getClassList().add("pe-name");
    card->name->setMinSize(0, 0);
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
        slot.root = jadefx::make<jadefx::HBox>();
        slot.root->getClassList().add("pe-slot");
        slot.root->setAlignment(jadefx::Pos::CenterLeft);
        slot.root->setElementId(std::string("pe-slot:") + std::to_string(id) + ":" + model_part_name(part));
        slot.thumb = icon_box(part_icon(part), 18, kThumbSize, "pe-thumb");
        slot.root->getChildren().add(slot.thumb);
        auto text = jadefx::make<jadefx::VBox>();
        text->setMouseTransparent(true);
        text->setAlignment(jadefx::Pos::CenterLeft);
        text->setStyle("width: 100%;");
        text->setMinSize(0, 0);
        slot.text = text_label("", "pe-slot-text");
        slot.sub = text_label("", "pe-slot-sub");
        slot.text->setMinSize(0, 0);
        slot.sub->setMinSize(0, 0);
        text->getChildren().add(slot.text);
        text->getChildren().add(slot.sub);
        slot.root->getChildren().add(text);
        slot.clear = icon_button("Cross.png", std::string("Clear ") + model_part_name(part), "pe-slot-clear",
                                 [this, raw, id, part] {
                                     raw->swallow = true;
                                     if (host_.set_part) {
                                         host_.set_part(id, part, 0);
                                     }
                                 });
        slot.root->getChildren().add(slot.clear);
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
    Card::Slot& slot = card.slot(part);
    const bool filled = view.id != 0;
    if (slot.shown == view && slot.text->getText().size() > 0) {
        return;
    }
    slot.shown = view;
    set_class(*slot.root, "empty", !filled && !view.missing);
    set_class(*slot.root, "missing", view.missing);
    // The thumb shows the asset's detailed icon, else the part's icon when
    // filled, and a plus when it asks for one.
    slot.thumb->getChildren().clear();
    const std::string file = filled ? part_icon(part) : view.missing ? "Warning.png" : "Plus.png";
    if (std::shared_ptr<jadefx::Node> detailed = filled ? AssetPicker::detailedIcon(view.id, kThumbSize - 2) : nullptr) {
        slot.thumb->getChildren().add(std::move(detailed));
    } else if (std::shared_ptr<jadefx::ImageView> icon = icon_file(file)) {
        icon->setPrefSize(18, 18);
        icon->setMinSize(18, 18);
        icon->setMaxSize(18, 18);
        icon->setMouseTransparent(true);
        slot.thumb->getChildren().add(std::move(icon));
    }
    if (filled) {
        slot.text->setText(view.name);
        slot.sub->setText(view.where.empty() ? std::string(model_part_name(part)) : view.where);
    } else if (view.missing) {
        slot.text->setText(std::string("Missing ") + part_word(part));
        slot.sub->setText("Its asset was deleted. Choose another.");
    } else {
        slot.text->setText(std::string("Choose a ") + part_word(part));
        slot.sub->setText("Click, or drop one from Assets");
    }
    slot.clear->setVisible(filled || view.missing);
    slot.filled = filled;
}

void IdePrefabEditor::show_selection() {
    for (const std::shared_ptr<Card>& card : cards_) {
        const bool on = std::find(selected_.begin(), selected_.end(), card->id) != selected_.end();
        set_class(*card->root, "selected", on);
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

void IdePrefabEditor::addModel() {
    if (!prefab_alive_ || !host_.add_model || pending_add_) {
        return;
    }
    closePicker();
    pending_add_ = std::make_shared<InsertResult>();
    host_.add_model(prefab_, 0, 0, pending_add_);
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
    std::vector<engine_core::InstanceId> ids;
    for (const ModelView& view : models_) {
        if (std::find(selected_.begin(), selected_.end(), view.id) != selected_.end()) {
            ids.push_back(view.id);
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

void IdePrefabEditor::accept_drops(jadefx::Node& node, engine_core::InstanceId model, const ModelPart* part) {
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
    node.setOnDragDropped([this, model, part](jadefx::DragEvent& event) {
        const std::vector<engine_core::InstanceId> ids = dragged(event);
        if (ids.empty()) {
            return;
        }
        mark_drop(nullptr);
        event.setDropCompleted(dropOnto(ids, model, part));
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

bool IdePrefabEditor::dropOnto(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId model,
                               const ModelPart* part) {
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
        host_.add_model(prefab_, parts.mesh, parts.material, pending_add_);
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
jadefx::Node* IdePrefabEditor::newModelTile() const { return new_tile_.get(); }
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
