#include "IdeConflicts.hpp"

#include "IdeIcons.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace ide {
namespace {

constexpr double kRowHeight = 24;
// A value longer than this many code points is cut, and shown whole on hover.
constexpr int kValueLength = 40;

constexpr const char* kConflictRules = R"CSS(
.conflicts-pane {
    background-color: var(--ide-panel-color);
}
.conflicts-header, .conflicts-footer {
    padding: 6px 8px;
    spacing: 6px;
}
.conflicts-summary, .conflicts-chosen {
    color: var(--ide-search-status-color);
    font-size: 12px;
}
.conflicts-problem {
    color: var(--ide-search-status-error-color);
    font-size: 12px;
    padding: 0 8px 6px 8px;
}
.conflicts-list {
    border-width: 1px 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.conflicts-path, .conflicts-side {
    color: var(--ide-search-path-color);
    font-size: 12px;
}
.conflicts-badge {
    background-color: var(--ide-search-badge-color);
    color: var(--ide-search-badge-text-color);
    border-radius: 8px;
    font-size: 11px;
    padding: 0 6px;
}
.conflict-toggle togglebutton {
    font-size: 11px;
    padding: 1px 8px;
}
)CSS";

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    if (style_class != nullptr) {
        label->getClassList().add(style_class);
    }
    label->setMouseTransparent(true);
    return label;
}

// A value cut to kValueLength code points, whole in a tooltip when cut.
std::shared_ptr<jadefx::Label> value_label(const std::string& text) {
    int points = 0;
    std::size_t cut = text.size();
    for (std::size_t byte = 0; byte < text.size(); ++byte) {
        if ((static_cast<unsigned char>(text[byte]) & 0xC0u) != 0x80u && points++ == kValueLength) {
            cut = byte;
            break;
        }
    }
    if (cut == text.size()) {
        return text_label(text, nullptr);
    }
    auto label = jadefx::make<jadefx::Label>(text.substr(0, cut) + "…");
    jadefx::Tooltip::install(label.get(), jadefx::make<jadefx::Tooltip>(text));
    return label;
}

std::string counted(std::size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// What a row with no key is about: the whole instance.
std::string whole_instance(const engine_core::SaveConflict& row) {
    if (row.kind == engine_core::SaveConflict::Kind::DeletedOutside) {
        return "deleted on disk, changed in the studio";
    }
    if (row.disk == "can't be read") {
        return "changed on disk, and can't be read";
    }
    return "deleted in the studio, changed on disk";
}

}  // namespace

// IDE and Disk, side by side; one or neither is on. A click on the one that is
// on turns it off.
class SideToggle : public jadefx::HBox {
public:
    explicit SideToggle(std::function<void(std::optional<bool>)> picked) : picked_(std::move(picked)) {
        getClassList().add("conflict-toggle");
        setAlignment(jadefx::Pos::CenterRight);
        ide_ = jadefx::make<jadefx::ToggleButton>("IDE");
        disk_ = jadefx::make<jadefx::ToggleButton>("Disk");
        ide_->getClassList().add("conflict-ide");
        disk_->getClassList().add("conflict-disk");
        for (const std::shared_ptr<jadefx::ToggleButton>& button : {ide_, disk_}) {
            button->setToggleGroup(&group_);
            button->setOnAction([this](jadefx::ActionEvent&) { report(); });
            getChildren().add(button);
        }
    }

    // Shows a pick without reporting it.
    void show(std::optional<bool> pick) {
        jadefx::Toggle* on = nullptr;
        if (pick) {
            on = *pick ? static_cast<jadefx::Toggle*>(disk_.get()) : static_cast<jadefx::Toggle*>(ide_.get());
        }
        if (group_.getSelectedToggle() != on) {
            group_.selectToggle(on);
        }
    }

private:
    void report() {
        const jadefx::Toggle* on = group_.getSelectedToggle();
        picked_(on == nullptr ? std::nullopt : std::optional<bool>(on == disk_.get()));
    }

