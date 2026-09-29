#include "IdeTheme.hpp"

#include "IdeResources.hpp"
#include "Strings.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <unordered_set>

namespace ide {
namespace {

// The CSS less its comments. A quoted string keeps whatever it holds.
std::string StripComments(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    char quote = 0;
    for (std::size_t i = 0; i < css.size(); ++i) {
        const char unit = css[i];
        if (quote != 0) {
            out += unit;
            if (unit == '\\' && i + 1 < css.size()) {
                out += css[++i];
            } else if (unit == quote) {
                quote = 0;
            }
            continue;
        }
        if (unit == '"' || unit == '\'') {
            quote = unit;
            out += unit;
            continue;
        }
        if (unit == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const std::size_t close = css.find("*/", i + 2);
            if (close == std::string::npos) {
                break;
            }
            i = close + 1;
            // Keep the tokens on either side apart.
            out += ' ';
            continue;
        }
        out += unit;
    }
    return out;
}

std::string Unquote(const std::string& value) {
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

const std::string* Find(const ThemeValues& values, std::string_view name) {
    for (const auto& [key, value] : values) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

// "--selection-hover-color" reads "Selection hover".
std::string LabelOf(const std::string& name) {
    std::string label = name.substr(name.rfind("--", 0) == 0 ? 2 : 0);
    const std::string suffix = "-color";
    if (label.size() > suffix.size() && label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0) {
        label.resize(label.size() - suffix.size());
    }
    std::replace(label.begin(), label.end(), '-', ' ');
    if (!label.empty()) {
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return label;
}

struct Listeners {
    int next = 0;
    std::map<int, std::function<void()>> changed;
};

Listeners& TheListeners() {
    static Listeners listeners;
    return listeners;
}

IdeTheme& TheCurrent() {
    static IdeTheme theme = shipped_theme("light");
    return theme;
}

}  // namespace

const std::vector<ThemeVariable>& theme_variables() {
    static const std::vector<ThemeVariable> variables = {
        {"--ide-window-color", "Studio", "Window"},
        {"--ide-text-color", "Studio", "Text"},
        {"--ide-muted-text-color", "Studio", "Muted text"},
        {"--ide-panel-color", "Studio", "Panel"},
        {"--ide-status-bar-color", "Studio", "Status bar"},
        {"--ide-field-color", "Studio", "Text field"},
        {"--ide-field-border-color", "Studio", "Text field border"},
        {"--ide-error-text-color", "Studio", "Error text"},
        {"--ide-warning-text-color", "Studio", "Warning text"},

        {"--ide-ribbon-color", "Ribbon", "Background"},
        {"--ide-ribbon-border-color", "Ribbon", "Border"},
        {"--ide-ribbon-hover-color", "Ribbon", "Button hover"},
        {"--ide-ribbon-pressed-color", "Ribbon", "Button pressed"},

        {"--ide-viewport-color", "Scene View", "Background"},
        {"--ide-fps-color", "Scene View", "Frame rate background"},
        {"--ide-fps-text-color", "Scene View", "Frame rate text"},

        {"--ide-dock-merge-color", "Docking", "Tab drop outline"},
        {"--ide-dock-merge-fill-color", "Docking", "Tab drop fill"},
        {"--ide-dock-split-color", "Docking", "Split drop outline"},
        {"--ide-dock-split-fill-color", "Docking", "Split drop fill"},
        {"--ide-dock-float-color", "Docking", "Float drop outline"},
        {"--ide-dock-float-fill-color", "Docking", "Float drop fill"},
        {"--ide-dock-caret-color", "Docking", "Tab insert mark"},

        {"--ide-popup-color", "Popups", "Background"},
        {"--ide-popup-border-color", "Popups", "Border"},
        {"--ide-popup-shadow-color", "Popups", "Shadow"},
        {"--ide-popup-text-color", "Popups", "Text"},
        {"--ide-popup-detail-text-color", "Popups", "Detail text"},
        {"--ide-popup-selection-color", "Popups", "Selected row"},
        {"--ide-popup-header-color", "Popups", "Header"},
        {"--ide-popup-header-text-color", "Popups", "Header text"},

        {"--ide-editor-color", "Script Editor", "Background"},
        {"--ide-editor-text-color", "Script Editor", "Text"},
        {"--ide-syntax-keyword-color", "Script Editor", "Keyword"},
        {"--ide-syntax-builtin-color", "Script Editor", "Built-in"},
        {"--ide-syntax-datatype-color", "Script Editor", "Data type"},
        {"--ide-syntax-comment-color", "Script Editor", "Comment"},
        {"--ide-syntax-string-color", "Script Editor", "String"},
        {"--ide-syntax-number-color", "Script Editor", "Number"},
        {"--ide-find-match-color", "Script Editor", "Find match"},
        {"--ide-find-current-color", "Script Editor", "Current find match"},
        {"--ide-find-scroll-color", "Script Editor", "Find match on scroll bar"},
        {"--ide-swatch-border-color", "Script Editor", "Color swatch border"},
        {"--ide-banner-color", "Script Editor", "Problem banner"},
        {"--ide-banner-text-color", "Script Editor", "Problem banner text"},
        {"--ide-banner-error-color", "Script Editor", "Error banner"},
        {"--ide-banner-warning-color", "Script Editor", "Warning banner"},

        {"--ide-find-bar-color", "Search", "Find bar"},
        {"--ide-find-bar-border-color", "Search", "Find bar border"},
        {"--ide-find-bar-shadow-color", "Search", "Find bar shadow"},
        {"--ide-search-field-text-color", "Search", "Field text"},
        {"--ide-search-field-border-color", "Search", "Field border"},
        {"--ide-search-field-focus-color", "Search", "Focused field border"},
        {"--ide-search-field-invalid-color", "Search", "Invalid field border"},
        {"--ide-find-button-text-color", "Search", "Button"},
        {"--ide-find-button-hover-color", "Search", "Button hover"},
        {"--ide-find-button-pressed-color", "Search", "Button pressed"},
        {"--ide-find-button-checked-color", "Search", "Option on"},
        {"--ide-find-button-checked-border-color", "Search", "Option on border"},
        {"--ide-search-status-color", "Search", "Result count"},
        {"--ide-search-status-error-color", "Search", "No results"},
        {"--ide-search-divider-color", "Search", "Results divider"},
        {"--ide-search-path-color", "Search", "Path and line number"},
        {"--ide-search-badge-color", "Search", "Match count badge"},
        {"--ide-search-badge-text-color", "Search", "Match count badge text"},

        {"--ide-console-command-color", "Console", "Command"},
        {"--ide-console-time-color", "Console", "Timestamp"},
        {"--ide-console-key-color", "Console", "Table key"},
        {"--ide-console-note-color", "Console", "Note"},

        {"--ide-explorer-button-color", "Explorer", "Button"},
        {"--ide-explorer-button-hover-color", "Explorer", "Button hover"},
        {"--ide-explorer-button-disabled-color", "Explorer", "Disabled button"},

        {"--ide-properties-group-color", "Properties", "Group header"},
        {"--ide-properties-group-text-color", "Properties", "Group header text"},
        {"--ide-properties-readonly-name-color", "Properties", "Read-only name"},
        {"--ide-properties-readonly-color", "Properties", "Read-only field"},
        {"--ide-properties-readonly-border-color", "Properties", "Read-only field border"},
        {"--ide-properties-picking-color", "Properties", "Picking field"},
        {"--ide-properties-picking-border-color", "Properties", "Picking field border"},
        {"--ide-properties-error-color", "Properties", "Refused edit"},
        {"--ide-properties-hint-color", "Properties", "Pick hint"},
        {"--ide-properties-x-color", "Properties", "Position X field"},
        {"--ide-properties-y-color", "Properties", "Position Y field"},
        {"--ide-properties-z-color", "Properties", "Position Z field"},
    };
    return variables;
}

const std::vector<ThemeVariable>& control_variables() {
    // Built once from JadeFX's light look, whose :root lists every one in order.
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& [name, value] : parse_theme_variables(jadefx::Theme::stylesheet(jadefx::Theme::LIGHT))) {
            out.push_back(name);
        }
        return out;
    }();
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> out;
        for (const std::string& name : names) {
            out.push_back(LabelOf(name));
        }
        return out;
    }();
    static const std::vector<ThemeVariable> variables = [] {
        std::vector<ThemeVariable> out;
        for (std::size_t i = 0; i < names.size(); ++i) {
            out.push_back({names[i].c_str(), "Controls", labels[i].c_str()});
        }
        return out;
    }();
    return variables;
}

ThemeValues parse_theme_variables(const std::string& css) {
    const std::string text = StripComments(css);
    ThemeValues out;
    std::unordered_map<std::string, std::size_t> index;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t open = text.find('{', at);
        if (open == std::string::npos) {
            break;
        }
        // The block's end, past any nested block such as an @media body.
        std::size_t close = open + 1;
        int depth = 1;
        bool nested = false;
        char quote = 0;
        for (; close < text.size() && depth > 0; ++close) {
            const char unit = text[close];
            if (quote != 0) {
                if (unit == '\\') {
                    ++close;
                } else if (unit == quote) {
                    quote = 0;
                }
            } else if (unit == '"' || unit == '\'') {
                quote = unit;
            } else if (unit == '{') {
                ++depth;
                nested = true;
            } else if (unit == '}') {
                --depth;
            }
        }
        if (depth > 0) {
            break;
        }
        // What precedes the brace, less any statement before it such as @charset.
        std::string_view prelude = std::string_view(text).substr(at, open - at);
        if (const std::size_t semicolon = prelude.rfind(';'); semicolon != std::string_view::npos) {
            prelude.remove_prefix(semicolon + 1);
        }
        if (Trim(prelude) == ":root" && !nested) {
            const std::string body = text.substr(open + 1, close - open - 2);
            for (jadefx::Declaration& declaration : jadefx::parseInlineDeclarations(body)) {
                if (declaration.property.rfind("--", 0) != 0) {
                    continue;
                }
                const auto found = index.find(declaration.property);
                if (found != index.end()) {
                    out[found->second].second = std::move(declaration.value);
                } else {
                    index.emplace(declaration.property, out.size());
                    out.emplace_back(std::move(declaration.property), std::move(declaration.value));
                }
            }
        }
        at = close;
    }
    return out;
}

