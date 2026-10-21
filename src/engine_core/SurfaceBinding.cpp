#include "SurfaceBinding.hpp"

#include "AssetInstances.hpp"
#include "Shader.hpp"
#include "ShaderEffect.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {
namespace {

// The FNV-1a hash BrushVisuals keys its bakes by.
struct Hash {
    std::uint64_t value = 1469598103934665603ull;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value = (value ^ p[i]) * 1099511628211ull;
        }
    }
    template <class T>
    void add(const T& item) {
        bytes(&item, sizeof(item));
    }
};

// The slot kind a uniform of type reads from an attribute.
LuaSlot::Kind slot_kind(ShaderParamType type) {
    switch (type) {
    case ShaderParamType::Float:
    case ShaderParamType::Int:
        return LuaSlot::Kind::Number;
    case ShaderParamType::Bool:
        return LuaSlot::Kind::Bool;
    case ShaderParamType::Vec2:
        return LuaSlot::Kind::Vec2;
    case ShaderParamType::Vec3:
        return LuaSlot::Kind::Vec3;
    case ShaderParamType::Color:
        return LuaSlot::Kind::Color;
    case ShaderParamType::Sampler:
        return LuaSlot::Kind::Instance;
    }
    return LuaSlot::Kind::Nil;
}

bool same_value(const LuaSlot& a, const LuaSlot& b) {
    return a.kind == b.kind && a.number == b.number && a.flag == b.flag && a.vec.x == b.vec.x &&
           a.vec.y == b.vec.y && a.vec.z == b.vec.z && a.color.r == b.color.r && a.color.g == b.color.g &&
           a.color.b == b.color.b;
}

bool same_values(const SurfaceValues& a, const SurfaceValues& b) {
    if (a.slots.size() != b.slots.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.slots.size(); ++i) {
        if (!same_value(a.slots[i], b.slots[i])) {
            return false;
        }
    }
    return true;
}

// The same Shader, version, and textures: all but the values.
bool same_binding(const SurfaceBinding& a, const SurfaceBinding& b) {
    if (a.shader != b.shader || a.version != b.version || a.parse != b.parse ||
        a.samplers.size() != b.samplers.size() || a.displacement_margin != b.displacement_margin || a.hdr != b.hdr) {
        return false;
    }
    for (std::size_t i = 0; i < a.samplers.size(); ++i) {
        if (a.samplers[i].path != b.samplers[i].path || a.samplers[i].flip_y != b.samplers[i].flip_y ||
            a.samplers[i].always_loaded != b.samplers[i].always_loaded) {
            return false;
        }
    }
    return true;
}

}  // namespace

SurfaceModes surface_modes(const ShaderParse& parse) {
    SurfaceModes modes;
    for (const std::string& mode : parse.header_modes.value_or(std::vector<std::string>{})) {
        modes.transparent = modes.transparent || mode == "transparent";
        modes.two_sided = modes.two_sided || mode == "two_sided";
        modes.no_shadow = modes.no_shadow || mode == "no_shadow";
    }
    return modes;
}

void describe_surface(SurfaceBinding& binding) {
    if (binding.parse == nullptr) {
        return;
    }
    binding.modes = surface_modes(*binding.parse);
    binding.reads_time = binding.parse->reads_time;
    binding.cuts = binding.parse->cuts;
    Hash hash;
    for (const SurfaceBinding::Sampler& sampler : binding.samplers) {
        hash.bytes(sampler.path.data(), sampler.path.size());
        hash.add(sampler.flip_y);
    }
    binding.samplers_hash = hash.value;
}

void SurfaceBinding::set_values(std::vector<LuaSlot> slots) const {
    auto next = std::make_shared<SurfaceValues>();
    Hash hash;
    for (const LuaSlot& value : slots) {
        hash.add(value.kind);
        hash.add(value.number);
        hash.add(value.flag);
        hash.add(value.vec);
        hash.add(value.color);
    }
    next->slots = std::move(slots);
    next->hash = hash.value;
    values_hash_.store(next->hash, std::memory_order_release);
    std::atomic_store(&values_, std::shared_ptr<const SurfaceValues>(std::move(next)));
}

namespace {

// What a surface Shader's binding carries beyond its uniforms: the modes, the
// margin, and the vertex stage. A post Shader's carries its stage.
void describe_shader(SurfaceBinding& binding, const Shader& shader) {
    describe_surface(binding);
    if (shader.kind() != ShaderKind::Surface) {
        binding.hdr = shader.hdr();
        return;
    }
    // The modes are the Shader's properties, not lines in its text.
    binding.modes.transparent = shader.transparent();
    binding.modes.two_sided = shader.two_sided();
    binding.modes.no_shadow = !shader.casts_shadow();
    binding.displacement_margin = static_cast<float>(shader.displacement_margin());
    const std::shared_ptr<const ShaderParse> vertex = shader.stage_parse(ShaderStage::Vertex);
    if (shader.has_vertex_stage() && vertex != nullptr && !vertex->error) {
        binding.has_vertex = true;
        binding.vertex_glsl = vertex->glsl;
        binding.vertex_reads_time = vertex->reads_time;
    }
}

}  // namespace

