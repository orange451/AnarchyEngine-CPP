#include "TerrainTextures.hpp"

#include "AssetInstances.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace engine_core {

namespace {

int texture_size_pixels(TextureSize size) {
    switch (size) {
        case TextureSize::Small:
            return 256;
        case TextureSize::Medium:
            return 512;
        case TextureSize::Large:
            return 1024;
        case TextureSize::Max:
            return 2048;
    }
    return 1024;
}

bool same_sources(const terrain::LayerSources& a, const terrain::LayerSources& b) {
    return a.diffuse == b.diffuse && a.normal == b.normal && a.roughness == b.roughness &&
           a.metalness == b.metalness && a.height == b.height;
}

// The live T a ReferenceAsset's reference at index holds, or null. As
// SnapshotPump.cpp's own ReferencedAs, which is private to that file.
template <typename T>
const T* referenced_as(DataModel& game, const ReferenceAsset& asset, std::size_t index) {
    const LuaSlot slot = asset.reference(index);
    return slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const T*>(game.instance(slot.id)) : nullptr;
}

}  // namespace

bool TerrainTextures::CacheKey::operator<(const CacheKey& other) const {
    if (size != other.size) {
        return size < other.size;
    }
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (paths[i] != other.paths[i]) {
            return paths[i] < other.paths[i];
        }
    }
    for (std::size_t i = 0; i < stamps.size(); ++i) {
        if (stamps[i] != other.stamps[i]) {
            return stamps[i] < other.stamps[i];
        }
    }
    return false;
}

TerrainTextures::TerrainTextures() {
    worker_ = std::thread([this] { worker_loop(); });
}

TerrainTextures::~TerrainTextures() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

terrain::LayerSources TerrainTextures::resolve_sources(DataModel& game, const std::filesystem::path& root,
                                                        InstanceId material_id) {
    terrain::LayerSources sources;
    if (material_id == 0 || root.empty()) {
        return sources;
    }
    const auto* material = dynamic_cast<const Material*>(game.instance(material_id));
    if (material == nullptr) {
        return sources;
    }
    const auto resolve = [&](std::size_t index) -> std::filesystem::path {
        const Texture* texture = referenced_as<Texture>(game, *material, index);
        if (texture == nullptr || texture->path().empty()) {
            return {};
        }
        return root / std::filesystem::u8path(texture->path());
    };
    sources.diffuse = resolve(Material::kDiffuseTextureReference);
    sources.normal = resolve(Material::kNormalTextureReference);
    sources.roughness = resolve(Material::kRoughnessTextureReference);
    sources.metalness = resolve(Material::kMetalnessTextureReference);
    sources.height = resolve(Material::kHeightTextureReference);
    return sources;
}

std::array<std::filesystem::file_time_type, 5> TerrainTextures::stamp_sources(const terrain::LayerSources& sources) {
    std::array<std::filesystem::file_time_type, 5> stamps{};
    const std::filesystem::path* paths[5] = {&sources.diffuse, &sources.normal, &sources.roughness,
                                             &sources.metalness, &sources.height};
    for (std::size_t i = 0; i < 5; ++i) {
        if (paths[i]->empty()) {
            continue;
        }
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(*paths[i], error);
        if (!error) {
            stamps[i] = stamp;
        }
        // A missing file keeps the default stamp; LayerBuilder's own warning
        // covers telling anyone about it, same as a texture that never
        // existed never changing TextureCache's entry either.
    }
    return stamps;
}

