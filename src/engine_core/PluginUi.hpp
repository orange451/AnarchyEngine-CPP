#pragma once

#include "Events.hpp"
#include "Vector3.hpp"

#include <atomic>
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

// One mouse event in a Scene View, as the active plugin's mouse hears it.
struct PluginMouseEvent {
    enum class Kind { Button1Down, Button1Up, Button2Down, Button2Up, Move, WheelForward, WheelBackward };
    Kind kind = Kind::Move;
    // Points from the view's top left.
    float x = 0;
    float y = 0;
    // The ray from the camera through the pointer; direction is unit length.
    Vec3 origin{};
    Vec3 direction{0.f, 0.f, -1.f};
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
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

    // The active plugin, which has the Scene View's left button to itself and
    // whose mouse hears the view. At most one; activating another, or this
    // one going, fires the last one's Deactivation.
    void activate(std::uint32_t serial);
    // Fires its Deactivation when serial is the active one.
    void deactivate(std::uint32_t serial);
    void deactivate_all();
    std::uint32_t active() const { return active_; }
    std::uint32_t deactivation_key(std::uint32_t serial) const;
    // True while a plugin is active. Any thread may read it: the Scene View
    // keeps the left button from the game's input while it is set.
    bool mouse_held() const { return held_.load(); }
    // Moves each time a plugin is activated, so the Scene View can tell that
    // one took over from its own tools.
    std::uint64_t activations() const { return activations_.load(); }

    // The last mouse event's state, and the key of each mouse signal of serial's mouse.
    void mouse_event(const PluginMouseEvent& event);
    const PluginMouseEvent& mouse() const { return mouse_; }
    std::uint32_t mouse_key(std::uint32_t serial, PluginMouseEvent::Kind kind) const;

    // Null for a key that is gone.
    Signal* signal(std::uint32_t key);
    // Moves at every change to what toolbars() returns.
    std::uint64_t revision() const { return revision_; }
    // The built-ins' toolbars, then the user plugins', by plugin name, each
    // plugin's in the order it made them.
    std::vector<PluginToolbarState> toolbars() const;

private:
    static constexpr int kMouseSignals = 7;
    struct Plugin {
        std::string name;
        bool builtin = false;
        std::uint32_t unloading = 0;
        std::uint32_t deactivation = 0;
        // By PluginMouseEvent::Kind.
        std::uint32_t mouse[kMouseSignals] = {};
    };
    void emit(std::uint32_t key);
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
    std::uint32_t active_ = 0;
    std::atomic<bool> held_{false};
    std::atomic<std::uint64_t> activations_{0};
    PluginMouseEvent mouse_;
};

// True for "icons/<file>": under the studio's icons folder, with no "..",
// no backslash, and a file name.
bool plugin_icon_path_ok(const std::string& path);

}  // namespace engine_core
