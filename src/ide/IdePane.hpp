#pragma once

#include "jadefx/jadefx.hpp"

#include <functional>
#include <string>

namespace ide {

// One docked page. The dock shows title() on the tab: `name` unless the page
// sets another, as Conflicts adds its count. name() stays the same, and is
// what layout.json and the Window menu know the page by. iconFile() is drawn
// beside the title when the file is under resources/icons. A page that is not
// closable keeps its tab for the life of the shell.
class IdePane : public jadefx::StackPane {
public:
    IdePane(std::string name, bool closable);

    const std::string& name() const { return name_; }
    bool closable() const { return closable_; }

    const std::string& title() const { return title_.empty() ? name_ : title_; }
    // Empty goes back to the name. The tab showing the page follows.
    void setTitle(std::string title);
    // Called with the new title. The dock holding the page sets this.
    void setOnTitle(std::function<void(const std::string&)> handler) { on_title_ = std::move(handler); }

    void setIconFile(std::string filename) { iconFile_ = std::move(filename); }
    const std::string& iconFile() const { return iconFile_; }

    // Adds this page's items to its tab's right-click menu as the menu opens.
    void setOnTabMenu(std::function<void(jadefx::Menu&)> handler) { on_tab_menu_ = std::move(handler); }
    void fillTabMenu(jadefx::Menu& menu) const {
        if (on_tab_menu_) {
            on_tab_menu_(menu);
        }
    }

    virtual void onOpen() {}
    virtual void onClose() {}

private:
    std::string name_;
    std::string title_;
    std::function<void(const std::string&)> on_title_;
    std::function<void(jadefx::Menu&)> on_tab_menu_;
    bool closable_;
    std::string iconFile_;
};

// Stretch a child across the page the dock gives this pane.
void Fill(jadefx::Node& node);

}  // namespace ide
