#include "IdeCollisionGroups.hpp"
#include "LockWaits.hpp"

#include "DataModelLock.hpp"
#include "IdeIcons.hpp"
#include "NodeClasses.hpp"
#include "SelectionService.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace ide {

namespace {

using FilterType = engine_core::CollisionGroups::FilterType;

// The table's geometry, in points. Every row places its parts at these x
// positions, so the columns line up without a grid.
constexpr double kRowHeight = 34;
constexpr double kTitleHeight = 22;
constexpr double kDotX = 0;
constexpr double kDot = 10;
constexpr double kNameX = 18;
constexpr double kNameWidth = 132;
constexpr double kCountX = 156;
constexpr double kCountWidth = 48;
constexpr double kFilterX = 218;
constexpr double kSegmentWidth = 57;
constexpr double kFilterHeight = 24;
// Select, Assign, and delete: icons after the filter, so they never scroll
// out of sight with a wide matrix.
constexpr double kActionsX = 346;
constexpr double kIconButton = 24;
constexpr double kIconGap = 3;
constexpr double kCellsX = kActionsX + 3 * kIconButton + 2 * kIconGap + 18;
constexpr double kPitch = 30;
constexpr double kCell = 22;
constexpr double kLabelWidth = 128;
constexpr double kLabelHeight = 22;
constexpr double kLabelDot = 8;

// Where column j's name starts: its dot centered over the column's cells.
double label_x(std::size_t column) {
    return kCellsX + static_cast<double>(column) * kPitch + kCell * 0.5 - kLabelDot * 0.5;
}
// Left and right inside a row, as its CSS padding.
constexpr double kRowPad = 14;

constexpr const char* kRules = R"CSS(
.collision-groups {
    background-color: var(--background-color);
}
.cg-header {
    background-color: var(--surface-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 9px 16px;
    spacing: 12px;
}
.cg-badge {
    background-color: var(--selection-color);
    border-radius: 9px;
}
.cg-title {
    color: var(--ide-text-color);
    font-size: 15px;
    font-weight: bold;
}
.cg-subtitle {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.cg-primary {
    background-color: var(--accent-color);
    border-width: 0;
    border-radius: 7px;
    color: var(--surface-color);
    font-weight: bold;
    padding: 5px 12px 5px 9px;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.15);
    transition: box-shadow 120ms;
}
.cg-primary:hover {
    box-shadow: 0 3px 10px rgba(0, 0, 0, 0.25);
}
.cg-primary image-view {
    image-color: currentColor;
}
.cg-notice {
    background-color: var(--wash-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 5px 16px;
    spacing: 10px;
}
.cg-banner {
    background-color: var(--selection-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 5px 16px;
    spacing: 10px;
}
.cg-notice-text {
    color: var(--ide-text-color);
    font-size: 12px;
}
.cg-banner-text {
    color: var(--accent-color);
    font-size: 12px;
    font-weight: bold;
}
.cg-secondary {
    border-radius: 7px;
    padding: 2px 12px;
}
.cg-scroll, .cg-canvas {
    background-color: var(--background-color);
}
.cg-table {
    padding: 8px 12px 20px 12px;
}
.cg-col-title {
    color: var(--ide-muted-text-color);
    font-size: 10px;
    font-weight: bold;
}
.cg-row {
    background-color: var(--background-color);
    border-radius: 9px;
    transition: background-color 120ms;
}
.cg-row:hover {
    background-color: var(--hover-color);
}
.cg-row.selected {
    background-color: var(--selection-color);
}
.cg-row.selected:hover {
    background-color: var(--selection-hover-color);
}
.cg-dot {
    border-radius: 5px;
}
.cg-name {
    color: var(--ide-text-color);
    font-size: 13px;
    font-weight: bold;
}
.cg-name.lit {
    color: var(--accent-color);
}
.cg-rename {
    font-size: 13px;
    padding: 1px 4px;
    border-radius: 5px;
}
.cg-count {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.cg-filter {
    background-color: var(--track-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 7px;
    padding: 1px;
}
.cg-segment {
    background-color: var(--track-color);
    color: var(--ide-muted-text-color);
    font-size: 11px;
    border-radius: 5px;
    cursor: pointer;
    transition: background-color 120ms, color 120ms;
}
.cg-segment:hover {
    color: var(--ide-text-color);
}
.cg-segment.on {
    background-color: var(--surface-color);
    color: var(--accent-color);
    font-weight: bold;
    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.18);
}
.cg-icon-button {
    border-radius: 6px;
    cursor: pointer;
    opacity: 0;
    transition: opacity 120ms;
}
.cg-row:hover .cg-icon-button, .cg-row.selected .cg-icon-button {
    opacity: 1;
}
.cg-row:hover .cg-icon-button:disabled, .cg-row.selected .cg-icon-button:disabled {
    opacity: 0.3;
}
.cg-icon-button:hover {
    background-color: var(--surface-color);
}
.cg-icon-button image-view {
    image-color: var(--ide-muted-text-color);
}
.cg-icon-button:hover image-view {
    image-color: var(--accent-color);
}
.cg-icon-button.danger:hover image-view {
    image-color: var(--error-color);
}
.cg-lock {
    opacity: 0;
}
.cg-row:hover .cg-lock, .cg-row.selected .cg-lock {
    opacity: 0.6;
}
.cg-lock image-view {
    image-color: var(--ide-muted-text-color);
}
.cg-cell {
    background-color: var(--surface-color);
    border-width: 1.5px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 7px;
    cursor: pointer;
    transition: background-color 120ms, border-color 120ms, box-shadow 120ms;
}
.cg-cell.self {
    border-radius: 11px;
}
.cg-cell:hover {
    border-color: var(--accent-color);
    box-shadow: 0 0 0 3px var(--selection-color);
}
.cg-cell.lit {
    border-color: var(--accent-color);
    background-color: var(--selection-color);
}
.cg-cell.on {
    background-color: var(--accent-color);
    border-color: var(--accent-color);
}
.cg-cell.on.lit {
    box-shadow: 0 0 0 2px var(--selection-color);
}
.cg-cell image-view {
    image-color: var(--surface-color);
    opacity: 0;
    transition: opacity 120ms;
}
.cg-cell.on image-view {
    opacity: 1;
}
.cg-column {
    spacing: 6px;
}
.cg-column-text {
    color: var(--ide-muted-text-color);
    font-size: 11px;
}
.cg-column.lit .cg-column-text {
    color: var(--accent-color);
    font-weight: bold;
}
.cg-empty {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: dashed;
    border-color: var(--border-color);
    border-radius: 12px;
    padding: 18px 22px;
    spacing: 6px;
}
.cg-empty-title {
    color: var(--ide-text-color);
    font-size: 14px;
    font-weight: bold;
}
.cg-empty-text {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
)CSS";

// A row of parts at fixed x positions, each centered on the row's height.
// The table's columns are these positions, the same in every row.
class Line : public jadefx::Pane {
public:
    Line(double width, double height) : width_(width), height_(height) {
        setPrefSize(width, height);
        setMinSize(width, height);
        setMaxSize(width, height);
    }

    void place(const std::shared_ptr<jadefx::Node>& node, double x, double width, double height) {
        getChildren().add(node);
        spots_.push_back(Spot{node.get(), x, width, height});
    }

protected:
    void layoutChildren() override {
        for (const Spot& spot : spots_) {
            const double top = contentTop() + (contentHeight() - spot.height) * 0.5;
            spot.node->performLayout(contentLeft() + spot.x, std::round(top), spot.width, spot.height);
        }
    }
    double preferredContentWidth(double) const override { return width_ - kRowPad * 2; }
    double preferredContentHeight(double) const override { return height_; }

private:
    struct Spot {
        jadefx::Node* node;
        double x;
        double width;
        double height;
    };
    double width_;
    double height_;
    std::vector<Spot> spots_;
};

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style_class);
    label->setMouseTransparent(true);
    return label;
}

std::shared_ptr<jadefx::Pane> spacer() {
    auto pane = jadefx::make<jadefx::Pane>();
    pane->setStyle("width: 100%;");
    pane->setMouseTransparent(true);
    return pane;
}

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

// A small flat button showing a white glyph under resources/icons, tinted by
// CSS. A left click runs run; a disabled one takes none.
std::shared_ptr<jadefx::StackPane> icon_button(const std::string& file, std::function<void()> run) {
    auto button = jadefx::make<jadefx::StackPane>();
    button->getClassList().add("cg-icon-button");
    button->setAlignment(jadefx::Pos::Center);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(file)) {
        icon->setPrefSize(15, 15);
        icon->setMinSize(15, 15);
        icon->setMaxSize(15, 15);
        button->getChildren().add(std::move(icon));
    }
    button->setOnMouseClicked([run = std::move(run)](const jadefx::MouseEvent& event) {
        if (event.button == 0) {
            run();
        }
    });
    return button;
}

void tip(jadefx::Node& node, const std::string& text) {
    jadefx::Tooltip::install(&node, jadefx::make<jadefx::Tooltip>(text));
}

// "1,204".
std::string grouped(std::size_t value) {
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

std::string bodies_text(std::size_t count) { return grouped(count) + (count == 1 ? " body" : " bodies"); }

// A group's color: the dot by its row and over its column. Default is grey;
// the rest go round the hue circle by the golden angle in registration order,
// which the place saves, so neighbours never share a hue and each group keeps
// its color from one session to the next.
std::string group_color(std::size_t index) {
    if (index == 0) {
        return "#9aa0a6";
    }
    const double hue = std::fmod(210.0 + static_cast<double>(index - 1) * 137.508, 360.0);
    const double s = 0.62;
    const double l = 0.55;
    const double c = (1.0 - std::fabs(2.0 * l - 1.0)) * s;
    const double x = c * (1.0 - std::fabs(std::fmod(hue / 60.0, 2.0) - 1.0));
    const double m = l - c * 0.5;
    double r = 0;
    double g = 0;
    double b = 0;
    if (hue < 60) {
        r = c, g = x;
    } else if (hue < 120) {
        r = x, g = c;
    } else if (hue < 180) {
        g = c, b = x;
    } else if (hue < 240) {
        g = x, b = c;
    } else if (hue < 300) {
        r = x, b = c;
    } else {
        r = c, b = x;
    }
    char text[8];
    std::snprintf(text, sizeof text, "#%02x%02x%02x", static_cast<int>(std::lround((r + m) * 255)),
                  static_cast<int>(std::lround((g + m) * 255)), static_cast<int>(std::lround((b + m) * 255)));
    return text;
}

std::shared_ptr<jadefx::Pane> dot(std::size_t index, double size) {
    auto pane = jadefx::make<jadefx::Pane>();
    pane->getClassList().add("cg-dot");
    pane->setPrefSize(size, size);
    pane->setMinSize(size, size);
    pane->setMaxSize(size, size);
    pane->setMouseTransparent(true);
    pane->setStyle("background-color: " + group_color(index) + "; border-radius: " +
                   std::to_string(static_cast<int>(size / 2)) + "px;");
    return pane;
}

// The name over a column: its dot sits over the column's cells, its text runs right.
std::shared_ptr<jadefx::HBox> column_label(const std::string& name, std::size_t index) {
    auto box = jadefx::make<jadefx::HBox>();
    box->getClassList().add("cg-column");
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    box->getChildren().add(dot(index, kLabelDot));
    box->getChildren().add(text_label(name, "cg-column-text"));
    return box;
}

std::string pair_text(const std::string& a, const std::string& b, bool collide) {
    if (a == b) {
        return a + (collide ? " bodies collide with each other" : " bodies don't collide with each other");
    }
    return a + " and " + b + (collide ? " collide" : " don't collide");
}

// A rename field: Escape drops the typing.
class NameField : public jadefx::TextField {
public:
    explicit NameField(std::function<void()> cancel) : cancel_(std::move(cancel)) {
        getClassList().add("cg-rename");
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

// One group's row and the widgets that change with it.
struct IdeCollisionGroups::Row {
    std::string name;
    std::shared_ptr<Line> root;
    std::shared_ptr<jadefx::Label> label;
    std::shared_ptr<jadefx::Tooltip> label_tip;
    std::shared_ptr<NameField> rename;
    std::shared_ptr<jadefx::Label> count;
    std::shared_ptr<jadefx::Tooltip> count_tip;
    std::shared_ptr<jadefx::Label> exclude;
    std::shared_ptr<jadefx::Label> include;
    std::shared_ptr<jadefx::StackPane> select;
    std::shared_ptr<jadefx::Tooltip> select_tip;
    std::shared_ptr<jadefx::StackPane> assign;
    // Delete, or null for Default, which shows a lock.
    std::shared_ptr<jadefx::StackPane> remove;
    // Cells for groups 0 to this row's own, left to right.
    std::vector<std::shared_ptr<jadefx::StackPane>> cells;
    std::vector<std::shared_ptr<jadefx::Tooltip>> cell_tips;
};

IdeCollisionGroups::IdeCollisionGroups(engine_core::DataModel& world, CollisionGroupsHost host)
    : IdePane("Collision Groups", true), world_(world), host_(std::move(host)) {
    setIconFile("CollisionGroups.png");
    getClassList().add("collision-groups");
    setStylesheet(kRules);
    setMinSize(240, 180);
    setFocusTraversable(true);

    // The header: the badge, the title and count, the legend, and New Group.
    auto header = jadefx::make<jadefx::HBox>();
    header->getClassList().add("cg-header");
    header->setAlignment(jadefx::Pos::CenterLeft);
    header->setStyle("width: 100%;");
    header->getChildren().add(icon_box("CollisionGroups.png", 20, 32, "cg-badge"));
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(1);
    heading->setMouseTransparent(true);
    heading->getChildren().add(text_label("Collision Groups", "cg-title"));
    subtitle_ = text_label("", "cg-subtitle");
    heading->getChildren().add(subtitle_);
    header->getChildren().add(heading);
    header->getChildren().add(spacer());
    new_button_ = jadefx::make<jadefx::Button>("New Group");
    new_button_->getClassList().add("cg-primary");
    new_button_->setElementId("collision-groups-new");
    if (std::shared_ptr<jadefx::ImageView> plus = icon_graphic("Plus.png")) {
        new_button_->setGraphic(std::move(plus));
    }
    new_button_->setGraphicTextGap(6);
    new_button_->setOnAction([this](jadefx::ActionEvent&) { addGroup(); });
    tip(*new_button_, "Add a collision group and name it");
    header->getChildren().add(new_button_);

    // During play: changes here are the run's.
    banner_ = jadefx::make<jadefx::HBox>();
    banner_->getClassList().add("cg-banner");
    banner_->setAlignment(jadefx::Pos::CenterLeft);
    banner_->setStyle("width: 100%;");
    banner_->getChildren().add(icon_box("Play.png", 14, 18, "cg-banner-icon"));
    banner_->getChildren().add(text_label("Playing: changes here last until Stop.", "cg-banner-text"));
    banner_->setVisible(false);

    // Names bodies use that no group has, and a way to make them groups.
    unregistered_ = jadefx::make<jadefx::HBox>();
    unregistered_->getClassList().add("cg-notice");
    unregistered_->setAlignment(jadefx::Pos::CenterLeft);
    unregistered_->setStyle("width: 100%;");
    unregistered_->getChildren().add(icon_box("Warning.png", 16, 20, "cg-notice-icon"));
    unregistered_text_ = text_label("", "cg-notice-text");
    unregistered_->getChildren().add(unregistered_text_);
    unregistered_->getChildren().add(spacer());
    register_button_ = jadefx::make<jadefx::Button>("Register");
    register_button_->getClassList().add("cg-secondary");
    register_button_->setElementId("collision-groups-register");
    register_button_->setOnAction([this](jadefx::ActionEvent&) {
        if (!host_.register_names || view_.unregistered.empty()) {
            return;
        }
        std::vector<std::string> names;
        for (const UnregisteredGroup& group : view_.unregistered) {
            names.push_back(group.name);
        }
        host_.register_names(std::move(names));
    });
    tip(*register_button_, "Make each of these names a collision group");
    unregistered_->getChildren().add(register_button_);
    unregistered_->setVisible(false);

    top_ = jadefx::make<jadefx::VBox>();
    top_->setStyle("width: 100%;");
    top_->getChildren().add(header);

    table_ = jadefx::make<jadefx::VBox>();
    table_->getClassList().add("cg-table");
    table_->setSpacing(0);
    scroll_ = jadefx::make<jadefx::ScrollPane>(table_);
    scroll_->getClassList().add("cg-scroll");
    Fill(*scroll_);

    auto empty = jadefx::make<jadefx::VBox>();
    empty->getClassList().add("cg-empty");
    empty->setMouseTransparent(true);
    empty->getChildren().add(text_label("Choose what collides", "cg-empty-title"));
    empty->getChildren().add(
        text_label("New Group makes a group. Tick the pairs that should collide, then put bodies in it:",
                   "cg-empty-text"));
    empty->getChildren().add(
        text_label("select them and press Assign on its row, or pick it in a body's CollisionGroup property.",
                   "cg-empty-text"));
    empty->getChildren().add(
        text_label("Groups are saved with the place, and scripts can change them through PhysicsService.",
                   "cg-empty-text"));
    // Set apart from the Default row above it.
    auto spaced = jadefx::make<jadefx::VBox>();
    spaced->setStyle("padding: 16px 0 0 0;");
    spaced->setMouseTransparent(true);
    spaced->getChildren().add(empty);
    empty_ = spaced;

    auto canvas = jadefx::make<jadefx::StackPane>();
    canvas->getClassList().add("cg-canvas");
    canvas->setAlignment(jadefx::Pos::TopLeft);
    Fill(*canvas);
    canvas->getChildren().add(scroll_);
    canvas->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        // Clicks bubble here from the rows, which mark them taken.
        if (took_click_) {
            took_click_ = false;
            return;
        }
        if (event.button == 0) {
            selectRow({});
        }
        requestFocus();
    });

    auto root = jadefx::make<jadefx::BorderPane>();
    Fill(*root);
    root->setTop(top_);
    root->setCenter(canvas);
    getChildren().add(root);
}

IdeCollisionGroups::~IdeCollisionGroups() {
    if (menu_) {
        menu_->hide();
    }
    // Closes the question without answering it.
    alert_.reset();
}

void IdeCollisionGroups::layoutChildren() {
    // A rename whose field lost the focus keeps what was typed.
    if (!renaming_.empty()) {
        jadefx::TextField* field = renameField();
        if (field == nullptr || !field->isFocused()) {
            finish_rename(true);
        }
    }
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (lock.owns()) {
            refresh();
        }
    }
    if (pending_add_ && pending_add_->done.load(std::memory_order_acquire)) {
        if (!pending_add_->name.empty()) {
            rename_when_shown_ = pending_add_->name;
        } else if (host_.notice && !pending_add_->error.empty()) {
            host_.notice(pending_add_->error);
        }
        pending_add_.reset();
    }
    if (!rename_when_shown_.empty() && row_named(rename_when_shown_) != nullptr) {
        const std::string name = std::move(rename_when_shown_);
        rename_when_shown_.clear();
        selectRow(name);
        beginRename(name);
    }
    IdePane::layoutChildren();
    // The groups, the bodies, and the selection change on their own, so the pane checks again next frame.
    markLayoutDirty(LayoutDirt::Arrange);
}

void IdeCollisionGroups::refresh() {
    const std::uint64_t groups = world_.collision_groups().revision();
    const std::uint64_t tree = world_.tree_revision();
    const std::uint64_t authored = world_.authored_revision();
    const std::uint64_t selection = world_.selection().revision();
    const bool playing = world_.simulation_running();
    // During play, bodies change groups with no authored edit to see, so the counts are polled.
    bool poll = false;
    if (playing && --poll_in_ <= 0) {
        poll_in_ = kPlayPollFrames;
        poll = true;
    }
    ++since_read_;
    // A selection changes only what Assign offers: no bodies to read.
    if (selection != seen_selection_) {
        seen_selection_ = selection;
        has_selection_ = !world_.selection().get().empty();
        for (const std::shared_ptr<Row>& row : rows_) {
            row->assign->setDisable(!has_selection_);
        }
    }
    if (tree != seen_tree_ || authored != seen_authored_ || poll) {
        seen_tree_ = tree;
        seen_authored_ = authored;
        counts_stale_ = true;
    }
    // The groups show at once. Body counts wait out kCountFrames after the last
    // read, so an edit made every frame, as a drag is, walks the place a few
    // times a second at most.
    const bool groups_moved = !read_once_ || groups != seen_groups_ || playing != view_.playing;
    if (!groups_moved && !(counts_stale_ && since_read_ >= kCountFrames)) {
        return;
    }
    seen_groups_ = groups;
    counts_stale_ = false;
    since_read_ = 0;
    ++reads_;
    CollisionGroupsView view = read_collision_groups(world_);
    const bool first = !read_once_;
    read_once_ = true;
    if (first || view != view_) {
        view_ = std::move(view);
        sync_rows();
        show_header();
        show_notices();
        show_selection();
    } else {
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            fill_row(*rows_[index], index);
        }
    }
    // The group being asked about is gone: there is nothing left to ask.
    if (!asking_.empty() && view_.index_of(asking_) < 0) {
        asking_.clear();
        alert_.reset();
    }
}

double IdeCollisionGroups::table_width() const {
    const std::size_t groups = view_.groups.size();
    return kRowPad * 2 + label_x(groups > 0 ? groups - 1 : 0) + kLabelWidth + 8;
}

void IdeCollisionGroups::show_header() {
    std::size_t bodies = 0;
    for (const CollisionGroupRow& row : view_.groups) {
        bodies += row.bodies;
    }
    for (const UnregisteredGroup& group : view_.unregistered) {
        bodies += group.bodies;
    }
    subtitle_->setText(std::to_string(view_.groups.size()) + " of " +
                       std::to_string(engine_core::CollisionGroups::kMaxGroups) + " groups \xC2\xB7 " +
                       bodies_text(bodies));
    new_button_->setDisable(view_.groups.size() >= static_cast<std::size_t>(engine_core::CollisionGroups::kMaxGroups));
}

void IdeCollisionGroups::show_notices() {
    // Out of the header's column while hidden, so each leaves no gap.
    auto show = [this](const std::shared_ptr<jadefx::HBox>& node, bool on) {
        if (on == node->isVisible()) {
            return;
        }
        node->setVisible(on);
        if (on) {
            top_->getChildren().add(node);
        } else {
            top_->getChildren().removeIf([&node](const std::shared_ptr<jadefx::Node>& item) { return item == node; });
        }
    };
    show(banner_, view_.playing);
    if (!view_.unregistered.empty()) {
        std::string text = "Bodies name groups that don't exist: ";
        const std::size_t shown = std::min<std::size_t>(view_.unregistered.size(), 4);
        for (std::size_t index = 0; index < shown; ++index) {
            const UnregisteredGroup& group = view_.unregistered[index];
            text += (index == 0 ? "" : ", ") + group.name + " (" + grouped(group.bodies) + ")";
        }
        if (view_.unregistered.size() > shown) {
            text += ", and " + std::to_string(view_.unregistered.size() - shown) + " more";
        }
        unregistered_text_->setText(text + ". They collide as Default.");
    }
    show(unregistered_, !view_.unregistered.empty());
    // The banner stays above the notice whichever showed first.
    if (banner_->isVisible() && unregistered_->isVisible()) {
        auto& children = top_->getChildren();
        children.removeIf([this](const std::shared_ptr<jadefx::Node>& item) { return item == banner_; });
        children.insert(1, banner_);
    }
}

std::shared_ptr<jadefx::Node> IdeCollisionGroups::make_header_row() {
    const double width = table_width();
    auto titles = std::make_shared<Line>(width, kTitleHeight);
    titles->setStyle("padding: 0 14px;");
    titles->setMouseTransparent(true);
    titles->place(text_label("GROUP", "cg-col-title"), kNameX, kNameWidth, 14);
    auto count = text_label("BODIES", "cg-col-title");
    count->setAlignment(jadefx::Pos::CenterRight);
    titles->place(count, kCountX, kCountWidth, 14);
    titles->place(text_label("FILTER", "cg-col-title"), kFilterX + 4, kSegmentWidth * 2, 14);
    titles->place(text_label("COLLIDES WITH", "cg-col-title"), kCellsX, kLabelWidth, 14);
    return titles;
}

void IdeCollisionGroups::sync_rows() {
    std::vector<std::string> names;
    for (const CollisionGroupRow& row : view_.groups) {
        names.push_back(row.name);
    }
    if (names != made_for_) {
        made_for_ = names;
        unlight();
        // A rename going on keeps its typing through the new rows.
        std::optional<std::string> typed;
        if (!renaming_.empty()) {
            if (std::find(names.begin(), names.end(), renaming_) == names.end()) {
                renaming_.clear();
            } else if (jadefx::TextField* field = renameField()) {
                typed = field->getText();
            }
        }
        rows_.clear();
        column_labels_.clear();
        for (std::size_t index = 0; index < names.size(); ++index) {
            const std::string& name = names[index];
            auto label = column_label(name, index);
            label->setElementId("cg-column:" + name);
            column_labels_.push_back(std::move(label));
        }
        auto& children = table_->getChildren();
        children.clear();
        children.add(make_header_row());
        // The line above the first row holds the first column's name.
        auto first = std::make_shared<Line>(table_width(), kLabelHeight + 8);
        first->setStyle("padding: 0 14px;");
        first->setMouseTransparent(true);
        if (!column_labels_.empty()) {
            first->place(column_labels_[0], label_x(0), kLabelWidth, kLabelHeight);
        }
        header_row_ = first;
        children.add(first);
        for (std::size_t index = 0; index < names.size(); ++index) {
            rows_.push_back(make_row(index));
            children.add(rows_.back()->root);
        }
        if (names.size() <= 1) {
            children.add(empty_);
        }
        if (typed) {
            for (const std::shared_ptr<Row>& row : rows_) {
                if (row->name == renaming_) {
                    row->rename->setText(*typed);
                    row->rename->positionCaret(static_cast<int>(typed->size()));
                    row->rename->setVisible(true);
                    row->label->setVisible(false);
                    row->rename->requestFocus();
                }
            }
        }
    }
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        fill_row(*rows_[index], index);
    }
}

