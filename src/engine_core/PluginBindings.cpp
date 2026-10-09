// The plugin global a plugin VM thread sees, and what it makes: toolbars and
// their buttons. Each is a handle on ScriptRuntime's PluginUi, by id, so a
// handle whose plugin has unloaded fails cleanly instead of dangling.

#include "ScriptBindings.hpp"

#include "Gui.hpp"
#include "Matrix4.hpp"
#include "PhysicsWorld.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace engine_core {

using namespace script_internal;

namespace {

int plugin_newindex(lua_State* state) {
    luaL_checkudata(state, 1, kPluginMeta);
    luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2));
}

int plugin_tostring(lua_State* state) {
    luaL_checkudata(state, 1, kPluginMeta);
    lua_pushstring(state, "Plugin");
    return 1;
}

int object_newindex(lua_State* state) { luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2)); }

int toolbar_tostring(lua_State* state) {
    lua_pushstring(state, "PluginToolbar");
    return 1;
}

int button_tostring(lua_State* state) {
    lua_pushstring(state, "PluginToolbarButton");
    return 1;
}

int mouse_tostring(lua_State* state) {
    lua_pushstring(state, "PluginMouse");
    return 1;
}

// A toolbar or button handle.
void push_object(lua_State* state, const char* meta, std::uint32_t id) {
    auto* ud = static_cast<PluginObjectUd*>(lua_newuserdata(state, sizeof(PluginObjectUd)));
    ud->id = id;
    luaL_getmetatable(state, meta);
    lua_setmetatable(state, -2);
}

// A metatable whose __index and __newindex are given, read-only, named type_name for tostring.
void install(lua_State* state, const char* meta, lua_CFunction index, lua_CFunction newindex,
             lua_CFunction tostring) {
    luaL_newmetatable(state, meta);
    lua_pushcfunction(state, index, "index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushstring(state, "The metatable is locked");
    lua_setfield(state, -2, "__metatable");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

}  // namespace

void push_plugin_signal(lua_State* state, std::uint32_t key, const char* cause) {
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    ud->kind = kSignalPlugin;
    ud->id = key;
    ud->event_name = cause;
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
}

int ScriptBindings::plugin_index(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        const std::string* name = runtime != nullptr ? runtime->plugin_ui().plugin_name(ud->serial) : nullptr;
        if (name == nullptr) {
            luaL_error(state, "the plugin has unloaded");
        }
        if (std::strcmp(key, "Name") == 0) {
            lua_pushstring(state, name->c_str());
            return 1;
        }
        if (std::strcmp(key, "Unloading") == 0) {
            push_plugin_signal(state, runtime->plugin_ui().unloading_key(ud->serial), "Unloading");
            return 1;
        }
        if (std::strcmp(key, "Deactivation") == 0) {
            push_plugin_signal(state, runtime->plugin_ui().deactivation_key(ud->serial), "Deactivation");
            return 1;
        }
        if (std::strcmp(key, "Activate") == 0) {
            lua_pushcfunction(state, &ScriptBindings::plugin_activate, "Activate");
            return 1;
        }
        if (std::strcmp(key, "Deactivate") == 0) {
            lua_pushcfunction(state, &ScriptBindings::plugin_deactivate, "Deactivate");
            return 1;
        }
        if (std::strcmp(key, "IsActivated") == 0) {
            lua_pushcfunction(state, &ScriptBindings::plugin_is_activated, "IsActivated");
            return 1;
        }
        if (std::strcmp(key, "GetMouse") == 0) {
            lua_pushcfunction(state, &ScriptBindings::plugin_get_mouse, "GetMouse");
            return 1;
        }
        if (std::strcmp(key, "CreateToolbar") == 0) {
            lua_pushcfunction(state, &ScriptBindings::plugin_create_toolbar, "CreateToolbar");
            return 1;
        }
        if (std::strcmp(key, "CreateDockWidget") == 0) {
            lua_pushcfunction(state, &ScriptBindings::plugin_create_dock_widget, "CreateDockWidget");
            return 1;
        }
        luaL_error(state, "%s is not a valid member of Plugin", key);
    });
}

// The plugin a method's self names, or a Lua error when it has unloaded.
static std::uint32_t live_plugin(lua_State* state, ScriptRuntime*& runtime) {
    const auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
    runtime = runtime_from(state);
    if (runtime == nullptr || runtime->plugin_ui().plugin_name(ud->serial) == nullptr) {
        luaL_error(state, "the plugin has unloaded");
    }
    return ud->serial;
}

int ScriptBindings::plugin_activate(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = nullptr;
        const std::uint32_t serial = live_plugin(state, runtime);
        // Roblox's exclusiveMouse argument is taken and ignored: an active plugin
        // always has the left button to itself.
        runtime->plugin_ui().activate(serial);
        return 0;
    });
}

