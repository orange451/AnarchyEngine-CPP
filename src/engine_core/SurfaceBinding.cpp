#include "SurfaceBinding.hpp"

#include "AssetInstances.hpp"
#include "Shader.hpp"

#include <algorithm>
#include <cmath>
#include <regex>

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

bool same_binding(const SurfaceBinding& a, const SurfaceBinding& b) {
    if (a.shader != b.shader || a.version != b.version || a.parse != b.parse || a.values.size() != b.values.size() ||
        a.samplers.size() != b.samplers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.values.size(); ++i) {
        if (!same_value(a.values[i], b.values[i])) {
            return false;
        }
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
    for (const std::string& mode : parse.modes) {
        modes.transparent = modes.transparent || mode == "transparent";
        modes.two_sided = modes.two_sided || mode == "two_sided";
        modes.no_shadow = modes.no_shadow || mode == "no_shadow";
    }
    return modes;
}

std::shared_ptr<const SurfaceBinding> make_surface_binding(const DataModel& world, const Material& material) {
    const Shader* shader = material.shader(world);
    if (shader == nullptr) {
        return nullptr;
    }
    auto binding = std::make_shared<SurfaceBinding>();
    binding->shader = shader->id();
    binding->version = shader->source_version();
    binding->parse = shader->parse();
    binding->modes = surface_modes(*binding->parse);
    static const std::regex time_word(R"(\bTIME\b)");
    binding->reads_time = std::regex_search(binding->parse->glsl, time_word);
    Hash hash;
    for (const ShaderParam& param : binding->parse->params) {
        const LuaSlot held = material.attribute(param.name);
        if (param.type == ShaderParamType::Sampler) {
            SurfaceBinding::Sampler& sampler = binding->samplers.emplace_back();
            const auto* texture =
                held.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Texture*>(world.instance(held.id)) : nullptr;
            if (texture != nullptr) {
                sampler.path = texture->path();
                sampler.flip_y = texture->flip_y();
                sampler.always_loaded = texture->streaming() == TextureStreaming::AlwaysLoaded;
            }
            binding->values.emplace_back();
            hash.bytes(sampler.path.data(), sampler.path.size());
            hash.add(sampler.flip_y);
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
        hash.add(kept.kind);
        hash.add(kept.number);
        hash.add(kept.flag);
        hash.add(kept.vec);
        hash.add(kept.color);
        binding->values.push_back(std::move(kept));
    }
    binding->values_hash = hash.value;
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
    const InstanceId id = material->id();
    if (const auto found = current_.find(id); found != current_.end()) {
        return found->second;
    }
    std::shared_ptr<const SurfaceBinding> made = make_surface_binding(world, *material);
    if (made != nullptr) {
        if (const auto last = previous_.find(id);
            last != previous_.end() && last->second != nullptr && same_binding(*last->second, *made)) {
            made = last->second;
        }
    }
    current_.emplace(id, made);
    return made;
}

}  // namespace engine_core
