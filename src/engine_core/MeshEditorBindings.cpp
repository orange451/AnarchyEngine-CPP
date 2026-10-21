// The MeshEditor datatype Mesh:Edit returns: a script's handle on its Mesh's
// one MeshEditor (docs/superpowers/specs/2026-10-11-mesh-editor-design.md).
// Every call runs on SimulationThread, as every script does. A handle whose
// editor has ended (a Play or Stop since, its Mesh gone, or its file changed
// by something else) raises "this MeshEditor has ended; call mesh:Edit()
// again" at its next use. Ids are numbers.

#include "ScriptBindings.hpp"

#include "Color3.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "MeshEditor.hpp"
#include "MeshEditors.hpp"
#include "MeshShapes.hpp"
#include "Vector2.hpp"

#include "lua.h"
#include "lualib.h"

#include <array>
#include <cmath>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

constexpr const char* kEditorMeta = "AE.MeshEditor";
using Id = MeshEditor::Id;

// Lives in a Luau userdata with a destructor, so the editor is let go with it.
struct EditorUd {
    std::shared_ptr<MeshEditor> editor;
};

void destroy_editor(lua_State* /*state*/, void* data) { static_cast<EditorUd*>(data)->~EditorUd(); }

MeshEditor& editor_self(lua_State* state) {
    auto* ud = static_cast<EditorUd*>(luaL_checkudata(state, 1, kEditorMeta));
    DataModel* game = ScriptBindings::game_of(state);
    if (game == nullptr || !game->mesh_editors().usable(*game, *ud->editor)) {
        luaL_error(state, "this MeshEditor has ended; call mesh:Edit() again");
    }
    return *ud->editor;
}

// Runs fn on the editor at 1, raising a refused edit as a Lua error. The
// error is raised after the catch, since lua_error unwinds on its own.
template <typename Fn>
int edit(lua_State* state, Fn fn) {
    MeshEditor& editor = editor_self(state);
    std::string refused;
    int results = 0;
    try {
        results = fn(editor);
    } catch (const MeshEditError& error) {
        refused = error.what();
    }
    if (!refused.empty()) {
        luaL_error(state, "%s", refused.c_str());
    }
    return results;
}

Id to_id(lua_State* state, double value, const char* what) {
    if (!(value >= 0.0 && value <= 4294967295.0) || std::floor(value) != value) {
        luaL_error(state, "%s must be an id from this MeshEditor", what);
    }
    return static_cast<Id>(value);
}

Id check_id(lua_State* state, int index, const char* what) {
    int is_number = 0;
    const double value = lua_tonumberx(state, index, &is_number);
    if (!is_number) {
        luaL_error(state, "%s must be an id from this MeshEditor", what);
    }
    return to_id(state, value, what);
}

std::optional<Id> opt_id(lua_State* state, int index, const char* what) {
    if (lua_isnoneornil(state, index)) {
        return std::nullopt;
    }
    return check_id(state, index, what);
}

void push_id(lua_State* state, Id id) { lua_pushnumber(state, static_cast<double>(id)); }

void push_opt_id(lua_State* state, std::optional<Id> id) {
    if (id) {
        push_id(state, *id);
    } else {
        lua_pushnil(state);
    }
}

void push_ids(lua_State* state, const std::vector<Id>& ids) {
    lua_createtable(state, static_cast<int>(ids.size()), 0);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        push_id(state, ids[i]);
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

std::vector<Id> check_ids(lua_State* state, int index, const char* what) {
    luaL_checktype(state, index, LUA_TTABLE);
    const int count = lua_objlen(state, index);
    std::vector<Id> ids;
    ids.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        ids.push_back(check_id(state, -1, what));
        lua_pop(state, 1);
    }
    return ids;
}

Vec3 check_vec3(lua_State* state, int index, const char* what) {
    const float* components = lua_tovector(state, index);
    if (components == nullptr) {
        luaL_error(state, "%s must be a Vector3", what);
    }
    return Vec3{components[0], components[1], components[2]};
}

const Matrix4& check_matrix4(lua_State* state, int index, const char* what) {
    const Matrix4* value = to_matrix4(state, index);
    if (value == nullptr) {
        luaL_error(state, "%s must be a Matrix4", what);
    }
    return *value;
}

// What the shapes take, as the Mesh's own methods took them.
float shape_size(lua_State* state, int index, const char* what) {
    const double value = luaL_checknumber(state, index);
    if (!(value > 0.0) || !std::isfinite(value)) {
        luaL_error(state, "%s must be a number above 0", what);
    }
    return static_cast<float>(value);
}

