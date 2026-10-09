#pragma once

#include "Events.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine_core {

// A toolbar button as the studio draws it.
struct PluginButtonState {
    std::uint32_t id = 0;
    // The id the plugin gave it, which Name reads.
    std::string key;
    std::string tooltip;
    // "icons/<file>", or empty for a button with only text.
    std::string icon;
    std::string text;
    bool active = false;
    bool enabled = true;
};

// A plugin's toolbar: a group on the studio's Plugins tab.
struct PluginToolbarState {
    std::uint32_t id = 0;
    // The plugin's name.
    std::string plugin;
    std::string name;
    std::vector<PluginButtonState> buttons;
};

// The plugins' toolbars, buttons, and signals, which plugin scripts make and
// the studio draws. A plugin is known by the serial that owns its threads.
// The simulation thread writes. The studio reads toolbars() under the
// DataModel read lock when revision() has moved, and posts clicks back.
class PluginUi {
public:
    PluginUi() = default;
    PluginUi(const PluginUi&) = delete;
    PluginUi& operator=(const PluginUi&) = delete;
    ~PluginUi();

    // Signals bind to events from here on. detach releases them all.
    void attach(EventQueue& events);
    void detach();

    // builtin puts its toolbars first, ahead of the user's plugins.
    void add_plugin(std::uint32_t serial, std::string name, bool builtin);
    // Drops its toolbars and releases its signals. Its connections must already be gone.
    void remove_plugin(std::uint32_t serial);
    // Null for a serial that is not a plugin.
    const std::string* plugin_name(std::uint32_t serial) const;

    // A signal's key, which a Lua signal object carries. 0 for none.
    std::uint32_t unloading_key(std::uint32_t serial) const;
    // Queues the plugin's Unloading; draining the events runs its handlers.
    void fire_unloading(std::uint32_t serial);

    // 0 for a serial that is not a plugin.
    std::uint32_t create_toolbar(std::uint32_t serial, std::string name);
    // 0, with error, for a toolbar that is gone, a key the toolbar has
    // already, or an icon that is not empty and not plugin_icon_path_ok.
    std::uint32_t create_button(std::uint32_t toolbar, std::string key, std::string tooltip, std::string icon,
                                std::string text, std::string& error);
    // Null for a button that is gone.
    const PluginButtonState* button(std::uint32_t id) const;
    std::uint32_t click_key(std::uint32_t button) const;
    // False for a button that is gone.
    bool set_active(std::uint32_t button, bool active);
    bool set_enabled(std::uint32_t button, bool enabled);
    // Queues Click. False for a button that is gone or disabled.
    bool click(std::uint32_t button);

    // Null for a key that is gone.
    Signal* signal(std::uint32_t key);
    // Moves at every change to what toolbars() returns.
    std::uint64_t revision() const { return revision_; }
    // The built-ins' toolbars, then the user plugins', by plugin name, each
    // plugin's in the order it made them.
    std::vector<PluginToolbarState> toolbars() const;

private:
    struct Plugin {
        std::string name;
        bool builtin = false;
        std::uint32_t unloading = 0;
    };
    struct Toolbar {
        std::uint32_t serial = 0;
        PluginToolbarState state;
        // Each button's Click key, in button order.
        std::vector<std::uint32_t> clicks;
    };

    std::uint32_t make_signal();
    void release(std::uint32_t key);
    PluginButtonState* find_button(std::uint32_t id, std::uint32_t* click = nullptr);
    const PluginButtonState* find_button(std::uint32_t id, std::uint32_t* click = nullptr) const;

    EventQueue* events_ = nullptr;
    std::map<std::uint32_t, Plugin> plugins_;
    std::vector<Toolbar> toolbars_;
    std::map<std::uint32_t, std::unique_ptr<Signal>> signals_;
    std::uint32_t next_id_ = 1;
    std::uint64_t revision_ = 1;
};

// True for "icons/<file>": under the studio's icons folder, with no "..",
// no backslash, and a file name.
bool plugin_icon_path_ok(const std::string& path);

}  // namespace engine_core
