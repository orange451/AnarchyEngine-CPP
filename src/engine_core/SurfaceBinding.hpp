#pragma once

#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "ShaderSource.hpp"

#include <atomic>
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

// A Material's uniform values, as its surface Shader reads them.
struct SurfaceValues {
    // One per parse->params, in order: the Material's attribute when its kind
    // fits the uniform's type, else the uniform's default; a ranged number held
    // to its range, an int whole, a color a Color. A sampler's is Nil.
    std::vector<LuaSlot> slots;
    // Of slots.
    std::uint64_t hash = 0;
};

// What a Material's surface Shader needs to draw, resolved on the simulation
// thread and shared by every mesh that draws the Material. All but its values
// is immutable once built; while only the values change, SurfaceBindingCache
// keeps the binding and swaps them (set_values), so no draw of the Material
// changes and the renderer reads the new ones at bind time.
struct SurfaceBinding {
    InstanceId shader = 0;
    // The Shader's source_version.
    std::uint64_t version = 0;
    std::shared_ptr<const ShaderParse> parse;
    // From parse's render_mode words.
    SurfaceModes modes;
    struct Sampler {
        std::string path;
        bool flip_y = false;
        bool always_loaded = false;
    };
    // One per sampler param, in order: the Texture the attribute names; an
    // empty path for none.
    std::vector<Sampler> samplers;
    // The Shader has a vertex stage that parses: its GLSL, and whether it
    // names TIME (its shadows then redraw each frame too).
    bool has_vertex = false;
    std::string vertex_glsl;
    bool vertex_reads_time = false;
    // How far the vertex stage may move a vertex, in units (culling grows a
    // draw's box by it).
    float displacement_margin = 1.f;
    // The glsl names TIME, so its shadows redraw each frame.
    bool reads_time = false;
    // The glsl names discard or ALPHA, so it can cut holes, and its shadows
    // draw through it; without, it casts as the plain Material does.
    bool cuts = false;
    // Of the sampler paths and flips, for shadow fingerprints.
    std::uint64_t samplers_hash = 0;

    // The values now, from any thread.
    std::shared_ptr<const SurfaceValues> values() const { return std::atomic_load(&values_); }
    std::uint64_t values_hash() const { return values_hash_.load(std::memory_order_acquire); }
    // Publishes slots as the values, hashed. Before the binding is shared, or
    // on the simulation thread through SurfaceBindingCache.
    void set_values(std::vector<LuaSlot> slots) const;

private:
    mutable std::shared_ptr<const SurfaceValues> values_ = std::make_shared<const SurfaceValues>();
    mutable std::atomic<std::uint64_t> values_hash_{0};
};

// Sets binding's modes, reads_time, and cuts from its parse, and samplers_hash
// from its samplers.
void describe_surface(SurfaceBinding& binding);

class Shader;
// shader's binding with every uniform at its default and no textures, as the
// Shader Editor previews it; null for a post Shader.
std::shared_ptr<const SurfaceBinding> make_default_surface_binding(const Shader& shader);

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
