#pragma once

#include "RenderProgram.hpp"
#include "ShaderAssembly.hpp"
#include "ShaderSource.hpp"
#include "SurfaceBinding.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace runner {

// The texture unit a surface shader's first sampler2D uniform reads; the
// others follow it, up to engine_core's kMaxSurfaceSamplers (3).
constexpr int kSurfaceTextureUnit = 6;

// A Material's surface Shader built into the engine's passes, by Shader id;
// each built from one source_version, and built again only when it moves.
// Needs the GL context current; Renderer owns one.
class SurfacePrograms {
public:
    // One pass's program and its uniforms' locations, one per parse param (-1
    // for one the program does not read).
    struct Variant {
        RenderProgram program;
        std::vector<int> locations;
    };
    struct Built {
        std::uint64_t version = 0;
        // The G-buffer pass's and the see-through pass's; id 0 when not built
        // or it failed.
        Variant gbuffer;
        Variant forward;
        // The shadow maps', only for a shader that cuts holes (SurfaceBinding::cuts).
        Variant shadow;
        // "" or "<line>: <message>", at the user's line of error_stage.
        std::string error;
        engine_core::ShaderStage error_stage = engine_core::ShaderStage::Fragment;
        // When get last asked for it, in the seconds reclaim is given.
        double used = 0;
    };
    // Fills a newly built program's engine uniforms and sampler units, as the
    // Renderer fills its own; program.id is set.
    using Finish = std::function<void(RenderProgram&)>;

    void setFinish(Finish finish) { finish_ = std::move(finish); }
    // The programs for binding, built on first use or when its version moved.
    // A binding with no parse, a parse error, or a post shader has none. now
    // is in seconds, as reclaim takes it.
    const Built& get(const engine_core::SurfaceBinding& binding, double now);
    // Errors found since the last call, as (shader id, version, stage, "<line>: <message>").
    using Error = std::tuple<engine_core::InstanceId, std::uint64_t, engine_core::ShaderStage, std::string>;
    std::vector<Error> take_errors();
    // Deletes programs of shaders not asked for in the 60 seconds before now.
    void reclaim(double now);
    // Deletes every GL object.
    void release();
    // How many Shader versions have been built, and how many shadow programs, for tests.
    std::uint64_t builds() const { return builds_; }
    std::uint64_t shadowBuilds() const { return shadowBuilds_; }

private:
    void build(const engine_core::SurfaceBinding& binding, Built& built);
    // Links vertex and fragment into variant; false, with built's error set
    // once, when it fails. vertex_map, when given, is the vertex text with the
    // user's lines in it, so a vertex failure maps to them.
    bool link(const engine_core::SurfaceBinding& binding, const std::string& vertex, const AssembledSource* vertex_map,
              const AssembledSource& fragment, Variant& variant, Built& built);
    static void destroy(Built& built);

    Finish finish_;
    std::unordered_map<engine_core::InstanceId, Built> built_;
    std::vector<Error> errors_;
    std::uint64_t builds_ = 0;
    std::uint64_t shadowBuilds_ = 0;
};

}  // namespace runner