IdeTheme::IdeTheme() : IdeTheme(ThemeValues(), ThemeValues()) {}

IdeTheme::IdeTheme(const std::string& css) : IdeTheme(parse_theme_variables(css), ThemeValues()) {}

IdeTheme::IdeTheme(ThemeValues declared, ThemeValues inherited)
    : declared_(std::move(declared)), inherited_(std::move(inherited)) {
    resolve();
}

void IdeTheme::resolve() {
    const std::string* base = Find(declared_, "--theme-base");
    if (base == nullptr) {
        base = Find(inherited_, "--theme-base");
    }
    base_ = base != nullptr && Unquote(*base) == jadefx::Theme::DARK ? jadefx::Theme::DARK : jadefx::Theme::LIGHT;
    const std::string* name = Find(declared_, "--theme-name");
    name_ = name != nullptr ? Unquote(*name) : std::string();
    const std::string* extends = Find(declared_, "--theme-extends");
    extends_ = extends != nullptr ? Unquote(*extends) : std::string();
    all_.clear();
    for (auto& [key, value] : parse_theme_variables(jadefx::Theme::stylesheet(base_))) {
        all_[key] = std::move(value);
    }
    for (const ThemeValues* layer : {&inherited_, &declared_}) {
        for (const auto& [key, value] : *layer) {
            all_[key] = value;
        }
    }
}

