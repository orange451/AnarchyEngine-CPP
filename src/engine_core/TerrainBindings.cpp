// Terrain's Lua methods: shape edits, materials, and raw voxel reads. Every
// binding runs on SimulationThread, as every script does; every voxel edit
// goes through Terrain::edit_volume, never volume() directly, so the place is
// marked unsaved and the Terrain gets its DataPath.

#include "ScriptBindings.hpp"

#include "AssetInstances.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/ShapeDistance.hpp"
#include "terrain/VoxelVolume.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

using terrain::CellCoord;
using terrain::Shape;
using terrain::VoxelVolume;

// Cell coordinates past this are clamped: any box reaching them is far over
// the per-call limit, and the clamp keeps the int conversion defined.
constexpr double kMaxCellCoord = 1.0e9;

// Argument index as this Terrain's material Id: 0 for nil.
std::uint8_t material_arg(lua_State* state, int index, const Terrain& terrain) {
    if (lua_isnoneornil(state, index)) {
        return 0;
    }
    const auto* entry = dynamic_cast<const TerrainMaterial*>(ScriptBindings::terrain_instance_arg(state, index));
    if (entry == nullptr) {
        luaL_error(state, "Pass a TerrainMaterial (see Terrain:GetMaterials)");
    }
    if (terrain.parent(entry->id()) != terrain.id()) {
        luaL_error(state, "TerrainMaterial belongs to another Terrain");
    }
    return static_cast<std::uint8_t>(entry->material_id());
}

// The optional Enum.TransformSpace at index; World when none.
bool local_space(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) {
        return false;
    }
    return check_enum_arg(state, index, transform_space_enum()) == static_cast<int>(TransformSpace::Local);
}

// A world (or local) position or frame into the Terrain's space.
Vec3 to_local(const Terrain& terrain, Vec3 p, bool local) {
    return local ? p : matrix4_point(matrix4_inverse(terrain.transform()), p);
}

Matrix4 to_local(const Terrain& terrain, const Matrix4& frame, bool local) {
    return local ? frame : matrix4_multiply(matrix4_inverse(terrain.transform()), frame);
}

// Raises with the volume's refusal, if any.
void raise_if(lua_State* state, const std::optional<std::string>& error) {
    if (error) {
        luaL_error(state, "%s", error->c_str());
    }
}

Vec3 vector_arg(lua_State* state, int index, const char* name) {
    const float* value = lua_tovector(state, index);
    if (value == nullptr) {
        luaL_error(state, "%s must be a Vector3", name);
    }
    return Vec3{value[0], value[1], value[2]};
}

Matrix4 matrix_arg(lua_State* state, int index) {
    const Matrix4* value = to_matrix4(state, index);
    if (value == nullptr) {
        luaL_error(state, "transform must be a Matrix4");
    }
    return *value;
}

float number_arg(lua_State* state, int index) { return static_cast<float>(luaL_checknumber(state, index)); }

// One coordinate as a whole cell: rounded, then clamped to kMaxCellCoord.
int cell_coord(lua_State* state, double value) {
    if (!std::isfinite(value)) {
        luaL_error(state, "cell coordinates must be finite");
    }
    return static_cast<int>(std::clamp(std::round(value), -kMaxCellCoord, kMaxCellCoord));
}

// A Terrain-local position as the cell nearest it.
CellCoord cell_of(lua_State* state, const Terrain& terrain, Vec3 local) {
    const double size = terrain.voxel_size();
    return CellCoord{cell_coord(state, local.x / size), cell_coord(state, local.y / size),
                     cell_coord(state, local.z / size)};
}

Shape ball(const Terrain& terrain, Vec3 center, float radius, bool local) {
    Shape shape;
    shape.kind = Shape::Kind::Ball;
    shape.center = to_local(terrain, center, local);
    shape.radius = radius;
    return shape;
}

// A Block or Wedge.
Shape boxed(Shape::Kind kind, const Terrain& terrain, const Matrix4& frame, Vec3 size, bool local) {
    Shape shape;
    shape.kind = kind;
    shape.frame = to_local(terrain, frame, local);
    shape.size = size;
    return shape;
}

Shape cylinder(const Terrain& terrain, const Matrix4& frame, float height, float radius, bool local) {
    Shape shape;
    shape.kind = Shape::Kind::Cylinder;
    shape.frame = to_local(terrain, frame, local);
    shape.size = Vec3{2.f * radius, height, 2.f * radius};
    return shape;
}

enum class Op { Fill, Subtract, Paint };

// Applies shape to terrain's voxels, raising on a refusal.
void apply(lua_State* state, Terrain& terrain, Op op, const Shape& shape, std::uint8_t id) {
    raise_if(state, terrain.edit_volume([&](VoxelVolume& volume) {
        if (op == Op::Fill) {
            return volume.fill(shape, id);
        }
        return op == Op::Subtract ? volume.subtract(shape) : volume.paint(shape, id);
    }));
}

