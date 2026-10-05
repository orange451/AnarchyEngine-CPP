#pragma once

#include "IdePane.hpp"

#include "jadefx/jadefx.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ide {

// The Welcome page: the studio opens it as the second tab beside the Scene
// View, in front, unless "Don't show this page on startup" was checked. A card
// with the engine's name, buttons that start work, and the shortcuts worth
// knowing first, which scrolls when the page is short; the checkbox stays at
// the bottom of the page. The studio's actions come in through Actions, so the page
// knows nothing of IdeLayout.
class LandingPage : public IdePane {
public:
    struct Actions {
        std::function<void()> new_place;
        std::function<void()> open_project;
        std::function<void()> show_assets;
        std::function<void()> open_preferences;
        // The checkbox changed: show is whether the page opens at startup.
        std::function<void(bool show)> set_show_on_startup;
    };

    // show_on_startup: the preference as it stands, which the checkbox shows inverted.
    LandingPage(Actions actions, bool show_on_startup);

    // For tests.
    jadefx::CheckBox* hide_box() const { return hide_box_.get(); }
    jadefx::ScrollPane* scroll_pane() const { return scroll_.get(); }
    // The start button whose text is text, or null.
    jadefx::Button* button(const std::string& text) const;

private:
    void build(bool show_on_startup);
    std::shared_ptr<jadefx::Node> start_column();
    std::shared_ptr<jadefx::Node> shortcut_column();

    Actions actions_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    std::shared_ptr<jadefx::CheckBox> hide_box_;
    std::vector<std::shared_ptr<jadefx::Button>> buttons_;
};

}  // namespace ide
