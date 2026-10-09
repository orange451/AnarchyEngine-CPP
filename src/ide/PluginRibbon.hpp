#pragma once

#include "PluginUi.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ide {

// The ribbon: a row of tabs, Home and Plugins, over the row the tab shows.
// Home is the row the shell builds. Plugins is built from the plugins'
// toolbars, one captioned group each; with none, it says how to add one.
class PluginRibbon : public jadefx::VBox {
public:
    // click gets a button's PluginUi id. warn gets a line for the output, once
    // per icon path that does not name a file under resources/icons.
    PluginRibbon(std::shared_ptr<jadefx::Node> home, std::function<void(std::uint32_t button)> click,
                 std::function<void(const std::string&)> warn);

    // Rebuilds the Plugins row when toolbars differ from the last ones given.
    void setToolbars(const std::vector<engine_core::PluginToolbarState>& toolbars);
    // 0 is Home, 1 is Plugins.
    void showTab(int index);
    int tab() const { return tab_; }

    // For tests: the Plugins row, and a button's node by its PluginUi id, or null.
    jadefx::Node* pluginsRow() const { return plugins_.get(); }
    jadefx::Node* buttonNode(std::uint32_t id) const;

private:
    void rebuild();

    std::shared_ptr<jadefx::Node> home_;
    std::shared_ptr<jadefx::HBox> plugins_;
    std::shared_ptr<jadefx::Label> tabs_[2];
    std::function<void(std::uint32_t)> click_;
    std::function<void(const std::string&)> warn_;
    std::vector<engine_core::PluginToolbarState> toolbars_;
    std::unordered_map<std::uint32_t, jadefx::Node*> buttons_;
    std::set<std::string> warned_;
    int tab_ = 0;
};

}  // namespace ide