std::shared_ptr<IdeCollisionGroups::Row> IdeCollisionGroups::make_row(std::size_t index) {
    auto row = std::make_shared<Row>();
    const std::string name = view_.groups[index].name;
    const bool base = name == engine_core::CollisionGroups::kDefault;
    row->name = name;
    Row* raw = row.get();

    row->root = std::make_shared<Line>(table_width(), kRowHeight);
    row->root->getClassList().add("cg-row");
    row->root->setStyle("padding: 0 14px;");
    row->root->setElementId("cg-row:" + name);
    row->root->setOnMouseClicked([this, name](const jadefx::MouseEvent& event) {
        took_click_ = true;
        if (event.button == 0) {
            selectRow(name);
            requestFocus();
        }
    });
    row->root->setOnContextMenuRequested(
        [this, name](const jadefx::MouseEvent& event) { show_row_menu(name, event.x, event.y); });

    row->root->place(dot(index, kDot), kDotX, kDot, kDot);
    row->label = jadefx::make<jadefx::Label>(name);
    row->label->getClassList().add("cg-name");
    row->label->setElementId("cg-name:" + name);
    row->label->setOnMouseClicked([this, name](const jadefx::MouseEvent& event) {
        if (event.button == 0 && event.clickCount == 2) {
            beginRename(name);
        }
    });
    row->label_tip = jadefx::make<jadefx::Tooltip>(base ? name + ": every body in no other group" : name);
    jadefx::Tooltip::install(row->label.get(), row->label_tip);
    row->root->place(row->label, kNameX, kNameWidth, 20);
    row->rename = std::make_shared<NameField>([this] { finish_rename(false); });
    row->rename->setOnAction([this](jadefx::ActionEvent&) { finish_rename(true); });
    row->root->place(row->rename, kNameX - 5, kNameWidth + 5, 26);

    row->count = jadefx::make<jadefx::Label>("");
    row->count->getClassList().add("cg-count");
    row->count->setAlignment(jadefx::Pos::CenterRight);
    row->count_tip = jadefx::make<jadefx::Tooltip>("");
    jadefx::Tooltip::install(row->count.get(), row->count_tip);
    row->root->place(row->count, kCountX, kCountWidth, 18);

    // Exclude and Include, as two segments of one pill.
    auto filter = std::make_shared<Line>(kSegmentWidth * 2 + 4, kFilterHeight);
    filter->getClassList().add("cg-filter");
    filter->setStyle("padding: 0;");
    auto segment = [this, name](const char* text, FilterType type, const char* help) {
        auto label = jadefx::make<jadefx::Label>(text);
        label->getClassList().add("cg-segment");
        label->setAlignment(jadefx::Pos::Center);
        label->setOnMouseClicked([this, name, type](const jadefx::MouseEvent& event) {
            if (event.button != 0 || !host_.set_filter_type) {
                return;
            }
            const int at = view_.index_of(name);
            if (at < 0 || view_.groups[static_cast<std::size_t>(at)].filter_type == type) {
                return;
            }
            host_.set_filter_type(name, type);
        });
        tip(*label, help);
        return label;
    };
    row->exclude = segment("Exclude", FilterType::Exclude,
                           "Exclude: collides with every group but those not ticked. Groups made later collide with it.");
    row->include = segment("Include", FilterType::Include,
                           "Include: collides with no group but those ticked. Groups made later don't collide with it.");
    filter->place(row->exclude, 1, kSegmentWidth, kFilterHeight - 4);
    filter->place(row->include, kSegmentWidth + 1, kSegmentWidth, kFilterHeight - 4);
    row->root->place(filter, kFilterX, kSegmentWidth * 2 + 4, kFilterHeight);

    row->select = icon_button("CGSelect.png", [this, name] {
        if (host_.select_bodies) {
            host_.select_bodies(name);
        }
    });
    row->select_tip = jadefx::make<jadefx::Tooltip>("");
    jadefx::Tooltip::install(row->select.get(), row->select_tip);
    row->root->place(row->select, kActionsX, kIconButton, kIconButton);
    row->assign = icon_button("CGAssign.png", [this, name] {
        if (host_.assign_selection) {
            host_.assign_selection(name);
        }
    });
    tip(*row->assign, "Put the selected bodies, and the bodies inside what is selected, in " + name);
    row->root->place(row->assign, kActionsX + kIconButton + kIconGap, kIconButton, kIconButton);
    const double third = kActionsX + 2 * (kIconButton + kIconGap);
    if (base) {
        auto lock = icon_box("lock.png", 14, kIconButton, "cg-lock");
        lock->setMouseTransparent(false);
        tip(*lock, "Default is always there: it holds every body in no other group");
        row->root->place(lock, third, kIconButton, kIconButton);
    } else {
        row->remove = icon_button("Cross.png", [this, name] { requestRemove(name); });
        row->remove->getClassList().add("danger");
        tip(*row->remove, "Delete " + name);
        row->root->place(row->remove, third, kIconButton, kIconButton);
    }

    // The cells: one for each group up to this one, the last against itself.
    for (std::size_t column = 0; column <= index; ++column) {
        auto cell = jadefx::make<jadefx::StackPane>();
        cell->getClassList().add("cg-cell");
        set_class(*cell, "self", column == index);
        cell->setAlignment(jadefx::Pos::Center);
        if (std::shared_ptr<jadefx::ImageView> check = icon_graphic("Check.png")) {
            check->setPrefSize(14, 14);
            check->setMinSize(14, 14);
            check->setMaxSize(14, 14);
            cell->getChildren().add(std::move(check));
        }
        const int r = static_cast<int>(index);
        const int c = static_cast<int>(column);
        cell->setOnMouseClicked([this, index, column](const jadefx::MouseEvent& event) {
            if (event.button == 0) {
                clicked_cell(index, column);
            }
        });
        cell->setOnMouseEntered([this, r, c](const jadefx::MouseEvent&) { light(r, c); });
        cell->setOnMouseExited([this, r, c](const jadefx::MouseEvent&) {
            if (lit_row_ == r && lit_column_ == c) {
                unlight();
            }
        });
        auto cell_tip = jadefx::make<jadefx::Tooltip>("");
        cell_tip->setShowDelay(0.35);
        jadefx::Tooltip::install(cell.get(), cell_tip);
        row->root->place(cell, kCellsX + static_cast<double>(column) * kPitch, kCell, kCell);
        row->cells.push_back(std::move(cell));
        row->cell_tips.push_back(std::move(cell_tip));
    }
    // The next column's name, just above that column's first cell.
    if (index + 1 < column_labels_.size()) {
        row->root->place(column_labels_[index + 1], label_x(index + 1), kLabelWidth, kLabelHeight);
    }
    (void)raw;
    return row;
}

