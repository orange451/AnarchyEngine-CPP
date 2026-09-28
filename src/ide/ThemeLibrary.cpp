#include "ThemeLibrary.hpp"

#include "IdeResources.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

namespace ide {
namespace {

namespace fs = std::filesystem;

constexpr const char* kUserPrefix = "user:";

std::string UserId(const fs::path& file) { return kUserPrefix + utf8_path(file.filename()); }

// A theme's own variables less its name, base, and parent: what it changes.
bool HasColors(const ThemeValues& declared) {
    return std::any_of(declared.begin(), declared.end(), [](const auto& item) {
        return item.first != "--theme-name" && item.first != "--theme-base" && item.first != "--theme-extends";
    });
}

std::string Lower(std::string text) {
    for (char& unit : text) {
        unit = static_cast<char>(std::tolower(static_cast<unsigned char>(unit)));
    }
    return text;
}

// Its --theme-name, or the file's name without .css when it has none.
std::string NameOf(const IdeTheme& theme, const fs::path& file) {
    return theme.name().empty() ? utf8_path(file.stem()) : theme.name();
}

}  // namespace

bool is_shipped_theme(const std::string& id) { return id.rfind(kUserPrefix, 0) != 0; }

ThemeLibrary::ThemeLibrary(fs::path folder) : folder_(std::move(folder)) {
    // Light and Dark, from wherever resources/ is, then every other theme beside them.
    shipped_.push_back({"light", shipped_theme("light")});
    shipped_.push_back({"dark", shipped_theme("dark")});
    const fs::path light = find_resource("themes/light.css");
    std::error_code error;
    if (light.empty() || !fs::is_directory(light.parent_path(), error)) {
        return;
    }
    std::vector<Shipped> others;
    for (fs::directory_iterator it(light.parent_path(), error), end; !error && it != end; it.increment(error)) {
        const fs::path& file = it->path();
        const std::string id = utf8_path(file.stem());
        if (file.extension() != ".css" || id == "light" || id == "dark" || !is_shipped_theme(id)) {
            continue;
        }
        std::string css;
        std::string failure;
        if (read_file(file, css, failure)) {
            others.push_back({id, IdeTheme(css)});
        }
    }
    std::sort(others.begin(), others.end(), [](const Shipped& a, const Shipped& b) {
        return Lower(NameOf(a.theme, a.id)) < Lower(NameOf(b.theme, b.id));
    });
    shipped_.insert(shipped_.end(), others.begin(), others.end());
}

const ThemeLibrary::Shipped* ThemeLibrary::find_shipped(const std::string& id) const {
    for (const Shipped& item : shipped_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const IdeTheme& ThemeLibrary::shipped(const std::string& id) const {
    const Shipped* found = find_shipped(id);
    return found != nullptr ? found->theme : shipped_.front().theme;
}

std::string ThemeLibrary::parent_of(const IdeTheme& theme) const {
    return !theme.extends().empty() && find_shipped(theme.extends()) != nullptr ? theme.extends() : theme.base();
}

ThemeValues ThemeLibrary::chain(const std::string& id, int depth) const {
    const Shipped* found = find_shipped(id);
    // A theme that extends itself, or a longer loop, stops here.
    if (found == nullptr || depth > 8) {
        return {};
    }
    const std::string parent = parent_of(found->theme);
    ThemeValues values = parent != id ? chain(parent, depth + 1) : ThemeValues();
    values.insert(values.end(), found->theme.declared().begin(), found->theme.declared().end());
    return values;
}

std::vector<ThemeEntry> ThemeLibrary::list() const {
    std::vector<ThemeEntry> entries;
    for (const Shipped& item : shipped_) {
        entries.push_back({item.id, NameOf(item.theme, item.id), true, {}});
    }
    std::error_code error;
    if (folder_.empty() || !fs::is_directory(folder_, error)) {
        return entries;
    }
    std::vector<ThemeEntry> own;
    for (fs::directory_iterator it(folder_, error), end; !error && it != end; it.increment(error)) {
        const fs::path& file = it->path();
        if (file.extension() != ".css" || !it->is_regular_file(error)) {
            continue;
        }
        std::string css;
        std::string failure;
        const IdeTheme theme = read_file(file, css, failure) ? IdeTheme(css) : IdeTheme();
        own.push_back({UserId(file), NameOf(theme, file), false, file});
    }
    std::sort(own.begin(), own.end(), [](const ThemeEntry& a, const ThemeEntry& b) {
        const std::string left = Lower(a.name);
        const std::string right = Lower(b.name);
        return left != right ? left < right : a.id < b.id;
    });
    entries.insert(entries.end(), own.begin(), own.end());
    return entries;
}

bool ThemeLibrary::load(const std::string& id, IdeTheme& theme, std::string& error) const {
    if (is_shipped_theme(id)) {
        const Shipped* found = find_shipped(id);
        if (found == nullptr) {
            error = "there is no theme \"" + id + "\"";
            return false;
        }
        const std::string parent = parent_of(found->theme);
        theme = IdeTheme(found->theme.declared(), parent != id ? chain(parent) : ThemeValues());
        return true;
    }
    const fs::path path = path_of(id);
    if (path.empty()) {
        error = "there is no theme \"" + id + "\"";
        return false;
    }
    std::string css;
    if (!read_file(path, css, error)) {
        return false;
    }
    ThemeValues declared = parse_theme_variables(css);
    if (!HasColors(declared)) {
        error = utf8_path(path.filename()) + " sets no colors on :root";
        return false;
    }
    const IdeTheme own(declared, {});
    theme = IdeTheme(std::move(declared), chain(parent_of(own)));
    return true;
}

std::string ThemeLibrary::save(const std::string& id, const std::string& name, const std::string& base,
                               const std::string& extends, const ThemeValues& variables, std::string& error) const {
    if (folder_.empty()) {
        error = "there is no folder to keep themes in";
        return {};
    }
    const fs::path path = id.empty() ? free_path(name) : path_of(id);
    if (path.empty()) {
        error = "\"" + id + "\" is not one of your themes";
        return {};
    }
    if (!write_file(path, write_theme(name, base, extends, variables), error)) {
        return {};
    }
    return UserId(path);
}

std::string ThemeLibrary::import_file(const fs::path& file, std::string& error) const {
    if (folder_.empty()) {
        error = "there is no folder to keep themes in";
        return {};
    }
    std::string css;
    if (!read_file(file, css, error)) {
        return {};
    }
    if (!HasColors(parse_theme_variables(css))) {
        error = utf8_path(file.filename()) + " is not a theme: it sets no colors on :root";
        return {};
    }
    const fs::path path = free_path(utf8_path(file.stem()));
    if (!write_file(path, css, error)) {
        return {};
    }
    return UserId(path);
}

bool ThemeLibrary::remove(const std::string& id, std::string& error) const {
    const fs::path path = path_of(id);
    if (path.empty()) {
        error = "\"" + id + "\" is not one of your themes";
        return false;
    }
    std::error_code failure;
    if (!fs::remove(path, failure) || failure) {
        error = "cannot delete " + utf8_path(path) + (failure ? ": " + failure.message() : std::string());
        return false;
    }
    return true;
}

fs::path ThemeLibrary::path_of(const std::string& id) const {
    if (folder_.empty() || id.rfind(kUserPrefix, 0) != 0) {
        return {};
    }
    const std::string file = id.substr(std::char_traits<char>::length(kUserPrefix));
    // An id from preferences.json names a file in the folder, never a path out of it.
    if (file.empty() || file == "." || file == ".." || file.find_first_of("/\\:") != std::string::npos) {
        return {};
    }
    return folder_ / path_from_utf8(file);
}

fs::path ThemeLibrary::free_path(const std::string& name) const {
    // Letters, digits, spaces, and - _ . stay, as does any byte of a non-ASCII
    // letter. The rest is a dash. No dots or spaces at either end.
    std::string stem;
    for (const char unit : name) {
        const auto byte = static_cast<unsigned char>(unit);
        const bool keep = byte >= 0x80 || std::isalnum(byte) != 0 || unit == ' ' || unit == '-' || unit == '_' ||
                          unit == '.';
        stem += keep ? unit : '-';
    }
    const auto edge = [](char unit) { return unit == ' ' || unit == '.'; };
    while (!stem.empty() && edge(stem.back())) {
        stem.pop_back();
    }
    stem.erase(stem.begin(), std::find_if_not(stem.begin(), stem.end(), edge));
    if (stem.empty()) {
        stem = "Theme";
    }
    std::error_code error;
    fs::path path = folder_ / path_from_utf8(stem + ".css");
    for (int number = 2; fs::exists(path, error); ++number) {
        path = folder_ / path_from_utf8(stem + " " + std::to_string(number) + ".css");
    }
    return path;
}

}  // namespace ide
