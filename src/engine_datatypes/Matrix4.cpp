#include "Matrix4.hpp"

#include "Enum.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "VectorMath.hpp"

#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace engine_core {
namespace {

const double kPi = 3.14159265358979323846;

// Vector math in double, so a composed rotation rounds once, to float, at the end.
struct D3 {
    double x = 0;
    double y = 0;
    double z = 0;
};

D3 to_d3(Vec3 value) { return D3{value.x, value.y, value.z}; }

Vec3 to_vec3(D3 value) { return Vec3{static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)}; }

double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

D3 cross(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

double length(D3 value) { return std::sqrt(dot(value, value)); }

// Zero stays zero, so a degenerate input gives a finite answer.
D3 normalize(D3 value) {
    const double size = length(value);
    return size > 0 ? D3{value.x / size, value.y / size, value.z / size} : D3{};
}

D3 column(const Matrix4& value, int index) {
    return D3{value.m[index * 4], value.m[index * 4 + 1], value.m[index * 4 + 2]};
}

// A 3x3 rotation, r[row][column].
struct Rotation {
    double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Rotation rotation_of(const Matrix4& value) {
    Rotation out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.r[row][col] = value.m[col * 4 + row];
        }
    }
    return out;
}

Rotation multiply(const Rotation& a, const Rotation& b) {
    Rotation out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.r[row][col] = a.r[row][0] * b.r[0][col] + a.r[row][1] * b.r[1][col] + a.r[row][2] * b.r[2][col];
        }
    }
    return out;
}

// Right-handed rotation about X (0), Y (1), or Z (2).
Rotation axis_rotation(int axis, double angle) {
    Rotation out;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const int a = (axis + 1) % 3;
    const int b = (axis + 2) % 3;
    out.r[a][a] = c;
    out.r[a][b] = -s;
    out.r[b][a] = s;
    out.r[b][b] = c;
    return out;
}

Matrix4 from_parts(const Rotation& rotation, Vec3 position) {
    Matrix4 out = matrix4_translation(position.x, position.y, position.z);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.m[col * 4 + row] = static_cast<float>(rotation.r[row][col]);
        }
    }
    return out;
}

Matrix4 from_columns(D3 x, D3 y, D3 z, Vec3 position) {
    Rotation rotation;
    const D3 columns[3] = {x, y, z};
    for (int col = 0; col < 3; ++col) {
        rotation.r[0][col] = columns[col].x;
        rotation.r[1][col] = columns[col].y;
        rotation.r[2][col] = columns[col].z;
    }
    return from_parts(rotation, position);
}

bool affine(const Matrix4& value) {
    return value.m[3] == 0.f && value.m[7] == 0.f && value.m[11] == 0.f && value.m[15] == 1.f;
}

// The axes each RotationOrder composes, leftmost first.
const int kOrderAxes[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}};

const int* order_axes(RotationOrder order) {
    const int index = static_cast<int>(order);
    return kOrderAxes[index >= 0 && index < 6 ? index : 0];
}

Matrix4 rotation_between(Vec3 from, Vec3 to) {
    const D3 a = normalize(to_d3(from));
    const D3 b = normalize(to_d3(to));
    const double cosine = dot(a, b);
    if (cosine < -1.0 + 1e-7) {
        // Opposite: half a turn about any axis at right angles to from.
        D3 axis = cross(a, D3{1, 0, 0});
        if (length(axis) < 1e-6) {
            axis = cross(a, D3{0, 1, 0});
        }
        return matrix4_axis_angle(to_vec3(axis), kPi);
    }
    const D3 axis = cross(a, b);
    return matrix4_from_quaternion(axis.x, axis.y, axis.z, 1.0 + cosine);
}

}  // namespace

Matrix4 matrix4_multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 out;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += double(a.m[k * 4 + row]) * b.m[col * 4 + k];
            }
            out.m[col * 4 + row] = static_cast<float>(sum);
        }
    }
    return out;
}

// Cofactors over the determinant, in double.
Matrix4 matrix4_inverse(const Matrix4& value) {
    double m[16];
    for (int index = 0; index < 16; ++index) {
        m[index] = value.m[index];
    }
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Matrix4 out;
    for (int index = 0; index < 16; ++index) {
        out.m[index] = static_cast<float>(inv[index] / det);
    }
    return out;
}