void IdeCollisionGroups::fill_row(Row& row, std::size_t index) {
    const CollisionGroupRow& group = view_.groups[index];
    if (row.label->getText() != group.name && renaming_ != row.name) {
        row.label->setText(group.name);
    }
    row.count->setText(grouped(group.bodies));
    row.count_tip->setText(bodies_text(group.bodies) + (group.bodies == 1 ? " is" : " are") + " in " + group.name);
    set_class(*row.exclude, "on", group.filter_type == FilterType::Exclude);
    set_class(*row.include, "on", group.filter_type == FilterType::Include);
    row.select->setDisable(group.bodies == 0);
    row.select_tip->setText(group.bodies == 0 ? "No bodies are in " + group.name
                                              : "Select the " + bodies_text(group.bodies) + " in " + group.name);
    row.assign->setDisable(!has_selection_);
    for (std::size_t column = 0; column < row.cells.size() && column < view_.collide[index].size(); ++column) {
        const bool collide = view_.collide[index][column];
        set_class(*row.cells[column], "on", collide);
        row.cell_tips[column]->setText(pair_text(group.name, view_.groups[column].name, collide));
    }
}

void IdeCollisionGroups::show_selection() {
    if (!selected_.empty() && view_.index_of(selected_) < 0) {
        selected_.clear();
    }
    for (const std::shared_ptr<Row>& row : rows_) {
        set_class(*row->root, "selected", row->name == selected_);
    }
}

