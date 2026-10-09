// Camera's Lua methods: between the view's points and the world. A point in
// the view is in ViewportSize's units with (0, 0) at its top-left corner, the
// whole view, nothing reserved for any bar. Both use DraggerMath, so a script
// sees exactly what the studio's own tools pick and draw with.

#include "ScriptBindings.hpp"

#include "Camera.hpp"
#include "DraggerMath.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "ScriptRuntime.hpp"
#include "Vector2.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>

namespace engine_core {

using namespace script_internal;

namespace {

// The view the camera is shown in; raises until one has reported its size.
DraggerView view_of(lua_State* state, const Camera& camera) {
    DraggerView view;
    view.camera = camera.transform();
    view.fov_degrees = static_cast<float>(camera.field_of_view());
    view.size = camera.viewport_size();
    if (!(view.size.x > 0.f && view.size.y > 0.f)) {
        luaL_error(state, "the camera is not shown in a view");
    }
    return view;
}

double finite_number(lua_State* state, int index, const char* name) {
    const double value = luaL_checknumber(state, index);
    if (!std::isfinite(value)) {
        luaL_error(state, "%s must be a finite number", name);
    }
    return value;
}

int camera_viewport_point_to_ray(lua_State* state) {
    return lua_guard(state, [&] {
        const Camera& camera = ScriptBindings::camera_self(state);
        const Vec2 point{static_cast<float>(finite_number(state, 2, "x")),
                         static_cast<float>(finite_number(state, 3, "y"))};
        const DraggerRay ray = viewport_ray(view_of(state, camera), point);
        lua_pushvector(state, ray.origin.x, ray.origin.y, ray.origin.z);
        lua_pushvector(state, ray.direction.x, ray.direction.y, ray.direction.z);
        return 2;
    });
}

int camera_world_to_viewport_point(lua_State* state) {
    return lua_guard(state, [&] {
        const Camera& camera = ScriptBindings::camera_self(state);
        const float* p = lua_tovector(state, 2);
        if (p == nullptr) {
            luaL_typeerrorL(state, 2, "Vector3");
        }
        const DraggerView view = view_of(state, camera);
        const float* m = view.camera.m;
        const auto unit = [](float x, float y, float z) {
            const float l = std::sqrt(x * x + y * y + z * z);
            return l > 0.f ? Vec3{x / l, y / l, z / l} : Vec3{};
        };
        const Vec3 right = unit(m[0], m[1], m[2]);
        const Vec3 up = unit(m[4], m[5], m[6]);
        const Vec3 forward = unit(-m[8], -m[9], -m[10]);
        const Vec3 v{p[0] - m[12], p[1] - m[13], p[2] - m[14]};
        const auto dot = [](Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        // Depth along the look; behind the camera it is negative and the
        // point mirrors through the middle, as a projection does.
        const float depth = dot(v, forward);
        const float tan_half = std::tan(view.fov_degrees * 3.14159265f / 360.f);
        const float aspect = view.size.x / view.size.y;
        const float safe = std::fabs(depth) > 1e-6f ? depth : 1e-6f;
        const float nx = dot(v, right) / (safe * tan_half * aspect);
        const float ny = dot(v, up) / (safe * tan_half);
        const float x = (nx + 1.f) * 0.5f * view.size.x;
        const float y = (1.f - ny) * 0.5f * view.size.y;
        lua_pushvector(state, x, y, depth);
        const bool on_screen = depth > 0.f && x >= 0.f && x <= view.size.x && y >= 0.f && y <= view.size.y;
        lua_pushboolean(state, on_screen ? 1 : 0);
        return 2;
    });
}

ANARCHY_LUA_REGISTER(register_camera_methods) {
    const LuaField methods[] = {
        lua_method("ViewportPointToRay", nullptr, reinterpret_cast<void*>(&camera_viewport_point_to_ray)),
        lua_method("WorldToViewportPoint", nullptr, reinterpret_cast<void*>(&camera_world_to_viewport_point)),
    };
    register_lua_class("Camera", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
}

}  // namespace

Camera& ScriptBindings::camera_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* camera = runtime == nullptr ? nullptr : dynamic_cast<Camera*>(runtime->resolve_id(ud->id, ud->world));
    if (camera == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *camera;
}

void ScriptBindings::link_camera_methods() {}

}  // namespace engine_core
