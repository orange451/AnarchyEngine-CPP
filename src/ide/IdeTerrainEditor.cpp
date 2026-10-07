#include "IdeTerrainEditor.hpp"
#include "LockWaits.hpp"

#include "AssetChoices.hpp"
#include "AssetPicker.hpp"
#include "DataModelLock.hpp"
#include "IdeIcons.hpp"
#include "NodeClasses.hpp"
#include "SelectionService.hpp"
#include "Terrain.hpp"

#include <algorithm>
#include <utility>

namespace ide {

namespace {

constexpr double kCardWidth = 240;
constexpr double kGap = 16;
// About a card's height, so the New Material tile lines up with the cards beside it.
constexpr double kTileHeight = 150;
// How many TerrainMaterials a Terrain may hold.
constexpr int kMaxMaterials = 255;

// Frames a picker waits for its card to be laid out and scrolled into view.
constexpr int kPickerWaitFrames = 30;

constexpr const char* kEditorRules = R"CSS(
.terrain-editor {
    background-color: var(--background-color);
}
.te-header {
    background-color: var(--surface-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 14px 20px;
    spacing: 12px;
}
.te-badge {
    background-color: var(--selection-color);
    border-radius: 9px;
}
.te-title {
    color: var(--ide-text-color);
    font-size: 17px;
    font-weight: bold;
}
.te-subtitle {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.te-primary {
    background-color: var(--accent-color);
    border-width: 0;
    border-radius: 7px;
    color: var(--surface-color);
    font-weight: bold;
    padding: 7px 14px 7px 10px;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.15);
    transition: box-shadow 120ms;
}
.te-primary:hover {
    box-shadow: 0 3px 10px rgba(0, 0, 0, 0.25);
}
.te-primary image-view {
    image-color: currentColor;
}
.te-unassigned {
    background-color: var(--wash-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 8px 20px;
    spacing: 10px;
}
.te-unassigned-text {
    color: var(--ide-text-color);
    font-size: 12px;
}
.te-secondary {
    border-radius: 7px;
    padding: 4px 12px;
}
.te-scroll, .te-canvas {
    background-color: var(--background-color);
}
.te-grid {
    padding: 20px;
}
.te-card {
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
.te-card:hover {
    box-shadow: 0 6px 18px rgba(0, 0, 0, 0.14);
}
.te-card.selected {
    border-color: var(--accent-color);
    box-shadow: 0 0 0 1px var(--accent-color);
}
.te-id-pill {
    background-color: var(--selection-color);
    border-radius: 6px;
    padding: 2px 7px;
}
.te-id {
    color: var(--accent-color);
    font-size: 11px;
    font-weight: bold;
}
.te-name {
    color: var(--ide-text-color);
    font-size: 14px;
    font-weight: bold;
}
.te-rename {
    font-size: 14px;
    padding: 1px 4px;
    border-radius: 4px;
}
.te-in-use {
    background-color: var(--accent-color);
    color: var(--ide-problems-badge-text-color);
    border-radius: 8px;
    font-size: 11px;
    padding: 0 6px;
}
.te-icon-button {
    border-radius: 6px;
    cursor: pointer;
    opacity: 0;
    transition: background-color 120ms;
}
.te-icon-button:hover {
    background-color: var(--row-hover-color);
}
.te-icon-button image-view {
    image-color: var(--ide-muted-text-color);
}
.te-icon-button:hover image-view {
    image-color: var(--ide-text-color);
}
.te-card:hover .te-card-delete, .te-card.selected .te-card-delete {
    opacity: 1;
}
.te-section {
    spacing: 6px;
}
.te-slot-label {
    color: var(--ide-muted-text-color);
    font-size: 10px;
    font-weight: bold;
}
.te-slot {
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
.te-slot:hover {
    border-color: var(--accent-color);
}
.te-slot.empty {
    background-color: var(--wash-color);
}
.te-slot.missing {
    border-color: var(--error-color);
}
.te-thumb {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 7px;
}
.te-slot.empty .te-thumb {
    background-color: rgba(0, 0, 0, 0);
}
.te-slot.empty .te-thumb image-view {
    image-color: var(--accent-color);
}
.te-slot-text {
    color: var(--ide-text-color);
    font-size: 13px;
}
.te-slot.empty .te-slot-text {
    color: var(--accent-color);
}
.te-slot.missing .te-slot-text {
    color: var(--error-color);
}
.te-slot-sub {
    color: var(--ide-muted-text-color);
    font-size: 11px;
}
.te-new {
    background-color: var(--wash-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 12px;
    spacing: 6px;
    cursor: pointer;
    transition: border-color 120ms, background-color 120ms, box-shadow 120ms;
}
.te-new:hover {
    border-color: var(--accent-color);
    background-color: var(--surface-color);
}
.te-new image-view {
    image-color: var(--accent-color);
}
.te-new-title {
    color: var(--accent-color);
    font-size: 13px;
    font-weight: bold;
}
.te-new-hint, .te-empty-text {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.te-empty {
    spacing: 10px;
    padding: 40px;
}
.te-empty-art {
    background-color: var(--selection-color);
    border-radius: 36px;
}
.te-empty-title {
    color: var(--ide-text-color);
    font-size: 17px;
    font-weight: bold;
}
)CSS";

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style_class);
    label->setMouseTransparent(true);
    return label;
}

// Takes the room left in a line, which pushes what follows it to the right edge.
std::shared_ptr<jadefx::Pane> spacer() {
    auto pane = jadefx::make<jadefx::Pane>();
    pane->setStyle("width: 100%;");
    pane->setMouseTransparent(true);
    return pane;
}

// An icon file at size, centered in a box of box points, with the box's class.
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

// A small icon that acts on a click, such as a card's delete. It shows while
// the pointer is over its card. The click still bubbles on.
std::shared_ptr<jadefx::StackPane> icon_button(const std::string& file, const std::string& tip,
                                               const char* style_class, std::function<void()> run) {
    auto button = jadefx::make<jadefx::StackPane>();
    button->getClassList().add("te-icon-button");
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

std::shared_ptr<jadefx::Button> primary_button(const std::string& text) {
    auto button = jadefx::make<jadefx::Button>(text);
    button->getClassList().add("te-primary");
    if (std::shared_ptr<jadefx::ImageView> plus = icon_graphic("Plus.png")) {
        button->setGraphic(std::move(plus));
    }
    button->setGraphicTextGap(6);
    return button;
}

// What the unassigned row says about ids: "Ids 3, 7 have voxels but no material: ...".
std::string unassigned_text(const std::vector<int>& ids) {
    std::string text = ids.size() == 1 ? "Id " : "Ids ";
    for (std::size_t index = 0; index < ids.size(); ++index) {
        text += (index == 0 ? "" : ", ") + std::to_string(ids[index]);
    }
    text += ids.size() == 1 ? " has" : " have";
    return text + " voxels but no material: they draw as the default.";
}

// A rename field: Escape drops the typing.
class NameField : public jadefx::TextField {
public:
    explicit NameField(std::function<void()> cancel) : cancel_(std::move(cancel)) {
        getClassList().add("te-rename");
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

// One TerrainMaterial's card and the widgets that change with it.
struct IdeTerrainEditor::Card {
    engine_core::InstanceId id = 0;
    std::shared_ptr<jadefx::VBox> root;
    std::shared_ptr<jadefx::Label> material_id;
    std::shared_ptr<jadefx::Label> name;
    std::shared_ptr<NameField> rename;
    std::shared_ptr<jadefx::Label> in_use;
    std::shared_ptr<jadefx::StackPane> remove;
    std::shared_ptr<jadefx::HBox> slot;
    std::shared_ptr<jadefx::StackPane> thumb;
    std::shared_ptr<jadefx::Label> slot_text;
    std::shared_ptr<jadefx::Label> slot_sub;
    // What the slot shows: its Material, or none, or a missing one.
    engine_core::InstanceId shown_material = ~engine_core::InstanceId{0};
    std::string shown_material_name;
    bool shown_missing = false;
    // Set by a button inside the card, so the click it bubbles to the card next does nothing more.
    bool swallow = false;
};

IdeTerrainEditor::IdeTerrainEditor(engine_core::DataModel& world, engine_core::InstanceId terrain,
                                   TerrainEditorHost host)
    : IdePane("Configure Terrain", true), world_(world), terrain_(terrain), host_(std::move(host)) {
    setIconFile("World.png");
    getClassList().add("terrain-editor");
    setStylesheet(kEditorRules);
    setMinSize(240, 180);
    setFocusTraversable(true);
    watch_ = world_.watch_changes(edited_.setter());

    // The header: the Terrain, how many materials it holds, and Add Material.
    auto header = jadefx::make<jadefx::HBox>();
    header->getClassList().add("te-header");
    header->setAlignment(jadefx::Pos::CenterLeft);
    header->setStyle("width: 100%;");
    header->getChildren().add(icon_box("World.png", 22, 38, "te-badge"));
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(1);
    heading->setMouseTransparent(true);
    title_ = text_label("", "te-title");
    subtitle_ = text_label("", "te-subtitle");
    heading->getChildren().add(title_);
    heading->getChildren().add(subtitle_);
    header->getChildren().add(heading);
    header->getChildren().add(spacer());
    add_button_ = primary_button("Add Material");
    add_button_->setOnAction([this](jadefx::ActionEvent&) { addMaterial(); });
    jadefx::Tooltip::install(add_button_.get(),
                             jadefx::make<jadefx::Tooltip>("Add a TerrainMaterial on the lowest free Id"));
    header->getChildren().add(add_button_);

    // Ids voxels use that no TerrainMaterial holds, and a way to move them.
    unassigned_ = jadefx::make<jadefx::HBox>();
    unassigned_->getClassList().add("te-unassigned");
    unassigned_->setAlignment(jadefx::Pos::CenterLeft);
    unassigned_->setStyle("width: 100%;");
    unassigned_->getChildren().add(icon_box("Warning.png", 16, 20, "te-unassigned-icon"));
    unassigned_text_ = text_label("", "te-unassigned-text");
    unassigned_->getChildren().add(unassigned_text_);
    unassigned_->getChildren().add(spacer());
    unassigned_replace_ = jadefx::make<jadefx::Button>("Replace…");
    unassigned_replace_->getClassList().add("te-secondary");
    unassigned_replace_->setElementId("te-unassigned-replace");
    unassigned_replace_->setOnAction([this](jadefx::ActionEvent&) {
        const std::vector<int> ids = view_.unassigned_in_use;
        open_id_picker(*unassigned_replace_, 0, [this, ids](int to) {
            if (!host_.replace_unassigned) {
                return;
            }
            for (const int from : ids) {
                host_.replace_unassigned(terrain_, from, to);
            }
        });
    });
    jadefx::Tooltip::install(unassigned_replace_.get(), jadefx::make<jadefx::Tooltip>(
                                                            "Move these voxels onto a material. This cannot be undone yet."));
    unassigned_->getChildren().add(unassigned_replace_);
    unassigned_->setVisible(false);

    top_ = jadefx::make<jadefx::VBox>();
    top_->setStyle("width: 100%;");
    top_->getChildren().add(header);

    // The cards, and the New Material tile after them.
    grid_ = jadefx::make<jadefx::FlowPane>(kGap, kGap);
    grid_->getClassList().add("te-grid");
    auto tile = jadefx::make<jadefx::VBox>();
    tile->getClassList().add("te-new");
    tile->setAlignment(jadefx::Pos::Center);
    tile->setPrefSize(kCardWidth, kTileHeight);
    tile->setMinSize(kCardWidth, kTileHeight);
    tile->getChildren().add(icon_box("Plus.png", 22, 30, "te-new-plus"));
    tile->getChildren().add(text_label("New Material", "te-new-title"));
    tile->getChildren().add(text_label("Takes the lowest free Id", "te-new-hint"));
    tile->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        took_click_ = true;
        if (event.button == 0) {
            addMaterial();
        }
    });
    new_tile_ = tile;
    grid_->getChildren().add(tile);

    scroll_ = jadefx::make<jadefx::ScrollPane>(grid_);
    scroll_->getClassList().add("te-scroll");
    scroll_->setFitToWidth(true);
    scroll_->setHbarPolicy(jadefx::ScrollBarPolicy::Never);
    Fill(*scroll_);

    auto gone = jadefx::make<jadefx::VBox>();
    gone->getClassList().add("te-empty");
    gone->setAlignment(jadefx::Pos::Center);
    Fill(*gone);
    gone->getChildren().add(icon_box("Warning.png", 30, 72, "te-empty-art"));
    gone->getChildren().add(text_label("This Terrain no longer exists", "te-empty-title"));
    gone->getChildren().add(text_label("It was deleted, or the place it was in was closed.", "te-empty-text"));
    gone->setVisible(false);
    gone_ = gone;

    auto canvas = jadefx::make<jadefx::StackPane>();
    canvas->getClassList().add("te-canvas");
    canvas->setAlignment(jadefx::Pos::TopLeft);
    Fill(*canvas);
    canvas->getChildren().add(scroll_);
    canvas->getChildren().add(gone);
    canvas->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        // Clicks bubble here from the cards and the tile, which mark them taken.
        if (took_click_) {
            took_click_ = false;
            return;
        }
        // A click on empty space clears the selection of this Terrain's materials.
        if (event.button == 0 && !selected_entries().empty()) {
            world_.selection().set({});
        }
        requestFocus();
    });

    auto root = jadefx::make<jadefx::BorderPane>();
    Fill(*root);
    root->setTop(top_);
    root->setCenter(canvas);
    getChildren().add(root);

    picker_ = AssetPicker::create();
}

IdeTerrainEditor::~IdeTerrainEditor() {
    if (picker_) {
        picker_->dismiss();
    }
    if (menu_) {
        menu_->hide();
    }
    // Closes the question without answering it.
    alert_.reset();
    world_.unwatch_changes(watch_);
}

void IdeTerrainEditor::layoutChildren() {
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
                    // The new card is last: bring it into view, and ask for its Material.
                    scroll_->setVvalue(scroll_->getVmax());
                    const TerrainMaterialView* view = entry_view(made);
                    if (view == nullptr || view->material == 0) {
                        pending_picker_ = PendingPicker{made};
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
        jadefx::Node* slot = materialSlot(pending_picker_->entry);
        const bool placed = slot != nullptr && slot->getWidth() > 0 && slot->getAbsoluteY() >= scroll_->getAbsoluteY() &&
                            slot->getAbsoluteY() + slot->getHeight() <= scroll_->getAbsoluteY() + scroll_->getHeight();
        if (placed || ++pending_picker_->waited > kPickerWaitFrames) {
            const engine_core::InstanceId entry = pending_picker_->entry;
            pending_picker_.reset();
            if (slot != nullptr) {
                openMaterialPicker(entry);
            }
        }
    }
}

std::uint64_t IdeTerrainEditor::voxel_revision() const {
    const auto* terrain =
        world_.alive(terrain_) ? dynamic_cast<const engine_core::Terrain*>(world_.instance(terrain_)) : nullptr;
    return terrain != nullptr ? terrain->volume().revision() : 0;
}

void IdeTerrainEditor::refresh() {
    const std::uint64_t tree = world_.tree_revision();
    // Taken before the read, so a change made while reading reads again next frame.
    const bool edited = edited_.take();
    // Sculpting moves no tree or property, only the voxels' revision.
    if (tree == seen_tree_ && !edited && voxel_revision() == view_.voxel_revision) {
        return;
    }
    seen_tree_ = tree;
    TerrainMaterialsView view = read_terrain_materials(world_, terrain_);
    if (view.alive) {
        setTitle(view.terrain_name);
    }
    std::vector<engine_core::InstanceId> watched;
    if (view.alive) {
        watched.push_back(terrain_);
    }
    for (const TerrainMaterialView& entry : view.materials) {
        watched.push_back(entry.id);
    }
    if (watched != watched_) {
        watched_ = watched;
        world_.set_watched(watch_, std::move(watched));
    }
    const bool cards_moved = view.materials != view_.materials;
    view_ = std::move(view);
    if (cards_moved) {
        sync_cards();
        show_selection();
    }
    show_unassigned();
    show_header();
    // The TerrainMaterial being asked about is gone: there is nothing left to ask.
    if (asking_ != 0 && entry_view(asking_) == nullptr) {
        asking_ = 0;
        alert_.reset();
    }
}

void IdeTerrainEditor::show_header() {
    title_->setText(view_.alive ? view_.terrain_name : std::string("Terrain"));
    const std::size_t count = view_.materials.size();
    subtitle_->setText(view_.alive ? std::to_string(count) + " / " + std::to_string(kMaxMaterials) + " materials"
                                   : std::string("Deleted"));
    add_button_->setDisable(!view_.alive || count >= static_cast<std::size_t>(kMaxMaterials));
    gone_->setVisible(!view_.alive);
    scroll_->setVisible(view_.alive);
}

void IdeTerrainEditor::show_unassigned() {
    const bool show = view_.alive && !view_.unassigned_in_use.empty();
    if (show) {
        unassigned_text_->setText(unassigned_text(view_.unassigned_in_use));
    }
    if (show == unassigned_->isVisible()) {
        return;
    }
    // Out of the header's column while hidden, so it leaves no gap.
    unassigned_->setVisible(show);
    if (show) {
        top_->getChildren().add(unassigned_);
    } else {
        top_->getChildren().removeIf(
            [this](const std::shared_ptr<jadefx::Node>& node) { return node == unassigned_; });
    }
}

void IdeTerrainEditor::sync_cards() {
    // Keeps the card of a TerrainMaterial still here, so a rename in it survives.
    std::vector<std::shared_ptr<Card>> kept;
    for (const TerrainMaterialView& view : view_.materials) {
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

std::shared_ptr<IdeTerrainEditor::Card> IdeTerrainEditor::make_card(const TerrainMaterialView& view) {
    auto card = std::make_shared<Card>();
    card->id = view.id;
    const engine_core::InstanceId id = view.id;
    Card* raw = card.get();

    card->root = jadefx::make<jadefx::VBox>();
    card->root->getClassList().add("te-card");
    card->root->setPrefWidth(kCardWidth);
    card->root->setMinSize(kCardWidth, 0);
    card->root->setElementId("te-card:" + std::to_string(id));

    // The Id, the name, and delete.
    auto top = jadefx::make<jadefx::HBox>();
    top->setSpacing(10);
    top->setAlignment(jadefx::Pos::CenterLeft);
    auto pill = jadefx::make<jadefx::StackPane>();
    pill->getClassList().add("te-id-pill");
    pill->setAlignment(jadefx::Pos::Center);
    pill->setMouseTransparent(true);
    card->material_id = text_label("", "te-id");
    pill->getChildren().add(card->material_id);
    top->getChildren().add(pill);
    auto name_stack = jadefx::make<jadefx::StackPane>();
    name_stack->setAlignment(jadefx::Pos::CenterLeft);
    name_stack->setStyle("width: 100%;");
    card->name = jadefx::make<jadefx::Label>(view.name);
    card->name->getClassList().add("te-name");
    card->name->setOnMouseClicked([this, id](const jadefx::MouseEvent& event) {
        if (event.button == 0 && event.clickCount == 2) {
            beginRename(id);
        }
    });
    card->rename = std::make_shared<NameField>([this] { finish_rename(false); });
    card->rename->setOnAction([this](jadefx::ActionEvent&) { finish_rename(true); });
    name_stack->getChildren().add(card->name);
    name_stack->getChildren().add(card->rename);
    top->getChildren().add(name_stack);
    card->remove = icon_button("Cross.png", "Delete TerrainMaterial", "te-card-delete", [this, raw, id] {
        raw->swallow = true;
        requestRemove(id);
    });
    top->getChildren().add(card->remove);
    card->root->getChildren().add(top);

    // The Material slot, with In use beside its label.
    auto section = jadefx::make<jadefx::VBox>();
    section->getClassList().add("te-section");
    auto label_row = jadefx::make<jadefx::HBox>();
    label_row->setAlignment(jadefx::Pos::CenterLeft);
    label_row->setMouseTransparent(true);
    label_row->getChildren().add(text_label("MATERIAL", "te-slot-label"));
    label_row->getChildren().add(spacer());
    card->in_use = text_label("In use", "te-in-use");
    label_row->getChildren().add(card->in_use);
    section->getChildren().add(label_row);
    card->slot = jadefx::make<jadefx::HBox>();
    card->slot->getClassList().add("te-slot");
    card->slot->setAlignment(jadefx::Pos::CenterLeft);
    card->slot->setElementId("te-slot:" + std::to_string(id));
    card->thumb = icon_box("Material.png", 18, 30, "te-thumb");
    card->slot->getChildren().add(card->thumb);
    auto text = jadefx::make<jadefx::VBox>();
    text->setMouseTransparent(true);
    text->setAlignment(jadefx::Pos::CenterLeft);
    text->setStyle("width: 100%;");
    card->slot_text = text_label("", "te-slot-text");
    card->slot_sub = text_label("", "te-slot-sub");
    text->getChildren().add(card->slot_text);
    text->getChildren().add(card->slot_sub);
    card->slot->getChildren().add(text);
    card->slot->setOnMouseClicked([this, raw, id](const jadefx::MouseEvent& event) {
        if (event.button != 0 || raw->swallow) {
            return;
        }
        raw->swallow = true;
        world_.selection().set({id});
        requestFocus();
        openMaterialPicker(id);
    });
    section->getChildren().add(card->slot);
    card->root->getChildren().add(section);

    card->root->setOnMouseClicked([this, raw, id](const jadefx::MouseEvent& event) {
        took_click_ = true;
        // A button or the slot inside took this click already.
        if (raw->swallow) {
            raw->swallow = false;
            return;
        }
        if (event.button == 0) {
            clicked_card(id);
        }
    });
    card->root->setOnContextMenuRequested(
        [this, id](const jadefx::MouseEvent& event) { show_card_menu(id, event.x, event.y); });
    return card;
}

void IdeTerrainEditor::fill_card(Card& card, const TerrainMaterialView& view) {
    const std::string id_text = std::to_string(view.material_id);
    if (card.material_id->getText() != id_text) {
        card.material_id->setText(id_text);
    }
    if (card.name->getText() != view.name) {
        card.name->setText(view.name);
    }
    card.in_use->setVisible(view.in_use);
    if (card.shown_material == view.material && card.shown_material_name == view.material_name &&
        card.shown_missing == view.material_missing) {
        return;
    }
    card.shown_material = view.material;
    card.shown_material_name = view.material_name;
    card.shown_missing = view.material_missing;
    const bool filled = view.material != 0;
    set_class(*card.slot, "empty", !filled && !view.material_missing);
    set_class(*card.slot, "missing", view.material_missing);
    // The thumb shows the Material icon when filled, and a plus when it asks for one.
    card.thumb->getChildren().clear();
    const std::string file = filled ? "Material.png" : view.material_missing ? "Warning.png" : "Plus.png";
    if (std::shared_ptr<jadefx::ImageView> icon = icon_file(file)) {
        icon->setPrefSize(18, 18);
        icon->setMinSize(18, 18);
        icon->setMaxSize(18, 18);
        icon->setMouseTransparent(true);
        card.thumb->getChildren().add(std::move(icon));
    }
    if (filled) {
        card.slot_text->setText(view.material_name);
        card.slot_sub->setText("Material");
    } else if (view.material_missing) {
        card.slot_text->setText("Missing material");
        card.slot_sub->setText("Its asset was deleted. Choose another.");
    } else {
        card.slot_text->setText("Choose a material");
        card.slot_sub->setText("Draws as the default until then");
    }
}

void IdeTerrainEditor::show_selection() {
    for (const std::shared_ptr<Card>& card : cards_) {
        const bool on = std::find(selected_.begin(), selected_.end(), card->id) != selected_.end();
        set_class(*card->root, "selected", on);
    }
}

void IdeTerrainEditor::clicked_card(engine_core::InstanceId entry) {
    requestFocus();
    world_.selection().set({entry});
}

void IdeTerrainEditor::show_card_menu(engine_core::InstanceId entry, double x, double y) {
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        return;
    }
    world_.selection().set({entry});
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
    };
    add("Choose Material", "Material.png", [this, entry] { openMaterialPicker(entry); });
    menu_->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    add("Rename", "Rename.png", [this, entry] { beginRename(entry); });
    add("Delete", "Cross.png", [this, entry] { requestRemove(entry); });
    menu_->show(*scene, x, y);
}

void IdeTerrainEditor::addMaterial() {
    if (!view_.alive || !host_.add || pending_add_) {
        return;
    }
    closePicker();
    pending_add_ = std::make_shared<InsertResult>();
    host_.add(terrain_, 0, pending_add_);
}

void IdeTerrainEditor::requestRemove(engine_core::InstanceId entry) {
    const TerrainMaterialView* view = entry_view(entry);
    if (!view_.alive || view == nullptr || !host_.remove) {
        return;
    }
    if (!view->in_use) {
        host_.remove(entry, RemoveChoice{});
        return;
    }
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        return;
    }
    closePicker();
    const jadefx::ButtonType replace("Replace…", jadefx::ButtonType::Data::Left);
    const jadefx::ButtonType keep("Keep Cells", jadefx::ButtonType::Data::OkDone);
    // The Alert draws its text in one Label that cuts a line too long for it short,
    // so the lines are broken here, as the conflicts Alert does.
    alert_ = std::make_shared<jadefx::Alert>(
        jadefx::AlertType::Warning,
        "Replace them with another material, or keep them:\nthey draw as the default material until a new\n"
        "material takes Id " +
            std::to_string(view->material_id) + ".\nA replacement cannot be undone yet.",
        std::vector<jadefx::ButtonType>{replace, keep, jadefx::ButtonType::Cancel()});
    alert_->setTitle("Anarchy Engine");
    alert_->setHeaderText(view->name + " is used by voxels");
    asking_ = entry;
    alert_->setOnClosed([this, entry, replace, keep](const jadefx::ButtonType* choice) {
        asking_ = 0;
        if (choice == nullptr || !host_.remove) {
            return;
        }
        if (*choice == keep) {
            host_.remove(entry, RemoveChoice{});
        } else if (*choice == replace) {
            if (jadefx::Node* card = cardNode(entry)) {
                open_id_picker(*card, entry, [this, entry](int to) {
                    if (host_.remove) {
                        host_.remove(entry, RemoveChoice{RemoveChoice::Kind::Replace, to});
                    }
                });
            }
        }
    });
    alert_->show(*scene);
    // Stable names for the three answers, so a test can find them.
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {
        {&replace, "terrain-remove-replace"}, {&keep, "terrain-remove-keep"},
        {&jadefx::ButtonType::Cancel(), "terrain-remove-cancel"}};
    for (const auto& [type, element_id] : ids) {
        if (jadefx::Button* button = alert_->lookupButton(*type)) {
            button->setElementId(element_id);
        }
    }
}

void IdeTerrainEditor::openMaterialPicker(engine_core::InstanceId entry) {
    jadefx::Node* anchor = materialSlot(entry);
    const TerrainMaterialView* view = entry_view(entry);
    if (anchor == nullptr || view == nullptr || !picker_) {
        return;
    }
    std::vector<AssetChoice> choices;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (!lock.owns()) {
            return;
        }
        choices = asset_choices(world_, "Material");
    }
    picker_->open(*anchor, "Material", std::move(choices), view->material,
                  [this, entry](engine_core::InstanceId material) {
                      const TerrainMaterialView* now = entry_view(entry);
                      if (now != nullptr && now->material != material && host_.set_material) {
                          host_.set_material(entry, material);
                      }
                  });
}

void IdeTerrainEditor::open_id_picker(jadefx::Node& anchor, engine_core::InstanceId except,
                                      std::function<void(int)> pick) {
    if (!picker_ || !view_.alive) {
        return;
    }
    // Default is Id 0; each TerrainMaterial is listed by its own Id.
    std::vector<AssetChoice> choices;
    choices.push_back(AssetChoice{0, "Default", "Id 0, the default material"});
    for (const TerrainMaterialView& view : view_.materials) {
        if (view.id != except) {
            choices.push_back(AssetChoice{view.id, view.name, "Id " + std::to_string(view.material_id)});
        }
    }
    picker_->open(anchor, "Material", std::move(choices), 0,
                  [this, pick = std::move(pick)](engine_core::InstanceId chosen) {
                      int to = 0;
                      if (chosen != 0) {
                          const TerrainMaterialView* view = entry_view(chosen);
                          if (view == nullptr) {
                              return;
                          }
                          to = view->material_id;
                      }
                      pick(to);
                  });
}

void IdeTerrainEditor::closePicker() {
    if (picker_) {
        picker_->dismiss();
    }
}

bool IdeTerrainEditor::pickerOpen() const { return picker_ && picker_->showing(); }

void IdeTerrainEditor::beginRename(engine_core::InstanceId entry) {
    if (renaming_ != 0) {
        finish_rename(false);
    }
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id != entry) {
            continue;
        }
        card->rename->setText(card->name->getText());
        card->rename->selectAll();
        card->rename->setVisible(true);
        card->name->setVisible(false);
        card->rename->requestFocus();
        renaming_ = entry;
        return;
    }
}