void IdeCollisionGroups::selectRow(const std::string& name) {
    selected_ = view_.index_of(name) >= 0 ? name : std::string();
    show_selection();
}

void IdeCollisionGroups::light(int row, int column) {
    unlight();
    if (row < 0 || column < 0 || static_cast<std::size_t>(row) >= rows_.size() || column > row) {
        return;
    }
    lit_row_ = row;
    lit_column_ = column;
    Row& line = *rows_[static_cast<std::size_t>(row)];
    // Left along the row to its name, and up the column to its name.
    for (int k = 0; k < column; ++k) {
        set_class(*line.cells[static_cast<std::size_t>(k)], "lit", true);
    }
    for (int k = column; k < row; ++k) {
        set_class(*rows_[static_cast<std::size_t>(k)]->cells[static_cast<std::size_t>(column)], "lit", true);
    }
    set_class(*line.label, "lit", true);
    set_class(*column_labels_[static_cast<std::size_t>(column)], "lit", true);
}

void IdeCollisionGroups::unlight() {
    if (lit_row_ < 0) {
        return;
    }
    for (const std::shared_ptr<Row>& row : rows_) {
        for (const std::shared_ptr<jadefx::StackPane>& cell : row->cells) {
            set_class(*cell, "lit", false);
        }
        set_class(*row->label, "lit", false);
    }
    for (const std::shared_ptr<jadefx::Node>& label : column_labels_) {
        set_class(*label, "lit", false);
    }
    lit_row_ = -1;
    lit_column_ = -1;
}