Vec3 matrix4_point(const Matrix4& value, Vec3 point) {
    const float* m = value.m;
    double x = m[0] * double(point.x) + m[4] * double(point.y) + m[8] * double(point.z) + m[12];
    double y = m[1] * double(point.x) + m[5] * double(point.y) + m[9] * double(point.z) + m[13];
    double z = m[2] * double(point.x) + m[6] * double(point.y) + m[10] * double(point.z) + m[14];
    if (!affine(value)) {
        const double w = m[3] * double(point.x) + m[7] * double(point.y) + m[11] * double(point.z) + m[15];
        x /= w;
        y /= w;
        z /= w;
    }
    return to_vec3(D3{x, y, z});
}

Vec3 matrix4_vector(const Matrix4& value, Vec3 direction) {
    const float* m = value.m;
    return to_vec3(D3{m[0] * double(direction.x) + m[4] * double(direction.y) + m[8] * double(direction.z),
                      m[1] * double(direction.x) + m[5] * double(direction.y) + m[9] * double(direction.z),
                      m[2] * double(direction.x) + m[6] * double(direction.y) + m[10] * double(direction.z)});
}

Matrix4 matrix4_axis_angle(Vec3 axis, double angle) {
    const D3 k = normalize(to_d3(axis));
    if (length(k) == 0) {
        return matrix4_identity();
    }
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double t = 1.0 - c;
    Rotation rotation;
    rotation.r[0][0] = c + k.x * k.x * t;
    rotation.r[0][1] = k.x * k.y * t - k.z * s;
    rotation.r[0][2] = k.x * k.z * t + k.y * s;
    rotation.r[1][0] = k.y * k.x * t + k.z * s;
    rotation.r[1][1] = c + k.y * k.y * t;
    rotation.r[1][2] = k.y * k.z * t - k.x * s;
    rotation.r[2][0] = k.z * k.x * t - k.y * s;
    rotation.r[2][1] = k.z * k.y * t + k.x * s;
    rotation.r[2][2] = c + k.z * k.z * t;
    return from_parts(rotation, Vec3{});
}

Matrix4 matrix4_from_quaternion(double x, double y, double z, double w) {
    const double size = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(size > 0)) {
        return matrix4_identity();
    }
    x /= size;
    y /= size;
    z /= size;
    w /= size;
    Rotation rotation;
    rotation.r[0][0] = 1 - 2 * (y * y + z * z);
    rotation.r[0][1] = 2 * (x * y - z * w);
    rotation.r[0][2] = 2 * (x * z + y * w);
    rotation.r[1][0] = 2 * (x * y + z * w);
    rotation.r[1][1] = 1 - 2 * (x * x + z * z);
    rotation.r[1][2] = 2 * (y * z - x * w);
    rotation.r[2][0] = 2 * (x * z - y * w);
    rotation.r[2][1] = 2 * (y * z + x * w);
    rotation.r[2][2] = 1 - 2 * (x * x + y * y);
    return from_parts(rotation, Vec3{});
}

