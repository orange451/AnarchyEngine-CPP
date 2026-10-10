#pragma once

namespace jadefx {
class Menu;
class MenuBar;
class MenuItem;
}  // namespace jadefx

namespace ide {

// macOS shows an application's menus in the screen's menu bar, not in its
// window. There the studio's MenuBar stays in the scene, hidden and zero
// height, as the model the native bar is built from.
#if defined(__APPLE__)
constexpr bool kNativeMenuBar = true;
#else
constexpr bool kNativeMenuBar = false;
#endif

// Items that move to their usual macOS places in the native bar. The JadeFX
// menus keep them.
struct NativeMenuSetup {
    // Goes to the application menu as Settings, with Cmd+Comma.
    const jadefx::MenuItem* settings = nullptr;
    // Left out: the application menu's own Quit closes the window the same way.
    const jadefx::MenuItem* quit = nullptr;
    // Leads with Minimize and Zoom, and takes the place of GLFW's Window menu.
    const jadefx::Menu* window = nullptr;
};

#if defined(__APPLE__)
// Puts the bar's menus in the screen's menu bar after the application menu.
// Each menu is rebuilt from its JadeFX items as it opens, so text, visibility,
// and the Window menu's entries stay current; enabled state is checked as the
// menu opens and before a shortcut runs. A shortcut runs the item's action
// through fire(), as a click in the JadeFX menu does. A no-op without an
// NSApplication, as in headless tests.
void InstallNativeMenuBar(const jadefx::MenuBar& bar, const NativeMenuSetup& setup);
// Takes the installed menus back out and restores GLFW's Window menu.
void RemoveNativeMenuBar();
#else
inline void InstallNativeMenuBar(const jadefx::MenuBar&, const NativeMenuSetup&) {}
inline void RemoveNativeMenuBar() {}
#endif

}  // namespace ide