bool IdeCollisionGroups::lit(const jadefx::Node* node) { return node != nullptr && has_class(*node, "lit"); }

void IdeCollisionGroups::clicked_cell(std::size_t row, std::size_t column) {
    if (row >= view_.groups.size() || column > row || !host_.set_collidable) {
        return;
    }
    // From what the cell shows, so a second click before the place catches up switches it back.
    const bool collide = !has_class(*rows_[row]->cells[column], "on");
    // Shown at once; the place catches up on the simulation thread.
    set_class(*rows_[row]->cells[column], "on", collide);
    host_.set_collidable(view_.groups[row].name, view_.groups[column].name, collide);
}

void IdeCollisionGroups::addGroup() {
    if (!host_.add || pending_add_) {
        return;
    }
    pending_add_ = std::make_shared<NewGroupResult>();
    host_.add(pending_add_);
}

void IdeCollisionGroups::requestRemove(const std::string& name) {
    const int at = view_.index_of(name);
    if (at < 0 || name == engine_core::CollisionGroups::kDefault || !host_.remove) {
        return;
    }
    const std::size_t bodies = view_.groups[static_cast<std::size_t>(at)].bodies;
    if (bodies == 0) {
        host_.remove(name, GroupBodies::MoveToDefault);
        return;
    }
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        return;
    }
    const jadefx::ButtonType keep("Leave Alone", jadefx::ButtonType::Data::Left);
    const jadefx::ButtonType move("Move to Default", jadefx::ButtonType::Data::OkDone);
    // The Alert cuts a line too long for its Label short, so the lines are broken here.
    alert_ = std::make_shared<jadefx::Alert>(
        jadefx::AlertType::Warning,
        "Move to Default sets their CollisionGroup to\nDefault. Leave Alone keeps it: they collide as\n"
        "Default anyway while no group has the name.",
        std::vector<jadefx::ButtonType>{keep, move, jadefx::ButtonType::Cancel()});
    alert_->setTitle("Anarchy Engine");
    alert_->setHeaderText(bodies_text(bodies) + (bodies == 1 ? " is" : " are") + " in " + name);
    asking_ = name;
    alert_->setOnClosed([this, name, keep, move](const jadefx::ButtonType* choice) {
        asking_.clear();
        if (choice == nullptr || !host_.remove) {
            return;
        }
        if (*choice == move) {
            host_.remove(name, GroupBodies::MoveToDefault);
        } else if (*choice == keep) {
            host_.remove(name, GroupBodies::Keep);
        }
    });
    alert_->show(*scene);
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {
        {&keep, "collision-groups-remove-keep"},
        {&move, "collision-groups-remove-move"},
        {&jadefx::ButtonType::Cancel(), "collision-groups-remove-cancel"}};
    for (const auto& [type, element_id] : ids) {
        if (jadefx::Button* button = alert_->lookupButton(*type)) {
            button->setElementId(element_id);
        }
    }
}

