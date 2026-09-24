#pragma once

#include "DataModel.hpp"

#include <atomic>

namespace engine_core {

// Test instance drawn as the Scene View triangle.
// step() turns the angle 90 degrees per second. Heartbeat calls it because
// this instance is under the root. The UI thread reads the angle and the
// position while drawing.
class TestTriangle : public DataModel {
public:
    TestTriangle(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override { return "TestTriangle"; }

    void step(double dt) override;
    void set_position(float x, float y, float z);
    // A dead id reads as 0.
    double angle_degrees() const;
    Vec3 position() const;

protected:
    void on_release() override;
    void on_reuse() override;
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    void clear_pose();

    // Heartbeat writes the angle. The UI thread reads both.
    std::atomic<double> angle_{0.0};
    std::atomic<float> x_{0.f};
    std::atomic<float> y_{0.f};
    std::atomic<float> z_{0.f};
};

}  // namespace engine_core