// Pushes values (x fastest, nx by ny by nz) as t[i][j][k].
template <typename T>
void push_grid(lua_State* state, const std::vector<T>& values, int nx, int ny, int nz) {
    lua_createtable(state, nx, 0);
    for (int i = 0; i < nx; ++i) {
        lua_createtable(state, ny, 0);
        for (int j = 0; j < ny; ++j) {
            lua_createtable(state, nz, 0);
            for (int k = 0; k < nz; ++k) {
                const std::size_t at = (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                                        static_cast<std::size_t>(j)) *
                                           static_cast<std::size_t>(nx) +
                                       static_cast<std::size_t>(i);
                lua_pushnumber(state, static_cast<double>(values[at]));
                lua_rawseti(state, -2, k + 1);
            }
            lua_rawseti(state, -2, j + 1);
        }
        lua_rawseti(state, -2, i + 1);
    }
}

}  // namespace

void ScriptBindings::link_terrain_methods() {}

Terrain& ScriptBindings::terrain_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* terrain = runtime == nullptr ? nullptr : dynamic_cast<Terrain*>(runtime->resolve_id(ud->id, ud->world));
    if (terrain == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *terrain;
}

DataModel* ScriptBindings::terrain_instance_arg(lua_State* state, int index) {
    const auto* ud = static_cast<InstanceUd*>(test_userdata(state, index, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    return ud == nullptr || runtime == nullptr ? nullptr : runtime->resolve_id(ud->id, ud->world);
}

// FillBall(center, radius, material, space?)
int ScriptBindings::terrain_fill_ball(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Vec3 center = vector_arg(state, 2, "center");
        const float radius = number_arg(state, 3);
        const std::uint8_t id = material_arg(state, 4, terrain);
        apply(state, terrain, Op::Fill, ball(terrain, center, radius, local_space(state, 5)), id);
        return 0;
    });
}

// FillBlock(transform, size, material, space?)
int ScriptBindings::terrain_fill_block(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const Vec3 size = vector_arg(state, 3, "size");
        const std::uint8_t id = material_arg(state, 4, terrain);
        apply(state, terrain, Op::Fill, boxed(Shape::Kind::Block, terrain, frame, size, local_space(state, 5)), id);
        return 0;
    });
}

// FillCylinder(transform, height, radius, material, space?)
int ScriptBindings::terrain_fill_cylinder(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const float height = number_arg(state, 3);
        const float radius = number_arg(state, 4);
        const std::uint8_t id = material_arg(state, 5, terrain);
        apply(state, terrain, Op::Fill, cylinder(terrain, frame, height, radius, local_space(state, 6)), id);
        return 0;
    });
}

// FillWedge(transform, size, material, space?)
int ScriptBindings::terrain_fill_wedge(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const Vec3 size = vector_arg(state, 3, "size");
        const std::uint8_t id = material_arg(state, 4, terrain);
        apply(state, terrain, Op::Fill, boxed(Shape::Kind::Wedge, terrain, frame, size, local_space(state, 5)), id);
        return 0;
    });
}

// SubtractBall(center, radius, space?)
int ScriptBindings::terrain_subtract_ball(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Vec3 center = vector_arg(state, 2, "center");
        const float radius = number_arg(state, 3);
        apply(state, terrain, Op::Subtract, ball(terrain, center, radius, local_space(state, 4)), 0);
        return 0;
    });
}

// SubtractBlock(transform, size, space?)
int ScriptBindings::terrain_subtract_block(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const Vec3 size = vector_arg(state, 3, "size");
        apply(state, terrain, Op::Subtract, boxed(Shape::Kind::Block, terrain, frame, size, local_space(state, 4)), 0);
        return 0;
    });
}

// SubtractCylinder(transform, height, radius, space?)
int ScriptBindings::terrain_subtract_cylinder(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const float height = number_arg(state, 3);
        const float radius = number_arg(state, 4);
        apply(state, terrain, Op::Subtract, cylinder(terrain, frame, height, radius, local_space(state, 5)), 0);
        return 0;
    });
}

// SubtractWedge(transform, size, space?)
int ScriptBindings::terrain_subtract_wedge(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const Vec3 size = vector_arg(state, 3, "size");
        apply(state, terrain, Op::Subtract, boxed(Shape::Kind::Wedge, terrain, frame, size, local_space(state, 4)), 0);
        return 0;
    });
}

// PaintBall(center, radius, material, space?)
int ScriptBindings::terrain_paint_ball(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Vec3 center = vector_arg(state, 2, "center");
        const float radius = number_arg(state, 3);
        const std::uint8_t id = material_arg(state, 4, terrain);
        apply(state, terrain, Op::Paint, ball(terrain, center, radius, local_space(state, 5)), id);
        return 0;
    });
}

// PaintBlock(transform, size, material, space?)
int ScriptBindings::terrain_paint_block(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Matrix4 frame = matrix_arg(state, 2);
        const Vec3 size = vector_arg(state, 3, "size");
        const std::uint8_t id = material_arg(state, 4, terrain);
        apply(state, terrain, Op::Paint, boxed(Shape::Kind::Block, terrain, frame, size, local_space(state, 5)), id);
        return 0;
    });
}