std::shared_ptr<const SurfaceBinding> make_surface_binding(const DataModel& world, const Material& material) {
    const Shader* shader = material.shader(world);
    return shader != nullptr ? make_shader_binding(world, material, *shader) : nullptr;
}

std::shared_ptr<const SurfaceBinding> make_effect_binding(const DataModel& world, const ShaderEffect& effect) {
    const Shader* shader = effect.shader(world);
    return shader != nullptr ? make_shader_binding(world, effect, *shader) : nullptr;
}

std::shared_ptr<const SurfaceBinding> make_shader_binding(const DataModel& world, const DataModel& holder,
                                                          const Shader& shader) {
    auto binding = std::make_shared<SurfaceBinding>();
    binding->shader = shader.id();
    binding->version = shader.source_version();
    binding->parse = shader.parse();
    std::vector<LuaSlot> slots;
    for (const ShaderParam& param : binding->parse->params) {
        const LuaSlot held = holder.attribute(param.name);
        if (param.type == ShaderParamType::Sampler) {
            SurfaceBinding::Sampler& sampler = binding->samplers.emplace_back();
            const auto* texture =
                held.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Texture*>(world.instance(held.id)) : nullptr;
            if (texture != nullptr) {
                sampler.path = texture->path();
                sampler.flip_y = texture->flip_y();
                sampler.always_loaded = texture->streaming() == TextureStreaming::AlwaysLoaded;
            }
            slots.emplace_back();
            continue;
        }
        const LuaSlot& value = held.kind == slot_kind(param.type) ? held : param.fallback;
        // Only what a uniform reads, so equal values compare and hash equal.
        LuaSlot kept;
        kept.kind = value.kind;
        kept.number = value.number;
        kept.flag = value.flag;
        kept.vec = value.vec;
        kept.color = value.color;
        if (kept.kind == LuaSlot::Kind::Number) {
            if (param.type == ShaderParamType::Int) {
                kept.number = std::trunc(kept.number);
            }
            if (param.ranged) {
                kept.number = std::clamp(kept.number, param.min, param.max);
            }
        }
        slots.push_back(std::move(kept));
    }
    describe_shader(*binding, shader);
    binding->set_values(std::move(slots));
    return binding;
}

std::shared_ptr<const SurfaceBinding> make_default_surface_binding(const Shader& shader) {
    auto binding = std::make_shared<SurfaceBinding>();
    binding->shader = shader.id();
    binding->version = shader.source_version();
    binding->parse = shader.parse();
    std::vector<LuaSlot> slots;
    for (const ShaderParam& param : binding->parse->params) {
        if (param.type == ShaderParamType::Sampler) {
            binding->samplers.emplace_back();
            slots.emplace_back();
            continue;
        }
        slots.push_back(param.fallback);
    }
    describe_shader(*binding, shader);
    binding->set_values(std::move(slots));
    return binding;
}

void SurfaceBindingCache::begin() {
    previous_.swap(current_);
    current_.clear();
}

std::shared_ptr<const SurfaceBinding> SurfaceBindingCache::get(const DataModel& world, const Material* material) {
    if (material == nullptr) {
        return nullptr;
    }
    return get(material->id(), [&] { return make_surface_binding(world, *material); });
}

std::shared_ptr<const SurfaceBinding> SurfaceBindingCache::get(const DataModel& world, const ShaderEffect* effect) {
    if (effect == nullptr) {
        return nullptr;
    }
    return get(effect->id(), [&] { return make_effect_binding(world, *effect); });
}

template <class Make>
std::shared_ptr<const SurfaceBinding> SurfaceBindingCache::get(InstanceId id, Make make) {
    if (const auto found = current_.find(id); found != current_.end()) {
        return found->second;
    }
    std::shared_ptr<const SurfaceBinding> made = make();
    if (made != nullptr) {
        if (const auto last = previous_.find(id);
            last != previous_.end() && last->second != nullptr && same_binding(*last->second, *made)) {
            // Kept, with only its values swapped when they moved: every draw
            // of the Material stays the same draw.
            const auto fresh = made->values();
            if (!same_values(*last->second->values(), *fresh)) {
                last->second->set_values(fresh->slots);
            }
            made = last->second;
        }
    }
    current_.emplace(id, made);
    return made;
}

}  // namespace engine_core
