#include "PreferencesPanel.hpp"

#include "IdeIcons.hpp"
#include "IdeResources.hpp"
#include "Preferences.hpp"
#include "ThemeLibrary.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace ide {
namespace {

constexpr double kLabelWidth = 172;
constexpr double kPickerWidth = 150;

// Colors are the theme's, so the window changes with the studio as colors are edited.
constexpr const char* kStylesheet = R"CSS(
.prefs {
    background-color: var(--ide-panel-color);
    color: var(--ide-text-color);
    font-family: "Open Sans";
    font-size: 13px;
}
.prefs-top {
    padding: 12px 14px 8px 14px;
    spacing: 8px;
}
.prefs-row {
    spacing: 6px;
}
.prefs-hint {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.prefs-list {
    padding: 0 14px 12px 14px;
    spacing: 2px;
}
.prefs-heading {
    color: var(--ide-muted-text-color);
    font-size: 12px;
    padding: 10px 0 4px 0;
    spacing: 4px;
}
.prefs-heading:hover {
    color: var(--ide-text-color);
}
.prefs-heading image-view {
    image-color: currentColor;
}
.prefs-count {
    color: var(--ide-muted-text-color);
    font-size: 11px;
    padding: 0 0 0 4px;
}
.prefs-group-body {
    padding: 0 0 0 20px;
    spacing: 2px;
}
.prefs-color-row {
    spacing: 8px;
}
.prefs-variable {
    color: var(--ide-muted-text-color);
    font-size: 11px;
}
.prefs-footer, .prefs-save-as {
    spacing: 6px;
    padding: 8px 14px;
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--border-color);
}
.prefs-status {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.prefs-status.error {
    color: var(--ide-error-text-color);
}
)CSS";

// A picker that says when its chooser closes, whether or not it chose a color.
class ThemeColorPicker : public jadefx::ColorPicker {
public:
    std::function<void()> on_hidden;

protected:
    void popupHidden() override {
        jadefx::ColorPicker::popupHidden();
        if (on_hidden) {
            on_hidden();
        }
    }
};

std::string Lower(std::string text) {
    for (char& unit : text) {
        unit = static_cast<char>(std::tolower(static_cast<unsigned char>(unit)));
    }
    return text;
}

std::string Trim(const std::string& text) {
    const auto space = [](char unit) { return std::isspace(static_cast<unsigned char>(unit)) != 0; };
    const auto begin = std::find_if_not(text.begin(), text.end(), space);
    const auto end = std::find_if_not(text.rbegin(), text.rend(), space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

const std::string* Find(const ThemeValues& values, const std::string& name) {
    for (const auto& [key, value] : values) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

void Put(ThemeValues& values, const std::string& name, std::string value) {
    for (auto& [key, old] : values) {
        if (key == name) {
            old = std::move(value);
            return;
        }
    }
    values.emplace_back(name, std::move(value));
}

bool IsMeta(const std::string& name) { return name == "--theme-name" || name == "--theme-base"; }

// Two values that draw the same: equal colors however they are written, or equal text.
bool SameValue(const std::string& a, const std::string& b) {
    bool okA = false;
    bool okB = false;
    const jadefx::Color colorA = jadefx::Color::parse(a, &okA);
    const jadefx::Color colorB = jadefx::Color::parse(b, &okB);
    return okA && okB ? css_color(colorA) == css_color(colorB) : a == b;
}

bool IsThemeFile(const std::string& path) {
    return path.size() > 4 && Lower(path.substr(path.size() - 4)) == ".css";
}

std::shared_ptr<jadefx::Button> MakeButton(const char* text, std::function<void()> action) {
    auto button = jadefx::make<jadefx::Button>(text);
    button->setOnAction([action = std::move(action)](jadefx::ActionEvent&) { action(); });
    return button;
}

// Takes what is left of a row, pushing what follows it to the right edge.
std::shared_ptr<jadefx::Pane> Spacer() {
    auto spacer = jadefx::make<jadefx::Pane>();
    spacer->setStyle("width: 100%;");
    return spacer;
}

}  // namespace

struct PreferencesPanel::Row {
    const ThemeVariable* variable = nullptr;
    std::shared_ptr<jadefx::HBox> box;
    std::shared_ptr<ThemeColorPicker> picker;
    std::shared_ptr<jadefx::Button> reset;
};

// A heading and the rows under it. The panel owns every part, so taking the
// rows out of the list or putting them back frees nothing, even mid-click.
struct PreferencesPanel::Group {
    std::string title;
    std::vector<Row*> rows;
    // Closed until its heading is clicked, so the list starts as a short list of groups.
    bool expanded = false;
    std::shared_ptr<jadefx::VBox> box;
    std::shared_ptr<jadefx::HBox> heading;
    std::shared_ptr<jadefx::StackPane> arrow;
    std::shared_ptr<jadefx::Label> count;
    std::shared_ptr<jadefx::VBox> body;
    // Where the arrow points now, so it is swapped only when that changes.
    int shown = -1;
};

PreferencesPanel::PreferencesPanel(ThemeLibrary& themes, Preferences& preferences)
    : themes_(themes), preferences_(preferences) {
    file_picker_ = [alive = std::weak_ptr<bool>(alive_)](std::function<void(const std::string&)> chosen) {
        jadefx::FolderDialogOptions options;
        options.title = "Import Theme";
        options.file = true;
        options.extensions = {"css"};
        jadefx::showFolderDialog(std::move(options), [alive, chosen = std::move(chosen)](jadefx::DialogResult result,
                                                                                         const std::string& path) {
            if (result == jadefx::DialogResult::Chosen && !alive.expired()) {
                chosen(path);
            }
        });
    };
    build();
    if (!show_theme(preferences_.theme(), false)) {
        show_theme("light", false);
    }
}

PreferencesPanel::~PreferencesPanel() = default;

void PreferencesPanel::build() {
    getClassList().add("prefs");
    // The window's whole height, however short the list of colors is.
    setPrefWidthRatio(1);
    setPrefHeightRatio(1);
    setStylesheet(kStylesheet);

    auto theme_row = jadefx::make<jadefx::HBox>();
    theme_row->getClassList().add("prefs-row");
    theme_row->setAlignment(jadefx::Pos::CenterLeft);
    theme_row->getChildren().add(jadefx::make<jadefx::Label>("Theme"));
    theme_list_ = jadefx::make<jadefx::ComboBox>();
    theme_list_->getClassList().add("prefs-theme-list");
    theme_list_->setPrefWidth(220);
    theme_list_->setOnAction([this](jadefx::ActionEvent&) {
        const int index = theme_list_->getSelectionIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= entries_.size()) {
            return;
        }
        const std::string id = entries_[static_cast<std::size_t>(index)].id;
        // On the next frame: picking a theme lists the themes again, and the list
        // is still delivering the click to the row that fired this.
        jadefx::runLater([this, alive = std::weak_ptr<bool>(alive_), id] {
            if (alive.expired() || id == id_) {
                return;
            }
            if (!modified_) {
                select_theme(id);
                return;
            }
            // The list shows the edited theme until the answer.
            refresh_list();
            ask("Discard your changes to " + display_name() + "?", "The colors you changed have not been saved.",
                "Discard", [this, id] { select_theme(id); }, jadefx::AlertType::Warning);
        });
    });
    theme_row->getChildren().add(theme_list_);
    theme_row->getChildren().add(MakeButton("Import…", [this] { choose_import(); }));
    delete_ = MakeButton("Delete", [this] {
        ask("Delete the theme " + display_name() + "?", "Its file is removed from your themes folder.", "Delete",
            [this] { delete_theme(); }, jadefx::AlertType::Warning);
    });
    theme_row->getChildren().add(delete_);
    theme_row->getChildren().add(Spacer());
    folder_ = MakeButton("Open Themes Folder", [this] {
        std::error_code error;
        std::filesystem::create_directories(themes_.folder(), error);
        if (error || !reveal_folder(themes_.folder())) {
            set_status("Could not open " + utf8_path(themes_.folder()), true);
        }
    });
    theme_row->getChildren().add(folder_);

    auto hint = jadefx::make<jadefx::Label>(
        "Colors change as you edit them. Built-in themes cannot be changed, so their edits save as a new theme.");
    hint->getClassList().add("prefs-hint");
    filter_field_ = jadefx::make<jadefx::TextField>();
    filter_field_->getClassList().add("prefs-filter");
    filter_field_->setPromptText("Filter colors");
    filter_field_->setStyle("width: 100%;");

    auto top = jadefx::make<jadefx::VBox>();
    top->getClassList().add("prefs-top");
    top->getChildren().add(theme_row);
    top->getChildren().add(hint);
    top->getChildren().add(filter_field_);

    list_ = jadefx::make<jadefx::VBox>();
    list_->getClassList().add("prefs-list");
    for (const std::vector<ThemeVariable>* variables : {&theme_variables(), &control_variables()}) {
        for (const ThemeVariable& variable : *variables) {
            auto row = std::make_unique<Row>();
            row->variable = &variable;
            row->box = jadefx::make<jadefx::HBox>();
            row->box->getClassList().add("prefs-color-row");
            row->box->setAlignment(jadefx::Pos::CenterLeft);
            auto label = jadefx::make<jadefx::Label>(variable.label);
            label->setPrefWidth(kLabelWidth);
            label->setMinSize(kLabelWidth, 0);
            row->picker = jadefx::make<ThemeColorPicker>();
            row->picker->setPrefWidth(kPickerWidth);
            row->picker->setMinSize(kPickerWidth, 0);
            const std::string name = variable.name;
            ThemeColorPicker* picker = row->picker.get();
            // While the chooser is open the studio shows the color; closing on it keeps it.
            picker->setOnValueChanged([this, name, picker] {
                if (refreshing_) {
                    return;
                }
                ThemeValues preview = edits_;
                Put(preview, name, css_color(picker->getValue()));
                apply(preview);
            });
            picker->setOnAction([this, name, picker](jadefx::ActionEvent&) { set_color(name, picker->getValue()); });
            picker->on_hidden = [this] { apply(edits_); };
            row->reset = MakeButton("Reset", [this, name] { reset_color(name); });
            auto code = jadefx::make<jadefx::Label>(variable.name);
            code->getClassList().add("prefs-variable");
            row->box->getChildren().add(label);
            row->box->getChildren().add(row->picker);
            row->box->getChildren().add(row->reset);
            row->box->getChildren().add(code);
            if (groups_.empty() || groups_.back()->title != variable.group) {
                auto group = std::make_unique<Group>();
                group->title = variable.group;
                group->box = jadefx::make<jadefx::VBox>();
                group->heading = jadefx::make<jadefx::HBox>();
                group->heading->getClassList().add("prefs-heading");
                group->heading->setAlignment(jadefx::Pos::CenterLeft);
                group->heading->setCursor(jadefx::Cursor::Pointer);
                group->arrow = jadefx::make<jadefx::StackPane>();
                group->arrow->setMouseTransparent(true);
                group->arrow->setMinSize(16, 16);
                group->arrow->setPrefSize(16, 16);
                auto title = jadefx::make<jadefx::Label>(group->title);
                title->setMouseTransparent(true);
                group->count = jadefx::make<jadefx::Label>("");
                group->count->getClassList().add("prefs-count");
                group->count->setMouseTransparent(true);
                group->heading->getChildren().add(group->arrow);
                group->heading->getChildren().add(title);
                group->heading->getChildren().add(group->count);
                Group* raw = group.get();
                group->heading->setOnMouseClicked([this, raw](const jadefx::MouseEvent&) { toggle_group(*raw); });
                group->body = jadefx::make<jadefx::VBox>();
                group->body->getClassList().add("prefs-group-body");
                group->box->getChildren().add(group->heading);
                groups_.push_back(std::move(group));
            }
            groups_.back()->rows.push_back(row.get());
            rows_.push_back(std::move(row));
        }
    }
    auto scroll = jadefx::make<jadefx::ScrollPane>(list_);
    scroll->getClassList().add("prefs-scroll");
    scroll->setFitToWidth(true);

    save_as_row_ = jadefx::make<jadefx::HBox>();
    save_as_row_->getClassList().add("prefs-save-as");
    save_as_row_->setAlignment(jadefx::Pos::CenterLeft);
    save_as_row_->getChildren().add(jadefx::make<jadefx::Label>("Name"));
    save_as_name_ = jadefx::make<jadefx::TextField>();
    save_as_name_->setStyle("width: 100%;");
    save_as_name_->setOnAction([this](jadefx::ActionEvent&) { save_as(save_as_name_->getText()); });
    save_as_row_->getChildren().add(save_as_name_);
    save_as_row_->getChildren().add(MakeButton("Save", [this] { save_as(save_as_name_->getText()); }));
    save_as_row_->getChildren().add(MakeButton("Cancel", [this] { show_save_as(false); }));

    auto footer = jadefx::make<jadefx::HBox>();
    footer->getClassList().add("prefs-footer");
    footer->setPrefWidthRatio(1);
    footer->setAlignment(jadefx::Pos::CenterLeft);
    status_ = jadefx::make<jadefx::Label>("");
    status_->getClassList().add("prefs-status");
    status_->setStyle("width: 100%;");
    footer->getChildren().add(status_);
    revert_ = MakeButton("Revert", [this] { revert(); });
    save_as_ = MakeButton("Save As…", [this] { show_save_as(true); });
    save_ = MakeButton("Save", [this] {
        if (shipped_) {
            show_save_as(true);
        } else {
            save();
        }
    });
    footer->getChildren().add(revert_);
    footer->getChildren().add(save_as_);
    footer->getChildren().add(save_);
    bottom_ = jadefx::make<jadefx::VBox>();
    bottom_->getChildren().add(footer);

    auto appearance = jadefx::make<jadefx::BorderPane>();
    appearance->setPrefWidthRatio(1);
    appearance->setPrefHeightRatio(1);
    appearance->setTop(top);
    appearance->setCenter(scroll);
    appearance->setBottom(bottom_);
    auto tabs = jadefx::make<jadefx::TabPane>();
    tabs->setPrefWidthRatio(1);
    tabs->setPrefHeightRatio(1);
    auto tab = std::make_shared<jadefx::Tab>("Appearance", appearance);
    tab->setClosable(false);
    tabs->getTabs().add(tab);
    auto performance = std::make_shared<jadefx::Tab>("Performance", build_performance());
    performance->setClosable(false);
    tabs->getTabs().add(performance);
    setCenter(tabs);

    // A theme file dropped on the window is imported.
    setOnDragOver([](jadefx::DragEvent& event) {
        if (event.dragboard != nullptr) {
            const std::vector<std::string>& files = event.getDragboard().getFiles();
            if (std::any_of(files.begin(), files.end(), IsThemeFile)) {
                event.acceptTransferModes(jadefx::TransferMode::Copy);
                event.consume();
            }
        }
    });
    setOnDragDropped([this](jadefx::DragEvent& event) {
        bool imported = false;
        if (event.dragboard != nullptr) {
            for (const std::string& file : event.getDragboard().getFiles()) {
                if (IsThemeFile(file)) {
                    imported = import_theme(file) || imported;
                }
            }
        }
        event.setDropCompleted(imported);
        event.consume();
    });
    rebuild_rows();
}

std::shared_ptr<jadefx::Node> PreferencesPanel::build_performance() {
    auto rate_row = jadefx::make<jadefx::HBox>();
    rate_row->getClassList().add("prefs-row");
    rate_row->setAlignment(jadefx::Pos::CenterLeft);
    auto label = jadefx::make<jadefx::Label>("Frame rate limit");
    label->setPrefWidth(kLabelWidth);
    label->setMinSize(kLabelWidth, 0);
    rate_row->getChildren().add(label);
    frame_rate_ = jadefx::make<jadefx::TextField>();
    frame_rate_->getClassList().add("prefs-frame-rate");
    frame_rate_->setPrefWidth(80);
    frame_rate_->setText(std::to_string(preferences_.frame_rate()));
    frame_rate_->setOnAction([this](jadefx::ActionEvent&) { set_frame_rate(frame_rate_->getText()); });
    rate_row->getChildren().add(frame_rate_);
    rate_row->getChildren().add(jadefx::make<jadefx::Label>("fps"));
    rate_row->getChildren().add(MakeButton("Apply", [this] { set_frame_rate(frame_rate_->getText()); }));

    auto hint = jadefx::make<jadefx::Label>(
        "The most frames a second the studio draws. -1 is uncapped. From " +
        std::to_string(Preferences::kMinFrameRate) + " to " + std::to_string(Preferences::kMaxFrameRate) +
        " otherwise. The game's simulation stays at 60 Hz.");
    hint->getClassList().add("prefs-hint");
    rate_status_ = jadefx::make<jadefx::Label>("");
    rate_status_->getClassList().add("prefs-status");

    auto page = jadefx::make<jadefx::VBox>();
    page->getClassList().add("prefs-top");
    page->setPrefWidthRatio(1);
    page->setPrefHeightRatio(1);
    page->getChildren().add(rate_row);
    page->getChildren().add(hint);
    page->getChildren().add(rate_status_);
    return page;
}

bool PreferencesPanel::set_frame_rate(const std::string& text) {
    const std::string trimmed = Trim(text);
    int fps = 0;
    std::size_t used = 0;
    try {
        fps = std::stoi(trimmed, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (trimmed.empty() || used != trimmed.size() ||
        (fps != Preferences::kUncappedFrameRate &&
         (fps < Preferences::kMinFrameRate || fps > Preferences::kMaxFrameRate))) {
        set_rate_status("Type -1 for uncapped, or a whole number from " + std::to_string(Preferences::kMinFrameRate) +
                            " to " + std::to_string(Preferences::kMaxFrameRate) + ".",
                        true);
        return false;
    }
    preferences_.set_frame_rate(fps);
    frame_rate_->setText(std::to_string(fps));
    if (on_frame_rate_) {
        on_frame_rate_(fps);
    }
    std::string failure;
    if (!preferences_.save(failure)) {
        set_rate_status("Could not save your preferences: " + failure, true);
        return false;
    }
    set_rate_status(fps == Preferences::kUncappedFrameRate ? "The studio draws uncapped."
                                                           : "The studio draws at up to " + std::to_string(fps) + " fps.");
    return true;
}

void PreferencesPanel::layoutChildren() {
    if (filter_field_ && Lower(Trim(filter_field_->getText())) != filter_) {
        filter_ = Lower(Trim(filter_field_->getText()));
        rebuild_rows();
    }
    jadefx::BorderPane::layoutChildren();
}

bool PreferencesPanel::select_theme(const std::string& id) {
    if (!show_theme(id, true)) {
        return false;
    }
    set_status("");
    return true;
}

bool PreferencesPanel::show_theme(const std::string& id, bool remember) {
    IdeTheme theme;
    std::string error;
    if (!themes_.load(id, theme, error)) {
        set_status("Could not open the theme: " + error, true);
        return false;
    }
    id_ = id;
    shipped_ = is_shipped_theme(id);
    base_ = theme.base();
    name_ = theme.name();
    edits_.clear();
    if (shipped_) {
        // Edits sit on all of it, and a copy of it extends it.
        inherited_ = theme.inherited();
        inherited_.insert(inherited_.end(), theme.declared().begin(), theme.declared().end());
        parent_ = id;
    } else {
        inherited_ = theme.inherited();
        parent_ = themes_.parent_of(theme);
        for (const auto& [key, value] : theme.declared()) {
            if (!IsMeta(key)) {
                edits_.emplace_back(key, value);
            }
        }
    }
    modified_ = false;
    if (remember) {
        preferences_.set_theme(id);
        std::string failure;
        if (!preferences_.save(failure)) {
            set_status("Could not save your preferences: " + failure, true);
        }
    }
    apply(edits_);
    refresh_list();
    refresh_rows();
    show_save_as(false);
    refresh_buttons();
    return true;
}

IdeTheme PreferencesPanel::working(const ThemeValues& edits) const {
    ThemeValues declared = {{"--theme-base", base_}};
    declared.insert(declared.end(), edits.begin(), edits.end());
    return IdeTheme(std::move(declared), inherited_);
}

ThemeValues PreferencesPanel::pruned_edits() const {
    const IdeTheme under({{"--theme-base", base_}}, inherited_);
    const IdeTheme theme = working(edits_);
    ThemeValues kept;
    for (const auto& [name, value] : edits_) {
        const std::string before = under.value(name);
        if (!before.empty() && SameValue(before, theme.value(name))) {
            continue;
        }
        kept.emplace_back(name, value);
    }
    return kept;
}

void PreferencesPanel::apply(const ThemeValues& edits) { set_current_theme(working(edits)); }

void PreferencesPanel::set_color(const std::string& name, const jadefx::Color& color) {
    Put(edits_, name, css_color(color));
    modified_ = true;
    apply(edits_);
    refresh_rows();
    refresh_buttons();
    set_status("");
}

void PreferencesPanel::reset_color(const std::string& name) {
    const auto found = std::find_if(edits_.begin(), edits_.end(), [&](const auto& item) { return item.first == name; });
    if (found == edits_.end()) {
        return;
    }
    edits_.erase(found);
    modified_ = true;
    apply(edits_);
    refresh_rows();
    refresh_buttons();
    set_status("");
}

bool PreferencesPanel::save() {
    const std::string name = display_name();
    if (shipped_) {
        set_status(name + " is built in. Save As keeps your colors as a new theme.", true);
        show_save_as(true);
        return false;
    }
    std::string error;
    if (themes_.save(id_, name, base_, parent_, pruned_edits(), error).empty()) {
        set_status("Could not save: " + error, true);
        return false;
    }
    show_theme(id_, true);
    set_status("Saved " + name + ".");
    return true;
}

bool PreferencesPanel::save_as(const std::string& name) {
    const std::string trimmed = Trim(name);
    if (trimmed.empty()) {
        set_status("Type a name for the theme.", true);
        return false;
    }
    std::string error;
    const std::string id = themes_.save("", trimmed, base_, parent_, pruned_edits(), error);
    if (id.empty()) {
        set_status("Could not save: " + error, true);
        return false;
    }
    show_theme(id, true);
    set_status("Saved " + display_name() + ".");
    return true;
}

void PreferencesPanel::revert() {
    const bool had = modified_;
    show_theme(id_, false);
    set_status(had ? "Your changes were dropped." : "");
}

bool PreferencesPanel::import_theme(const std::string& path) {
    std::string error;
    const std::string id = themes_.import_file(path_from_utf8(path), error);
    if (id.empty()) {
        set_status("Could not import: " + error, true);
        return false;
    }
    if (!show_theme(id, true)) {
        return false;
    }
    set_status("Imported " + display_name() + ".");
    return true;
}

bool PreferencesPanel::delete_theme() {
    const std::string name = display_name();
    if (shipped_) {
        set_status(name + " is built in and cannot be deleted.", true);
        return false;
    }
    std::string error;
    if (!themes_.remove(id_, error)) {
        set_status("Could not delete: " + error, true);
        return false;
    }
    show_theme(parent_, true);
    set_status("Deleted " + name + ".");
    return true;
}

bool PreferencesPanel::request_close(std::function<void()> close) {
    jadefx::Scene* scene = getScene();
    if (!modified_ || scene == nullptr) {
        if (modified_) {
            revert();
        }
        return true;
    }
    const jadefx::ButtonType save_type("Save", jadefx::ButtonType::Data::OkDone);
    const jadefx::ButtonType discard("Don't Save", jadefx::ButtonType::Data::Left);
    auto alert = std::make_shared<jadefx::Alert>(
        jadefx::AlertType::Warning, "Your color changes will be lost if you don't save them.",
        std::vector<jadefx::ButtonType>{save_type, discard, jadefx::ButtonType::Cancel()});
    alert->setTitle("Preferences");
    alert->setHeaderText("Save changes to the theme " + display_name() + "?");
    alert->setOnClosed([this, save_type, discard, close = std::move(close)](const jadefx::ButtonType* choice) {
        if (choice == nullptr) {
            return;
        }
        if (*choice == discard) {
            revert();
            close();
        } else if (*choice == save_type && save()) {
            // A shipped theme's save asks for a name instead, and the window stays.
            close();
        }
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene);
    alerts_.push_back(std::move(alert));
    return false;
}

jadefx::Node* PreferencesPanel::group_heading(const std::string& group) const {
    for (const std::unique_ptr<Group>& item : groups_) {
        if (item->title == group) {
            return item->heading.get();
        }
    }
    return nullptr;
}

jadefx::ColorPicker* PreferencesPanel::picker(const std::string& name) const {
    for (const std::unique_ptr<Row>& row : rows_) {
        if (row->variable->name == name) {
            return row->picker.get();
        }
    }
    return nullptr;
}

void PreferencesPanel::refresh_list() {
    entries_ = themes_.list();
    std::vector<std::string> names;
    int selected = -1;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        names.push_back(entries_[index].name);
        if (entries_[index].id == id_) {
            selected = static_cast<int>(index);
        }
    }
    // Setting the items rebuilds an open list's rows, so it is left alone when they are the same.
    if (theme_list_->getItems().items() != names) {
        theme_list_->getItems().setAll(std::move(names));
    }
    theme_list_->select(selected);
}

void PreferencesPanel::refresh_rows() {
    const IdeTheme theme = working(edits_);
    refreshing_ = true;
    for (const std::unique_ptr<Row>& row : rows_) {
        row->picker->setValue(theme.color(row->variable->name));
        row->reset->setDisable(Find(edits_, row->variable->name) == nullptr);
    }
    refreshing_ = false;
}

void PreferencesPanel::refresh_buttons() {
    delete_->setDisable(shipped_);
    folder_->setDisable(themes_.folder().empty());
    revert_->setDisable(!modified_);
    save_->setDisable(!modified_);
}

void PreferencesPanel::rebuild_rows() {
    list_->getChildren().clear();
    for (const std::unique_ptr<Group>& group : groups_) {
        group->body->getChildren().clear();
        for (Row* row : group->rows) {
            const ThemeVariable& variable = *row->variable;
            if (filter_.empty() || Lower(variable.label).find(filter_) != std::string::npos ||
                Lower(variable.name).find(filter_) != std::string::npos ||
                Lower(variable.group).find(filter_) != std::string::npos) {
                group->body->getChildren().add(row->box);
            }
        }
        const std::size_t shown = group->body->getChildren().size();
        if (shown == 0) {
            continue;
        }
        // A filtered group says how many of its colors match.
        group->count->setText(filter_.empty() ? std::to_string(shown)
                                              : std::to_string(shown) + " of " + std::to_string(group->rows.size()));
        place_body(*group);
        list_->getChildren().add(group->box);
    }
    if (list_->getChildren().empty()) {
        auto none = jadefx::make<jadefx::Label>("No colors match “" + Trim(filter_field_->getText()) + "”.");
        none->getClassList().add("prefs-hint");
        list_->getChildren().add(none);
    }
}

void PreferencesPanel::toggle_group(Group& group) {
    group.expanded = !group.expanded;
    place_body(group);
}

void PreferencesPanel::place_body(Group& group) {
    const bool open = group.expanded || !filter_.empty();
    const bool placed = group.box->getChildren().size() > 1;
    if (open && !placed) {
        group.box->getChildren().add(group.body);
    } else if (!open && placed) {
        group.box->getChildren().removeIf(
            [&group](const std::shared_ptr<jadefx::Node>& node) { return node == group.body; });
    }
    if (group.shown == static_cast<int>(open)) {
        return;
    }
    group.shown = static_cast<int>(open);
    group.arrow->getChildren().clear();
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(open ? "FindExpanded.png" : "FindCollapsed.png")) {
        group.arrow->getChildren().add(std::move(icon));
    } else {
        auto glyph = jadefx::make<jadefx::Label>(open ? "v" : ">");
        glyph->setMouseTransparent(true);
        group.arrow->getChildren().add(std::move(glyph));
    }
}

void PreferencesPanel::set_status(std::string text, bool error) {
    status_text_ = std::move(text);
    status_->setText(status_text_);
    status_->getClassList().removeIf([](const std::string& name) { return name == "error"; });
    if (error) {
        status_->getClassList().add("error");
    }
}

void PreferencesPanel::set_rate_status(std::string text, bool error) {
    rate_status_text_ = std::move(text);
    rate_status_->setText(rate_status_text_);
    rate_status_->getClassList().removeIf([](const std::string& name) { return name == "error"; });
    if (error) {
        rate_status_->getClassList().add("error");
    }
}

void PreferencesPanel::show_save_as(bool shown) {
    if (shown == save_as_shown_) {
        if (shown) {
            save_as_name_->requestFocus();
        }
        return;
    }
    save_as_shown_ = shown;
    if (!shown) {
        bottom_->getChildren().removeIf([this](const std::shared_ptr<jadefx::Node>& node) { return node == save_as_row_; });
        return;
    }
    bottom_->getChildren().insert(0, save_as_row_);
    save_as_name_->setText(shipped_ ? "My " + display_name() : display_name() + " Copy");
    save_as_name_->selectAll();
    save_as_name_->requestFocus();
}

void PreferencesPanel::ask(const std::string& header, const std::string& content, const std::string& yes,
                           std::function<void()> then, jadefx::AlertType type) {
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        then();
        return;
    }
    const jadefx::ButtonType confirm(yes, jadefx::ButtonType::Data::OkDone);
    auto alert = std::make_shared<jadefx::Alert>(type, content,
                                                 std::vector<jadefx::ButtonType>{confirm, jadefx::ButtonType::Cancel()});
    alert->setTitle("Preferences");
    alert->setHeaderText(header);
    alert->setOnClosed([confirm, then = std::move(then)](const jadefx::ButtonType* choice) {
        if (choice != nullptr && *choice == confirm) {
            then();
        }
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene);
    alerts_.push_back(std::move(alert));
}

void PreferencesPanel::choose_import() {
    auto pick = [this] {
        if (file_picker_) {
            file_picker_([this](const std::string& path) { import_theme(path); });
        }
    };
    if (!modified_) {
        pick();
        return;
    }
    ask("Discard your changes to " + display_name() + "?", "Importing a theme picks it, and drops the colors you changed.",
        "Discard", pick, jadefx::AlertType::Warning);
}

std::string PreferencesPanel::display_name() const {
    if (!name_.empty()) {
        return name_;
    }
    for (const ThemeEntry& entry : entries_) {
        if (entry.id == id_) {
            return entry.name;
        }
    }
    return id_;
}

}  // namespace ide
