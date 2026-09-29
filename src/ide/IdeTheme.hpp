#pragma once

#include "jadefx/paint/Color.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ide {

// One color the studio draws with. A theme sets it as a CSS custom property on
// :root, and the studio's stylesheets and code read it by that name.
struct ThemeVariable {
    const char* name;   // such as "--ide-syntax-keyword-color"
    const char* group;  // the heading it is listed under, such as "Script Editor"
    const char* label;  // its name under that heading, such as "Keyword"
};

// Every --ide-* variable, in the order a theme editor lists them, grouped.
// A shipped theme sets each one.
const std::vector<ThemeVariable>& theme_variables();
// JadeFX's own colors, such as --accent-color, under "Controls". Its light and
// dark looks set them; a theme may too.
const std::vector<ThemeVariable>& control_variables();

// Custom properties and their values, in the order a theme declares them.
using ThemeValues = std::vector<std::pair<std::string, std::string>>;

// The custom properties a theme's CSS declares on :root, in the order it first
// declares them; a later declaration of the same name replaces the value. A
// theme is variables only, so everything else is ignored: comments, other
// rules, at-rules, and properties that are not custom.
ThemeValues parse_theme_variables(const std::string& css);

// A theme: the variables it declares, over those it inherits, over the JadeFX
// built-in look it names in --theme-base. What it inherits is the shipped
// theme it names in --theme-extends, or else the shipped theme of its base,
// and so on up. So a theme sets only what it changes.
class IdeTheme {
public:
    IdeTheme();
    explicit IdeTheme(const std::string& css);
    IdeTheme(ThemeValues declared, ThemeValues inherited);

    // "light" or "dark". Anything else in --theme-base is light.
    const std::string& base() const { return base_; }
    // --theme-name without its quotes. Empty when the theme has none.
    const std::string& name() const { return name_; }
    // --theme-extends: the id of the shipped theme this one is built on. Empty when it has none.
    const std::string& extends() const { return extends_; }
    // What this theme sets itself, in its order, including --theme-name, --theme-base, and --theme-extends.
    const ThemeValues& declared() const { return declared_; }
    const ThemeValues& inherited() const { return inherited_; }
    bool declares(std::string_view name) const;

    // A variable's value with any var() in it resolved, from this theme, what
    // it inherits, or its base look. Empty when none sets it.
    std::string value(std::string_view name) const;
    // That value as a color. Magenta, with a note on stderr the first time,
    // when it is unset or not a color, so a missing variable is plain to see.
    jadefx::Color color(std::string_view name) const;
    // A :root rule of the inherited variables and then the declared ones, less
    // --theme-name, --theme-base, and --theme-extends. It goes after the base look's stylesheet.
    std::string stylesheet() const;

private:
    void resolve();

    std::string base_;
    std::string name_;
    std::string extends_;
    ThemeValues declared_;
    ThemeValues inherited_;
    // The base look's variables, then the inherited and declared ones over
    // them. The same type as jadefx::CssVariables, so var() resolves against it.
    std::unordered_map<std::string, std::string> all_;
};

// A theme file's own facts, not a color: --theme-name, --theme-base, and
// --theme-extends.
bool is_theme_meta(std::string_view name);

// A theme file: a :root rule of --theme-name, --theme-base, --theme-extends
// when extends is not the base, and variables, each under the heading of its
// group in the order theme_variables and control_variables list them.
// Variables neither lists come last.
std::string write_theme(const std::string& name, const std::string& base, const std::string& extends,
                        const ThemeValues& variables);

// A color as CSS: #rrggbb, or rgba() when it is not opaque.
std::string css_color(const jadefx::Color& color);

// A theme the studio ships, from resources/themes, such as "light" or "dark",
// as its file declares it. Empty, with a note on stderr, when the file cannot be read.
IdeTheme shipped_theme(const std::string& file);

// The theme the studio draws with. The shipped light theme until one is set.
const IdeTheme& current_theme();
// Makes theme the one the studio draws with. JadeFX's user-agent stylesheet
// becomes the base look plus the theme's variables, so every scene's var()
// rules follow at their next layout. Then each ThemeListener runs, for the
// colors code set.
void set_current_theme(IdeTheme theme);

// current_theme().color(name), for a color set from code rather than CSS.
jadefx::Color theme_color(std::string_view name);

// Runs changed after each set_current_theme, for as long as it lives. For
// colors code sets, such as a text area's style classes; CSS needs none.
class ThemeListener {
public:
    explicit ThemeListener(std::function<void()> changed);
    ~ThemeListener();
    ThemeListener(const ThemeListener&) = delete;
    ThemeListener& operator=(const ThemeListener&) = delete;

private:
    int id_;
};

}  // namespace ide