int ScriptBindings::plugin_deactivate(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = nullptr;
        const std::uint32_t serial = live_plugin(state, runtime);
        runtime->plugin_ui().deactivate(serial);
        return 0;
    });
}

int ScriptBindings::plugin_is_activated(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = nullptr;
        const std::uint32_t serial = live_plugin(state, runtime);
        lua_pushboolean(state, runtime->plugin_ui().active() == serial);
        return 1;
    });
}

int ScriptBindings::plugin_get_mouse(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = nullptr;
        const std::uint32_t serial = live_plugin(state, runtime);
        // One mouse per plugin, kept in the plugin cache beside the plugin's own userdata.
        lua_getfield(state, LUA_REGISTRYINDEX, kPluginCache);
        const double key = -static_cast<double>(serial);
        lua_pushnumber(state, key);
        lua_rawget(state, -2);
        if (test_userdata(state, -1, kPluginMouseMeta) == nullptr) {
            lua_pop(state, 1);
            auto* mouse = static_cast<PluginUd*>(lua_newuserdata(state, sizeof(PluginUd)));
            mouse->serial = serial;
            luaL_getmetatable(state, kPluginMouseMeta);
            lua_setmetatable(state, -2);
            lua_pushnumber(state, key);
            lua_pushvalue(state, -2);
            lua_rawset(state, -4);
        }
        lua_remove(state, -2);
        return 1;
    });
}

int ScriptBindings::mouse_index(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMouseMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->plugin_ui().plugin_name(ud->serial) == nullptr) {
            luaL_error(state, "the mouse's plugin has unloaded");
        }
        const PluginUi& ui = runtime->plugin_ui();
        const PluginMouseEvent& mouse = ui.mouse();
        static const std::pair<const char*, PluginMouseEvent::Kind> kSignals[] = {
            {"Button1Down", PluginMouseEvent::Kind::Button1Down},
            {"Button1Up", PluginMouseEvent::Kind::Button1Up},
            {"Button2Down", PluginMouseEvent::Kind::Button2Down},
            {"Button2Up", PluginMouseEvent::Kind::Button2Up},
            {"Move", PluginMouseEvent::Kind::Move},
            {"WheelForward", PluginMouseEvent::Kind::WheelForward},
            {"WheelBackward", PluginMouseEvent::Kind::WheelBackward}};
        for (const auto& [name, kind] : kSignals) {
            if (std::strcmp(key, name) == 0) {
                push_plugin_signal(state, ui.mouse_key(ud->serial, kind), name);
                return 1;
            }
        }
        if (std::strcmp(key, "X") == 0) {
            lua_pushnumber(state, mouse.x);
            return 1;
        }
        if (std::strcmp(key, "Y") == 0) {
            lua_pushnumber(state, mouse.y);
            return 1;
        }
        if (std::strcmp(key, "Shift") == 0 || std::strcmp(key, "Ctrl") == 0 || std::strcmp(key, "Alt") == 0) {
            lua_pushboolean(state, key[0] == 'S' ? mouse.shift : key[0] == 'C' ? mouse.ctrl : mouse.alt);
            return 1;
        }
        if (std::strcmp(key, "UnitRay") == 0) {
            lua_createtable(state, 0, 2);
            lua_pushvector(state, mouse.origin.x, mouse.origin.y, mouse.origin.z);
            lua_setfield(state, -2, "Origin");
            lua_pushvector(state, mouse.direction.x, mouse.direction.y, mouse.direction.z);
            lua_setfield(state, -2, "Direction");
            return 1;
        }
        if (std::strcmp(key, "Hit") == 0 || std::strcmp(key, "Target") == 0) {
            // Read when asked, so a move costs nothing until a script looks.
            constexpr float kReach = 1000.f;
            const Vec3 reach{mouse.direction.x * kReach, mouse.direction.y * kReach, mouse.direction.z * kReach};
            std::optional<RayHit> hit;
            if (PhysicsWorld* physics = runtime->game_->physics()) {
                hit = physics->raycast(*runtime->game_, mouse.origin, reach, RayFilter{});
            }
            if (key[0] == 'T') {
                if (hit && hit->instance != 0) {
                    runtime->push_instance(state, hit->instance);
                } else {
                    lua_pushnil(state);
                }
                return 1;
            }
            const Vec3 at = hit ? hit->position
                                : Vec3{mouse.origin.x + reach.x, mouse.origin.y + reach.y, mouse.origin.z + reach.z};
            // Like Roblox's Mouse.Hit: at the point, looking along the ray.
            const Vec3 ahead{at.x + mouse.direction.x, at.y + mouse.direction.y, at.z + mouse.direction.z};
            push_matrix4(state, matrix4_look_at(at, ahead, Vec3{0.f, 1.f, 0.f}));
            return 1;
        }
        luaL_error(state, "%s is not a valid member of PluginMouse", key);
    });
}