// Shepperd's method: divide by the largest of the four candidates.
void matrix4_to_quaternion(const Matrix4& value, double out[4]) {
    const Rotation rotation = rotation_of(matrix4_orthonormalize(value));
    const auto& r = rotation.r;
    const double trace = r[0][0] + r[1][1] + r[2][2];
    double x = 0;
    double y = 0;
    double z = 0;
    double w = 1;
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2;
        w = s / 4;
        x = (r[2][1] - r[1][2]) / s;
        y = (r[0][2] - r[2][0]) / s;
        z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2;
        w = (r[2][1] - r[1][2]) / s;
        x = s / 4;
        y = (r[0][1] + r[1][0]) / s;
        z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2;
        w = (r[0][2] - r[2][0]) / s;
        x = (r[0][1] + r[1][0]) / s;
        y = s / 4;
        z = (r[1][2] + r[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2;
        w = (r[1][0] - r[0][1]) / s;
        x = (r[0][2] + r[2][0]) / s;
        y = (r[1][2] + r[2][1]) / s;
        z = s / 4;
    }
    const double sign = w < 0 ? -1.0 : 1.0;
    const double size = std::sqrt(x * x + y * y + z * z + w * w);
    out[0] = sign * x / size;
    out[1] = sign * y / size;
    out[2] = sign * z / size;
    out[3] = sign * w / size;
}

Matrix4 matrix4_from_euler(double rx, double ry, double rz, RotationOrder order) {
    const double angles[3] = {rx, ry, rz};
    const int* axes = order_axes(order);
    const Rotation rotation = multiply(multiply(axis_rotation(axes[0], angles[axes[0]]), axis_rotation(axes[1], angles[axes[1]])),
                                       axis_rotation(axes[2], angles[axes[2]]));
    return from_parts(rotation, Vec3{});
}

// For R = Ri * Rj * Rk, the middle angle is asin(±R[i][k]). An even
// permutation of XYZ takes +, an odd one -. When cos of the middle angle is 0,
// the outer two turn about the same axis, so the last is taken as 0.
void matrix4_to_euler(const Matrix4& value, RotationOrder order, double out[3]) {
    const Rotation rotation = rotation_of(matrix4_orthonormalize(value));
    const auto& r = rotation.r;
    const int* axes = order_axes(order);
    const int i = axes[0];
    const int j = axes[1];
    const int k = axes[2];
    const double parity = static_cast<int>(order) % 2 == 0 ? 1.0 : -1.0;
    const double sine = std::clamp(parity * r[i][k], -1.0, 1.0);
    const double cosine = std::hypot(r[i][i], r[i][j]);
    out[j] = std::atan2(sine, cosine);
    if (cosine > 1e-6) {
        out[i] = std::atan2(-parity * r[j][k], r[k][k]);
        out[k] = std::atan2(-parity * r[i][j], r[i][i]);
    } else {
        out[i] = std::atan2(parity * r[k][j], r[j][j]);
        out[k] = 0;
    }
}

Matrix4 matrix4_look_at(Vec3 at, Vec3 target, Vec3 up) {
    const D3 look = normalize(D3{double(target.x) - at.x, double(target.y) - at.y, double(target.z) - at.z});
    if (length(look) == 0) {
        return matrix4_translation(at.x, at.y, at.z);
    }
    D3 right = cross(look, to_d3(up));
    if (length(right) < 1e-6) {
        right = cross(look, D3{0, 0, 1});
    }
    right = normalize(right);
    const D3 top = cross(right, look);
    return from_columns(right, top, D3{-look.x, -look.y, -look.z}, at);
}

Matrix4 matrix4_orthonormalize(const Matrix4& value) {
    const D3 back = normalize(column(value, 2));
    const D3 right = normalize(cross(column(value, 1), back));
    const D3 top = cross(back, right);
    return from_columns(right, top, back, matrix4_position(value));
}

Matrix4 matrix4_lerp(const Matrix4& a, const Matrix4& b, double alpha) {
    if (alpha == 0.0) {
        return a;
    }
    if (alpha == 1.0) {
        return b;
    }
    double from[4];
    double to[4];
    matrix4_to_quaternion(a, from);
    matrix4_to_quaternion(b, to);
    double cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
    if (cosine < 0) {
        cosine = -cosine;
        for (double& part : to) {
            part = -part;
        }
    }
    // Nearly the same rotation: a straight blend, where sin(theta) is too small to divide by.
    double from_weight = 1.0 - alpha;
    double to_weight = alpha;
    if (cosine < 0.9995) {
        const double theta = std::acos(cosine);
        const double sine = std::sin(theta);
        from_weight = std::sin((1.0 - alpha) * theta) / sine;
        to_weight = std::sin(alpha * theta) / sine;
    }
    Matrix4 out = matrix4_from_quaternion(from[0] * from_weight + to[0] * to_weight, from[1] * from_weight + to[1] * to_weight,
                                          from[2] * from_weight + to[2] * to_weight, from[3] * from_weight + to[3] * to_weight);
    for (int col = 0; col < 3; ++col) {
        const double scale = length(column(a, col)) + (length(column(b, col)) - length(column(a, col))) * alpha;
        for (int row = 0; row < 3; ++row) {
            out.m[col * 4 + row] = static_cast<float>(out.m[col * 4 + row] * scale);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        out.m[12 + axis] = vector_math::lerp_component(a.m[12 + axis], b.m[12 + axis], alpha);
    }
    return out;
}

namespace {

const char* kMatrix4Meta = "AE.Matrix4";

const Matrix4& check_matrix4(lua_State* state, int index) {
    return check_userdata<Matrix4>(state, index, kMatrix4Meta, "Matrix4");
}

Vec3 check_vector(lua_State* state, int index) {
    const float* value = lua_tovector(state, index);
    if (value == nullptr) {
        luaL_typeerrorL(state, index, "Vector3");
    }
    return Vec3{value[0], value[1], value[2]};
}

Vec3 opt_vector(lua_State* state, int index, Vec3 fallback) {
    return lua_isnoneornil(state, index) ? fallback : check_vector(state, index);
}

void push_vector(lua_State* state, Vec3 value) { lua_pushvector(state, value.x, value.y, value.z); }

void push_column(lua_State* state, const Matrix4& value, int index, float sign) {
    lua_pushvector(state, sign * value.m[index * 4], sign * value.m[index * 4 + 1], sign * value.m[index * 4 + 2]);
}

void push_row(lua_State* state, const Matrix4& value, int index) {
    lua_pushvector(state, value.m[index], value.m[4 + index], value.m[8 + index]);
}

RotationOrder opt_order(lua_State* state, int index) {
    if (lua_isnoneornil(state, index)) {
        return RotationOrder::XYZ;
    }
    return static_cast<RotationOrder>(check_enum_arg(state, index, rotation_order_enum()));
}

// x, y, z, then the rotation's rows: Matrix4.new's twelve and GetComponents's order.
Matrix4 from_components(const double values[12]) {
    Matrix4 out = matrix4_translation(static_cast<float>(values[0]), static_cast<float>(values[1]),
                                      static_cast<float>(values[2]));
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.m[col * 4 + row] = static_cast<float>(values[3 + row * 3 + col]);
        }
    }
    return out;
}

Matrix4 at_position(Matrix4 value, Vec3 position) {
    value.m[12] = position.x;
    value.m[13] = position.y;
    value.m[14] = position.z;
    return value;
}

int matrix4_new(lua_State* state) {
    const int count = lua_gettop(state);
    switch (count) {
    case 0:
        push_matrix4(state, matrix4_identity());
        return 1;
    case 1: {
        const Vec3 position = check_vector(state, 1);
        push_matrix4(state, matrix4_translation(position.x, position.y, position.z));
        return 1;
    }
    case 2:
        push_matrix4(state, matrix4_look_at(check_vector(state, 1), check_vector(state, 2), Vec3{0.f, 1.f, 0.f}));
        return 1;
    case 3:
        push_matrix4(state, matrix4_translation(static_cast<float>(luaL_checknumber(state, 1)),
                                                static_cast<float>(luaL_checknumber(state, 2)),
                                                static_cast<float>(luaL_checknumber(state, 3))));
        return 1;
    case 7: {
        const Matrix4 rotation = matrix4_from_quaternion(luaL_checknumber(state, 4), luaL_checknumber(state, 5),
                                                         luaL_checknumber(state, 6), luaL_checknumber(state, 7));
        const Vec3 position{static_cast<float>(luaL_checknumber(state, 1)), static_cast<float>(luaL_checknumber(state, 2)),
                            static_cast<float>(luaL_checknumber(state, 3))};
        push_matrix4(state, at_position(rotation, position));
        return 1;
    }
    case 12: {
        double values[12];
        for (int index = 0; index < 12; ++index) {
            values[index] = luaL_checknumber(state, index + 1);
        }
        push_matrix4(state, from_components(values));
        return 1;
    }
    default:
        luaL_error(state, "Invalid number of arguments: %d", count);
    }
}

int matrix4_look_at_lua(lua_State* state) {
    push_matrix4(state, matrix4_look_at(check_vector(state, 1), check_vector(state, 2),
                                        opt_vector(state, 3, Vec3{0.f, 1.f, 0.f})));
    return 1;
}

int matrix4_look_along(lua_State* state) {
    const Vec3 at = check_vector(state, 1);
    const Vec3 direction = check_vector(state, 2);
    const Vec3 target{at.x + direction.x, at.y + direction.y, at.z + direction.z};
    push_matrix4(state, matrix4_look_at(at, target, opt_vector(state, 3, Vec3{0.f, 1.f, 0.f})));
    return 1;
}

int matrix4_from_rotation_between(lua_State* state) {
    push_matrix4(state, rotation_between(check_vector(state, 1), check_vector(state, 2)));
    return 1;
}

int push_euler(lua_State* state, RotationOrder order) {
    push_matrix4(state, matrix4_from_euler(luaL_checknumber(state, 1), luaL_checknumber(state, 2),
                                           luaL_checknumber(state, 3), order));
    return 1;
}

int matrix4_from_euler_lua(lua_State* state) { return push_euler(state, opt_order(state, 4)); }

int matrix4_from_euler_xyz(lua_State* state) { return push_euler(state, RotationOrder::XYZ); }

int matrix4_from_euler_yxz(lua_State* state) { return push_euler(state, RotationOrder::YXZ); }

int matrix4_from_axis_angle_lua(lua_State* state) {
    push_matrix4(state, matrix4_axis_angle(check_vector(state, 1), luaL_checknumber(state, 2)));
    return 1;
}

// vZ defaults to the unit cross product of vX and vY.
int matrix4_from_matrix(lua_State* state) {
    const Vec3 position = check_vector(state, 1);
    const D3 x = to_d3(check_vector(state, 2));
    const D3 y = to_d3(check_vector(state, 3));
    const D3 z = lua_isnoneornil(state, 4) ? normalize(cross(x, y)) : to_d3(check_vector(state, 4));
    push_matrix4(state, from_columns(x, y, z, position));
    return 1;
}

int matrix4_inverse_lua(lua_State* state) {
    push_matrix4(state, matrix4_inverse(check_matrix4(state, 1)));
    return 1;
}

int matrix4_lerp_lua(lua_State* state) {
    const Matrix4& from = check_matrix4(state, 1);
    const Matrix4& goal = check_matrix4(state, 2);
    push_matrix4(state, matrix4_lerp(from, goal, luaL_checknumber(state, 3)));
    return 1;
}

int matrix4_orthonormalize_lua(lua_State* state) {
    push_matrix4(state, matrix4_orthonormalize(check_matrix4(state, 1)));
    return 1;
}

// The space methods take any number of values after the receiver and return
// one result for each.
template <typename Each>
int each_argument(lua_State* state, Each each) {
    const int count = lua_gettop(state) - 1;
    luaL_checkstack(state, count, "too many arguments");
    for (int index = 2; index <= count + 1; ++index) {
        each(index);
    }
    return count;
}

int matrix4_to_world_space(lua_State* state) {
    const Matrix4 self = check_matrix4(state, 1);
    return each_argument(state, [&](int index) { push_matrix4(state, matrix4_multiply(self, check_matrix4(state, index))); });
}

int matrix4_to_object_space(lua_State* state) {
    const Matrix4 inverse = matrix4_inverse(check_matrix4(state, 1));
    return each_argument(state,
                         [&](int index) { push_matrix4(state, matrix4_multiply(inverse, check_matrix4(state, index))); });
}

int matrix4_point_to_world_space(lua_State* state) {
    const Matrix4 self = check_matrix4(state, 1);
    return each_argument(state, [&](int index) { push_vector(state, matrix4_point(self, check_vector(state, index))); });
}

int matrix4_point_to_object_space(lua_State* state) {
    const Matrix4 inverse = matrix4_inverse(check_matrix4(state, 1));
    return each_argument(state, [&](int index) { push_vector(state, matrix4_point(inverse, check_vector(state, index))); });
}

int matrix4_vector_to_world_space(lua_State* state) {
    const Matrix4 self = check_matrix4(state, 1);
    return each_argument(state, [&](int index) { push_vector(state, matrix4_vector(self, check_vector(state, index))); });
}

int matrix4_vector_to_object_space(lua_State* state) {
    const Matrix4 inverse = matrix4_inverse(check_matrix4(state, 1));
    return each_argument(state, [&](int index) { push_vector(state, matrix4_vector(inverse, check_vector(state, index))); });
}

int matrix4_get_components(lua_State* state) {
    const Matrix4& value = check_matrix4(state, 1);
    lua_pushnumber(state, value.m[12]);
    lua_pushnumber(state, value.m[13]);
    lua_pushnumber(state, value.m[14]);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            lua_pushnumber(state, value.m[col * 4 + row]);
        }
    }
    return 12;
}