void IdeCollisionGroups::beginRename(const std::string& name) {
    if (name == engine_core::CollisionGroups::kDefault) {
        return;
    }
    if (!renaming_.empty()) {
        finish_rename(false);
    }
    for (const std::shared_ptr<Row>& row : rows_) {
        if (row->name != name) {
            continue;
        }
        row->rename->setText(row->label->getText());
        row->rename->selectAll();
        row->rename->setVisible(true);
        row->label->setVisible(false);
        row->rename->requestFocus();
        renaming_ = name;
        return;
    }
}

void IdeCollisionGroups::finish_rename(bool apply) {
    const std::string from = renaming_;
    renaming_.clear();
    for (const std::shared_ptr<Row>& row : rows_) {
        if (row->name != from) {
            continue;
        }
        const bool had_focus = row->rename->isFocused();
        const std::string to = row->rename->getText();
        row->rename->setVisible(false);
        row->label->setVisible(true);
        if (apply && to != from && host_.rename) {
            // The row keeps its selection under the new name, as the model trims it.
            const std::size_t first = to.find_first_not_of(" \t");
            const std::size_t last = to.find_last_not_of(" \t");
            if (selected_ == from && first != std::string::npos) {
                selected_ = to.substr(first, last - first + 1);
            }
            host_.rename(from, to);
        }
        if (had_focus) {
            requestFocus();
        }
    }
}

