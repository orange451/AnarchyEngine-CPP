#include "ide/IdeTheme.hpp"
#include "ide/ThemeLibrary.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Theme files: what a theme declares, the two the studio ships, and the
// mounted studio drawing with the light one.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

void ExpectText(const std::string& got, const std::string& want, const char* message) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got [%s], want [%s]\n", message, got.c_str(), want.c_str());
        ++gFailures;
    }
}

using Declared = std::vector<std::pair<std::string, std::string>>;

void TestParse() {
    const Declared plain = ide::parse_theme_variables(":root { --a: #fff; --b: rgba(0, 0, 0, 0.5); }");
    Expect(plain == Declared{{"--a", "#fff"}, {"--b", "rgba(0, 0, 0, 0.5)"}}, "a :root rule's variables, in order");

    const Declared mixed = ide::parse_theme_variables(
        "@charset \"utf-8\";\n"
        "/* --skipped: red; */\n"
        ":root {\n"
        "    --a: #111; /* after */\n"
        "    color: red;\n"
        "    --theme-name: \"Night /* not a comment */\";\n"
        "}\n"
        ".ide-ribbon { --not-root: #222; }\n"
        "@media (min-width: 1px) { :root { --nested: #333; } }\n"
        ":root { --a: #444; --c: var(--a); }\n");
    Expect(mixed == Declared{{"--a", "#444"}, {"--theme-name", "\"Night /* not a comment */\""}, {"--c", "var(--a)"}},
           "only custom properties of top-level :root rules, a later one replacing the value");

    Expect(ide::parse_theme_variables("").empty(), "an empty file declares nothing");
    Expect(ide::parse_theme_variables(":root { --a: #fff;").empty(), "an unclosed rule declares nothing");
}

void TestTheme() {
    const ide::IdeTheme light(":root { --theme-name: 'Paper'; --ide-x: var(--accent-color); --ide-y: var(--ide-x); }");
    ExpectText(light.name(), "Paper", "the name loses its quotes");
    ExpectText(light.base(), jadefx::Theme::LIGHT, "no --theme-base is light");
    ExpectText(light.value("--ide-x"), "#1a73e8", "var() reaches the light look's accent");
    ExpectText(light.value("--ide-y"), "#1a73e8", "and through another variable");
    ExpectText(light.color("--ide-x").toHex(), "#1a73e8", "the value as a color");
    Expect(light.value("--ide-unset").empty(), "an unset variable is empty");
    ExpectText(light.color("--ide-unset").toHex(), "#ff00ff", "and magenta as a color");

    const ide::IdeTheme dark(":root { --theme-base: dark; --ide-x: var(--accent-color); --accent-color: #ff8800; }");
    ExpectText(dark.base(), jadefx::Theme::DARK, "--theme-base: dark");
    ExpectText(dark.value("--ide-x"), "#ff8800", "a theme can set JadeFX's variables too");
    ExpectText(dark.value("--surface-color"), "#292a2d", "and gets the dark look's for the rest");

    const std::string sheet = dark.stylesheet();
    Expect(sheet.find(":root {") == 0 && sheet.find("--accent-color: #ff8800;") != std::string::npos,
           "the stylesheet is one :root rule of what the file declares");
    Expect(sheet.find("--surface-color") == std::string::npos, "and leaves the base look's variables to JadeFX");
}

void TestShipped() {
    std::set<std::string> names;
    for (const ide::ThemeVariable& variable : ide::theme_variables()) {
        Expect(names.insert(variable.name).second, "each theme variable is listed once");
        Expect(std::string(variable.name).rfind("--ide-", 0) == 0, "theme variables are --ide-*");
    }
    const ide::ThemeLibrary library{std::filesystem::path()};
    const std::vector<ide::ThemeEntry> entries = library.list();
    Expect(entries.size() >= 9 && entries[0].id == "light" && entries[1].id == "dark",
           "Light and Dark come first among the shipped themes");
    for (const ide::ThemeEntry& entry : entries) {
        const char* file = entry.id.c_str();
        ide::IdeTheme theme;
        std::string error;
        Expect(library.load(entry.id, theme, error), "a shipped theme loads");
        Expect(!theme.name().empty(), "a shipped theme is named");
        Expect(theme.base() == "light" || theme.base() == "dark", "a shipped theme has a base");
        // Each shipped theme sets every studio color itself, so none falls through to another's.
        for (const ide::ThemeVariable& variable : ide::theme_variables()) {
            bool ok = false;
            jadefx::Color::parse(theme.value(variable.name), &ok);
            if (!ok || !theme.declares(variable.name)) {
                std::fprintf(stderr, "FAIL %s.css does not set %s to a color\n", file, variable.name);
                ++gFailures;
            }
        }
        for (const ide::ThemeVariable& variable : ide::control_variables()) {
            bool ok = false;
            jadefx::Color::parse(theme.value(variable.name), &ok);
            if (!ok) {
                std::fprintf(stderr, "FAIL %s.css leaves %s without a color\n", file, variable.name);
                ++gFailures;
            }
        }
        for (const auto& [name, value] : theme.declared()) {
            if (name.rfind("--ide-", 0) == 0 && names.count(name) == 0) {
                std::fprintf(stderr, "FAIL %s.css sets %s, which is not a theme variable\n", file, name.c_str());
                ++gFailures;
            }
        }
    }

    // Dracula is built on Dark: what it leaves out, Dark gives.
    ide::IdeTheme dracula;
    std::string error;
    Expect(library.load("dracula", dracula, error), "Dracula ships");
    ExpectText(library.parent_of(dracula), "dark", "Dracula is built on Dark");
    ExpectText(dracula.value("--ide-syntax-keyword-color"), "#ff79c6", "a palette color reaches the editor");
    ExpectText(dracula.value("--ide-fps-text-color"), "#f2f2f2", "and so does a color it sets outright");
    ExpectText(dracula.value("--gutter-color"), "rgba(255, 255, 255, 0.04)", "and what it leaves out comes from Dark");
}

std::string Background(jadefx::Node& root, const char* className) {
    const std::vector<jadefx::Node*> found = root.getElementsByClassName(className);
    return found.empty() ? std::string("missing") : found.front()->computedStyle().background.color.toHex();
}

// The studio's rules read the theme's variables: the light theme draws as before it was one.
void TestMounted(jadefx::Scene& scene) {
    ExpectText(scene.computedStyle().variable("--ide-window-color"), "#d0d0d0",
               "the scene holds the theme's variables");
    ExpectText(scene.computedStyle().background.color.toHex(), "#d0d0d0", "the window color");
    ExpectText(scene.computedStyle().color.toHex(), "#202124", "the studio's text color");
    ExpectText(Background(scene, "ide-ribbon"), "#f5f6f7", "the ribbon color");
    ExpectText(Background(scene, "ide-status"), "#eceff1", "the status bar color");
    ExpectText(Background(scene, "ide-root"), "#d0d0d0", "the root pane's color");
    ExpectText(Background(scene, "properties-pane"), "#ffffff", "the Properties pane's color");
}

}  // namespace

int RunThemeTests(jadefx::Scene& scene) {
    gFailures = 0;
    TestParse();
    TestTheme();
    TestShipped();
    TestMounted(scene);
    return gFailures;
}
