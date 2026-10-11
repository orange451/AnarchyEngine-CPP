// Brush's Lua methods and the BrushFace datatype. Every binding runs on
// SimulationThread, as every script does. Every face edit goes through
// Brush::apply, which builds first, so a refused call raises its reason and
// changes nothing. Face indices start at 1 here and at 0 in brush::.

#include "ScriptBindings.hpp"

#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "Matrix4.hpp"
#include "ScriptRuntime.hpp"
#include "Vector2.hpp"
#include "brush/BrushGeometry.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

using brush::DVec3;
using brush::Face;

constexpr const char* kBrushFaceMeta = "AE.BrushFace";

struct BrushFaceUd {
    Face face;
};

DVec3 to_d(const float* v) { return {v[0], v[1], v[2]}; }

void push_dvec(lua_State* state, DVec3 v) {
    lua_pushvector(state, static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}

DVec3 vector_arg(lua_State* state, int index, const char* name) {
    const float* value = lua_tovector(state, index);
    if (value == nullptr) {
        luaL_error(state, "%s must be a Vector3", name);
    }
    if (!std::isfinite(value[0]) || !std::isfinite(value[1]) || !std::isfinite(value[2])) {
        luaL_error(state, "%s must be finite", name);
    }
    return to_d(value);
}

double finite_arg(lua_State* state, int index, const char* name) {
    const double value = luaL_checknumber(state, index);
    if (!std::isfinite(value)) {
        luaL_error(state, "%s must be a finite number", name);
    }
    return value;
}

void push_face(lua_State* state, Face face) {
    void* memory = lua_newuserdatadtor(state, sizeof(BrushFaceUd), [](lua_State*, void* p) { static_cast<BrushFaceUd*>(p)->~BrushFaceUd(); });
    new (memory) BrushFaceUd{std::move(face)};
    luaL_getmetatable(state, kBrushFaceMeta);
    lua_setmetatable(state, -2);
}

const Face* to_face(lua_State* state, int index) {
    auto* ud = static_cast<BrushFaceUd*>(test_userdata(state, index, kBrushFaceMeta));
    return ud != nullptr ? &ud->face : nullptr;
}

const Face& check_face(lua_State* state, int index) {
    const Face* face = to_face(state, index);
    if (face == nullptr) {
        luaL_typeerrorL(state, index, "BrushFace");
    }
    return *face;
}

void raise_if(lua_State* state, const std::optional<std::string>& error) {
    if (error) {
        luaL_error(state, "%s", error->c_str());
    }
}

void raise_unless_ok(lua_State* state, const brush::Built& built) {
    if (!built.ok()) {
        luaL_error(state, "%s", built.error.c_str());
    }
}

// --- BrushFace ---------------------------------------------------------------

int face_new(lua_State* state) {
    const DVec3 p1 = vector_arg(state, 1, "p1");
    const DVec3 p2 = vector_arg(state, 2, "p2");
    const DVec3 p3 = vector_arg(state, 3, "p3");
    if (!brush::plane_of(p1, p2, p3)) {
        luaL_error(state, "the points lie on a line");
    }
    push_face(state, brush::face_through(p1, p2, p3));
    return 1;
}

int face_from_plane(lua_State* state) {
    const DVec3 normal = vector_arg(state, 1, "normal");
    const DVec3 point = vector_arg(state, 2, "point");
    if (brush::length(normal) <= 1e-12) {
        luaL_error(state, "normal must not be zero");
    }
    push_face(state, brush::face_from_plane(normal, point));
    return 1;
}

int face_index(lua_State* state) {
    const Face& face = check_face(state, 1);
    const char* key = luaL_checkstring(state, 2);
    if (std::strcmp(key, "P1") == 0) {
        push_dvec(state, face.p1);
    } else if (std::strcmp(key, "P2") == 0) {
        push_dvec(state, face.p2);
    } else if (std::strcmp(key, "P3") == 0) {
        push_dvec(state, face.p3);
    } else if (std::strcmp(key, "Material") == 0) {
        ScriptBindings::push_material_guid(state, face.material);
    } else if (std::strcmp(key, "UAxis") == 0) {
        push_dvec(state, face.u_axis);
    } else if (std::strcmp(key, "VAxis") == 0) {
        push_dvec(state, face.v_axis);
    } else if (std::strcmp(key, "Offset") == 0) {
        push_vector2(state, Vec2{static_cast<float>(face.offset_u), static_cast<float>(face.offset_v)});
    } else if (std::strcmp(key, "Scale") == 0) {
        push_vector2(state, Vec2{static_cast<float>(face.scale_u), static_cast<float>(face.scale_v)});
    } else if (std::strcmp(key, "Rotation") == 0) {
        lua_pushnumber(state, face.rotation);
    } else if (std::strcmp(key, "Normal") == 0) {
        const std::optional<brush::Plane> plane = brush::plane_of(face);
        push_dvec(state, plane ? plane->normal : DVec3{});
    } else {
        lua_pushvalue(state, lua_upvalueindex(1));
        lua_pushvalue(state, 2);
        lua_rawget(state, -2);
        if (lua_isfunction(state, -1)) {
            return 1;
        }
        luaL_error(state, "%s is not a valid member of BrushFace", key);
    }
    return 1;
}

int face_newindex(lua_State* state) {
    luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2));
}

