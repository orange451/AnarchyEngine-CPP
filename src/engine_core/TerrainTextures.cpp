#include "TerrainTextures.hpp"

#include "AssetInstances.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"

#include "texture/Atex.hpp"
#include "texture/TextureBake.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
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

namespace {

// What every layer's cache key carries besides its five files and their
// times: anything else that changes the baked bytes.
std::string layer_settings(int size) {
    return "terrain|v1|size=" + std::to_string(size) + "|s3tc=" + (texture::s3tc_available() ? "1" : "0");
}

// The first level of a size chain no larger than 64 a side: what a cache
// read loads before anything else.
int first_small_level(int size) {
    int level = 0;
    for (int w = size; w > 64; w = std::max(1, (w + 1) / 2)) ++level;
    return level;
}

int chain_length(int size) {
    int levels = 1;
    for (int w = size; w > 1; w = std::max(1, (w + 1) / 2)) ++levels;
    return levels;
}

texture::BakedTexture to_baked(const terrain::LayerBytes& layer) {
    texture::BakedTexture baked;
    baked.width = baked.height = layer.size;
    const std::array<texture::PixelFormat, 3> formats = terrain::layer_formats();
    baked.formats.assign(formats.begin(), formats.end());
    baked.planes.assign(layer.planes.begin(), layer.planes.end());
    return baked;
}

}  // namespace

struct TerrainTextures::Shared {
    std::mutex mutex;
    bool stopping = false;
    // Load steps finished since update() last looked, in the order they landed.
    std::vector<std::pair<std::string, std::shared_ptr<const terrain::LayerBytes>>> landed;
    // Keys being loaded now; a slot asking for one waits for its steps.
    std::set<std::string> in_flight;
    // Whole layers by key, while any published set still holds them.
    std::map<std::string, std::weak_ptr<const terrain::LayerBytes>> whole;

    bool stopped() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping;
    }

    // One step of key's load: bytes, and whether it is the last.
    void post(const std::string& key, std::shared_ptr<const terrain::LayerBytes> bytes, bool last) {
        std::lock_guard<std::mutex> lock(mutex);
        if (last) {
            in_flight.erase(key);
            whole[key] = bytes;
            for (auto it = whole.begin(); it != whole.end();) {
                it = it->second.expired() ? whole.erase(it) : std::next(it);
            }
        }
        landed.emplace_back(key, std::move(bytes));
    }
};

TerrainTextures::TerrainTextures() : TerrainTextures(texture::TexturePool::shared()) {}

TerrainTextures::TerrainTextures(texture::TexturePool& pool) : pool_(pool), shared_(std::make_shared<Shared>()) {}

TerrainTextures::~TerrainTextures() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stopping = true;   // jobs still queued or running see it and stop
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