int push_angles(lua_State* state, RotationOrder order) {
    double angles[3];
    matrix4_to_euler(check_matrix4(state, 1), order, angles);
    for (double angle : angles) {
        lua_pushnumber(state, angle);
    }
    return 3;
}

int matrix4_to_euler_lua(lua_State* state) { return push_angles(state, opt_order(state, 2)); }

int matrix4_to_euler_xyz(lua_State* state) { return push_angles(state, RotationOrder::XYZ); }

int matrix4_to_euler_yxz(lua_State* state) { return push_angles(state, RotationOrder::YXZ); }

// The angle is from 0 to pi. No rotation has no axis, so it reports +X.
int matrix4_to_axis_angle(lua_State* state) {
    double quaternion[4];
    matrix4_to_quaternion(check_matrix4(state, 1), quaternion);
    const D3 axis{quaternion[0], quaternion[1], quaternion[2]};
    const double sine = length(axis);
    push_vector(state, sine > 1e-12 ? to_vec3(normalize(axis)) : Vec3{1.f, 0.f, 0.f});
    lua_pushnumber(state, 2.0 * std::atan2(sine, quaternion[3]));
    return 2;
}

int matrix4_fuzzy_eq(lua_State* state) {
    const Matrix4& a = check_matrix4(state, 1);
    const Matrix4& b = check_matrix4(state, 2);
    const double epsilon = luaL_optnumber(state, 3, 1e-5);
    bool equal = true;
    for (int index = 0; index < 16 && equal; ++index) {
        equal = vector_math::fuzzy_component(a.m[index], b.m[index], epsilon);
    }
    lua_pushboolean(state, equal ? 1 : 0);
    return 1;
}

