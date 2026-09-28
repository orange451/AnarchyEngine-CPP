#pragma once

#include "IdeTheme.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ide {

// One theme the studio can draw with.
struct ThemeEntry {
    // A shipped theme's file name without .css, such as "light" or "nord";
    // "user:" and its file's name for one of the user's.
    std::string id;
    // --theme-name, or the file's name without .css when it has none.
    std::string name;
    bool shipped = false;
    // A user's theme only.
    std::filesystem::path path;
};

// Whether an id names a shipped theme, which cannot be written.
bool is_shipped_theme(const std::string& id);

// The themes the studio ships, every file in resources/themes, and the user's
// own in a themes folder. Each is over the shipped theme it extends, or else
// the shipped theme of its base: Dracula over Dark, a user's copy of Dracula
// over Dracula. The folder is read each time, so a file put there by hand
// shows up the next time the themes are listed.
class ThemeLibrary {
public:
    // folder: where the user's themes are kept. Empty: the shipped themes only.
    explicit ThemeLibrary(std::filesystem::path folder);

    const std::filesystem::path& folder() const { return folder_; }
    // Light, Dark, the other shipped themes by name, then the user's themes by name.
    std::vector<ThemeEntry> list() const;
    // A shipped theme as its file declares it. Light for an id there is none of.
    const IdeTheme& shipped(const std::string& id) const;
    // The shipped theme a theme is built on: the one it extends, or else the
    // shipped theme of its base.
    std::string parent_of(const IdeTheme& theme) const;
    // The theme with that id. False, with error set, when there is none or its
    // file cannot be read.
    bool load(const std::string& id, IdeTheme& theme, std::string& error) const;
    // Writes one of the user's themes, built on the shipped theme extends: over
    // the file of id, or to a new file named for name when id is empty. The id
    // written, or empty with error set.
    std::string save(const std::string& id, const std::string& name, const std::string& base,
                     const std::string& extends, const ThemeValues& variables, std::string& error) const;
    // Copies a theme file into the folder under a name no file there has yet.
    // Its id, or empty with error set when it is not a theme.
    std::string import_file(const std::filesystem::path& file, std::string& error) const;
    // Deletes one of the user's themes. False, with error set, when it cannot.
    bool remove(const std::string& id, std::string& error) const;

private:
    struct Shipped {
        std::string id;
        IdeTheme theme;
    };

    const Shipped* find_shipped(const std::string& id) const;
    // The variables a shipped theme has, its parents' first. Empty for an id there is none of.
    ThemeValues chain(const std::string& id, int depth = 0) const;
    // Empty when id is not one of the user's themes.
    std::filesystem::path path_of(const std::string& id) const;
    // A new file in the folder for name, numbered past any file already there.
    std::filesystem::path free_path(const std::string& name) const;

    std::filesystem::path folder_;
    // Light and Dark first, then the rest by name.
    std::vector<Shipped> shipped_;
};

}  // namespace ide
