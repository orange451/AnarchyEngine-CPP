#include "PluginUi.hpp"

#include "LuaApi.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace engine_core {

PluginUi::~PluginUi() { detach(); }

void PluginUi::attach(EventQueue& events) {
    events_ = &events;
    for (auto& [key, signal] : signals_) {
        events.host_signal(signal.get());
    }
}

void PluginUi::detach() {
    if (events_ != nullptr) {
        for (auto& [key, signal] : signals_) {
            events_->release_signal(*signal);
        }
    }
    events_ = nullptr;
}

std::uint32_t PluginUi::make_signal() {
    const std::uint32_t key = next_id_++;
    auto signal = std::make_unique<Signal>();
    if (events_ != nullptr) {
        events_->host_signal(signal.get());
    }
    signals_.emplace(key, std::move(signal));
    return key;
}

void PluginUi::release(std::uint32_t key) {
    const auto found = signals_.find(key);
    if (found == signals_.end()) {
        return;
    }
    if (events_ != nullptr) {
        events_->release_signal(*found->second);
    }
    signals_.erase(found);
}

void PluginUi::add_plugin(std::uint32_t serial, std::string name, bool builtin) {
    remove_plugin(serial);
    Plugin plugin;
    plugin.name = std::move(name);
    plugin.builtin = builtin;
    plugin.unloading = make_signal();
    plugin.deactivation = make_signal();
    for (std::uint32_t& key : plugin.mouse) {
        key = make_signal();
    }
    plugins_.emplace(serial, std::move(plugin));
}

void PluginUi::remove_plugin(std::uint32_t serial) {
    const auto found = plugins_.find(serial);
    if (found == plugins_.end()) {
        return;
    }
    // Its handlers are gone with it, so nothing hears its Deactivation.
    if (active_ == serial) {
        active_ = 0;
        held_.store(false);
    }
    release(found->second.unloading);
    release(found->second.deactivation);
    for (std::uint32_t key : found->second.mouse) {
        release(key);
    }
    plugins_.erase(found);
    const auto gone = std::remove_if(toolbars_.begin(), toolbars_.end(), [&](const Toolbar& toolbar) {
        if (toolbar.serial != serial) {
            return false;
        }
        for (std::uint32_t key : toolbar.clicks) {
            release(key);
        }
        return true;
    });
    if (gone != toolbars_.end()) {
        toolbars_.erase(gone, toolbars_.end());
        ++revision_;
    }
}

const std::string* PluginUi::plugin_name(std::uint32_t serial) const {
    const auto found = plugins_.find(serial);
    return found != plugins_.end() ? &found->second.name : nullptr;
}

std::uint32_t PluginUi::unloading_key(std::uint32_t serial) const {
    const auto found = plugins_.find(serial);
    return found != plugins_.end() ? found->second.unloading : 0;
}

void PluginUi::fire_unloading(std::uint32_t serial) {
    Signal* unloading = signal(unloading_key(serial));
    if (unloading != nullptr && events_ != nullptr) {
        events_->emit_args(unloading->id(), 0, {});
    }
}

std::uint32_t PluginUi::create_toolbar(std::uint32_t serial, std::string name) {
    if (plugins_.count(serial) == 0) {
        return 0;
    }
    Toolbar toolbar;
    toolbar.serial = serial;
    toolbar.state.id = next_id_++;
    toolbar.state.plugin = plugins_.at(serial).name;
    toolbar.state.name = std::move(name);
    toolbars_.push_back(std::move(toolbar));
    ++revision_;
    return toolbars_.back().state.id;
}

std::uint32_t PluginUi::create_button(std::uint32_t toolbar, std::string key, std::string tooltip, std::string icon,
                                      std::string text, std::string& error) {
    const auto found = std::find_if(toolbars_.begin(), toolbars_.end(),
                                    [&](const Toolbar& item) { return item.state.id == toolbar; });
    if (found == toolbars_.end()) {
        error = "the toolbar's plugin has unloaded";
        return 0;
    }
    for (const PluginButtonState& button : found->state.buttons) {
        if (button.key == key) {
            error = "a button with id \"" + key + "\" is in this toolbar already";
            return 0;
        }
    }
    if (!icon.empty() && !plugin_icon_path_ok(icon)) {
        error = "icon must be a file under icons/, such as icons/Play.png";
        return 0;
    }
    PluginButtonState button;
    button.id = next_id_++;
    button.key = std::move(key);
    button.tooltip = std::move(tooltip);
    button.icon = std::move(icon);
    button.text = std::move(text);
    found->state.buttons.push_back(std::move(button));
    found->clicks.push_back(make_signal());
    ++revision_;
    return found->state.buttons.back().id;
}

