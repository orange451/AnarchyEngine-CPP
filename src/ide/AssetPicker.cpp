#include "AssetPicker.hpp"

#include "Containment.hpp"
#include "IdeIcons.hpp"

#include <algorithm>
#include <cctype>

namespace ide {

namespace {

constexpr double kPickerWidth = 300;
constexpr double kPickerRowHeight = 38;
constexpr int kPickerRows = 7;

// The picker is a popup, outside any page, so it carries its own rules.
constexpr const char* kPickerRules = R"CSS(
.pe-picker {
    background-color: var(--ide-popup-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--ide-popup-border-color);
    border-radius: 10px;
    box-shadow: 0 10px 28px var(--ide-popup-shadow-color);
    padding: 8px;
    spacing: 6px;
}
.pe-picker-field {
    width: 100%;
    border-radius: 6px;
    padding: 5px 8px;
}
.pe-picker-list {
    background-color: rgba(0, 0, 0, 0);
}
.pe-pick-row {
    border-radius: 6px;
    padding: 0 8px;
    spacing: 10px;
    cursor: pointer;
}
.pe-pick-row.active {
    background-color: var(--ide-popup-selection-color);
}
.pe-pick-name {
    color: var(--ide-popup-text-color);
    font-size: 13px;
}
.pe-pick-where {
    color: var(--ide-popup-detail-text-color);
    font-size: 11px;
}
.pe-pick-check image-view {
    image-color: var(--accent-color);
}
.pe-pick-none image-view {
    image-color: var(--ide-popup-detail-text-color);
}
.pe-picker-empty {
    color: var(--ide-popup-detail-text-color);
    font-size: 12px;
    padding: 10px 8px;
}
.pe-picker-hint {
    color: var(--ide-popup-detail-text-color);
    font-size: 11px;
    padding: 2px 4px 0 4px;
}
)CSS";

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style_class);
    label->setMouseTransparent(true);
    return label;
}

void set_class(jadefx::Node& node, const std::string& name, bool on) {
    const auto& items = node.getClassList().items();
    if (on == (std::find(items.begin(), items.end(), name) != items.end())) {
        return;
    }
    if (on) {
        node.getClassList().add(name);
    } else {
        node.getClassList().removeIf([&name](const std::string& item) { return item == name; });
    }
}

}  // namespace

// A field whose Up, Down, and Escape go to the picker, not the caret.
class AssetPickerField : public jadefx::TextField {
public:
    AssetPickerField(std::function<void(int)> move, std::function<void()> cancel)
        : move_(std::move(move)), cancel_(std::move(cancel)) {
        getClassList().add("pe-picker-field");
        setCapturesKeys(true);
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && (event.key == jadefx::Key::Up || event.key == jadefx::Key::Down)) {
            event.consume();
            move_(event.key == jadefx::Key::Up ? -1 : 1);
            return;
        }
        if (event.pressed && event.key == jadefx::Key::Escape) {
            event.consume();
            cancel_();
            return;
        }
        TextField::handleKey(event);
    }

private:
    std::function<void(int)> move_;
    std::function<void()> cancel_;
};

std::shared_ptr<AssetPicker> AssetPicker::create() {
    std::shared_ptr<AssetPicker> picker(new AssetPicker());
    picker->self_ = picker;
    return picker;
}

AssetPicker::AssetPicker() {
    getClassList().add("pe-picker");
    setStylesheet(kPickerRules);
    setPrefWidth(kPickerWidth);
    field_ = std::make_shared<AssetPickerField>([this](int delta) { move(delta); }, [this] { dismiss(); });
    field_->setOnAction([this](jadefx::ActionEvent&) { choose(active_); });
    getChildren().add(field_);
    list_ = jadefx::make<jadefx::VBox>();
    list_->setSpacing(1);
    scroll_ = jadefx::make<jadefx::ScrollPane>(list_);
    scroll_->getClassList().add("pe-picker-list");
    scroll_->setFitToWidth(true);
    scroll_->setHbarPolicy(jadefx::ScrollBarPolicy::Never);
    getChildren().add(scroll_);
    getChildren().add(text_label("Enter to choose  ·  Esc to close", "pe-picker-hint"));
}

void AssetPicker::open(jadefx::Node& anchor, std::string asset_class, std::vector<AssetChoice> choices,
                       engine_core::InstanceId current, std::function<void(engine_core::InstanceId)> pick) {
    jadefx::Scene* scene = anchor.getScene();
    if (scene == nullptr) {
        return;
    }
    asset_class_ = std::move(asset_class);
    all_ = std::move(choices);
    current_ = current;
    pick_ = std::move(pick);
    const char* plural = engine_core::asset_plural(asset_class_);
    field_->setPromptText("Search " + lower(plural != nullptr ? plural : asset_class_));
    field_->setText("");
    query_.clear();
    rebuild();
    // A steady height, so filtering does not make the popover jump.
    const int rows = std::clamp(static_cast<int>(all_.size()) + (current_ != 0 ? 1 : 0), 1, kPickerRows);
    scroll_->setPrefViewportHeight(rows * (kPickerRowHeight + 1));
    setPrefWidth(std::max(kPickerWidth, anchor.getWidth()));
    jadefx::PopupOptions options;
    options.autoHide = true;
    scene->showPopupNear(self_.lock(), &anchor, jadefx::Side::Bottom, options);
    field_->requestFocus();
}