bool IdeTheme::declares(std::string_view name) const { return Find(declared_, name) != nullptr; }

std::string IdeTheme::value(std::string_view name) const {
    const auto found = all_.find(std::string(name));
    if (found == all_.end()) {
        return {};
    }
    return jadefx::resolveCssVariables(found->second, &all_);
}

jadefx::Color IdeTheme::color(std::string_view name) const {
    const std::string text = value(name);
    bool ok = false;
    const jadefx::Color parsed = text.empty() ? jadefx::Color() : jadefx::Color::parse(text, &ok);
    if (ok) {
        return parsed;
    }
    static std::unordered_set<std::string> noted;
    if (noted.insert(std::string(name)).second) {
        std::fprintf(stderr, "Theme color %.*s is %s\n", static_cast<int>(name.size()), name.data(),
                     text.empty() ? "not set" : ("not a color: " + text).c_str());
    }
    return jadefx::Color::rgb8(255, 0, 255);
}

std::string IdeTheme::stylesheet() const {
    std::string css = ":root {\n";
    for (const ThemeValues* layer : {&inherited_, &declared_}) {
        for (const auto& [key, value] : *layer) {
            if (!is_theme_meta(key)) {
                css += "    " + key + ": " + value + ";\n";
            }
        }
    }
    css += "}\n";
    return css;
}