std::shared_ptr<const terrain::LayerBytes> TerrainTextures::build_or_share(const terrain::LayerSources& sources,
                                                                           int size) {
    CacheKey key;
    key.paths = {sources.diffuse, sources.normal, sources.roughness, sources.metalness, sources.height};
    key.stamps = stamp_sources(sources);
    key.size = size;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto found = cache_.find(key);
        if (found != cache_.end()) {
            if (std::shared_ptr<const terrain::LayerBytes> existing = found->second.lock()) {
                return existing;   // TT2: another Terrain already built this Material at this size
            }
        }
    }
    auto built = std::make_shared<const terrain::LayerBytes>(terrain::build_layer(sources, size));
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[key] = built;
        // Weak entries whose LayerBytes nothing published holds anymore (TT5:
        // a superseded size or a Material no Terrain uses) are dropped here
        // rather than left to grow the map forever.
        for (auto it = cache_.begin(); it != cache_.end();) {
            it = it->second.expired() ? cache_.erase(it) : std::next(it);
        }
    }
    return built;
}

void TerrainTextures::worker_loop() {
    while (true) {
        InstanceId terrain_id = 0;
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !order_.empty(); });
            if (stopping_) {
                return;   // drops whatever is still queued, as TerrainMesher's dtor does
            }
            terrain_id = order_.front();
            order_.pop_front();
            const auto found = queued_.find(terrain_id);
            if (found == queued_.end()) {
                continue;   // defensive: every order_ entry has a queued_ entry
            }
            request = std::move(found->second);
            queued_.erase(found);
            current_running_ = true;
        }

        std::vector<std::shared_ptr<const terrain::LayerBytes>> layers;
        layers.reserve(request.layers.size());
        for (LayerPlan& plan : request.layers) {
            layers.push_back(plan.needs_build ? build_or_share(plan.sources, request.size) : plan.carry);
        }
        auto set = std::make_shared<TerrainTextureSet>();
        set->size = request.size;
        set->layers = std::move(layers);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            set->revision = next_revision_++;
            results_.push_back(BuiltResult{terrain_id, request.generation, std::move(set)});
            current_running_ = false;
        }
        cv_.notify_all();   // wait_idle, and a result waiting to be drained
    }
}

void TerrainTextures::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return order_.empty() && !current_running_; });
}