int face_tostring(lua_State* state) {
    const Face& face = check_face(state, 1);
    const std::optional<brush::Plane> plane = brush::plane_of(face);
    const DVec3 n = plane ? plane->normal : DVec3{};
    lua_pushfstring(state, "BrushFace(normal %g, %g, %g; distance %g)", n.x, n.y, n.z, plane ? plane->distance : 0.0);
    return 1;
}

int face_eq(lua_State* state) {
    const Face* a = to_face(state, 1);
    const Face* b = to_face(state, 2);
    lua_pushboolean(state, a != nullptr && b != nullptr && *a == *b ? 1 : 0);
    return 1;
}

Vec2 vector2_field(lua_State* state, int index, const char* name) {
    const Vec2* value = to_vector2(state, index);
    if (value == nullptr || !std::isfinite(value->x) || !std::isfinite(value->y)) {
        luaL_error(state, "%s must be a finite Vector2", name);
    }
    return *value;
}

// face:With({Field = value, ...})
int face_with(lua_State* state) {
    Face face = check_face(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_pushnil(state);
    while (lua_next(state, 2) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            luaL_error(state, "BrushFace:With takes field names");
        }
        const char* key = lua_tostring(state, -2);
        if (std::strcmp(key, "P1") == 0) {
            face.p1 = vector_arg(state, -1, "P1");
        } else if (std::strcmp(key, "P2") == 0) {
            face.p2 = vector_arg(state, -1, "P2");
        } else if (std::strcmp(key, "P3") == 0) {
            face.p3 = vector_arg(state, -1, "P3");
        } else if (std::strcmp(key, "Material") == 0) {
            face.material = ScriptBindings::brush_material_arg(state, lua_gettop(state));
        } else if (std::strcmp(key, "UAxis") == 0) {
            face.u_axis = brush::normalize(vector_arg(state, -1, "UAxis"));
        } else if (std::strcmp(key, "VAxis") == 0) {
            face.v_axis = brush::normalize(vector_arg(state, -1, "VAxis"));
        } else if (std::strcmp(key, "Offset") == 0) {
            const Vec2 v = vector2_field(state, lua_gettop(state), "Offset");
            face.offset_u = v.x;
            face.offset_v = v.y;
        } else if (std::strcmp(key, "Scale") == 0) {
            const Vec2 v = vector2_field(state, lua_gettop(state), "Scale");
            if (v.x == 0.f || v.y == 0.f) {
                luaL_error(state, "Scale must not be zero");
            }
            face.scale_u = v.x;
            face.scale_v = v.y;
        } else if (std::strcmp(key, "Rotation") == 0) {
            face.rotation = finite_arg(state, lua_gettop(state), "Rotation");
        } else if (std::strcmp(key, "Normal") == 0) {
            luaL_error(state, "Normal cannot be set; it comes from the points");
        } else {
            luaL_error(state, "%s is not a valid member of BrushFace", key);
        }
        lua_pop(state, 1);
    }
    if (!brush::plane_of(face)) {
        luaL_error(state, "the points lie on a line");
    }
    push_face(state, std::move(face));
    return 1;
}

const luaL_Reg kFaceMethods[] = {{"With", face_with}, {nullptr, nullptr}};

// --- Brush methods ------------------------------------------------------------

