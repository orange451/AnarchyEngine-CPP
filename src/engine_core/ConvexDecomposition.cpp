#include "ConvexDecomposition.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "PhysicsObject.hpp"

#pragma warning(push, 0)
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"
#pragma warning(pop)

#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace engine_core {

namespace {

constexpr std::size_t kCachedMeshes = 64;

std::atomic<std::uint64_t> decompositions{0};

struct Cache {
    std::mutex mutex;
    std::deque<std::pair<std::uint64_t, std::vector<anarchy::amesh::ConvexPiece>>> entries;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

// FNV-1a over the points, the triangles, and the recipe.
std::uint64_t geometry_key(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        }
    };
    for (const Vec3& p : points) {
        const float xyz[3] = {p.x, p.y, p.z};
        mix(xyz, sizeof(xyz));
    }
    mix(triangles.data(), triangles.size() * sizeof(std::uint32_t));
    mix(&kRecipe, sizeof(kRecipe));
    return hash;
}

// Cancels a V-HACD run once stop is set. Compute clears a Cancel made before
// it starts, so each progress report looks again.
class StopCheck final : public VHACD::IVHACD::IUserCallback {
public:
    StopCheck(VHACD::IVHACD& vhacd, const std::atomic<bool>* stop) : vhacd_(vhacd), stop_(stop) {}
    void Update(const double, const double, const char* const, const char*) override {
        if (stop_ != nullptr && stop_->load()) {
            vhacd_.Cancel();
        }
    }

private:
    VHACD::IVHACD& vhacd_;
    const std::atomic<bool>* stop_;
};

// decompose, given up when stop is set. attach gets a way to cancel the run as
// it starts, and an empty one before the run is freed.
std::vector<anarchy::amesh::ConvexPiece> decompose_until(
    const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles, const std::atomic<bool>* stop,
    const std::function<void(std::function<void()>)>& attach) {
    ++decompositions;
    std::vector<anarchy::amesh::ConvexPiece> pieces;
    if (points.empty() || triangles.size() < 3) {
        return pieces;
    }
    std::vector<float> flat;
    flat.reserve(points.size() * 3);
    for (const Vec3& p : points) {
        flat.push_back(p.x);
        flat.push_back(p.y);
        flat.push_back(p.z);
    }
    VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
    StopCheck check(*vhacd, stop);
    // The settings kRecipe names.
    VHACD::IVHACD::Parameters parameters;
    parameters.m_callback = &check;
    parameters.m_maxConvexHulls = 32;
    parameters.m_resolution = 100000;
    parameters.m_maxNumVerticesPerCH = 64;
    parameters.m_minimumVolumePercentErrorAllowed = 1;
    parameters.m_fillMode = VHACD::FillMode::FLOOD_FILL;
    parameters.m_shrinkWrap = true;
    // Use V-HACD's own threads; Compute still blocks the caller.
    parameters.m_asyncACD = true;

    if (attach) {
        attach([vhacd] { vhacd->Cancel(); });
    }
    const bool computed = vhacd->Compute(flat.data(), static_cast<std::uint32_t>(points.size()), triangles.data(),
                                         static_cast<std::uint32_t>(triangles.size() / 3), parameters);
    if (attach) {
        attach(nullptr);
    }
    if (computed && (stop == nullptr || !stop->load())) {
        for (std::uint32_t index = 0; index < vhacd->GetNConvexHulls(); ++index) {
            VHACD::IVHACD::ConvexHull hull;
            if (!vhacd->GetConvexHull(index, hull) || hull.m_points.size() < anarchy::amesh::kMinPiecePoints) {
                continue;
            }
            anarchy::amesh::ConvexPiece piece;
            for (const VHACD::Vertex& v : hull.m_points) {
                if (piece.points.size() == anarchy::amesh::kMaxPiecePoints) {
                    break;
                }
                piece.points.push_back(
                    {static_cast<float>(v.mX), static_cast<float>(v.mY), static_cast<float>(v.mZ)});
            }
            pieces.push_back(std::move(piece));
            if (pieces.size() == anarchy::amesh::kMaxPieces) {
                break;
            }
        }
    }
    vhacd->Clean();
    vhacd->Release();
    return pieces;
}

}  // namespace

