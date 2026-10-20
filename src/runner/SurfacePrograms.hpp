#pragma once

#include "RenderProgram.hpp"
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
        // The G-buffer pass's; id 0 when not built or it failed.
        Variant gbuffer;
        // "" or "<line>: <message>", at the user's line.
        std::string error;
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
    // Errors found since the last call, as (shader id, version, text).
    std::vector<std::tuple<engine_core::InstanceId, std::uint64_t, std::string>> take_errors();
    // Deletes programs of shaders not asked for in the 60 seconds before now.
    void reclaim(double now);
    // Deletes every GL object.
    void release();
    // How many programs have been built, for tests.
    std::uint64_t builds() const { return builds_; }

private:
    void build(const engine_core::SurfaceBinding& binding, Built& built);
    static void destroy(Built& built);

    Finish finish_;
    std::unordered_map<engine_core::InstanceId, Built> built_;
    std::vector<std::tuple<engine_core::InstanceId, std::uint64_t, std::string>> errors_;
    std::uint64_t builds_ = 0;
};

}  // namespace runner