std::size_t face_index_arg(lua_State* state, int index, const Brush& brush) {
    const double value = luaL_checknumber(state, index);
    const double count = static_cast<double>(brush.faces().size());
    if (!(value >= 1.0 && value <= count) || value != std::floor(value)) {
        luaL_error(state, "face index %g is out of range (1 to %d)", value, static_cast<int>(count));
    }
    return static_cast<std::size_t>(value) - 1;
}

void push_faces(lua_State* state, const std::vector<Face>& faces) {
    lua_createtable(state, static_cast<int>(faces.size()), 0);
    for (std::size_t i = 0; i < faces.size(); ++i) {
        push_face(state, faces[i]);
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

std::vector<Face> faces_arg(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    const int count = lua_objlen(state, index);
    std::vector<Face> faces;
    faces.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        const Face* face = to_face(state, -1);
        if (face == nullptr) {
            luaL_error(state, "faces[%d] must be a BrushFace", i);
        }
        faces.push_back(*face);
        lua_pop(state, 1);
    }
    return faces;
}

void push_points(lua_State* state, const brush::Shape& shape, const std::vector<std::uint32_t>& indices) {
    lua_createtable(state, static_cast<int>(indices.size()), 0);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        push_dvec(state, shape.vertices[indices[i]]);
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

int brush_get_faces(lua_State* state) {
    return lua_guard(state, [&] {
        push_faces(state, ScriptBindings::brush_self(state).faces());
        return 1;
    });
}

int brush_set_faces(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        raise_if(state, brush.set_faces(faces_arg(state, 2)));
        return 0;
    });
}

int brush_get_face(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        push_face(state, brush.faces()[face_index_arg(state, 2, brush)]);
        return 1;
    });
}

int brush_set_face(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const std::size_t index = face_index_arg(state, 2, brush);
        std::vector<Face> faces = brush.faces();
        faces[index] = check_face(state, 3);
        raise_if(state, brush.set_faces(std::move(faces)));
        return 0;
    });
}

int brush_set_face_material(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const std::size_t index = face_index_arg(state, 2, brush);
        std::vector<Face> faces = brush.faces();
        faces[index].material = ScriptBindings::brush_material_arg(state, 3);
        raise_if(state, brush.set_faces(std::move(faces)));
        return 0;
    });
}

int brush_set_material(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const std::string material = ScriptBindings::brush_material_arg(state, 2);
        std::vector<Face> faces = brush.faces();
        for (Face& face : faces) {
            face.material = material;
        }
        raise_if(state, brush.set_faces(std::move(faces)));
        return 0;
    });
}

int brush_get_face_vertices(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const std::size_t index = face_index_arg(state, 2, brush);
        push_points(state, brush.shape(), brush.shape().polygons[index].vertices);
        return 1;
    });
}

int brush_get_vertices(lua_State* state) {
    return lua_guard(state, [&] {
        const brush::Shape& shape = ScriptBindings::brush_self(state).shape();
        lua_createtable(state, static_cast<int>(shape.vertices.size()), 0);
        for (std::size_t i = 0; i < shape.vertices.size(); ++i) {
            push_dvec(state, shape.vertices[i]);
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        return 1;
    });
}

int brush_get_bounds(lua_State* state) {
    return lua_guard(state, [&] {
        const brush::Shape& shape = ScriptBindings::brush_self(state).shape();
        push_dvec(state, shape.min);
        push_dvec(state, shape.max);
        return 2;
    });
}

int brush_contains_point(lua_State* state) {
    return lua_guard(state, [&] {
        const brush::Shape& shape = ScriptBindings::brush_self(state).shape();
        lua_pushboolean(state, brush::contains_point(shape, vector_arg(state, 2, "point")) ? 1 : 0);
        return 1;
    });
}

int brush_clip(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        brush::Built built = brush::clip(brush.faces(), check_face(state, 2));
        raise_unless_ok(state, built);
        raise_if(state, brush.apply(std::move(built)));
        return 0;
    });
}

int brush_move_face(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const std::size_t index = face_index_arg(state, 2, brush);
        brush::Built built = brush::move_face(brush.faces(), index, finite_arg(state, 3, "distance"));
        raise_unless_ok(state, built);
        raise_if(state, brush.apply(std::move(built)));
        return 0;
    });
}

