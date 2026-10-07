#include "terrain/ShapeDistance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine_core::terrain {

void prepare_shape(Shape& shape) { shape.inverse = matrix4_inverse(shape.frame); }

void shape_bounds(const Shape& shape, float margin, Vec3& min, Vec3& max) {
    if (shape.kind == Shape::Kind::Ball) {
        const float r = shape.radius + margin;
        min = Vec3{shape.center.x - r, shape.center.y - r, shape.center.z - r};
        max = Vec3{shape.center.x + r, shape.center.y + r, shape.center.z + r};
        return;
    }
    const Vec3 half = shape.kind == Shape::Kind::Cylinder
                           ? Vec3{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.x * 0.5f}
                           : Vec3{shape.size.x * 0.5f, shape.size.y * 0.5f, shape.size.z * 0.5f};
    min = Vec3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()};
    max = Vec3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
               std::numeric_limits<float>::lowest()};
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{(i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z};
        const Vec3 world = matrix4_point(shape.frame, corner);
        min.x = std::min(min.x, world.x);
        min.y = std::min(min.y, world.y);
        min.z = std::min(min.z, world.z);
        max.x = std::max(max.x, world.x);
        max.y = std::max(max.y, world.y);
        max.z = std::max(max.z, world.z);
    }
    min.x -= margin;
    min.y -= margin;
    min.z -= margin;
    max.x += margin;
    max.y += margin;
    max.z += margin;
}

}  // namespace engine_core::terrain