PluginButtonState* PluginUi::find_button(std::uint32_t id, std::uint32_t* click) {
    for (Toolbar& toolbar : toolbars_) {
        for (std::size_t i = 0; i < toolbar.state.buttons.size(); ++i) {
            if (toolbar.state.buttons[i].id == id) {
                if (click != nullptr) {
                    *click = toolbar.clicks[i];
                }
                return &toolbar.state.buttons[i];
            }
        }
    }
    return nullptr;
}

const PluginButtonState* PluginUi::find_button(std::uint32_t id, std::uint32_t* click) const {
    return const_cast<PluginUi*>(this)->find_button(id, click);
}

const PluginButtonState* PluginUi::button(std::uint32_t id) const { return find_button(id); }

std::uint32_t PluginUi::click_key(std::uint32_t button) const {
    std::uint32_t key = 0;
    find_button(button, &key);
    return key;
}

bool PluginUi::set_active(std::uint32_t button, bool active) {
    PluginButtonState* found = find_button(button);
    if (found == nullptr) {
        return false;
    }
    if (found->active != active) {
        found->active = active;
        ++revision_;
    }
    return true;
}

bool PluginUi::set_enabled(std::uint32_t button, bool enabled) {
    PluginButtonState* found = find_button(button);
    if (found == nullptr) {
        return false;
    }
    if (found->enabled != enabled) {
        found->enabled = enabled;
        ++revision_;
    }
    return true;
}

bool PluginUi::click(std::uint32_t button) {
    std::uint32_t key = 0;
    const PluginButtonState* found = find_button(button, &key);
    if (found == nullptr || !found->enabled) {
        return false;
    }
    Signal* clicked = signal(key);
    if (clicked == nullptr || events_ == nullptr) {
        return false;
    }
    events_->emit_args(clicked->id(), 0, {});
    return true;
}

void PluginUi::emit(std::uint32_t key) {
    Signal* fired = signal(key);
    if (fired != nullptr && events_ != nullptr) {
        events_->emit_args(fired->id(), 0, {});
    }
}

void PluginUi::activate(std::uint32_t serial) {
    if (plugins_.count(serial) == 0) {
        return;
    }
    if (active_ != 0 && active_ != serial) {
        emit(deactivation_key(active_));
    }
    active_ = serial;
    held_.store(true);
    activations_.fetch_add(1);
}

void PluginUi::deactivate(std::uint32_t serial) {
    if (serial == 0 || active_ != serial) {
        return;
    }
    active_ = 0;
    held_.store(false);
    emit(deactivation_key(serial));
}

void PluginUi::deactivate_all() { deactivate(active_); }

std::uint32_t PluginUi::deactivation_key(std::uint32_t serial) const {
    const auto found = plugins_.find(serial);
    return found != plugins_.end() ? found->second.deactivation : 0;
}

void PluginUi::mouse_event(const PluginMouseEvent& event) {
    mouse_ = event;
    if (active_ != 0) {
        emit(mouse_key(active_, event.kind));
    }
}

std::uint32_t PluginUi::mouse_key(std::uint32_t serial, PluginMouseEvent::Kind kind) const {
    const auto found = plugins_.find(serial);
    const int index = static_cast<int>(kind);
    return found != plugins_.end() && index >= 0 && index < kMouseSignals ? found->second.mouse[index] : 0;
}

Signal* PluginUi::signal(std::uint32_t key) {
    const auto found = signals_.find(key);
    return found != signals_.end() ? found->second.get() : nullptr;
}

std::vector<PluginToolbarState> PluginUi::toolbars() const {
    std::vector<const Toolbar*> order;
    order.reserve(toolbars_.size());
    for (const Toolbar& toolbar : toolbars_) {
        order.push_back(&toolbar);
    }
    auto rank = [this](const Toolbar* toolbar) {
        const auto plugin = plugins_.find(toolbar->serial);
        const bool builtin = plugin != plugins_.end() && plugin->second.builtin;
        // The built-ins in the order they loaded, which is the studio's own list; the
        // user's plugins by name, so a reload keeps a plugin's place.
        return std::make_tuple(builtin ? 0 : 1, builtin ? std::string() : toolbar->state.plugin, toolbar->serial,
                               toolbar->state.id);
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](const Toolbar* a, const Toolbar* b) { return rank(a) < rank(b); });
    std::vector<PluginToolbarState> out;
    out.reserve(order.size());
    for (const Toolbar* toolbar : order) {
        out.push_back(toolbar->state);
        const auto plugin = plugins_.find(toolbar->serial);
        out.back().builtin = plugin != plugins_.end() && plugin->second.builtin;
    }
    return out;
}

bool plugin_icon_path_ok(const std::string& path) {
    const std::string prefix = "icons/";
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
           path.find("..") == std::string::npos && path.find('\\') == std::string::npos;
}

}  // namespace engine_core
