#pragma once

// TerrainTextures: decides each Terrain's texture layers (one per distinct
// Material its TerrainMaterials use, plus layer 0 for the untextured
// default), builds them off SimulationThread on its own worker thread, caches
// built layers across Terrains that share a Material at the same size, and
// publishes an immutable TerrainTextureSet the renderer will upload (Task 6).
// Task 5 of the terrain textures sub-project.
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

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace engine_core {

// One Terrain's texture arrays, immutable once published. layers[0] is the
// untextured default (white, 0.5 height, flat normal, roughness 1, metalness
// 1); layers[1..] are one per distinct Material in TerrainWorld::rebuild_look's
// layer_of order.
struct TerrainTextureSet {
    int size = 0;
    std::vector<std::shared_ptr<const terrain::LayerBytes>> layers;
    std::uint64_t revision = 0;
};

class TerrainTextures {
public:
    TerrainTextures();
    ~TerrainTextures();

    TerrainTextures(const TerrainTextures&) = delete;
    TerrainTextures& operator=(const TerrainTextures&) = delete;

    // SimulationThread, under game's write lock (the Engine's contract, as
    // TerrainWorld::update's). For each Terrain in Workspace: finds the
    // distinct Materials its TerrainMaterials use (stable order: existing
    // layers keep their index, a newly seen Material is appended), resolves
    // each one's five texture files under game.resources_root(), and queues a
    // build for any layer whose resolved paths, their last_write_times, or
    // the Terrain's TextureSize changed since it was last built. A file's
    // stamp is checked at most once a second, as TextureCache does. Publishes
    // a new TerrainTextureSet once every layer a request covers has built.
    void update(DataModel& game);

    // The layer index material draws as on terrain, or 0 (the untextured
    // default) when material is nil, dead, not in use on terrain, or terrain
    // is not one this TerrainTextures has seen.
    int layer_of(InstanceId terrain, InstanceId material) const;
    // The latest published set for terrain, or null before its first publish
    // or once it leaves Workspace.
    std::shared_ptr<const TerrainTextureSet> published(InstanceId terrain) const;
    // The published set's layers' bytes, in memory: layers * size^2 * 4 * 2 *
    // 4/3 (two RGBA8 arrays, full mip chains). 0 with nothing published yet.
    std::size_t memory_bytes(InstanceId terrain) const;

    // Tests: blocks until the worker has no request running or queued.
    void wait_idle();

private:
    // What one layer was last (or is about to be) built from: the five
    // resolved source paths, their stamps, and the size, so a later update()
    // can tell whether it changed. bytes is the layer's current pointer (the
    // one in the latest published set, once a build lands).
    struct LayerSlot {
        InstanceId material = 0;
        terrain::LayerSources sources;
        std::array<std::filesystem::file_time_type, 5> stamps{};
        int size = 0;
        std::shared_ptr<const terrain::LayerBytes> bytes;
    };

    struct TerrainRecord {
        std::vector<LayerSlot> layers{LayerSlot{}};   // index 0: the untextured default
        std::chrono::steady_clock::time_point stamps_checked_at{};
        bool stamps_checked_once = false;
        std::uint64_t generation = 0;   // bumped each time a new request is queued for this terrain
        std::shared_ptr<const TerrainTextureSet> published;
    };

    // One layer a request asks the worker to settle: either reused as it is
    // (needs_build false, carry already holds its bytes) or rebuilt from
    // sources at the request's size.
    struct LayerPlan {
        bool needs_build = false;
        terrain::LayerSources sources;
        std::shared_ptr<const terrain::LayerBytes> carry;
    };

    // A Terrain's whole set of layers to settle, queued as one unit: the
    // ruling's "at most one queued request per Terrain" -- a newer update()
    // for the same Terrain replaces it in queued_ rather than adding another.
    struct Request {
        InstanceId terrain = 0;
        std::uint64_t generation = 0;
        int size = 0;
        std::vector<LayerPlan> layers;
    };

    struct BuiltResult {
        InstanceId terrain = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<const TerrainTextureSet> set;
    };

    // The built-layer cache's key: build_layer's inputs. Terrains sharing a
    // Material at the same size, with the same files on disk, share the
    // result (a weak_ptr: it expires once no published set holds it anymore).
    struct CacheKey {
        std::array<std::filesystem::path, 5> paths;
        std::array<std::filesystem::file_time_type, 5> stamps;
        int size = 0;
        bool operator<(const CacheKey& other) const;
    };

    void worker_loop();
    // Resolves material's five texture references under root, with stamps:
    // empty when the reference is nil, dead, or its file does not exist.
    static terrain::LayerSources resolve_sources(DataModel& game, const std::filesystem::path& root,
                                                  InstanceId material);
    static std::array<std::filesystem::file_time_type, 5> stamp_sources(const terrain::LayerSources& sources);
    std::shared_ptr<const terrain::LayerBytes> build_or_share(const terrain::LayerSources& sources, int size);

    std::unordered_map<InstanceId, TerrainRecord> terrains_;   // SimulationThread only

    // Guards everything below, so published()/memory_bytes() and the worker
    // thread can be called from any thread while update() runs on
    // SimulationThread.
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<InstanceId, Request> queued_;    // at most one per Terrain, replaced by a newer update()
    std::deque<InstanceId> order_;                       // FIFO of Terrains with a queued_ entry
    bool current_running_ = false;                       // the worker holds a request right now
    bool stopping_ = false;
    std::vector<BuiltResult> results_;                    // finished since the last update()
    std::uint64_t next_revision_ = 1;                     // unique across every TerrainTextureSet published
    // The latest set update() accepted for each Terrain still in Workspace;
    // published()/memory_bytes()'s own view, kept apart from terrains_ so
    // reading it never races SimulationThread's use of terrains_ itself.
    std::unordered_map<InstanceId, std::shared_ptr<const TerrainTextureSet>> published_;

    // The built-layer cache, guarded by its own mutex (the worker and
    // update()'s cache reads/writes never need terrains_'s lock together).
    std::mutex cache_mutex_;
    std::map<CacheKey, std::weak_ptr<const terrain::LayerBytes>> cache_;

    std::thread worker_;
};

}  // namespace engine_core