int ScriptBindings::plugin_create_toolbar(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
        const std::string name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        const std::uint32_t toolbar = runtime != nullptr ? runtime->plugin_ui().create_toolbar(ud->serial, name) : 0;
        if (toolbar == 0) {
            luaL_error(state, "the plugin has unloaded");
        }
        push_object(state, kPluginToolbarMeta, toolbar);
        return 1;
    });
}

int ScriptBindings::plugin_create_dock_widget(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginUd*>(luaL_checkudata(state, 1, kPluginMeta));
        const std::string key = luaL_checkstring(state, 2);
        const bool options = lua_istable(state, 3);
        if (!options && !lua_isnoneornil(state, 3)) {
            luaL_error(state, "CreateDockWidget's options must be a table");
        }
        ScriptRuntime* runtime = runtime_from(state);
        const std::string* plugin = runtime != nullptr ? runtime->plugin_ui().plugin_name(ud->serial) : nullptr;
        const InstanceId root = runtime != nullptr ? runtime->plugin_root(ud->serial) : 0;
        if (plugin == nullptr || root == 0) {
            luaL_error(state, "the plugin has unloaded");
        }
        DataModel& game = *runtime->game_;
        for (InstanceId child = game.first_child(root); child != 0; child = game.next_sibling(child)) {
            const auto* other = dynamic_cast<const DockWidget*>(game.instance(child));
            if (other != nullptr && other->key() == key) {
                luaL_error(state, "a dock widget with id \"%s\" exists already", key.c_str());
            }
        }
        // Read every option before anything is made, so a bad one leaves nothing behind.
        std::string title = key;
        bool enabled = false;
        DockSide side = DockSide::TopRight;
        double size[4] = {300, 400, 0, 0};
        if (options) {
            lua_getfield(state, 3, "Title");
            if (lua_isstring(state, -1)) {
                title = lua_tostring(state, -1);
            }
            lua_pop(state, 1);
            lua_getfield(state, 3, "Enabled");
            enabled = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            lua_getfield(state, 3, "InitialDock");
            if (!lua_isnil(state, -1)) {
                const char* name = lua_tostring(state, -1);
                const std::string dock = name != nullptr ? name : "";
                static const std::pair<const char*, DockSide> kSides[] = {
                    {"TopLeft", DockSide::TopLeft},         {"BottomLeft", DockSide::BottomLeft},
                    {"TopRight", DockSide::TopRight},       {"BottomRight", DockSide::BottomRight},
                    {"Bottom", DockSide::Bottom},           {"Center", DockSide::Center},
                    {"Float", DockSide::Float}};
                bool known = false;
                for (const auto& [label, value] : kSides) {
                    if (dock == label) {
                        side = value;
                        known = true;
                    }
                }
                if (!known) {
                    luaL_error(state,
                               "InitialDock must be TopLeft, BottomLeft, TopRight, BottomRight, Bottom, Center, or Float");
                }
            }
            lua_pop(state, 1);
            const char* const fields[4] = {"Width", "Height", "MinWidth", "MinHeight"};
            for (int i = 0; i < 4; ++i) {
                lua_getfield(state, 3, fields[i]);
                if (lua_isnumber(state, -1)) {
                    size[i] = std::max(0.0, static_cast<double>(lua_tonumber(state, -1)));
                }
                lua_pop(state, 1);
            }
        }
        DockWidget& widget = game.create<DockWidget>();
        widget.set_origin(*plugin, key);
        game.set_name(widget.id(), key);
        widget.initial_dock = side;
        widget.width = size[0];
        widget.height = size[1];
        widget.min_width = size[2];
        widget.min_height = size[3];
        widget.set_title(title);
        widget.set_enabled(enabled);
        game.set_parent(widget.id(), root);
        runtime->push_instance(state, widget.id());
        return 1;
    });
}

int ScriptBindings::toolbar_index(lua_State* state) {
    luaL_checkudata(state, 1, kPluginToolbarMeta);
    const char* key = luaL_checkstring(state, 2);
    if (std::strcmp(key, "CreateButton") == 0) {
        lua_pushcfunction(state, &ScriptBindings::toolbar_create_button, "CreateButton");
        return 1;
    }
    luaL_error(state, "%s is not a valid member of PluginToolbar", key);
}