namespace {

using SharedPtr = std::shared_ptr<TerrainTextures::Shared>;

struct LoadJob {
    std::string key;
    terrain::LayerSources sources;
    int size = 0;
    std::filesystem::path root;    // the resources folder; empty with no project: nothing on disk
    std::filesystem::path cache;   // set by first_look, from the sources' bytes
};

void bake(const SharedPtr& shared, const LoadJob& job) {
    if (shared->stopped()) return;
    auto whole = std::make_shared<const terrain::LayerBytes>(
        terrain::compress_layer(terrain::build_layer(job.sources, job.size)));
    if (!job.cache.empty()) {
        // Next time reads it; this time needs it not, so a failure costs only that.
        std::string error;
        texture::write_atex(job.cache, to_baked(*whole), error);
    }
    shared->post(job.key, std::move(whole), true);
}

// Reads the level just above so_far's first, then queues the next.
void read_larger(texture::TexturePool& pool, const SharedPtr& shared, const LoadJob& job,
                 const texture::AtexHeader& header, std::shared_ptr<const terrain::LayerBytes> so_far) {
    pool.submit(texture::JobPriority::TerrainLargeLevels, [&pool, shared, job, header, so_far] {
        if (shared->stopped()) return;
        const int level = so_far->first_level - 1;
        std::optional<std::vector<std::vector<std::uint8_t>>> data = texture::read_atex_level(job.cache, header, level);
        if (!data) {
            // The file changed under this load: build the layer instead.
            pool.submit(texture::JobPriority::TerrainBake, [shared, job] { bake(shared, job); });
            return;
        }
        auto next = std::make_shared<terrain::LayerBytes>(*so_far);
        for (std::size_t p = 0; p < 3; ++p) next->planes[p][std::size_t(level)] = std::move((*data)[p]);
        next->first_level = level;
        std::shared_ptr<const terrain::LayerBytes> landed = next;
        shared->post(job.key, landed, level == 0);
        if (level > 0) read_larger(pool, shared, job, header, std::move(landed));
    });
}

// The first step of every load, at the pool's top priority: a cache file's
// small levels, or, with none, a preview of the diffuse alone. The rest
// follows at lower priority, so every layer gets its first look before any
// layer's full build.
void first_look(texture::TexturePool& pool, const SharedPtr& shared, LoadJob job) {
    if (shared->stopped()) return;
    if (!job.root.empty()) {
        // Named by the sources' bytes, read here off SimulationThread.
        const terrain::LayerSources& s = job.sources;
        job.cache = texture::cache_path(
            job.root, texture::content_key({s.diffuse, s.normal, s.roughness, s.metalness, s.height}, layer_settings(job.size)));
    }
    if (!job.cache.empty()) {
        const std::optional<texture::AtexHeader> header = texture::read_atex_header(job.cache);
        const std::array<texture::PixelFormat, 3> formats = terrain::layer_formats();
        const bool fits = header && header->width == job.size && header->height == job.size &&
                          header->levels == chain_length(job.size) &&
                          header->formats == std::vector<texture::PixelFormat>(formats.begin(), formats.end());
        if (fits) {
            auto layer = std::make_shared<terrain::LayerBytes>();
            layer->size = job.size;
            const int first = first_small_level(job.size);
            for (auto& plane : layer->planes) plane.resize(std::size_t(header->levels));
            bool ok = true;
            for (int level = header->levels - 1; level >= first && ok; --level) {
                std::optional<std::vector<std::vector<std::uint8_t>>> data =
                    texture::read_atex_level(job.cache, *header, level);
                ok = data.has_value();
                for (std::size_t p = 0; ok && p < 3; ++p) layer->planes[p][std::size_t(level)] = std::move((*data)[p]);
            }
            if (ok) {
                layer->first_level = first;
                std::shared_ptr<const terrain::LayerBytes> landed = layer;
                shared->post(job.key, landed, first == 0);
                if (first > 0) read_larger(pool, shared, job, *header, std::move(landed));
                return;
            }
        }
        if (header || std::filesystem::exists(job.cache)) {
            std::error_code error;
            std::filesystem::remove(job.cache, error);   // stale or broken: built again below
        }
    }
    shared->post(job.key, std::make_shared<const terrain::LayerBytes>(terrain::preview_layer(job.sources, job.size)),
                 false);
    pool.submit(texture::JobPriority::TerrainBake, [shared, job] { bake(shared, job); });
}

}  // namespace

bool TerrainTextures::start_layer(LayerSlot& slot, const std::filesystem::path& root) {
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        const auto found = shared_->whole.find(slot.key);
        if (found != shared_->whole.end()) {
            if (std::shared_ptr<const terrain::LayerBytes> existing = found->second.lock()) {
                // Another Terrain (or an earlier start) already holds it whole (TT2).
                slot.bytes = std::move(existing);
                slot.bytes_key = slot.key;
                slot.revision = next_layer_revision_++;
                return true;
            }
        }
        if (!shared_->in_flight.insert(slot.key).second) {
            return false;   // already loading: its steps land in this slot too
        }
    }
    LoadJob job;
    job.key = slot.key;
    job.sources = slot.sources;
    job.size = slot.size;
    job.root = root;
    SharedPtr shared = shared_;
    texture::TexturePool* pool = &pool_;
    pool_.submit(texture::JobPriority::TerrainPreview, [pool, shared, job] { first_look(*pool, shared, job); });
    return false;
}

void TerrainTextures::land(const std::string& key, const std::shared_ptr<const terrain::LayerBytes>& bytes,
                           std::vector<InstanceId>& changed) {
    for (auto& [id, record] : terrains_) {
        bool any = false;
        for (LayerSlot& slot : record.layers) {
            if (slot.key != key || bytes == nullptr || bytes->size != slot.size) continue;
            bool better;
            if (slot.bytes == nullptr || slot.bytes_key.empty() || slot.bytes->size != slot.size) {
                better = true;   // a placeholder: anything beats it
            } else if (slot.bytes_key != key) {
                better = bytes->first_level == 0;   // an older version keeps drawing until this one is whole
            } else {
                better = bytes->first_level < slot.bytes->first_level;
            }
            if (!better) continue;
            slot.bytes = bytes;
            slot.bytes_key = key;
            slot.revision = next_layer_revision_++;
            any = true;
        }
        if (any && std::find(changed.begin(), changed.end(), id) == changed.end()) changed.push_back(id);
    }
}