    std::function<void(std::optional<bool>)> picked_;
    jadefx::ToggleGroup group_;
    std::shared_ptr<jadefx::ToggleButton> ide_;
    std::shared_ptr<jadefx::ToggleButton> disk_;
};

IdeConflicts::IdeConflicts(ConflictsHost host) : IdePane("Conflicts", true), host_(std::move(host)) {
    setIconFile("Warning.png");
    getClassList().add("conflicts-pane");
    setStylesheet(kConflictRules);
    setMinSize(200, 140);

    summary_ = text_label("", "conflicts-summary");
    all_ide_ = jadefx::make<jadefx::Button>("All IDE");
    all_ide_->getClassList().add("conflicts-all-ide");
    all_ide_->setOnAction([this](jadefx::ActionEvent&) { chooseAll(false); });
    all_disk_ = jadefx::make<jadefx::Button>("All Disk");
    all_disk_->getClassList().add("conflicts-all-disk");
    all_disk_->setOnAction([this](jadefx::ActionEvent&) { chooseAll(true); });
    // Two lines, so a narrow side dock still shows both buttons.
    auto picks = jadefx::make<jadefx::HBox>();
    picks->setSpacing(6);
    picks->getChildren().add(all_ide_);
    picks->getChildren().add(all_disk_);
    auto header = jadefx::make<jadefx::VBox>();
    header->getClassList().add("conflicts-header");
    header->setStyle("width: 100%;");
    header->getChildren().add(summary_);
    header->getChildren().add(picks);
    problem_ = text_label("", "conflicts-problem");
    top_ = jadefx::make<jadefx::VBox>();
    top_->setStyle("width: 100%;");
    top_->getChildren().add(header);

    root_ = jadefx::make<jadefx::TreeItem>("");
    root_->setExpanded(true);
    tree_ = jadefx::make<jadefx::TreeView>();
    tree_->setRoot(root_);
    tree_->getClassList().add("conflicts-list");
    tree_->setShowRoot(false);
    tree_->setFixedCellSize(kRowHeight);
    // Row clicks bubble here after the row has selected itself.
    tree_->setOnMouseClicked([this](const jadefx::MouseEvent& event) { clicked(event); });

    chosen_ = text_label("", "conflicts-chosen");
    refresh_ = jadefx::make<jadefx::Button>("Refresh");
    refresh_->getClassList().add("conflicts-refresh");
    refresh_->setOnAction([this](jadefx::ActionEvent&) {
        if (host_.refresh) {
            host_.refresh();
        }
    });
    apply_ = jadefx::make<jadefx::Button>("Apply");
    apply_->getClassList().add("conflicts-apply");
    apply_->setOnAction([this](jadefx::ActionEvent&) { apply(); });
    auto buttons = jadefx::make<jadefx::HBox>();
    buttons->setSpacing(6);
    buttons->getChildren().add(refresh_);
    buttons->getChildren().add(apply_);
    auto footer = jadefx::make<jadefx::VBox>();
    footer->getClassList().add("conflicts-footer");
    footer->setStyle("width: 100%;");
    footer->getChildren().add(chosen_);
    footer->getChildren().add(buttons);

    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setTop(top_);
    column->setCenter(tree_);
    column->setBottom(footer);
    getChildren().add(column);
    rebuild();
}

IdeConflicts::~IdeConflicts() = default;

IdeConflicts::Identity IdeConflicts::identity(const engine_core::SaveConflict& row) {
    return Identity{row.guid, row.key, row.studio, row.disk};
}

void IdeConflicts::setConflicts(std::vector<engine_core::SaveConflict> rows) {
    std::map<Identity, bool> kept;
    for (const engine_core::SaveConflict& row : rows) {
        const auto found = picks_.find(identity(row));
        if (found != picks_.end()) {
            kept.insert(*found);
        }
    }
    picks_ = std::move(kept);
    rows_ = std::move(rows);
    // The tab counts the rows, as "Conflicts (2)". None leaves it "Conflicts".
    setTitle(rows_.empty() ? std::string() : name() + " (" + std::to_string(rows_.size()) + ")");
    rebuild();
}