bool is_theme_meta(std::string_view name) {
    return name == "--theme-name" || name == "--theme-base" || name == "--theme-extends";
}

std::string write_theme(const std::string& name, const std::string& base, const std::string& extends,
                        const ThemeValues& variables) {
    // The name less what would end its string, its declaration, or the heading comment.
    std::string clean;
    for (const char unit : name) {
        if (std::string_view("\"'\\;{}\r\n").find(unit) == std::string_view::npos &&
            !(unit == '/' && !clean.empty() && clean.back() == '*')) {
            clean += unit;
        }
    }
    const bool extending = !extends.empty() && extends != base;
    std::string css = "/* " + (clean.empty() ? std::string("A theme") : clean) +
                      ", for Anarchy Engine. Each variable here replaces the one in the shipped " +
                      (extending ? extends : base) + " theme. */\n:root {\n";
    css += "    --theme-name: \"" + clean + "\";\n";
    css += "    --theme-base: " + base + ";\n";
    if (extending) {
        css += "    --theme-extends: " + extends + ";\n";
    }
    std::unordered_set<std::string> written = {"--theme-name", "--theme-base", "--theme-extends"};
    std::string group;
    for (const std::vector<ThemeVariable>* list : {&theme_variables(), &control_variables()}) {
        for (const ThemeVariable& variable : *list) {
            const std::string* value = Find(variables, variable.name);
            if (value == nullptr) {
                continue;
            }
            if (group != variable.group) {
                group = variable.group;
                css += "\n    /* " + group + " */\n";
            }
            css += "    " + std::string(variable.name) + ": " + *value + ";\n";
            written.insert(variable.name);
        }
    }
    bool other = false;
    for (const auto& [key, value] : variables) {
        if (written.count(key) != 0) {
            continue;
        }
        if (!other) {
            other = true;
            css += "\n    /* Other */\n";
        }
        css += "    " + key + ": " + value + ";\n";
    }
    css += "}\n";
    return css;
}

std::string css_color(const jadefx::Color& color) {
    const auto channel = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
    };
    if (color.a >= 0.9995f) {
        return color.toHex();
    }
    char alpha[16];
    std::snprintf(alpha, sizeof(alpha), "%.3f", std::clamp(color.a, 0.f, 1.f));
    std::string trimmed = alpha;
    while (trimmed.size() > 1 && trimmed.back() == '0') {
        trimmed.pop_back();
    }
    if (trimmed.back() == '.') {
        trimmed.pop_back();
    }
    return "rgba(" + std::to_string(channel(color.r)) + ", " + std::to_string(channel(color.g)) + ", " +
           std::to_string(channel(color.b)) + ", " + trimmed + ")";
}

IdeTheme shipped_theme(const std::string& file) {
    const std::filesystem::path path = find_resource("themes/" + file + ".css");
    if (path.empty()) {
        return IdeTheme();
    }
    std::string css;
    std::string error;
    if (!read_file(path, css, error)) {
        std::fprintf(stderr, "Could not read theme: %s\n", error.c_str());
        return IdeTheme();
    }
    return IdeTheme(css);
}

const IdeTheme& current_theme() { return TheCurrent(); }

void set_current_theme(IdeTheme theme) {
    TheCurrent() = std::move(theme);
    jadefx::Theme::setUserAgentStylesheet(jadefx::Theme::stylesheet(TheCurrent().base()) + TheCurrent().stylesheet());
    // A copy, since a listener may add or remove listeners.
    const std::map<int, std::function<void()>> listeners = TheListeners().changed;
    for (const auto& [id, changed] : listeners) {
        if (TheListeners().changed.count(id) != 0 && changed) {
            changed();
        }
    }
}

jadefx::Color theme_color(std::string_view name) { return current_theme().color(name); }

ThemeListener::ThemeListener(std::function<void()> changed) : id_(++TheListeners().next) {
    TheListeners().changed.emplace(id_, std::move(changed));
}

ThemeListener::~ThemeListener() { TheListeners().changed.erase(id_); }

const std::string& editor_mono_family() {
    static const std::string family = [] {
        const std::string name = "Editor Mono";
        const char* paths[] = {
            "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Supplemental/Courier New.ttf",
            "C:/Windows/Fonts/consola.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        };
        for (const char* path : paths) {
            if (jadefx::Font::loadFile(name, path)) {
                break;
            }
        }
        return name;
    }();
    return family;
}

}  // namespace ide
