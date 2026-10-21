#pragma once

#include "AssetInstances.hpp"
#include "DataModel.hpp"

#include <optional>
#include <string>

namespace engine_core {

class Shader;

// A post Shader drawn over the finished frame, in the Scene View and the
// player. Like a BloomEffect it belongs under Lighting, at any depth through
// Folders, and nowhere else; unlike Bloom, every one under Lighting runs,
// Order low to high, equal Orders in tree order. Its uniforms are its
// attributes (SetAttribute), as a Material's are its surface Shader's.
//
// Enabled  boolean  false draws nothing. True.
// Shader   Shader?  the post Shader it draws with; a surface Shader is
//                   refused, one that turns Surface later is kept and draws
//                   nothing. Nil.
// Order    number   its place in the chain, any finite number. 0.
//
// Each is a saved registry property, so DataModel saves, loads, undoes, and
// restores it at Stop. The render snapshot reads every ShaderEffect at every
// Prepare (VisualShaderEffect), as it reads the BloomEffect.
class ShaderEffect : public ReferenceAsset {
public:
    // reference() index.
    static constexpr std::size_t kShaderReference = 0;
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultOrder = 0.0;

    ShaderEffect(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : ReferenceAsset(tag, state, id) {}
    const char* class_name() const override { return "ShaderEffect"; }

    bool enabled() const { return enabled_; }
    double order() const { return order_; }
    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_order(double value);
    // The live post Shader it draws with, or null: none, gone, or not Post.
    const Shader* shader(const DataModel& world) const;

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
    std::optional<std::string> refuse_target(std::size_t index, const DataModel& target) const override;
    void on_reuse() override;

private:
    bool enabled_ = kDefaultEnabled;
    double order_ = kDefaultOrder;
};

}  // namespace engine_core
