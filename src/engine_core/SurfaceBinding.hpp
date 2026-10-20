#pragma once

#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "ShaderSource.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

class Material;

// A surface shader's render_mode words, as the passes read them.
struct SurfaceModes {
    // Drawn in the see-through pass whatever the Transparency.
    bool transparent = false;
    // No face culling.
    bool two_sided = false;
    // Casts no shadow.
    bool no_shadow = false;
};
SurfaceModes surface_modes(const ShaderParse& parse);

// What a Material's surface Shader needs to draw, resolved once per Prepare and
// shared by every mesh that draws the Material. Immutable once built.
struct SurfaceBinding {
    InstanceId shader = 0;
    // The Shader's source_version.
    std::uint64_t version = 0;
    std::shared_ptr<const ShaderParse> parse;
    // From parse's render_mode words.
    SurfaceModes modes;
    // One per parse->params, in order: the Material's attribute when its kind
    // fits the uniform's type, else the uniform's default; a ranged number held
    // to its range, an int whole, a color a Color. A sampler's is Nil.
    std::vector<LuaSlot> values;
    struct Sampler {
        std::string path;
        bool flip_y = false;
        bool always_loaded = false;
    };
    // One per sampler param, in order: the Texture the attribute names; an
    // empty path for none.
    std::vector<Sampler> samplers;
    // The glsl names TIME, so its shadows redraw each frame.
    bool reads_time = false;
    // Of values and sampler paths, for shadow fingerprints.
    std::uint64_t values_hash = 0;
};

// The binding for material, or null when it has no live surface Shader.
// Callers hold the world's lock.
std::shared_ptr<const SurfaceBinding> make_surface_binding(const DataModel& world, const Material& material);

// Each Material's binding, built once per frame and kept from the frame before
// while nothing in it changed, so a draw's binding pointer stays put.
class SurfaceBindingCache {
public:
    // Starts a frame: what the last one built is kept only for reuse.
    void begin();
    // The binding for material this frame, or null for none; material may be null.
    std::shared_ptr<const SurfaceBinding> get(const DataModel& world, const Material* material);

private:
    std::unordered_map<InstanceId, std::shared_ptr<const SurfaceBinding>> current_;
    std::unordered_map<InstanceId, std::shared_ptr<const SurfaceBinding>> previous_;
};

}  // namespace engine_core