std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles) {
    return decompose_until(points, triangles, nullptr, nullptr);
}

std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles,
                                                   const std::atomic<bool>& stop) {
    return decompose_until(points, triangles, &stop, nullptr);
}

void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                     std::vector<anarchy::amesh::ConvexPiece> pieces) {
    const std::uint64_t key = geometry_key(points, triangles);
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    for (auto& entry : kept.entries) {
        if (entry.first == key) {
            entry.second = std::move(pieces);
            return;
        }
    }
    kept.entries.emplace_back(key, std::move(pieces));
    if (kept.entries.size() > kCachedMeshes) {
        kept.entries.pop_front();
    }
}

bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                  std::vector<anarchy::amesh::ConvexPiece>& out) {
    if (mesh.file_pieces(kRecipe, out)) {
        return true;
    }
    const std::uint64_t key = geometry_key(points, triangles);
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    for (const auto& entry : kept.entries) {
        if (entry.first == key) {
            out = entry.second;
            return true;
        }
    }
    out.clear();
    return false;
}

std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                                    const std::vector<std::uint32_t>& triangles) {
    std::vector<anarchy::amesh::ConvexPiece> pieces;
    if (known_pieces(mesh, points, triangles, pieces)) {
        return pieces;
    }
    pieces = decompose(points, triangles);
    remember_pieces(points, triangles, pieces);
    return pieces;
}

std::uint64_t decompose_count() { return decompositions.load(); }

void clear_piece_cache() {
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    kept.entries.clear();
}

ConvexDecomposer::ConvexDecomposer() : thread_([this] { work(); }) {}

ConvexDecomposer::~ConvexDecomposer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        waiting_.clear();
        // V-HACD can take seconds; the worker should not hold the studio's exit.
        if (cancel_) {
            cancel_();
        }
    }
    wake_.notify_all();
    thread_.join();
}

bool ConvexDecomposer::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_.empty() && !running_;
}

void ConvexDecomposer::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || !waiting_.empty(); });
        if (stopping_) {
            return;
        }
        Job job = std::move(waiting_.front());
        waiting_.pop_front();
        running_ = true;
        lock.unlock();
        job.pieces = decompose_until(job.points, job.triangles, &stopping_, [this](std::function<void()> cancel) {
            std::lock_guard<std::mutex> hold(mutex_);
            cancel_ = std::move(cancel);
        });
        lock.lock();
        running_ = false;
        // A cancelled job has no result.
        if (stopping_) {
            return;
        }
        done_.push_back(std::move(job));
    }
}

void ConvexDecomposer::update(DataModel& game) {
    std::vector<Job> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished.swap(done_);
    }
    for (Job& job : finished) {
        // Right for its geometry whatever became of the Mesh.
        remember_pieces(job.points, job.triangles, job.pieces);
        auto* mesh = dynamic_cast<Mesh*>(game.instance(job.mesh));
        if (mesh == nullptr || job.pieces.empty() || mesh->file_stamp() != job.stamp) {
            continue;
        }
        if (!mesh->store_pieces(kRecipe, std::move(job.pieces))) {
            // Its own write is not a change to queue again for.
            queued_[job.mesh] = mesh->file_stamp();
        }
    }

    game.physics_bodies(bodies_);
    for (const InstanceId id : bodies_) {
        const auto* object = dynamic_cast<const PhysicsObject*>(game.instance(id));
        if (object == nullptr || object->shape() != PhysicsObject::Shape::Custom) {
            continue;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(game.instance(object->mesh_id()));
        if (mesh == nullptr) {
            continue;
        }
        const std::string stamp = mesh->file_stamp();
        if (stamp.empty()) {
            continue;
        }
        const auto seen = queued_.find(mesh->id());
        if (seen != queued_.end() && seen->second == stamp) {
            continue;
        }
        queued_[mesh->id()] = stamp;
        std::vector<anarchy::amesh::ConvexPiece> stored;
        if (mesh->file_pieces(kRecipe, stored)) {
            continue;
        }
        Job job;
        job.mesh = mesh->id();
        job.stamp = stamp;
        if (mesh->vertex_positions(job.points, &job.triangles)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting_.push_back(std::move(job));
        }
        wake_.notify_one();
        // The rest wait for later calls.
        return;
    }
}

}  // namespace engine_core