void TerrainTextures::update(DataModel& game) {
    // SimulationThread, under game's write lock (the Engine's contract, as
    // TerrainWorld::update's).

    // 1. Drain finished builds, dropping any result a newer request for its
    // Terrain already superseded (the ruling's "newer supersedes older").
    std::vector<BuiltResult> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished.swap(results_);
    }
    for (BuiltResult& result : finished) {
        const auto found = terrains_.find(result.terrain);
        if (found == terrains_.end() || result.generation != found->second.generation) {
            continue;   // the Terrain left, or a later update() already replaced this request
        }
        TerrainRecord& record = found->second;
        const std::size_t count = std::min(result.set->layers.size(), record.layers.size());
        for (std::size_t i = 0; i < count; ++i) {
            record.layers[i].bytes = result.set->layers[i];
        }
        record.published = result.set;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            published_[result.terrain] = std::move(result.set);
        }
    }

    // 2. Drop records (and their published entry) for Terrains no longer in
    // Workspace; a later return re-queues everything (bytes == nullptr).
    std::vector<InstanceId> current;
    game.terrains(current);
    for (auto it = terrains_.begin(); it != terrains_.end();) {
        if (std::find(current.begin(), current.end(), it->first) == current.end()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                published_.erase(it->first);
            }
            it = terrains_.erase(it);
        } else {
            ++it;
        }
    }

    const std::filesystem::path root = game.resources_root();
    const auto now = std::chrono::steady_clock::now();

    for (InstanceId id : current) {
        auto* terrain = dynamic_cast<Terrain*>(game.instance(id));
        if (terrain == nullptr) {
            continue;
        }
        TerrainRecord& record = terrains_[id];
        const int size = texture_size_pixels(terrain->texture_size());

        // The distinct Materials this Terrain's TerrainMaterials use, stable
        // order: every Material already holding a layer keeps it; a newly
        // seen one is appended.
        std::vector<InstanceId> seen;
        seen.reserve(record.layers.size());
        for (std::size_t i = 1; i < record.layers.size(); ++i) {
            seen.push_back(record.layers[i].material);
        }
        for (TerrainMaterial* entry : terrain->materials()) {
            const InstanceId material_id = entry->material_instance();
            if (material_id == 0 || dynamic_cast<const Material*>(game.instance(material_id)) == nullptr) {
                continue;   // unassigned, nil, or dead: draws the untextured default (layer 0)
            }
            if (std::find(seen.begin(), seen.end(), material_id) == seen.end()) {
                seen.push_back(material_id);
                LayerSlot slot;
                slot.material = material_id;
                record.layers.push_back(std::move(slot));
            }
        }

        // A file's mtime stamp is checked at most once a second (as
        // TextureCache does) -- that is disk I/O, worth throttling. Which
        // Texture a Material's reference points at (resolve_sources) is an
        // in-memory read of that reference alone, so it costs nothing to
        // check every update(): without this, reassigning a Material's
        // NormalTexture (or any other reference) between two stamp checks
        // less than a second apart would sit unnoticed until the throttle
        // next let a disk check through, instead of rebuilding the one
        // layer it touched within this very update (TL-T2).
        const bool check_stamps = !record.stamps_checked_once || now - record.stamps_checked_at >= std::chrono::seconds(1);

        Request request;
        request.terrain = id;
        request.size = size;
        request.layers.resize(record.layers.size());
        bool any_change = false;
        for (std::size_t i = 0; i < record.layers.size(); ++i) {
            LayerSlot& slot = record.layers[i];
            LayerPlan& plan = request.layers[i];
            terrain::LayerSources sources = i == 0 ? terrain::LayerSources{} : resolve_sources(game, root, slot.material);
            const bool sources_changed = !same_sources(slot.sources, sources);
            bool changed = slot.bytes == nullptr || slot.size != size || sources_changed;
            std::array<std::filesystem::file_time_type, 5> stamps = slot.stamps;
            if (changed) {
                // Already rebuilding (a new/changed reference, a missing
                // build, or a size change): fresh stamps for the cache key
                // and for the next throttled comparison, regardless of
                // check_stamps.
                stamps = stamp_sources(sources);
            } else if (check_stamps) {
                stamps = stamp_sources(sources);
                changed = slot.stamps != stamps;   // the same file(s), touched on disk
            }
            if (changed) {
                plan.needs_build = true;
                plan.sources = sources;
                any_change = true;
                slot.sources = sources;
                slot.stamps = stamps;
                slot.size = size;
            } else {
                plan.carry = slot.bytes;
            }
        }
        if (check_stamps) {
            record.stamps_checked_once = true;
            record.stamps_checked_at = now;
        }

        if (!any_change) {
            continue;   // nothing to (re)build; record.published already reflects this state
        }
        request.generation = ++record.generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool already_queued = queued_.count(id) != 0;
            queued_[id] = std::move(request);
            if (!already_queued) {
                order_.push_back(id);
            }
        }
        cv_.notify_all();
    }
}

int TerrainTextures::layer_of(InstanceId terrain, InstanceId material) const {
    // SimulationThread only: terrains_ is update()'s own bookkeeping, read
    // here the way TerrainWorld::rebuild_look reads its own per-record state.
    if (material == 0) {
        return 0;
    }
    const auto found = terrains_.find(terrain);
    if (found == terrains_.end()) {
        return 0;
    }
    const TerrainRecord& record = found->second;
    for (std::size_t i = 1; i < record.layers.size(); ++i) {
        if (record.layers[i].material == material) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

std::shared_ptr<const TerrainTextureSet> TerrainTextures::published(InstanceId terrain) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = published_.find(terrain);
    return found != published_.end() ? found->second : nullptr;
}

std::size_t TerrainTextures::memory_bytes(InstanceId terrain) const {
    const std::shared_ptr<const TerrainTextureSet> set = published(terrain);
    if (!set) {
        return 0;
    }
    return set->layers.size() * terrain::layer_bytes(set->size);
}

}  // namespace engine_core
