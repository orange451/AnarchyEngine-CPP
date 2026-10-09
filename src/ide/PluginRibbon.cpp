#include "PluginRibbon.hpp"

#include "IdeIcons.hpp"
#include "IdeLayoutInternal.hpp"

#include <utility>

namespace ide {
namespace {

constexpr const char* kIconPrefix = "icons/";

bool SameButton(const engine_core::PluginButtonState& a, const engine_core::PluginButtonState& b) {
    return a.id == b.id && a.key == b.key && a.tooltip == b.tooltip && a.icon == b.icon && a.text == b.text &&
           a.active == b.active && a.enabled == b.enabled;
}

// Whether the two lists have the same toolbars and buttons, apart from each
// button's active and enabled, which change in place.
bool SameShape(const std::vector<engine_core::PluginToolbarState>& a,
               const std::vector<engine_core::PluginToolbarState>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].buttons.size() != b[i].buttons.size()) {
            return false;
        }
        for (std::size_t j = 0; j < a[i].buttons.size(); ++j) {
            engine_core::PluginButtonState left = a[i].buttons[j];
            engine_core::PluginButtonState right = b[i].buttons[j];
            left.active = right.active;
            left.enabled = right.enabled;
            if (!SameButton(left, right)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

PluginRibbon::PluginRibbon(std::shared_ptr<jadefx::Node> home, jadefx::Node* homeAnchor,
                           std::function<void(std::uint32_t)> click, std::function<void(const std::string&)> warn)
    : home_(std::move(home)), homeAnchor_(homeAnchor), click_(std::move(click)), warn_(std::move(warn)) {
    getClassList().add("ide-ribbon-area");
    setPrefWidthRatio(1);
    auto tabs = jadefx::make<jadefx::HBox>();
    tabs->getClassList().add("ide-ribbon-tabs");
    tabs->setAlignment(jadefx::Pos::CenterLeft);
    tabs->setPrefWidthRatio(1);
    tabs->setSpacing(2);
    const char* names[] = {"Home", "Plugins"};
    for (int i = 0; i < 2; ++i) {
        tabs_[i] = jadefx::make<jadefx::Label>(names[i]);
        tabs_[i]->getClassList().add("ide-ribbon-tab");
        tabs_[i]->setCursor(jadefx::Cursor::Pointer);
        tabs_[i]->setOnMouseClicked([this, i](const jadefx::MouseEvent& event) {
            if (event.button == 0) {
                showTab(i);
            }
        });
        tabs->getChildren().add(tabs_[i]);
    }
    plugins_ = jadefx::make<jadefx::HBox>();
    plugins_->getClassList().add("ide-ribbon");
    plugins_->setSpacing(2);
    plugins_->setAlignment(jadefx::Pos::CenterLeft);
    plugins_->setPrefWidthRatio(1);
    plugins_->setMinSize(0, kRibbonHeight);
    plugins_->setPrefHeight(kRibbonHeight);
    getChildren().add(std::move(tabs));
    getChildren().add(home_);
    rebuild();
    showTab(0);
}

void PluginRibbon::showTab(int index) {
    tab_ = index == 1 ? 1 : 0;
    for (int i = 0; i < 2; ++i) {
        SetStyleClass(*tabs_[i], "on", i == tab_);
    }
    std::shared_ptr<jadefx::Node> row = tab_ == 1 ? std::static_pointer_cast<jadefx::Node>(plugins_) : home_;
    if (getChildren()[1] != row) {
        getChildren().set(1, row);
    }
}

void PluginRibbon::setToolbars(const std::vector<engine_core::PluginToolbarState>& toolbars) {
    // Lighting or dimming a button keeps its node, so a press or hover on it goes on.
    if (SameShape(toolbars, toolbars_)) {
        for (const engine_core::PluginToolbarState& toolbar : toolbars) {
            for (const engine_core::PluginButtonState& state : toolbar.buttons) {
                if (jadefx::Node* node = buttonNode(state.id)) {
                    SetStyleClass(*node, "on", state.active);
                    node->setDisable(!state.enabled);
                }
            }
        }
        toolbars_ = toolbars;
        return;
    }
    toolbars_ = toolbars;
    rebuild();
}

jadefx::Node* PluginRibbon::buttonNode(std::uint32_t id) const {
    const auto found = buttons_.find(id);
    return found != buttons_.end() ? found->second : nullptr;
}

std::shared_ptr<jadefx::Node> PluginRibbon::makeGroup(const engine_core::PluginToolbarState& toolbar) {
    auto group = jadefx::make<jadefx::HBox>();
    group->getClassList().add("ide-ribbon-group");
    group->setSpacing(2);
    group->setAlignment(jadefx::Pos::CenterLeft);
    for (const engine_core::PluginButtonState& state : toolbar.buttons) {
        std::string icon;
        if (state.icon.rfind(kIconPrefix, 0) == 0) {
            icon = state.icon.substr(std::char_traits<char>::length(kIconPrefix));
            if (!icon_graphic(icon) && warned_.insert(state.icon).second && warn_) {
                warn_("Plugin icon " + state.icon + " was not found in resources/icons");
            }
        }
        const std::uint32_t id = state.id;
        auto button = jadefx::make<RibbonButton>(state.text.c_str(), icon.c_str(), [this, id] {
            if (click_) {
                click_(id);
            }
        });
        SetStyleClass(*button, "on", state.active);
        button->setDisable(!state.enabled);
        if (!state.tooltip.empty()) {
            jadefx::Tooltip::install(button.get(), jadefx::make<jadefx::Tooltip>(state.tooltip));
        }
        buttons_[id] = button.get();
        group->getChildren().add(std::move(button));
    }
    auto caption = jadefx::make<jadefx::Label>(toolbar.name);
    caption->getClassList().add("ide-ribbon-caption");
    group->getChildren().add(std::move(caption));
    return group;
}

namespace {

std::shared_ptr<jadefx::Node> MakeSeparator() {
    auto separator = jadefx::make<jadefx::Pane>();
    separator->getClassList().add("ide-ribbon-separator");
    separator->setMouseTransparent(true);
    separator->setPrefSize(9, 20);
    return separator;
}

}  // namespace

void PluginRibbon::rebuild() {
    buttons_.clear();
    // The user's plugins on the Plugins tab, a separator between each.
    std::vector<std::shared_ptr<jadefx::Node>> groups;
    for (const engine_core::PluginToolbarState& toolbar : toolbars_) {
        if (toolbar.builtin) {
            continue;
        }
        if (!groups.empty()) {
            groups.push_back(MakeSeparator());
        }
        groups.push_back(makeGroup(toolbar));
    }
    if (groups.empty()) {
        auto hint = jadefx::make<jadefx::Label>("No plugins installed — right-click a Folder → Save as Plugin");
        hint->getClassList().add("ide-ribbon-empty");
        groups.push_back(std::move(hint));
    }
    plugins_->getChildren().setAll(std::move(groups));

    // The studio's own plugins on Home, after the shell's buttons, each after a separator.
    auto* home = dynamic_cast<jadefx::Pane*>(home_.get());
    if (home == nullptr) {
        return;
    }
    auto& children = home->getChildren();
    for (const std::shared_ptr<jadefx::Node>& placed : homeGroups_) {
        children.removeIf([&placed](const std::shared_ptr<jadefx::Node>& item) { return item == placed; });
    }
    homeGroups_.clear();
    for (const engine_core::PluginToolbarState& toolbar : toolbars_) {
        if (toolbar.builtin) {
            homeGroups_.push_back(MakeSeparator());
            homeGroups_.push_back(makeGroup(toolbar));
        }
    }
    std::size_t at = children.size();
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (children[i].get() == homeAnchor_) {
            at = i;
        }
    }
    for (const std::shared_ptr<jadefx::Node>& group : homeGroups_) {
        children.insert(at++, group);
    }
}

}  // namespace ide
