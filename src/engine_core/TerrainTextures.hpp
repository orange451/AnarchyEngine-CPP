#pragma once

// TerrainTextures: decides each Terrain's texture layers (one per distinct
// Material its TerrainMaterials use, plus layer 0 for the untextured
// default), loads them off SimulationThread on the shared TexturePool,
// caches them across Terrains that share a Material at the same size and in
// the project's texture cache on disk, and publishes immutable
// TerrainTextureSets the renderer uploads.
//
// Terrain must never sit blank while it loads, so every layer is published
// the moment it is known, as a grey placeholder, and is replaced as soon as
// anything better lands: from a cache file, its small levels first and then
// each larger one; with no cache file, a 64-pixel preview of its diffuse
// first, then the full build (written to the cache for next time). Layers
// land one at a time, never waiting on each other.
//
// Owned by the Engine next to TerrainWorld; update() runs on SimulationThread,
// under the DataModel's write lock, once per tick. published() and
// memory_bytes() are safe to call from any thread (guarded by this object's
// own mutex, independent of the DataModel lock), so the IDE (read lock only)
// can show a Terrain's texture memory without touching SimulationThread.
// layer_of() reads update()'s own per-Terrain bookkeeping and so, like
// TerrainWorld::rebuild_look that calls it, is SimulationThread-only.

#include "DataModel.hpp"
#include "terrain/LayerBuilder.hpp"
#include "texture/TexturePool.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {

// One Terrain's texture layers, immutable once published. layers[0] is the
// untextured default (white, 0.5 height, flat normal, roughness 1, metalness
// 1); layers[1..] are one per distinct Material in TerrainWorld::rebuild_look's
// layer_of order. A layer may be a placeholder or partly loaded
// (LayerBytes::first_level above 0); layer_revisions[i] changes whenever
// layers[i] does, so an uploader redoes only the layers that changed.
struct TerrainTextureSet {
    int size = 0;
    std::vector<std::shared_ptr<const terrain::LayerBytes>> layers;
    std::vector<std::uint64_t> layer_revisions;
    std::uint64_t revision = 0;
};

class TerrainTextures {
public:
    // Loads on the process's shared pool, or on pool (which must outlive this).
    TerrainTextures();
    explicit TerrainTextures(texture::TexturePool& pool);
    ~TerrainTextures();

    TerrainTextures(const TerrainTextures&) = delete;
    TerrainTextures& operator=(const TerrainTextures&) = delete;

    // SimulationThread, under game's write lock (the Engine's contract, as
    // TerrainWorld::update's). Applies whatever loads finished since the last
    // call, then, for each Terrain in Workspace: finds the distinct Materials
    // its TerrainMaterials use (stable order: existing layers keep their
    // index, a newly seen Material is appended), resolves each one's five
    // texture files under game.resources_root(), and starts loading any layer
    // whose resolved paths, their last_write_times, or the Terrain's
    // TextureSize changed since it was last started. A file's stamp is
    // checked at most once a second, as TextureCache does. Publishes a new
    // TerrainTextureSet whenever any layer changed.
    void update(DataModel& game);

    // The layer index material draws as on terrain, or 0 (the untextured
    // default) when material is nil, dead, not in use on terrain, or terrain
    // is not one this TerrainTextures has seen.
    int layer_of(InstanceId terrain, InstanceId material) const;
    // The latest published set for terrain, or null before its first publish
    // or once it leaves Workspace.
    std::shared_ptr<const TerrainTextureSet> published(InstanceId terrain) const;
    // The published set's layers' bytes, in memory: layers * layer_bytes(size).
    // 0 with nothing published yet.
    std::size_t memory_bytes(InstanceId terrain) const;

    // What loading jobs share with this object, kept alive by the jobs
    // themselves so one still running after this is destroyed finds
    // stopping set and does nothing. Internal: public only so the job
    // functions in TerrainTextures.cpp can name it.
    struct Shared;

    // Tests: blocks until the pool has no job running or queued.
    void wait_idle();

private:
    // What one layer was last started from: the five resolved source paths,
    // their stamps, and the size, so a later update() can tell whether it
    // changed. key is the load that start asked for (texture::cache_key of
    // all three); bytes is what the layer shows now, and bytes_key the load
    // it came from (empty for a placeholder). Loads are shared by key, so
    // two Terrains using one Material at one size load it once.
    struct LayerSlot {
        InstanceId material = 0;
        terrain::LayerSources sources;
        std::array<std::filesystem::file_time_type, 5> stamps{};
        int size = 0;
        std::string key;
        std::string bytes_key;
        std::uint64_t revision = 0;
        std::shared_ptr<const terrain::LayerBytes> bytes;
    };

    struct TerrainRecord {
        std::vector<LayerSlot> layers{LayerSlot{}};   // index 0: the untextured default
        std::chrono::steady_clock::time_point stamps_checked_at{};
        bool stamps_checked_once = false;
        int size = 0;
    };


    // Starts loading slot's key, unless a load of it is already running,
    // or its whole layer is still in memory (then it is applied at once).
    // True when it applied a whole layer from memory to slot.
    bool start_layer(LayerSlot& slot, const std::filesystem::path& root);
    // Puts bytes, a load step of key, into every slot waiting on key, when
    // it is better than what the slot shows; adds each Terrain it changed to
    // changed.
    void land(const std::string& key, const std::shared_ptr<const terrain::LayerBytes>& bytes,
              std::vector<InstanceId>& changed);
    void publish(InstanceId terrain, TerrainRecord& record);

    static terrain::LayerSources resolve_sources(DataModel& game, const std::filesystem::path& root,
                                                  InstanceId material);
    static std::array<std::filesystem::file_time_type, 5> stamp_sources(const terrain::LayerSources& sources);

    texture::TexturePool& pool_;
    std::shared_ptr<Shared> shared_;
    std::unordered_map<InstanceId, TerrainRecord> terrains_;   // SimulationThread only
    std::uint64_t next_layer_revision_ = 1;                     // SimulationThread only

    // Guards published_ and next_revision_, so published()/memory_bytes()
    // can be called from any thread while update() runs on SimulationThread.
    mutable std::mutex mutex_;
    std::uint64_t next_revision_ = 1;   // unique across every set and layer published
    std::unordered_map<InstanceId, std::shared_ptr<const TerrainTextureSet>> published_;
};

}  // namespace engine_core