int brush_expand(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        brush::Built built = brush::expand(brush.faces(), finite_arg(state, 2, "distance"));
        raise_unless_ok(state, built);
        raise_if(state, brush.apply(std::move(built)));
        return 0;
    });
}

int make_shape(lua_State* state, std::vector<Face> faces);

void push_index_list(lua_State* state, const std::vector<std::uint32_t>& indices) {
    lua_createtable(state, static_cast<int>(indices.size()), 0);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        lua_pushnumber(state, static_cast<double>(indices[i]) + 1.0);
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

std::vector<DVec3> points_arg(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    const int count = lua_objlen(state, index);
    std::vector<DVec3> points;
    points.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        if (lua_tovector(state, -1) == nullptr) {
            luaL_error(state, "points[%d] must be a Vector3", i);
        }
        points.push_back(vector_arg(state, -1, "point"));
        lua_pop(state, 1);
    }
    return points;
}

brush::Plane plane_args(lua_State* state, int normal_index, int point_index) {
    const DVec3 normal = brush::normalize(vector_arg(state, normal_index, "normal"));
    if (brush::length(normal) < 0.5) {
        luaL_error(state, "normal must not be zero");
    }
    return {normal, brush::dot(normal, vector_arg(state, point_index, "point"))};
}

int brush_get_face_vertex_indices(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        push_index_list(state, brush.shape().polygons[face_index_arg(state, 2, brush)].vertices);
        return 1;
    });
}

int brush_get_edges(lua_State* state) {
    return lua_guard(state, [&] {
        const brush::Shape& shape = ScriptBindings::brush_self(state).shape();
        lua_createtable(state, static_cast<int>(shape.edges.size()), 0);
        for (std::size_t i = 0; i < shape.edges.size(); ++i) {
            push_index_list(state, {shape.edges[i].first, shape.edges[i].second});
            lua_rawseti(state, -2, static_cast<int>(i + 1));
        }
        return 1;
    });
}

int brush_get_face_at(lua_State* state) {
    return lua_guard(state, [&] {
        const brush::Shape& shape = ScriptBindings::brush_self(state).shape();
        const auto face = brush::face_at(shape, vector_arg(state, 2, "point"), vector_arg(state, 3, "normal"));
        if (face) {
            lua_pushnumber(state, static_cast<double>(*face) + 1.0);
        } else {
            lua_pushnil(state);
        }
        return 1;
    });
}

int brush_split(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        auto halves = brush::split(brush.faces(), plane_args(state, 2, 3));
        if (!halves) {
            luaL_error(state, "the plane does not cut the brush");
        }
        DVec3 moved;
        raise_if(state, brush.apply(std::move(halves->first), &moved));
        // The front half in this brush's local space, which recentring moved.
        brush::shift(halves->second, moved);
        push_faces(state, halves->second.faces);
        return 1;
    });
}

int brush_get_loop_cut(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const std::size_t face = face_index_arg(state, 2, brush);
        const DVec3 point = vector_arg(state, 3, "point");
        const double grid = luaL_optnumber(state, 4, 0.0);
        if (!std::isfinite(grid) || grid < 0.0) {
            luaL_error(state, "grid must be zero or more");
        }
        const bool middle = lua_toboolean(state, 5) != 0;
        const auto cut = brush::loop_cut(brush.shape(), face, point, grid, middle);
        if (!cut) {
            lua_pushnil(state);
            return 1;
        }
        push_dvec(state, cut->plane.normal);
        // Where the cut crosses the edge.
        push_dvec(state, cut->from + cut->plane.normal * (cut->plane.distance - brush::dot(cut->plane.normal, cut->from)));
        push_dvec(state, cut->from);
        push_dvec(state, cut->to);
        return 4;
    });
}

int brush_move_vertex(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        brush::Built built =
            brush::move_vertex(brush.faces(), vector_arg(state, 2, "from"), vector_arg(state, 3, "to"));
        raise_unless_ok(state, built);
        raise_if(state, brush.apply(std::move(built)));
        return 0;
    });
}