// The properties are read here. Anything else is a method from the table in upvalue 1.
int matrix4_index(lua_State* state) {
    const Matrix4& value = check_matrix4(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (name[0] != '\0' && name[1] == '\0' && name[0] >= 'X' && name[0] <= 'Z') {
        lua_pushnumber(state, value.m[12 + (name[0] - 'X')]);
        return 1;
    }
    if (std::strcmp(name, "Position") == 0) {
        push_vector(state, matrix4_position(value));
        return 1;
    }
    if (std::strcmp(name, "Rotation") == 0) {
        push_matrix4(state, at_position(value, Vec3{}));
        return 1;
    }
    if (std::strcmp(name, "RightVector") == 0) {
        push_column(state, value, 0, 1.f);
        return 1;
    }
    if (std::strcmp(name, "UpVector") == 0) {
        push_column(state, value, 1, 1.f);
        return 1;
    }
    if (std::strcmp(name, "LookVector") == 0) {
        push_column(state, value, 2, -1.f);
        return 1;
    }
    // XVector, YVector, and ZVector are the rotation's rows.
    if (name[0] >= 'X' && name[0] <= 'Z' && std::strcmp(name + 1, "Vector") == 0) {
        push_row(state, value, name[0] - 'X');
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (lua_isfunction(state, -1)) {
        return 1;
    }
    luaL_error(state, "%s is not a valid member of Matrix4", name);
}

int matrix4_newindex(lua_State* state) { luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2)); }