const IdeCollisionGroups::Row* IdeCollisionGroups::row_named(const std::string& name) const {
    for (const std::shared_ptr<Row>& row : rows_) {
        if (row->name == name) {
            return row.get();
        }
    }
    return nullptr;
}

void IdeCollisionGroups::handleKey(jadefx::KeyEvent& event) {
    const jadefx::Scene* scene = getScene();
    const jadefx::Node* focus = scene != nullptr ? scene->focusedNode() : nullptr;
    if (!event.pressed || dynamic_cast<const jadefx::TextField*>(focus) != nullptr || selected_.empty()) {
        IdePane::handleKey(event);
        return;
    }
    if (event.key == jadefx::Key::Delete) {
        event.consume();
        requestRemove(selected_);
        return;
    }
    if (event.key == jadefx::Key::F2 || event.key == jadefx::Key::Enter) {
        event.consume();
        beginRename(selected_);
        return;
    }
    if (event.key == jadefx::Key::Up || event.key == jadefx::Key::Down) {
        event.consume();
        const int at = view_.index_of(selected_);
        const int next = std::clamp(at + (event.key == jadefx::Key::Up ? -1 : 1), 0,
                                    static_cast<int>(view_.groups.size()) - 1);
        selectRow(view_.groups[static_cast<std::size_t>(next)].name);
        return;
    }
    if (event.key == jadefx::Key::Escape) {
        event.consume();
        selectRow({});
        return;
    }
    IdePane::handleKey(event);
}