int brush_transform_shape(lua_State* state) {
    return lua_guard(state, [&] {
        Brush& brush = ScriptBindings::brush_self(state);
        const Matrix4* value = to_matrix4(state, 2);
        if (value == nullptr) {
            luaL_typeerrorL(state, 2, "Matrix4");
        }
        // Column-major in, row-major 3x4 out.
        const float* m = value->m;
        double rows[12];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                rows[r * 4 + c] = m[c * 4 + r];
            }
        }
        for (double v : rows) {
            if (!std::isfinite(v)) {
                luaL_error(state, "transform must be finite");
            }
        }
        brush::Built built = brush::transform(brush.faces(), rows);
        raise_unless_ok(state, built);
        raise_if(state, brush.apply(std::move(built)));
        return 0;
    });
}

int brush_make_hull(lua_State* state) {
    return lua_guard(state, [&] {
        const auto faces = brush::hull_faces(points_arg(state, 2));
        if (!faces) {
            luaL_error(state, "the points span no volume");
        }
        return make_shape(state, *faces);
    });
}

DVec3 size_arg(lua_State* state, int index) {
    const DVec3 size = vector_arg(state, index, "size");
    if (!(size.x > 0.0 && size.y > 0.0 && size.z > 0.0)) {
        luaL_error(state, "size must be positive on every axis");
    }
    return size;
}

int sides_arg(lua_State* state, int index) {
    const double sides = luaL_checknumber(state, index);
    if (!(sides >= 3.0 && sides <= 256.0) || sides != std::floor(sides)) {
        luaL_error(state, "sides must be a whole number from 3 to 256");
    }
    return static_cast<int>(sides);
}

int make_shape(lua_State* state, std::vector<Face> faces) {
    Brush& brush = ScriptBindings::brush_self(state);
    raise_if(state, brush.set_faces(std::move(faces)));
    return 0;
}

int brush_make_box(lua_State* state) {
    return lua_guard(state, [&] { return make_shape(state, brush::make_box(size_arg(state, 2))); });
}

int brush_make_cylinder(lua_State* state) {
    return lua_guard(state,
                     [&] { return make_shape(state, brush::make_cylinder(size_arg(state, 2), sides_arg(state, 3))); });
}

int brush_make_cone(lua_State* state) {
    return lua_guard(state,
                     [&] { return make_shape(state, brush::make_cone(size_arg(state, 2), sides_arg(state, 3))); });
}

int brush_make_sphere(lua_State* state) {
    return lua_guard(state, [&] {
        const double detail = luaL_optnumber(state, 3, 1.0);
        if (!(detail >= 0.0 && detail <= 3.0) || detail != std::floor(detail)) {
            luaL_error(state, "detail must be a whole number from 0 to 3");
        }
        return make_shape(state, brush::make_sphere(size_arg(state, 2), static_cast<int>(detail)));
    });
}