// ReplaceMaterial(min, max, from, to, space?): both corners become cells, and
// the box between them is their component-wise min and max.
int ScriptBindings::terrain_replace_material(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        const Vec3 min = vector_arg(state, 2, "min");
        const Vec3 max = vector_arg(state, 3, "max");
        const std::uint8_t from = material_arg(state, 4, terrain);
        const std::uint8_t to = material_arg(state, 5, terrain);
        const bool local = local_space(state, 6);
        const CellCoord a = cell_of(state, terrain, to_local(terrain, min, local));
        const CellCoord b = cell_of(state, terrain, to_local(terrain, max, local));
        const CellCoord low{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
        const CellCoord high{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
        raise_if(state, terrain.edit_volume([&](VoxelVolume& volume) { return volume.replace(low, high, from, to); }));
        return 0;
    });
}

// AddMaterial(material?) -> TerrainMaterial
int ScriptBindings::terrain_add_material(lua_State* state) {
    return lua_guard(state, [&] {
        Terrain& terrain = terrain_self(state);
        InstanceId material = 0;
        if (!lua_isnoneornil(state, 2)) {
            const DataModel* value = terrain_instance_arg(state, 2);
            if (value == nullptr || !lua_class_inherits(value->class_name(), "Material")) {
                luaL_error(state, "material must be a Material");
            }
            material = value->id();
        }
        TerrainMaterial* entry = nullptr;
        raise_if(state, terrain.add_material(material, entry));
        runtime_from(state)->push_instance(state, entry->id());
        return 1;
    });
}

// ReadVoxels(min, max) -> {Distances = t, Materials = m}, t[i][j][k] being
// cell (min.x + i - 1, min.y + j - 1, min.z + k - 1). Corners are rounded to
// whole cells and ordered.
int ScriptBindings::terrain_read_voxels(lua_State* state) {
    return lua_guard(state, [&] {
        const Terrain& terrain = terrain_self(state);
        const Vec3 a = vector_arg(state, 2, "min");
        const Vec3 b = vector_arg(state, 3, "max");
        const CellCoord ca{cell_coord(state, a.x), cell_coord(state, a.y), cell_coord(state, a.z)};
        const CellCoord cb{cell_coord(state, b.x), cell_coord(state, b.y), cell_coord(state, b.z)};
        const CellCoord low{std::min(ca.x, cb.x), std::min(ca.y, cb.y), std::min(ca.z, cb.z)};
        const CellCoord high{std::max(ca.x, cb.x), std::max(ca.y, cb.y), std::max(ca.z, cb.z)};
        std::vector<float> distances;
        std::vector<std::uint8_t> materials;
        raise_if(state, terrain.volume().read(low, high, distances, materials));
        const int nx = high.x - low.x + 1;
        const int ny = high.y - low.y + 1;
        const int nz = high.z - low.z + 1;
        lua_createtable(state, 0, 2);
        push_grid(state, distances, nx, ny, nz);
        lua_setfield(state, -2, "Distances");
        push_grid(state, materials, nx, ny, nz);
        lua_setfield(state, -2, "Materials");
        return 1;
    });
}

// WorldToCell(position) -> Vector3: the nearest cell, in whole numbers.
int ScriptBindings::terrain_world_to_cell(lua_State* state) {
    return lua_guard(state, [&] {
        const Terrain& terrain = terrain_self(state);
        const Vec3 local = to_local(terrain, vector_arg(state, 2, "position"), false);
        const float size = static_cast<float>(terrain.voxel_size());
        lua_pushvector(state, std::round(local.x / size), std::round(local.y / size), std::round(local.z / size));
        return 1;
    });
}

ANARCHY_LUA_REGISTER(register_terrain_methods) {
    // Terrain.cpp declares the class and its properties.
    const LuaField methods[] = {
        lua_method("FillBall", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_fill_ball)),
        lua_method("FillBlock", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_fill_block)),
        lua_method("FillCylinder", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_fill_cylinder)),
        lua_method("FillWedge", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_fill_wedge)),
        lua_method("SubtractBall", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_subtract_ball)),
        lua_method("SubtractBlock", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_subtract_block)),
        lua_method("SubtractCylinder", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_subtract_cylinder)),
        lua_method("SubtractWedge", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_subtract_wedge)),
        lua_method("PaintBall", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_paint_ball)),
        lua_method("PaintBlock", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_paint_block)),
        lua_method("ReplaceMaterial", "nil", reinterpret_cast<void*>(&ScriptBindings::terrain_replace_material)),
        lua_method("AddMaterial", "TerrainMaterial", reinterpret_cast<void*>(&ScriptBindings::terrain_add_material)),
        lua_method("ReadVoxels", "", reinterpret_cast<void*>(&ScriptBindings::terrain_read_voxels)),
        lua_method("WorldToCell", "Vector3", reinterpret_cast<void*>(&ScriptBindings::terrain_world_to_cell)),
    };
    register_lua_class("Terrain", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
}

}  // namespace engine_core