// The twelve components, "x, y, z, R00, R01, ..., R22".
int matrix4_tostring(lua_State* state) {
    check_matrix4(state, 1);
    const int count = matrix4_get_components(state);
    std::string text;
    for (int index = 0; index < count; ++index) {
        if (index > 0) {
            text += ", ";
        }
        text += luaL_tolstring(state, -count + index, nullptr);
        lua_pop(state, 1);
    }
    lua_pushlstring(state, text.data(), text.size());
    return 1;
}

int matrix4_eq(lua_State* state) {
    const Matrix4* a = to_matrix4(state, 1);
    const Matrix4* b = to_matrix4(state, 2);
    bool equal = a != nullptr && b != nullptr;
    for (int index = 0; index < 16 && equal; ++index) {
        equal = a->m[index] == b->m[index];
    }
    lua_pushboolean(state, equal ? 1 : 0);
    return 1;
}

// Matrix4 * Matrix4 composes. Matrix4 * Vector3 moves the point.
int matrix4_mul(lua_State* state) {
    const Matrix4& self = check_matrix4(state, 1);
    if (const Matrix4* other = to_matrix4(state, 2)) {
        push_matrix4(state, matrix4_multiply(self, *other));
        return 1;
    }
    if (lua_isvector(state, 2)) {
        push_vector(state, matrix4_point(self, check_vector(state, 2)));
        return 1;
    }
    luaL_typeerrorL(state, 2, "Matrix4 or Vector3");
}

