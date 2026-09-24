#pragma once

#include "DataModel.hpp"

namespace engine_core {

// Spatial instance. Plain DataModel instances do not have these fields.
class GameObject : public DataModel {
public:
    GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    void set_transform(const Transform& transform);
    void set_transform(const Transform& transform, ForceSimWrite);
    void set_color(ColorRgb color);
    void set_color(ColorRgb color, ForceSimWrite);
    void set_size(float x, float y, float z);
    void set_linear_velocity(float x, float y, float z);

    // A dead id fails closed: transform() is a zero matrix, not a recycled slot.
    Transform transform() const;
    ColorRgb color() const;
    bool copy_size(float out[3]) const;

private:
    friend class DataModel;

    void reset_spatial();
    void clear_spatial();

    Transform transform_ = transform_identity();
    ColorRgb color_{};
    float size_[3] = {1.f, 1.f, 1.f};
    float velocity_[3] = {};
};

}  // namespace engine_core