void AssetPicker::dismiss() {
    jadefx::Scene* scene = getScene();
    if (scene != nullptr && !scene->isTearingDown() && scene->isPopupShowing(this)) {
        scene->hidePopup(this);
    }
}

bool AssetPicker::showing() const {
    const jadefx::Scene* scene = getScene();
    return scene != nullptr && !scene->isTearingDown() && scene->isPopupShowing(this);
}

jadefx::TextField* AssetPicker::field() const { return field_.get(); }

jadefx::Node* AssetPicker::row(engine_core::InstanceId id) const {
    for (const auto& [row_id, node] : rows_) {
        if (row_id == id) {
            return node.get();
        }
    }
    return nullptr;
}

void AssetPicker::layoutChildren() {
    if (field_->getText() != query_) {
        query_ = field_->getText();
        rebuild();
    }
    jadefx::VBox::layoutChildren();
}

void AssetPicker::rebuild() {
    list_->getChildren().clear();
    rows_.clear();
    // None comes first while something is picked, and only with no search typed.
    if (current_ != 0 && query_.empty()) {
        add_row(0, "No " + lower(asset_class_), "Leave it empty", "Cross.png", "pe-pick-none");
    }
    const std::string icon = icon_filename(asset_class_);
    for (const AssetChoice& choice : filter_choices(all_, query_)) {
        add_row(choice.id, choice.name, choice.where, icon, nullptr);
    }
    if (rows_.empty()) {
        const char* plural = engine_core::asset_plural(asset_class_);
        const char* category = engine_core::asset_home(asset_class_);
        const std::string text =
            all_.empty() ? "No " + lower(plural != nullptr ? plural : asset_class_) + " yet. Add one under Assets › " +
                               (category != nullptr ? category : "") + "."
                         : "Nothing matches “" + query_ + "”";
        list_->getChildren().add(text_label(text, "pe-picker-empty"));
    }
    // The current asset starts highlighted, else the first row.
    active_ = rows_.empty() ? -1 : 0;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].first == current_ && current_ != 0 && query_.empty()) {
            active_ = static_cast<int>(index);
        }
    }
    paint();
}

void AssetPicker::add_row(engine_core::InstanceId id, const std::string& name, const std::string& where,
                          const std::string& icon, const char* extra_class) {
    auto row = jadefx::make<jadefx::HBox>();
    row->getClassList().add("pe-pick-row");
    if (extra_class != nullptr) {
        row->getClassList().add(extra_class);
    }
    row->setAlignment(jadefx::Pos::CenterLeft);
    row->setPrefHeight(kPickerRowHeight);
    row->setMinSize(0, kPickerRowHeight);
    if (std::shared_ptr<jadefx::ImageView> image = icon_graphic(icon)) {
        row->getChildren().add(std::move(image));
    }
    auto text = jadefx::make<jadefx::VBox>();
    text->setAlignment(jadefx::Pos::CenterLeft);
    text->setMouseTransparent(true);
    text->getChildren().add(text_label(name, "pe-pick-name"));
    if (!where.empty()) {
        text->getChildren().add(text_label(where, "pe-pick-where"));
    }
    row->getChildren().add(std::move(text));
    // Takes the room left, pushing the check to the right edge.
    auto spacer = jadefx::make<jadefx::Pane>();
    spacer->setStyle("width: 100%;");
    spacer->setMouseTransparent(true);
    row->getChildren().add(std::move(spacer));
    if (id != 0 && id == current_) {
        auto check = jadefx::make<jadefx::StackPane>();
        check->getClassList().add("pe-pick-check");
        check->setMouseTransparent(true);
        if (std::shared_ptr<jadefx::ImageView> image = icon_graphic("Check.png")) {
            check->getChildren().add(std::move(image));
        }
        row->getChildren().add(std::move(check));
    }
    const int index = static_cast<int>(rows_.size());
    row->setOnMouseEntered([this, index](const jadefx::MouseEvent&) {
        active_ = index;
        paint();
    });
    row->setOnMouseClicked([this, index](const jadefx::MouseEvent& event) {
        if (event.button == 0) {
            choose(index);
        }
    });
    row->setElementId("pe-pick:" + std::to_string(id));
    list_->getChildren().add(row);
    rows_.emplace_back(id, std::move(row));
}

void AssetPicker::paint() {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        set_class(*rows_[index].second, "active", static_cast<int>(index) == active_);
    }
}

void AssetPicker::move(int delta) {
    if (rows_.empty()) {
        return;
    }
    const int count = static_cast<int>(rows_.size());
    active_ = std::clamp(active_ + delta, 0, count - 1);
    paint();
    // Keeps the highlighted row in view: the scroll value runs 0 to 1 over what is hidden.
    const double row = kPickerRowHeight + 1;
    const double view = scroll_->getPrefViewportHeight();
    const double hidden = count * row - view;
    if (hidden > 0) {
        const double top = scroll_->getVvalue() * hidden;
        const double at = active_ * row;
        if (at < top) {
            scroll_->setVvalue(at / hidden);
        } else if (at + row > top + view) {
            scroll_->setVvalue(std::min(1.0, (at + row - view) / hidden));
        }
    }
}

void AssetPicker::choose(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return;
    }
    const engine_core::InstanceId id = rows_[static_cast<std::size_t>(index)].first;
    // Closed before the pick runs, which may open the next picker.
    auto pick = pick_;
    dismiss();
    if (pick) {
        pick(id);
    }
}

}  // namespace ide