void IdeTerrainEditor::finish_rename(bool apply) {
    const engine_core::InstanceId entry = renaming_;
    renaming_ = 0;
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id != entry) {
            continue;
        }
        const bool had_focus = card->rename->isFocused();
        const std::string name = card->rename->getText();
        card->rename->setVisible(false);
        card->name->setVisible(true);
        if (apply && !name.empty() && name != card->name->getText() && host_.rename) {
            // Shown at once; the place catches up on the simulation thread.
            card->name->setText(name);
            host_.rename(entry, name);
        }
        if (had_focus) {
            requestFocus();
        }
    }
}

std::vector<engine_core::InstanceId> IdeTerrainEditor::selected_entries() const {
    std::vector<engine_core::InstanceId> ids;
    for (const TerrainMaterialView& view : view_.materials) {
        if (std::find(selected_.begin(), selected_.end(), view.id) != selected_.end()) {
            ids.push_back(view.id);
        }
    }
    return ids;
}

void IdeTerrainEditor::handleKey(jadefx::KeyEvent& event) {
    const jadefx::Scene* scene = getScene();
    const jadefx::Node* focus = scene != nullptr ? scene->focusedNode() : nullptr;
    if (!event.pressed || dynamic_cast<const jadefx::TextField*>(focus) != nullptr) {
        IdePane::handleKey(event);
        return;
    }
    // Each removal may ask its own question, so the keys act on one card at a time.
    const std::vector<engine_core::InstanceId> ids = selected_entries();
    if ((event.key == jadefx::Key::Delete || event.key == jadefx::Key::Backspace) && ids.size() == 1) {
        event.consume();
        requestRemove(ids.front());
        return;
    }
    if ((event.key == jadefx::Key::F2 || event.key == jadefx::Key::Enter) && ids.size() == 1) {
        event.consume();
        beginRename(ids.front());
        return;
    }
    IdePane::handleKey(event);
}