int ScriptBindings::toolbar_create_button(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginObjectUd*>(luaL_checkudata(state, 1, kPluginToolbarMeta));
        std::string key = luaL_checkstring(state, 2);
        std::string tooltip = luaL_optstring(state, 3, "");
        std::string icon = luaL_optstring(state, 4, "");
        std::string text = lua_isnoneornil(state, 5) ? key : std::string(luaL_checkstring(state, 5));
        ScriptRuntime* runtime = runtime_from(state);
        std::string error = "the toolbar's plugin has unloaded";
        const std::uint32_t button =
            runtime != nullptr ? runtime->plugin_ui().create_button(ud->id, std::move(key), std::move(tooltip),
                                                                      std::move(icon), std::move(text), error)
                               : 0;
        if (button == 0) {
            luaL_error(state, "%s", error.c_str());
        }
        push_object(state, kPluginButtonMeta, button);
        return 1;
    });
}

int ScriptBindings::button_index(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginObjectUd*>(luaL_checkudata(state, 1, kPluginButtonMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        const PluginButtonState* button = runtime != nullptr ? runtime->plugin_ui().button(ud->id) : nullptr;
        if (button == nullptr) {
            luaL_error(state, "the button's plugin has unloaded");
        }
        if (std::strcmp(key, "Name") == 0) {
            lua_pushstring(state, button->key.c_str());
            return 1;
        }
        if (std::strcmp(key, "Enabled") == 0) {
            lua_pushboolean(state, button->enabled);
            return 1;
        }
        if (std::strcmp(key, "Click") == 0) {
            push_plugin_signal(state, runtime->plugin_ui().click_key(ud->id), "Click");
            return 1;
        }
        if (std::strcmp(key, "SetActive") == 0) {
            lua_pushcfunction(state, &ScriptBindings::button_set_active, "SetActive");
            return 1;
        }
        luaL_error(state, "%s is not a valid member of PluginToolbarButton", key);
    });
}

int ScriptBindings::button_newindex(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginObjectUd*>(luaL_checkudata(state, 1, kPluginButtonMeta));
        const char* key = luaL_checkstring(state, 2);
        if (std::strcmp(key, "Enabled") != 0) {
            luaL_error(state, "%s cannot be assigned to", key);
        }
        if (!lua_isboolean(state, 3)) {
            luaL_error(state, "Enabled must be true or false");
        }
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || !runtime->plugin_ui().set_enabled(ud->id, lua_toboolean(state, 3) != 0)) {
            luaL_error(state, "the button's plugin has unloaded");
        }
        return 0;
    });
}

int ScriptBindings::button_set_active(lua_State* state) {
    return lua_guard(state, [&] {
        const auto* ud = static_cast<PluginObjectUd*>(luaL_checkudata(state, 1, kPluginButtonMeta));
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || !runtime->plugin_ui().set_active(ud->id, lua_toboolean(state, 2) != 0)) {
            luaL_error(state, "the button's plugin has unloaded");
        }
        return 0;
    });
}

void open_plugin_api(lua_State* state) {
    install(state, kPluginMeta, &ScriptBindings::plugin_index, plugin_newindex, plugin_tostring);
    install(state, kPluginToolbarMeta, &ScriptBindings::toolbar_index, object_newindex, toolbar_tostring);
    install(state, kPluginButtonMeta, &ScriptBindings::button_index, &ScriptBindings::button_newindex,
            button_tostring);
    install(state, kPluginMouseMeta, &ScriptBindings::mouse_index, object_newindex, mouse_tostring);
    // One userdata per plugin, so plugin == plugin holds across its threads.
    lua_newtable(state);
    lua_newtable(state);
    lua_pushstring(state, "v");
    lua_setfield(state, -2, "__mode");
    lua_setreadonly(state, -1, 1);
    lua_setmetatable(state, -2);
    lua_setfield(state, LUA_REGISTRYINDEX, kPluginCache);
}

void ScriptRuntime::set_plugin_global(lua_State* co, std::uint32_t serial) {
    if (plugin_ui_.plugin_name(serial) == nullptr) {
        lua_pushnil(co);
        lua_setglobal(co, "plugin");
        return;
    }
    lua_getfield(co, LUA_REGISTRYINDEX, kPluginCache);
    lua_pushnumber(co, static_cast<double>(serial));
    lua_rawget(co, -2);
    if (test_userdata(co, -1, kPluginMeta) == nullptr) {
        lua_pop(co, 1);
        auto* ud = static_cast<PluginUd*>(lua_newuserdata(co, sizeof(PluginUd)));
        ud->serial = serial;
        luaL_getmetatable(co, kPluginMeta);
        lua_setmetatable(co, -2);
        lua_pushnumber(co, static_cast<double>(serial));
        lua_pushvalue(co, -2);
        lua_rawset(co, -4);
    }
    lua_remove(co, -2);
    lua_setglobal(co, "plugin");
}

}  // namespace engine_core
