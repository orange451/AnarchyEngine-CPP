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

bool SurfacePrograms::link(const engine_core::SurfaceBinding& binding, const std::string& vertex,
                           const AssembledSource& fragment, Variant& variant, Built& built) {
    std::string log;
    variant.program.id = LinkProgramLog(vertex, fragment.text, log);
    if (variant.program.id == 0) {
        // One error a version: the first pass's that fails.
        if (built.error.empty()) {
            built.error = MapShaderError(log, fragment);
            if (built.error.empty()) {
                built.error = "1: the shader did not build";
            }
            errors_.emplace_back(binding.shader, binding.version, built.error);
        }
        return false;
    }
    if (finish_) {
        finish_(variant.program);
    }
    FindParams(*binding.parse, variant);
    return true;
}

void SurfacePrograms::build(const engine_core::SurfaceBinding& binding, Built& built) {
    built = Built{};
    built.version = binding.version;
    ++builds_;
    const std::string& glsl = binding.parse->glsl;
    const AssembledSource gbuffer =
        AssembleSurface("pipeline/deferred.frag", {"pipeline/gbuffer.glsl", "pipeline/surface.glsl"}, glsl, true);
    const std::string geometry = LoadShader("pipeline/geometry.vert");
    if (!link(binding, geometry, gbuffer, built.gbuffer, built)) {
        return;
    }
    AssembledSource forward = AssembleSurface("pipeline/forward.frag",
                                              {"pipeline/surface.glsl", "pipeline/lighting.glsl",
                                               "pipeline/environment.glsl", "pipeline/image_lighting.glsl",
                                               "pipeline/atmosphere.glsl"},
                                              glsl, true);
    if (binding.modes.transparent) {
        // forward.frag cuts only where ALPHA is none; the user's lines move down one.
        const std::string mark = "#define SURFACE_SHADER\n";
        const std::size_t at = forward.text.find(mark);
        if (at != std::string::npos) {
            forward.text.insert(at + mark.size(), "#define SURFACE_TRANSPARENT\n");
            ++forward.user_first_line;
        }
    }
    link(binding, geometry, forward, built.forward, built);
    if (binding.cuts) {
        ++shadowBuilds_;
        const AssembledSource vertex = AssembleSurface("pipeline/shadow.vert", {}, std::string(), false);
        const AssembledSource shadow = AssembleSurface("pipeline/shadow.frag", {"pipeline/surface.glsl"}, glsl, true);
        link(binding, vertex.text, shadow, built.shadow, built);
    }
}

void SurfacePrograms::destroy(Built& built) {
    for (Variant* variant : {&built.gbuffer, &built.forward, &built.shadow}) {
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
