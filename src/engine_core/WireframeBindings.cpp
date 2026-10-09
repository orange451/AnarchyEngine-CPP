// WireframeAdornment's Lua methods: AddLine, AddLines, AddPath, and Clear.
// Points are in the Adornee's space, or the world's with none. A color given
// to a call is those lines' own; without one they follow Color3.

#include "ScriptBindings.hpp"

#include "Color3.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "ScriptRuntime.hpp"
#include "WireframeAdornment.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

using Line = WireframeAdornment::Line;

Vec3 point_arg(lua_State* state, int index, const char* name) {
    const float* value = lua_tovector(state, index);
    if (value == nullptr) {
        luaL_error(state, "%s must be a Vector3", name);
    }
    return Vec3{value[0], value[1], value[2]};
}

// The optional color at index into line; nil leaves the line on Color3.
void color_arg(lua_State* state, int index, Line& line) {
    if (lua_isnoneornil(state, index)) {
        return;
    }
    const Color3* color = to_color3(state, index);
    if (color == nullptr) {
        luaL_typeerrorL(state, index, "Color3");
    }
    line.own_color = true;
    line.color = ColorRgb{color->r, color->g, color->b, 1.f};
}

std::vector<Vec3> points_arg(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    const int count = lua_objlen(state, index);
    std::vector<Vec3> points;
    points.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        if (lua_tovector(state, -1) == nullptr) {
            luaL_error(state, "points[%d] must be a Vector3", i);
        }
        points.push_back(point_arg(state, -1, "point"));
        lua_pop(state, 1);
    }
    return points;
}

// Adds every line or none: a refusal part way leaves the lines as they were.
void add_all(lua_State* state, WireframeAdornment& wire, const std::vector<Line>& lines) {
    if (wire.lines().size() + lines.size() > WireframeAdornment::kMaxLines) {
        luaL_error(state, "a WireframeAdornment holds at most 65536 lines");
    }
    for (const Line& line : lines) {
        if (std::optional<std::string> error = wire.add_line(line)) {
            luaL_error(state, "%s", error->c_str());
        }
    }
}

int wire_add_line(lua_State* state) {
    return lua_guard(state, [&] {
        WireframeAdornment& wire = ScriptBindings::wireframe_self(state);
        Line line;
        line.from = point_arg(state, 2, "from");
        line.to = point_arg(state, 3, "to");
        color_arg(state, 4, line);
        add_all(state, wire, {line});
        return 0;
    });
}

int wire_add_lines(lua_State* state) {
    return lua_guard(state, [&] {
        WireframeAdornment& wire = ScriptBindings::wireframe_self(state);
        const std::vector<Vec3> points = points_arg(state, 2);
        if (points.size() % 2 != 0) {
            luaL_error(state, "points must come in pairs");
        }
        Line shape;
        color_arg(state, 3, shape);
        std::vector<Line> lines;
        lines.reserve(points.size() / 2);
        for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
            Line line = shape;
            line.from = points[i];
            line.to = points[i + 1];
            lines.push_back(line);
        }
        add_all(state, wire, lines);
        return 0;
    });
}

int wire_add_path(lua_State* state) {
    return lua_guard(state, [&] {
        WireframeAdornment& wire = ScriptBindings::wireframe_self(state);
        const std::vector<Vec3> points = points_arg(state, 2);
        const bool closed = lua_toboolean(state, 3) != 0;
        Line shape;
        color_arg(state, 4, shape);
        std::vector<Line> lines;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            Line line = shape;
            line.from = points[i];
            line.to = points[i + 1];
            lines.push_back(line);
        }
        if (closed && points.size() > 2) {
            Line line = shape;
            line.from = points.back();
            line.to = points.front();
            lines.push_back(line);
        }
        add_all(state, wire, lines);
        return 0;
    });
}

int wire_clear(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptBindings::wireframe_self(state).clear_lines();
        return 0;
    });
}

int wire_get_line_count(lua_State* state) {
    return lua_guard(state, [&] {
        lua_pushnumber(state, static_cast<double>(ScriptBindings::wireframe_self(state).lines().size()));
        return 1;
    });
}

ANARCHY_LUA_REGISTER(register_wireframe_methods) {
    const LuaField methods[] = {
        lua_method("AddLine", "nil", reinterpret_cast<void*>(&wire_add_line)),
        lua_method("AddLines", "nil", reinterpret_cast<void*>(&wire_add_lines)),
        lua_method("AddPath", "nil", reinterpret_cast<void*>(&wire_add_path)),
        lua_method("Clear", "nil", reinterpret_cast<void*>(&wire_clear)),
        lua_method("GetLineCount", "number", reinterpret_cast<void*>(&wire_get_line_count)),
    };
    register_lua_class("WireframeAdornment", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
}

}  // namespace

WireframeAdornment& ScriptBindings::wireframe_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* wire =
        runtime == nullptr ? nullptr : dynamic_cast<WireframeAdornment*>(runtime->resolve_id(ud->id, ud->world));
    if (wire == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *wire;
}

void ScriptBindings::link_wireframe_methods() {}

}  // namespace engine_core