Vec3 shape_position(lua_State* state, int index) {
    return lua_isnoneornil(state, index) ? Vec3{} : check_vec3(state, index, "position");
}

int shape_segments(lua_State* state, int index, int fallback) {
    return lua_isnoneornil(state, index) ? fallback : static_cast<int>(luaL_checkinteger(state, index));
}

bool shape_flag(lua_State* state, int index, bool fallback) {
    return lua_isnoneornil(state, index) ? fallback : lua_toboolean(state, index) != 0;
}

// Adds what make builds and returns its vertices' ids.
template <typename Make>
int add_shape(lua_State* state, MeshEditor& editor, Make make) {
    push_ids(state, editor.add_shape(make));
    return 1;
}

int add_vertex(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_id(s, e.add_vertex(check_vec3(s, 2, "position")));
        return 1;
    });
}

int remove_vertex(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        e.remove_vertex(check_id(s, 2, "vertex"));
        return 0;
    });
}

int get_vertices(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_ids(s, e.vertices());
        return 1;
    });
}

int get_position(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Vec3 p = e.position(check_id(s, 2, "vertex"));
        lua_pushvector(s, p.x, p.y, p.z);
        return 1;
    });
}

int set_position(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        e.set_position(check_id(s, 2, "vertex"), check_vec3(s, 3, "position"));
        return 0;
    });
}

int get_normal(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Vec3 n = e.normal(check_id(s, 2, "vertex"));
        lua_pushvector(s, n.x, n.y, n.z);
        return 1;
    });
}

int set_normal(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id vertex = check_id(s, 2, "vertex");
        e.set_normal(vertex, lua_isnoneornil(s, 3) ? std::nullopt : std::optional<Vec3>(check_vec3(s, 3, "normal")));
        return 0;
    });
}

int get_uv(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_vector2(s, e.uv(check_id(s, 2, "vertex")));
        return 1;
    });
}

int set_uv(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id vertex = check_id(s, 2, "vertex");
        const Vec2* uv = to_vector2(s, 3);
        if (uv == nullptr) {
            luaL_error(s, "uv must be a Vector2");
        }
        e.set_uv(vertex, *uv);
        return 0;
    });
}

int get_color(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const std::array<float, 4> rgba = e.color(check_id(s, 2, "vertex"));
        push_color3(s, Color3{rgba[0], rgba[1], rgba[2]});
        lua_pushnumber(s, rgba[3]);
        return 2;
    });
}

int set_color(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id vertex = check_id(s, 2, "vertex");
        const Color3* color = to_color3(s, 3);
        if (color == nullptr) {
            luaL_error(s, "color must be a Color3");
        }
        const double alpha = lua_isnoneornil(s, 4) ? 1.0 : luaL_checknumber(s, 4);
        e.set_color(vertex, {{color->r, color->g, color->b, static_cast<float>(alpha)}});
        return 0;
    });
}

int add_triangle(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_id(s, e.add_triangle(check_id(s, 2, "a"), check_id(s, 3, "b"), check_id(s, 4, "c")));
        return 1;
    });
}

int remove_triangle(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        e.remove_triangle(check_id(s, 2, "triangle"));
        return 0;
    });
}

int get_triangles(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_ids(s, e.triangles());
        return 1;
    });
}

int get_triangle_vertices(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const std::array<Id, 3> corners = e.triangle_vertices(check_id(s, 2, "triangle"));
        for (const Id corner : corners) {
            push_id(s, corner);
        }
        return 3;
    });
}

int add_box_shape(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const float* size = lua_tovector(s, 2);
        if (size == nullptr || !(size[0] > 0.f && size[1] > 0.f && size[2] > 0.f) ||
            !(std::isfinite(size[0]) && std::isfinite(size[1]) && std::isfinite(size[2]))) {
            luaL_error(s, "size must be a Vector3 above 0 on every axis");
        }
        const Vec3 extent{size[0], size[1], size[2]};
        const Vec3 at = shape_position(s, 3);
        return add_shape(s, e, [&](anarchy::amesh::Data& data) { add_box(data, extent, at); });
    });
}

int add_sphere_shape(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const float radius = shape_size(s, 2, "radius");
        const int segments = shape_segments(s, 3, 24);
        const Vec3 at = shape_position(s, 4);
        return add_shape(s, e, [&](anarchy::amesh::Data& data) { add_sphere(data, radius, segments, at); });
    });
}