ANARCHY_LUA_REGISTER(register_brush_methods) {
    const LuaField methods[] = {
        lua_method("GetFaces", "BrushFace", reinterpret_cast<void*>(&brush_get_faces), false, false, true),
        lua_method("SetFaces", "nil", reinterpret_cast<void*>(&brush_set_faces)),
        lua_method("GetFace", "BrushFace", reinterpret_cast<void*>(&brush_get_face)),
        lua_method("SetFace", "nil", reinterpret_cast<void*>(&brush_set_face)),
        lua_method("SetFaceMaterial", "nil", reinterpret_cast<void*>(&brush_set_face_material)),
        lua_method("SetMaterial", "nil", reinterpret_cast<void*>(&brush_set_material)),
        lua_method("GetFaceVertices", "", reinterpret_cast<void*>(&brush_get_face_vertices)),
        lua_method("GetVertices", "", reinterpret_cast<void*>(&brush_get_vertices)),
        lua_method("GetBounds", nullptr, reinterpret_cast<void*>(&brush_get_bounds)),
        lua_method("ContainsPoint", "boolean", reinterpret_cast<void*>(&brush_contains_point)),
        lua_method("Clip", "nil", reinterpret_cast<void*>(&brush_clip)),
        lua_method("MoveFace", "nil", reinterpret_cast<void*>(&brush_move_face)),
        lua_method("Expand", "nil", reinterpret_cast<void*>(&brush_expand)),
        lua_method("MakeBox", "nil", reinterpret_cast<void*>(&brush_make_box)),
        lua_method("MakeCylinder", "nil", reinterpret_cast<void*>(&brush_make_cylinder)),
        lua_method("MakeCone", "nil", reinterpret_cast<void*>(&brush_make_cone)),
        lua_method("MakeSphere", "nil", reinterpret_cast<void*>(&brush_make_sphere)),
        lua_method("MakeHull", "nil", reinterpret_cast<void*>(&brush_make_hull)),
        lua_method("GetEdges", "", reinterpret_cast<void*>(&brush_get_edges)),
        lua_method("GetFaceVertexIndices", "", reinterpret_cast<void*>(&brush_get_face_vertex_indices)),
        lua_method("GetFaceAt", "number", reinterpret_cast<void*>(&brush_get_face_at)),
        lua_method("Split", "BrushFace", reinterpret_cast<void*>(&brush_split), false, false, true),
        lua_method("GetLoopCut", nullptr, reinterpret_cast<void*>(&brush_get_loop_cut)),
        lua_method("MoveVertex", "nil", reinterpret_cast<void*>(&brush_move_vertex)),
        lua_method("TransformShape", "nil", reinterpret_cast<void*>(&brush_transform_shape)),
    };
    register_lua_class("Brush", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));

    const LuaField fields[] = {
        lua_property("P1", "Vector3", false, nullptr, nullptr),
        lua_property("P2", "Vector3", false, nullptr, nullptr),
        lua_property("P3", "Vector3", false, nullptr, nullptr),
        lua_property("Material", "Material?", false, nullptr, nullptr),
        lua_property("UAxis", "Vector3", false, nullptr, nullptr),
        lua_property("VAxis", "Vector3", false, nullptr, nullptr),
        lua_property("Offset", "Vector2", false, nullptr, nullptr),
        lua_property("Scale", "Vector2", false, nullptr, nullptr),
        lua_property("Rotation", "number", false, nullptr, nullptr),
        lua_property("Normal", "Vector3", false, nullptr, nullptr),
        lua_method("With", "BrushFace", nullptr),
    };
    register_lua_class("BrushFace", nullptr, fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
    lua_note_result("BrushFace", "new", "BrushFace", false);
    lua_note_result("BrushFace", "fromPlane", "BrushFace", false);
    const LuaOperator eq{"__eq", "BrushFace", "BrushFace", "boolean"};
    register_lua_operators("BrushFace", &eq, 1);
}

}  // namespace

Brush& ScriptBindings::brush_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* brush = runtime == nullptr ? nullptr : dynamic_cast<Brush*>(runtime->resolve_id(ud->id, ud->world));
    if (brush == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *brush;
}

void ScriptBindings::link_brush_methods() {}

std::string ScriptBindings::brush_material_arg(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) {
        return std::string();
    }
    auto* ud = static_cast<InstanceUd*>(test_userdata(state, index, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    DataModel* object = ud != nullptr && runtime != nullptr ? runtime->resolve_id(ud->id, ud->world) : nullptr;
    if (object == nullptr || dynamic_cast<Material*>(object) == nullptr || runtime->game_ == nullptr) {
        luaL_error(state, "material must be a Material or nil");
    }
    return runtime->game_->guid(object->id());
}

void ScriptBindings::push_material_guid(lua_State* state, const std::string& guid) {
    ScriptRuntime* runtime = runtime_from(state);
    if (guid.empty() || runtime == nullptr || runtime->game_ == nullptr) {
        lua_pushnil(state);
        return;
    }
    const std::optional<InstanceId> id = runtime->game_->find_guid(guid);
    if (!id || dynamic_cast<Material*>(runtime->game_->instance(*id)) == nullptr) {
        lua_pushnil(state);
        return;
    }
    runtime->push_instance(state, *id);
}

void open_brush_face(lua_State* state) {
    luaL_newmetatable(state, kBrushFaceMeta);
    lua_createtable(state, 0, 1);
    for (const luaL_Reg* entry = kFaceMethods; entry->name != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    lua_setreadonly(state, -1, true);
    lua_pushcclosure(state, face_index, "index", 1);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, face_newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, face_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, face_eq, "eq");
    lua_setfield(state, -2, "__eq");
    lua_pushliteral(state, "BrushFace");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);

    lua_createtable(state, 0, 2);
    lua_pushcfunction(state, face_new, "new");
    lua_setfield(state, -2, "new");
    lua_pushcfunction(state, face_from_plane, "fromPlane");
    lua_setfield(state, -2, "fromPlane");
    lua_setreadonly(state, -1, true);
    lua_setglobal(state, "BrushFace");
}

}  // namespace engine_core