jadefx::Node* IdeCollisionGroups::rowNode(const std::string& name) const {
    const Row* row = row_named(name);
    return row != nullptr ? row->root.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::cellNode(const std::string& a, const std::string& b) const {
    int first = view_.index_of(a);
    int second = view_.index_of(b);
    if (first < 0 || second < 0 || static_cast<std::size_t>(std::max(first, second)) >= rows_.size()) {
        return nullptr;
    }
    if (second > first) {
        std::swap(first, second);
    }
    const Row& row = *rows_[static_cast<std::size_t>(first)];
    return static_cast<std::size_t>(second) < row.cells.size() ? row.cells[static_cast<std::size_t>(second)].get()
                                                              : nullptr;
}

jadefx::Node* IdeCollisionGroups::filterSegment(const std::string& name, FilterType type) const {
    const Row* row = row_named(name);
    if (row == nullptr) {
        return nullptr;
    }
    return type == FilterType::Include ? row->include.get() : row->exclude.get();
}

jadefx::Node* IdeCollisionGroups::selectButton(const std::string& name) const {
    const Row* row = row_named(name);
    return row != nullptr ? row->select.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::assignButton(const std::string& name) const {
    const Row* row = row_named(name);
    return row != nullptr ? row->assign.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::deleteButton(const std::string& name) const {
    const Row* row = row_named(name);
    return row != nullptr ? row->remove.get() : nullptr;
}

jadefx::Label* IdeCollisionGroups::nameLabel(const std::string& name) const {
    const Row* row = row_named(name);
    return row != nullptr ? row->label.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::columnLabel(const std::string& name) const {
    const int at = view_.index_of(name);
    return at >= 0 && static_cast<std::size_t>(at) < column_labels_.size()
               ? column_labels_[static_cast<std::size_t>(at)].get()
               : nullptr;
}

jadefx::TextField* IdeCollisionGroups::renameField() const {
    const Row* row = row_named(renaming_);
    return row != nullptr ? row->rename.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::newButton() const { return new_button_.get(); }

jadefx::Label* IdeCollisionGroups::subtitle() const { return subtitle_.get(); }

jadefx::Node* IdeCollisionGroups::unregisteredRow() const {
    return unregistered_->isVisible() ? unregistered_.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::registerButton() const {
    return unregistered_->isVisible() ? register_button_.get() : nullptr;
}

jadefx::Node* IdeCollisionGroups::playBanner() const { return banner_->isVisible() ? banner_.get() : nullptr; }

jadefx::Node* IdeCollisionGroups::emptyHint() const {
    return empty_->getParent() != nullptr && view_.groups.size() <= 1 ? empty_.get() : nullptr;
}

jadefx::Menu* IdeCollisionGroups::rowMenu() const { return menu_.get(); }

void IdeCollisionGroups::show_row_menu(const std::string& name, double x, double y) {
    jadefx::Scene* scene = getScene();
    const int at = view_.index_of(name);
    if (scene == nullptr || at < 0) {
        return;
    }
    selectRow(name);
    requestFocus();
    if (menu_) {
        menu_->hide();
    }
    menu_ = jadefx::make<jadefx::Menu>();
    auto add = [this](const std::string& text, const char* icon, bool enabled, std::function<void()> run) {
        auto item = jadefx::make<jadefx::MenuItem>(text);
        if (std::shared_ptr<jadefx::ImageView> image = icon_graphic(icon)) {
            item->setGraphic(std::move(image));
        }
        item->setDisable(!enabled);
        item->setOnAction([run = std::move(run)](jadefx::ActionEvent&) { run(); });
        menu_->getItems().add(item);
    };
    const bool base = name == engine_core::CollisionGroups::kDefault;
    const std::size_t bodies = view_.groups[static_cast<std::size_t>(at)].bodies;
    if (!base) {
        add("Rename", "Rename.png", true, [this, name] { beginRename(name); });
    }
    add("Select Bodies", "selection-select.png", bodies > 0, [this, name] {
        if (host_.select_bodies) {
            host_.select_bodies(name);
        }
    });
    add("Assign Selection", "arrow-in.png", has_selection_, [this, name] {
        if (host_.assign_selection) {
            host_.assign_selection(name);
        }
    });
    if (!base) {
        menu_->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
        add("Delete", "Cross.png", true, [this, name] { requestRemove(name); });
    }
    menu_->show(*scene, x, y);
}

jadefx::Alert* IdeCollisionGroups::removeAlert() const { return asking_.empty() ? nullptr : alert_.get(); }

}  // namespace ide