int add_cylinder_shape(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const float radius = shape_size(s, 2, "radius");
        const float height = shape_size(s, 3, "height");
        const int segments = shape_segments(s, 4, 16);
        const bool capped = shape_flag(s, 5, true);
        const Vec3 at = shape_position(s, 6);
        return add_shape(s, e,
                         [&](anarchy::amesh::Data& data) { add_cylinder(data, radius, height, segments, capped, at); });
    });
}

int add_cone_shape(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const float radius = shape_size(s, 2, "radius");
        const float height = shape_size(s, 3, "height");
        const int segments = shape_segments(s, 4, 16);
        const bool capped = shape_flag(s, 5, true);
        const Vec3 at = shape_position(s, 6);
        return add_shape(s, e, [&](anarchy::amesh::Data& data) { add_cone(data, radius, height, segments, capped, at); });
    });
}

int add_plane_shape(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const float width = shape_size(s, 2, "width");
        const float depth = shape_size(s, 3, "depth");
        const Vec3 at = shape_position(s, 4);
        const int segments = shape_segments(s, 5, 1);
        return add_shape(s, e, [&](anarchy::amesh::Data& data) { add_plane(data, width, depth, at, segments); });
    });
}

int add_teapot_shape(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const float size = shape_size(s, 2, "size");
        const Vec3 at = shape_position(s, 3);
        return add_shape(s, e, [&](anarchy::amesh::Data& data) { add_teapot(data, size, at); });
    });
}

int clear(lua_State* s) {
    return edit(s, [](MeshEditor& e) {
        e.clear();
        return 0;
    });
}

int add_bone(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const std::string name = luaL_checkstring(s, 2);
        const std::optional<Id> parent = opt_id(s, 3, "parent");
        const Matrix4 rest = lua_isnoneornil(s, 4) ? matrix4_identity() : check_matrix4(s, 4, "rest");
        push_id(s, e.add_bone(name, parent, rest));
        return 1;
    });
}

int remove_bone(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        e.remove_bone(check_id(s, 2, "bone"));
        return 0;
    });
}

int get_bones(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_ids(s, e.bones());
        return 1;
    });
}

int find_bone(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_opt_id(s, e.find_bone(luaL_checkstring(s, 2)));
        return 1;
    });
}

int get_bone_name(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const std::string& name = e.bone_name(check_id(s, 2, "bone"));
        lua_pushlstring(s, name.data(), name.size());
        return 1;
    });
}

int set_bone_name(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id bone = check_id(s, 2, "bone");
        e.set_bone_name(bone, luaL_checkstring(s, 3));
        return 0;
    });
}

int get_bone_parent(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_opt_id(s, e.bone_parent(check_id(s, 2, "bone")));
        return 1;
    });
}

int set_bone_parent(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id bone = check_id(s, 2, "bone");
        e.set_bone_parent(bone, opt_id(s, 3, "parent"));
        return 0;
    });
}

int get_bone_transform(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        push_matrix4(s, e.bone_transform(check_id(s, 2, "bone")));
        return 1;
    });
}

int set_bone_transform(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id bone = check_id(s, 2, "bone");
        e.set_bone_transform(bone, check_matrix4(s, 3, "rest"));
        return 0;
    });
}

int set_vertex_bones(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const Id vertex = check_id(s, 2, "vertex");
        const std::vector<Id> bones = check_ids(s, 3, "bones");
        std::vector<float> weights;
        const bool weighted = !lua_isnoneornil(s, 4);
        if (weighted) {
            luaL_checktype(s, 4, LUA_TTABLE);
            const int count = lua_objlen(s, 4);
            for (int i = 1; i <= count; ++i) {
                lua_rawgeti(s, 4, i);
                int is_number = 0;
                const double weight = lua_tonumberx(s, -1, &is_number);
                if (!is_number) {
                    luaL_error(s, "weights must hold only numbers");
                }
                weights.push_back(static_cast<float>(weight));
                lua_pop(s, 1);
            }
        }
        e.set_vertex_bones(vertex, bones, weighted ? &weights : nullptr);
        return 0;
    });
}

int get_vertex_bones(lua_State* s) {
    return edit(s, [s](MeshEditor& e) {
        const std::vector<MeshEditor::Influence>& skin = e.vertex_bones(check_id(s, 2, "vertex"));
        lua_createtable(s, static_cast<int>(skin.size()), 0);
        for (std::size_t i = 0; i < skin.size(); ++i) {
            push_id(s, skin[i].first);
            lua_rawseti(s, -2, static_cast<int>(i + 1));
        }
        lua_createtable(s, static_cast<int>(skin.size()), 0);
        for (std::size_t i = 0; i < skin.size(); ++i) {
            lua_pushnumber(s, skin[i].second);
            lua_rawseti(s, -2, static_cast<int>(i + 1));
        }
        return 2;
    });
}