// + and - move the position by a Vector3 in world space.
int translate(lua_State* state, float sign) {
    const Matrix4& self = check_matrix4(state, 1);
    const Vec3 offset = check_vector(state, 2);
    const Vec3 position = matrix4_position(self);
    push_matrix4(state, at_position(self, Vec3{position.x + sign * offset.x, position.y + sign * offset.y,
                                               position.z + sign * offset.z}));
    return 1;
}

int matrix4_add(lua_State* state) { return translate(state, 1.f); }

int matrix4_sub(lua_State* state) { return translate(state, -1.f); }

// The operators, and how script analysis types them. Two rows for one
// metamethod are overloads. The metatable takes each metamethod once.
struct Operator {
    const char* metamethod;
    lua_CFunction call;
    const char* left;
    const char* right;
    const char* result;
};

const Operator kOperators[] = {
    {"__mul", matrix4_mul, "Matrix4", "Matrix4", "Matrix4"},
    {"__mul", matrix4_mul, "Matrix4", "Vector3", "Vector3"},
    {"__add", matrix4_add, "Matrix4", "Vector3", "Matrix4"},
    {"__sub", matrix4_sub, "Matrix4", "Vector3", "Matrix4"},
    {"__eq", matrix4_eq, "Matrix4", "Matrix4", "boolean"},
};

const luaL_Reg kMethods[] = {
    {"Inverse", matrix4_inverse_lua},
    {"Lerp", matrix4_lerp_lua},
    {"Orthonormalize", matrix4_orthonormalize_lua},
    {"ToWorldSpace", matrix4_to_world_space},
    {"ToObjectSpace", matrix4_to_object_space},
    {"PointToWorldSpace", matrix4_point_to_world_space},
    {"PointToObjectSpace", matrix4_point_to_object_space},
    {"VectorToWorldSpace", matrix4_vector_to_world_space},
    {"VectorToObjectSpace", matrix4_vector_to_object_space},
    {"GetComponents", matrix4_get_components},
    {"ToEulerAngles", matrix4_to_euler_lua},
    {"ToEulerAnglesXYZ", matrix4_to_euler_xyz},
    {"ToEulerAnglesYXZ", matrix4_to_euler_yxz},
    {"ToOrientation", matrix4_to_euler_yxz},
    {"ToAxisAngle", matrix4_to_axis_angle},
    {"FuzzyEq", matrix4_fuzzy_eq},
    {nullptr, nullptr},
};

const luaL_Reg kConstructors[] = {
    {"new", matrix4_new},
    {"lookAt", matrix4_look_at_lua},
    {"lookAlong", matrix4_look_along},
    {"fromRotationBetweenVectors", matrix4_from_rotation_between},
    {"fromEulerAngles", matrix4_from_euler_lua},
    {"fromEulerAnglesXYZ", matrix4_from_euler_xyz},
    {"Angles", matrix4_from_euler_xyz},
    {"fromEulerAnglesYXZ", matrix4_from_euler_yxz},
    {"fromOrientation", matrix4_from_euler_yxz},
    {"fromAxisAngle", matrix4_from_axis_angle_lua},
    {"fromMatrix", matrix4_from_matrix},
    {nullptr, nullptr},
};