void IdeConflicts::setProblem(const std::string& text) {
    problem_->setText(text);
    const bool shown = std::any_of(top_->getChildren().items().begin(), top_->getChildren().items().end(),
                                   [this](const std::shared_ptr<jadefx::Node>& child) { return child == problem_; });
    if (text.empty() && shown) {
        const jadefx::Label* label = problem_.get();
        top_->getChildren().removeIf([label](const std::shared_ptr<jadefx::Node>& child) { return child.get() == label; });
    } else if (!text.empty() && !shown) {
        top_->getChildren().add(problem_);
    }
}

void IdeConflicts::setApplyEnabled(bool enabled) {
    apply_enabled_ = enabled;
    sync();
}

std::optional<bool> IdeConflicts::pick(std::size_t index) const {
    if (index >= rows_.size()) {
        return std::nullopt;
    }
    const auto found = picks_.find(identity(rows_[index]));
    return found == picks_.end() ? std::nullopt : std::optional<bool>(found->second);
}

void IdeConflicts::set_pick(std::size_t index, std::optional<bool> pick) {
    if (index >= rows_.size()) {
        return;
    }
    if (pick) {
        picks_[identity(rows_[index])] = *pick;
    } else {
        picks_.erase(identity(rows_[index]));
    }
}

void IdeConflicts::choose(std::size_t index, bool disk) {
    set_pick(index, disk);
    sync();
}

void IdeConflicts::chooseInstance(const std::string& guid, bool disk) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].guid == guid) {
            set_pick(index, disk);
        }
    }
    sync();
}

void IdeConflicts::chooseAll(bool disk) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        set_pick(index, disk);
    }
    sync();
}

std::vector<engine_core::DiskChoice> IdeConflicts::choices() const {
    std::vector<engine_core::DiskChoice> out;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (const std::optional<bool> side = pick(index)) {
            out.push_back({rows_[index], *side});
        }
    }
    return out;
}

void IdeConflicts::apply() {
    const std::vector<engine_core::DiskChoice> picked = choices();
    if (!picked.empty() && apply_enabled_ && host_.apply) {
        host_.apply(picked);
    }
}

std::string IdeConflicts::summary() const { return summary_->getText(); }

std::string IdeConflicts::chosenText() const { return chosen_->getText(); }

bool IdeConflicts::showInstance(const jadefx::TreeItem* item) {
    const auto found = items_.find(item);
    if (found == items_.end()) {
        return false;
    }
    if (host_.select) {
        // Copied first: showing the instance can refresh this list.
        const std::string guid = found->second;
        host_.select(guid);
    }
    return true;
}

void IdeConflicts::clicked(const jadefx::MouseEvent& event) {
    if (event.button != 0) {
        return;
    }
    // A toggle's click picks a side; the disclosure arrow opens or closes the group.
    for (jadefx::Node* node = tree_->pick(event.x, event.y); node != nullptr && node != tree_.get();
         node = node->getParent()) {
        const std::string_view type = node->getElementType();
        if (type == "togglebutton" || type == "tree-disclosure-node") {
            return;
        }
        if (type == "tree-cell") {
            showInstance(tree_->getSelectedItem());
            return;
        }
    }
}

