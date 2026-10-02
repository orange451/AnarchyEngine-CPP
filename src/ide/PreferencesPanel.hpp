#pragma once

#include "IdeTheme.hpp"
#include "ThemeLibrary.hpp"

#include "jadefx/jadefx.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ide {

class Preferences;

// What the Preferences window shows. Its Appearance tab picks the theme the
// studio draws with, imports a theme file (from a file dialog, or dropped on
// the window), and edits the theme's colors, which the studio shows as they
// change. The colors are listed in groups, closed at first; a click on a
// group's heading opens or closes it, and a filter opens every group with a match. Each color can go back to what the theme under it gives. A shipped
// theme is never written: its edits save as a new theme of the user's, built
// on it. A choice of theme is written to preferences.json at once. Its
// Performance tab sets the most frames a second the studio draws.
class PreferencesPanel : public jadefx::BorderPane {
public:
    // Asks for a file and passes its path to chosen, or never calls it. The
    // system file dialog unless set_file_picker gives another.
    using FilePicker = std::function<void(std::function<void(const std::string& path)> chosen)>;

    PreferencesPanel(ThemeLibrary& themes, Preferences& preferences);
    ~PreferencesPanel() override;

    void set_file_picker(FilePicker picker) { file_picker_ = std::move(picker); }
    // Adds a tab after the panel's own, for a page the studio builds, such as AI.
    void add_page(const std::string& title, std::shared_ptr<jadefx::Node> page);
    // Runs with the frame rate the Performance tab keeps, so the studio draws at it.
    void set_on_frame_rate(std::function<void(int fps)> handler) { on_frame_rate_ = std::move(handler); }

    // The Performance tab's limit, as typed: a whole number, -1 for uncapped.
    // Remembers it and passes it on. False, with the reason on that tab, for other text.
    bool set_frame_rate(const std::string& text);
    const std::string& frame_rate_status() const { return rate_status_text_; }
    // The Performance tab's field, for tests.
    jadefx::TextField* frame_rate_field() const { return frame_rate_.get(); }

    // The theme the panel shows and edits.
    const std::string& theme_id() const { return id_; }
    // Colors changed since the theme was picked or last saved.
    bool modified() const { return modified_; }
    // What the last action did, or why it failed.
    const std::string& status() const { return status_text_; }

    // What the controls do. Each shows its outcome on the status line.
    // Draws with the theme id and remembers it. Edits not saved are dropped.
    bool select_theme(const std::string& id);
    // One color, as a picker sets it when it closes on a new color.
    void set_color(const std::string& name, const jadefx::Color& color);
    // A color back to what the theme under this one gives it.
    void reset_color(const std::string& name);
    // Writes the edits over the user's theme. False for a shipped theme, which needs save_as.
    bool save();
    // Writes the theme and its edits as a new theme of the user's, and picks it.
    bool save_as(const std::string& name);
    // Drops the edits.
    void revert();
    // Copies a theme file in and picks it.
    bool import_theme(const std::string& path);
    // Deletes the user's theme and picks the shipped theme it was built on.
    bool delete_theme();

    // True when the window may close now. With edits not saved it asks Save,
    // Don't Save, or Cancel, returns false, and runs close once an answer allows.
    bool request_close(std::function<void()> close);

    // The picker of a color, for tests. Null for a name the editor does not list.
    jadefx::ColorPicker* picker(const std::string& name) const;
    // A group's heading, for tests. Null for a group the editor does not list.
    jadefx::Node* group_heading(const std::string& group) const;

protected:
    void layoutChildren() override;

private:
    struct Row;
    struct Group;

    void build();
    // Loads id into the editor. remember writes it to the preferences.
    bool show_theme(const std::string& id, bool remember);
    // The theme being edited: its edits over what it inherits.
    IdeTheme working(const ThemeValues& edits) const;
    // Edits that change nothing are dropped, so a file keeps only what differs.
    ThemeValues pruned_edits() const;
    void apply(const ThemeValues& edits);
    void refresh_list();
    void refresh_rows();
    void refresh_buttons();
    void rebuild_rows();
    void toggle_group(Group& group);
    // Puts a group's rows under its heading when it is open, or while a filter
    // is typed, and takes them out otherwise. Its arrow says which.
    void place_body(Group& group);
    void set_status(std::string text, bool error = false);
    void set_rate_status(std::string text, bool error = false);
    // The Performance tab: the frame rate limit.
    std::shared_ptr<jadefx::Node> build_performance();
    // Sets the limit from the field unless it already shows it. restore puts
    // the limit back in the field when its text is not a limit.
    void commit_frame_rate(bool restore);
    void show_save_as(bool shown);
    void ask(const std::string& header, const std::string& content, const std::string& yes,
             std::function<void()> then, jadefx::AlertType type = jadefx::AlertType::Confirmation);
    void choose_import();
    std::string display_name() const;

    ThemeLibrary& themes_;
    Preferences& preferences_;
    FilePicker file_picker_;
    std::function<void(int)> on_frame_rate_;

    std::string id_;
    std::string name_;
    std::string base_;
    // The shipped theme the edits save on: the theme itself when it is shipped.
    std::string parent_;
    bool shipped_ = true;
    // What the theme's own colors sit on: for a shipped theme, all of them.
    ThemeValues inherited_;
    // The theme's own colors, with the edits made to them.
    ThemeValues edits_;
    bool modified_ = false;
    // Set while the panel writes the pickers, so their change handlers keep out.
    bool refreshing_ = false;

    std::vector<ThemeEntry> entries_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::string filter_;

    std::shared_ptr<jadefx::TabPane> tabs_;
    std::shared_ptr<jadefx::ComboBox> theme_list_;
    std::shared_ptr<jadefx::ComboBox> font_list_;
    std::shared_ptr<jadefx::HBox> font_row_;
    std::shared_ptr<jadefx::Button> delete_;
    std::shared_ptr<jadefx::Button> folder_;
    std::shared_ptr<jadefx::TextField> filter_field_;
    std::shared_ptr<jadefx::VBox> list_;
    std::shared_ptr<jadefx::VBox> bottom_;
    std::shared_ptr<jadefx::HBox> save_as_row_;
    std::shared_ptr<jadefx::TextField> save_as_name_;
    std::shared_ptr<jadefx::Label> status_;
    std::shared_ptr<jadefx::Button> revert_;
    std::shared_ptr<jadefx::Button> save_as_;
    std::shared_ptr<jadefx::Button> save_;
    std::string status_text_;
    std::shared_ptr<jadefx::TextField> frame_rate_;
    std::shared_ptr<jadefx::Label> rate_status_;
    std::string rate_status_text_;
    bool save_as_shown_ = false;
    // An alert must outlive its popup.
    std::vector<std::shared_ptr<jadefx::Alert>> alerts_;
    // A file dialog that answers after the panel is gone finds this expired.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace ide