void install_matrix4_metatable(lua_State* state) {
    luaL_newmetatable(state, kMatrix4Meta);

    lua_newtable(state);
    for (const luaL_Reg* method = kMethods; method->func != nullptr; ++method) {
        lua_pushcfunction(state, method->func, method->name);
        lua_setfield(state, -2, method->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_pushcclosure(state, matrix4_index, "index", 1);
    lua_setfield(state, -2, "__index");

    lua_pushcfunction(state, matrix4_newindex, "__newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, matrix4_tostring, "__tostring");
    lua_setfield(state, -2, "__tostring");
    for (const Operator& op : kOperators) {
        lua_pushcfunction(state, op.call, op.metamethod);
        lua_setfield(state, -2, op.metamethod);
    }
    lua_pushliteral(state, "Matrix4");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
}

void install_matrix4_library(lua_State* state) {
    lua_newtable(state);
    for (const luaL_Reg* entry = kConstructors; entry->func != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    push_matrix4(state, matrix4_identity());
    lua_setfield(state, -2, "identity");
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, "Matrix4");
}

// Built here, not at namespace scope, for the reason Vector3.cpp gives. A
// method with no type here returns several values; its documentation types them.
ANARCHY_LUA_REGISTER(register_matrix4_lua) {
    const LuaField fields[] = {
        lua_property("X", "number", false, nullptr, nullptr),
        lua_property("Y", "number", false, nullptr, nullptr),
        lua_property("Z", "number", false, nullptr, nullptr),
        lua_property("Position", "Vector3", false, nullptr, nullptr),
        lua_property("Rotation", "Matrix4", false, nullptr, nullptr),
        lua_property("LookVector", "Vector3", false, nullptr, nullptr),
        lua_property("RightVector", "Vector3", false, nullptr, nullptr),
        lua_property("UpVector", "Vector3", false, nullptr, nullptr),
        lua_property("XVector", "Vector3", false, nullptr, nullptr),
        lua_property("YVector", "Vector3", false, nullptr, nullptr),
        lua_property("ZVector", "Vector3", false, nullptr, nullptr),
        lua_method("Inverse", "Matrix4", nullptr),
        lua_method("Lerp", "Matrix4", nullptr),
        lua_method("Orthonormalize", "Matrix4", nullptr),
        lua_method("ToWorldSpace", "Matrix4", nullptr),
        lua_method("ToObjectSpace", "Matrix4", nullptr),
        lua_method("PointToWorldSpace", "Vector3", nullptr),
        lua_method("PointToObjectSpace", "Vector3", nullptr),
        lua_method("VectorToWorldSpace", "Vector3", nullptr),
        lua_method("VectorToObjectSpace", "Vector3", nullptr),
        lua_method("GetComponents", nullptr, nullptr),
        lua_method("ToEulerAngles", nullptr, nullptr),
        lua_method("ToEulerAnglesXYZ", nullptr, nullptr),
        lua_method("ToEulerAnglesYXZ", nullptr, nullptr),
        lua_method("ToOrientation", nullptr, nullptr),
        lua_method("ToAxisAngle", nullptr, nullptr),
        lua_method("FuzzyEq", "boolean", nullptr),
    };
    register_lua_class("Matrix4", nullptr, fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
    for (const luaL_Reg* entry = kConstructors; entry->func != nullptr; ++entry) {
        lua_note_result("Matrix4", entry->name, "Matrix4", false);
    }
    for (const Operator& op : kOperators) {
        LuaOperator row;
        row.metamethod = op.metamethod;
        row.left = op.left;
        row.right = op.right;
        row.result = op.result;
        register_lua_operators("Matrix4", &row, 1);
    }
}

}  // namespace

void push_matrix4(lua_State* state, const Matrix4& value) { push_userdata(state, value, kMatrix4Meta); }

const Matrix4* to_matrix4(lua_State* state, int index) {
    return static_cast<const Matrix4*>(test_userdata(state, index, kMatrix4Meta));
}

void open_matrix4(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    install_matrix4_metatable(state);
    install_matrix4_library(state);
}

}  // namespace engine_core