void IdeConflicts::rebuild() {
    items_.clear();
    row_toggles_.assign(rows_.size(), nullptr);
    group_toggles_.clear();
    std::vector<std::shared_ptr<jadefx::TreeItem>> groups;
    std::map<std::string, jadefx::TreeItem*> group_of;
    std::map<std::string, std::size_t> counts;
    for (const engine_core::SaveConflict& row : rows_) {
        ++counts[row.guid];
    }
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const engine_core::SaveConflict& row = rows_[index];
        if (group_of.count(row.guid) == 0) {
            auto box = jadefx::make<jadefx::HBox>();
            box->setSpacing(6);
            box->setAlignment(jadefx::Pos::CenterLeft);
            // The toggle first, so a narrow dock never pushes it out of sight.
            const std::string guid = row.guid;
            auto toggle = jadefx::make<SideToggle>([this, guid](std::optional<bool> side) {
                for (std::size_t at = 0; at < rows_.size(); ++at) {
                    if (rows_[at].guid == guid) {
                        set_pick(at, side);
                    }
                }
                sync();
            });
            box->getChildren().add(toggle);
            group_toggles_[guid] = toggle;
            const std::string klass = host_.class_of ? host_.class_of(row.guid) : std::string();
            if (std::shared_ptr<jadefx::ImageView> icon = klass.empty() ? nullptr : icon_view(klass)) {
                icon->setPrefSize(16, 16);
                box->getChildren().add(icon);
            }
            box->getChildren().add(text_label(row.name.empty() ? row.path : row.name, nullptr));
            box->getChildren().add(text_label(row.where, "conflicts-path"));
            box->getChildren().add(text_label(std::to_string(counts[row.guid]), "conflicts-badge"));
            auto group = jadefx::make<jadefx::TreeItem>("", box);
            group->setExpanded(true);
            items_[group.get()] = guid;
            group_of[guid] = group.get();
            groups.push_back(std::move(group));
        }
        auto box = jadefx::make<jadefx::HBox>();
        box->setSpacing(6);
        box->setAlignment(jadefx::Pos::CenterLeft);
        auto toggle = jadefx::make<SideToggle>([this, index](std::optional<bool> side) {
            set_pick(index, side);
            sync();
        });
        box->getChildren().add(toggle);
        row_toggles_[index] = toggle;
        if (row.key.empty()) {
            box->getChildren().add(text_label(whole_instance(row), nullptr));
        } else {
            box->getChildren().add(text_label(row.key, nullptr));
            box->getChildren().add(text_label("IDE", "conflicts-side"));
            box->getChildren().add(value_label(row.studio));
            box->getChildren().add(text_label("Disk", "conflicts-side"));
            box->getChildren().add(value_label(row.disk));
        }
        auto item = jadefx::make<jadefx::TreeItem>("", box);
        items_[item.get()] = row.guid;
        group_of[row.guid]->getChildren().add(std::move(item));
    }
    root_->getChildren().setAll(std::move(groups));
    sync();
}

void IdeConflicts::sync() {
    std::size_t chosen = 0;
    std::map<std::string, std::optional<bool>> group_picks;
    std::map<std::string, bool> group_mixed;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const std::optional<bool> side = pick(index);
        chosen += side ? 1 : 0;
        if (row_toggles_.size() > index && row_toggles_[index]) {
            row_toggles_[index]->show(side);
        }
        const std::string& guid = rows_[index].guid;
        if (group_picks.count(guid) == 0 && group_mixed.count(guid) == 0) {
            group_picks[guid] = side;
        } else if (group_picks[guid] != side) {
            group_mixed[guid] = true;
        }
    }
    // A group shows a side only when every row under it has that side.
    for (const auto& [guid, toggle] : group_toggles_) {
        toggle->show(group_mixed.count(guid) != 0 ? std::nullopt : group_picks[guid]);
    }
    const std::size_t instances = group_toggles_.size();
    summary_->setText(rows_.empty() ? std::string("No conflicts. Changes made outside the studio load when you switch "
                                                  "back to it.")
                                    : counted(rows_.size(), "conflict", "conflicts") + " in " +
                                          counted(instances, "instance", "instances"));
    if (!apply_enabled_ && !rows_.empty()) {
        chosen_->setText("Stop the test to apply");
    } else {
        chosen_->setText(rows_.empty() ? std::string()
                                       : std::to_string(chosen) + " of " + std::to_string(rows_.size()) + " chosen");
    }
    all_ide_->setDisable(rows_.empty());
    all_disk_->setDisable(rows_.empty());
    apply_->setDisable(chosen == 0 || !apply_enabled_);
}

}  // namespace ide
