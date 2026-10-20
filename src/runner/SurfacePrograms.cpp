#include "SurfacePrograms.hpp"

#include "ShaderAssembly.hpp"
#include "ShaderFile.hpp"
#include "gl.hpp"

namespace runner {
namespace {

// Seconds a Shader's programs live after the last draw that asked for them.
constexpr double kUnusedSeconds = 60.0;

// The Variant's per-param locations, and each sampler pointed at its unit.
void FindParams(const engine_core::ShaderParse& parse, SurfacePrograms::Variant& variant) {
    const unsigned id = variant.program.id;
    variant.locations.assign(parse.params.size(), -1);
    glUseProgram(id);
    int sampler = 0;
    for (std::size_t i = 0; i < parse.params.size(); ++i) {
        const engine_core::ShaderParam& param = parse.params[i];
        variant.locations[i] = glGetUniformLocation(id, param.name.c_str());
        if (param.type == engine_core::ShaderParamType::Sampler) {
            if (variant.locations[i] >= 0) {
                glUniform1i(variant.locations[i], kSurfaceTextureUnit + sampler);
            }
            ++sampler;
        }
    }
    glUseProgram(0);
}

}  // namespace

const SurfacePrograms::Built& SurfacePrograms::get(const engine_core::SurfaceBinding& binding, double now) {
    static const Built kNone;
    if (binding.parse == nullptr || binding.parse->error || binding.parse->kind != engine_core::ShaderKind::Surface) {
        return kNone;
    }
    auto found = built_.find(binding.shader);
    if (found == built_.end()) {
        found = built_.emplace(binding.shader, Built{}).first;
        build(binding, found->second);
    } else if (found->second.version != binding.version) {
        destroy(found->second);
        build(binding, found->second);
    }
    found->second.used = now;
    return found->second;
}

void SurfacePrograms::build(const engine_core::SurfaceBinding& binding, Built& built) {
    built = Built{};
    built.version = binding.version;
    ++builds_;
    const AssembledSource fragment = AssembleSurface("pipeline/deferred.frag",
                                                     {"pipeline/gbuffer.glsl", "pipeline/surface.glsl"},
                                                     binding.parse->glsl, true);
    std::string log;
    built.gbuffer.program.id = LinkProgramLog(LoadShader("pipeline/geometry.vert"), fragment.text, log);
    if (built.gbuffer.program.id == 0) {
        built.error = MapShaderError(log, fragment);
        if (built.error.empty()) {
            built.error = "1: the shader did not build";
        }
        errors_.emplace_back(binding.shader, binding.version, built.error);
        return;
    }
    if (finish_) {
        finish_(built.gbuffer.program);
    }
    FindParams(*binding.parse, built.gbuffer);
}

void SurfacePrograms::destroy(Built& built) {
    for (Variant* variant : {&built.gbuffer}) {
        if (variant->program.id != 0) {
            glDeleteProgram(variant->program.id);
        }
        *variant = Variant{};
    }
}

std::vector<std::tuple<engine_core::InstanceId, std::uint64_t, std::string>> SurfacePrograms::take_errors() {
    auto out = std::move(errors_);
    errors_.clear();
    return out;
}

void SurfacePrograms::reclaim(double now) {
    for (auto it = built_.begin(); it != built_.end();) {
        if (now - it->second.used > kUnusedSeconds) {
            destroy(it->second);
            it = built_.erase(it);
        } else {
            ++it;
        }
    }
}

void SurfacePrograms::release() {
    for (auto& [id, built] : built_) {
        destroy(built);
    }
    built_.clear();
    errors_.clear();
}

}  // namespace runner