// The methods, with the return type completion and analysis show.
struct Method {
    const char* name;
    lua_CFunction call;
    const char* returns;
    bool list = false;
};

const Method kMethods[] = {
    {"AddVertex", add_vertex, "number"},
    {"RemoveVertex", remove_vertex, "nil"},
    {"GetVertices", get_vertices, "number", true},
    {"GetPosition", get_position, "Vector3"},
    {"SetPosition", set_position, "nil"},
    {"GetNormal", get_normal, "Vector3"},
    {"SetNormal", set_normal, "nil"},
    {"GetUV", get_uv, "Vector2"},
    {"SetUV", set_uv, "nil"},
    {"GetColor", get_color, "Color3"},
    {"SetColor", set_color, "nil"},
    {"AddTriangle", add_triangle, "number"},
    {"RemoveTriangle", remove_triangle, "nil"},
    {"GetTriangles", get_triangles, "number", true},
    {"GetTriangleVertices", get_triangle_vertices, "number"},
    {"AddBox", add_box_shape, "number", true},
    {"AddSphere", add_sphere_shape, "number", true},
    {"AddCylinder", add_cylinder_shape, "number", true},
    {"AddCone", add_cone_shape, "number", true},
    {"AddPlane", add_plane_shape, "number", true},
    {"AddTeapot", add_teapot_shape, "number", true},
    {"Clear", clear, "nil"},
    {"AddBone", add_bone, "number"},
    {"RemoveBone", remove_bone, "nil"},
    {"GetBones", get_bones, "number", true},
    {"FindBone", find_bone, "number?"},
    {"GetBoneName", get_bone_name, "string"},
    {"SetBoneName", set_bone_name, "nil"},
    {"GetBoneParent", get_bone_parent, "number?"},
    {"SetBoneParent", set_bone_parent, "nil"},
    {"GetBoneTransform", get_bone_transform, "Matrix4"},
    {"SetBoneTransform", set_bone_transform, "nil"},
    {"SetVertexBones", set_vertex_bones, "nil"},
    {"GetVertexBones", get_vertex_bones, "number", true},
};
constexpr int kMethodCount = static_cast<int>(sizeof(kMethods) / sizeof(kMethods[0]));

int editor_index(lua_State* state) {
    luaL_checkudata(state, 1, kEditorMeta);
    const char* key = luaL_checkstring(state, 2);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (!lua_isfunction(state, -1)) {
        luaL_error(state, "%s is not a valid member of MeshEditor", key);
    }
    return 1;
}

int editor_newindex(lua_State* state) {
    luaL_checkudata(state, 1, kEditorMeta);
    luaL_error(state, "%s cannot be assigned to on a MeshEditor", luaL_checkstring(state, 2));
}

int editor_tostring(lua_State* state) {
    lua_pushliteral(state, "MeshEditor");
    return 1;
}

int editor_eq(lua_State* state) {
    const auto* a = static_cast<EditorUd*>(test_userdata(state, 1, kEditorMeta));
    const auto* b = static_cast<EditorUd*>(test_userdata(state, 2, kEditorMeta));
    lua_pushboolean(state, a != nullptr && b != nullptr && a->editor == b->editor);
    return 1;
}

ANARCHY_LUA_REGISTER(register_mesh_editor_lua) {
    std::vector<LuaField> fields;
    fields.reserve(kMethodCount);
    for (const Method& method : kMethods) {
        fields.push_back(lua_method(method.name, method.returns, nullptr, false, false, method.list));
    }
    register_lua_class("MeshEditor", nullptr, fields.data(), kMethodCount);
}

}  // namespace

void push_mesh_editor(lua_State* state, std::shared_ptr<MeshEditor> editor) {
    void* data = lua_newuserdatadtor(state, sizeof(EditorUd), destroy_editor);
    new (data) EditorUd{std::move(editor)};
    luaL_getmetatable(state, kEditorMeta);
    lua_setmetatable(state, -2);
}

void open_mesh_editor(lua_State* state) {
    luaL_newmetatable(state, kEditorMeta);
    lua_createtable(state, 0, kMethodCount);
    for (const Method& method : kMethods) {
        lua_pushcfunction(state, method.call, method.name);
        lua_setfield(state, -2, method.name);
    }
    lua_setreadonly(state, -1, true);
    lua_pushcclosure(state, editor_index, "index", 1);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, editor_newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, editor_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, editor_eq, "eq");
    lua_setfield(state, -2, "__eq");
    lua_pushliteral(state, "MeshEditor");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

}  // namespace engine_core