const TerrainMaterialView* IdeTerrainEditor::entry_view(engine_core::InstanceId entry) const {
    for (const TerrainMaterialView& view : view_.materials) {
        if (view.id == entry) {
            return &view;
        }
    }
    return nullptr;
}

jadefx::Node* IdeTerrainEditor::cardNode(engine_core::InstanceId entry) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == entry) {
            return card->root.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdeTerrainEditor::materialSlot(engine_core::InstanceId entry) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == entry) {
            return card->slot.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdeTerrainEditor::deleteButton(engine_core::InstanceId entry) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == entry) {
            return card->remove.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdeTerrainEditor::inUseBadge(engine_core::InstanceId entry) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == entry && card->in_use->isVisible()) {
            return card->in_use.get();
        }
    }
    return nullptr;
}

jadefx::Node* IdeTerrainEditor::addTile() const { return new_tile_.get(); }
jadefx::Label* IdeTerrainEditor::counter() const { return subtitle_.get(); }
jadefx::Node* IdeTerrainEditor::unassignedRow() const {
    return unassigned_->isVisible() ? unassigned_.get() : nullptr;
}
jadefx::Node* IdeTerrainEditor::goneNote() const { return gone_->isVisible() ? gone_.get() : nullptr; }

jadefx::TextField* IdeTerrainEditor::renameField(engine_core::InstanceId entry) const {
    for (const std::shared_ptr<Card>& card : cards_) {
        if (card->id == entry) {
            return card->rename.get();
        }
    }
    return nullptr;
}

jadefx::Alert* IdeTerrainEditor::removeAlert() const {
    return alert_ && asking_ != 0 && alert_->getResult() == nullptr ? alert_.get() : nullptr;
}

jadefx::Node* IdeTerrainEditor::pickerRow(engine_core::InstanceId choice) const {
    return pickerOpen() ? picker_->row(choice) : nullptr;
}

}  // namespace ide