void TerrainTextures::publish(InstanceId terrain, TerrainRecord& record) {
    auto set = std::make_shared<TerrainTextureSet>();
    set->size = record.size;
    for (const LayerSlot& slot : record.layers) {
        set->layers.push_back(slot.bytes);
        set->layer_revisions.push_back(slot.revision);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    set->revision = next_revision_++;
    published_[terrain] = std::move(set);
}

void TerrainTextures::wait_idle() { pool_.wait_idle(); }

void TerrainTextures::update(DataModel& game) {
    // SimulationThread, under game's write lock (the Engine's contract, as
    // TerrainWorld::update's).

    // 1. Put every load step that landed into the slots waiting on it; a step
    // whose key no slot wants anymore (a superseded size or file) is dropped.
    std::vector<std::pair<std::string, std::shared_ptr<const terrain::LayerBytes>>> landed;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        landed.swap(shared_->landed);
    }
    std::vector<InstanceId> changed;
    for (const auto& [key, bytes] : landed) land(key, bytes, changed);

    // 2. Drop records (and their published entry) for Terrains no longer in
    // Workspace; a later return starts everything again.
    std::vector<InstanceId> current;
    game.terrains(current);
    for (auto it = terrains_.begin(); it != terrains_.end();) {
        if (std::find(current.begin(), current.end(), it->first) == current.end()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                published_.erase(it->first);
            }
            changed.erase(std::remove(changed.begin(), changed.end(), it->first), changed.end());
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
        bool publish_now = std::find(changed.begin(), changed.end(), id) != changed.end();
        record.size = size;

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
        // next let a disk check through, instead of restarting the one
        // layer it touched within this very update (TL-T2).
        const bool check_stamps = !record.stamps_checked_once || now - record.stamps_checked_at >= std::chrono::seconds(1);

        for (std::size_t i = 0; i < record.layers.size(); ++i) {
            LayerSlot& slot = record.layers[i];
            terrain::LayerSources sources = i == 0 ? terrain::LayerSources{} : resolve_sources(game, root, slot.material);
            const bool sources_changed = !same_sources(slot.sources, sources);
            bool restart = slot.key.empty() || slot.size != size || sources_changed;
            std::array<std::filesystem::file_time_type, 5> stamps = slot.stamps;
            if (restart) {
                if (i != 0) stamps = stamp_sources(sources);
            } else if (check_stamps && i != 0) {
                stamps = stamp_sources(sources);
                restart = slot.stamps != stamps;   // the same file(s), touched on disk
            }
            if (!restart) {
                continue;
            }
            const bool resized = slot.size != size || slot.bytes == nullptr;
            slot.sources = sources;
            slot.stamps = stamps;
            slot.size = size;
            if (i == 0) {
                // The untextured default reads no files: it is whole at once.
                // Shared by every Terrain at this size, as any whole layer is.
                slot.key = "untextured|" + std::to_string(size) + "|" + layer_settings(size);
                {
                    std::lock_guard<std::mutex> lock(shared_->mutex);
                    std::weak_ptr<const terrain::LayerBytes>& held = shared_->whole[slot.key];
                    slot.bytes = held.lock();
                    if (slot.bytes == nullptr) {
                        slot.bytes = std::make_shared<const terrain::LayerBytes>(terrain::untextured_layer(size));
                        held = slot.bytes;
                    }
                }
                slot.bytes_key = slot.key;
                slot.revision = next_layer_revision_++;
                publish_now = true;
                continue;
            }
            slot.key = texture::cache_key(
                {sources.diffuse, sources.normal, sources.roughness, sources.metalness, sources.height},
                std::vector<std::filesystem::file_time_type>(stamps.begin(), stamps.end()), layer_settings(size));
            if (resized) {
                // Grey, at once, until its first load step lands. (A layer
                // whose files changed at the same size keeps drawing its old
                // version instead, until the new one is whole.)
                slot.bytes = std::make_shared<const terrain::LayerBytes>(terrain::placeholder_layer(size));
                slot.bytes_key.clear();
                slot.revision = next_layer_revision_++;
                publish_now = true;
            }
            if (start_layer(slot, root)) publish_now = true;
        }
        if (check_stamps) {
            record.stamps_checked_once = true;
            record.stamps_checked_at = now;
        }
        if (publish_now) publish(id, record);
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
