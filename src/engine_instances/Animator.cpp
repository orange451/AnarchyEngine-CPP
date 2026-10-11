#include "Animator.hpp"

#include "AssetInstances.hpp"
#include "Containment.hpp"
#include "Contract.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Skinning.hpp"

#include <atomic>
#include <iterator>

namespace engine_core {
namespace {

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

std::uint64_t next_revision() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

std::optional<std::string> Animator::set_auto_step(bool value) {
    if (!on_gameplay_thread()) {
        contract_fail("Animator setters run on SimulationThread");
    }
    if (value == auto_step_) {
        return std::nullopt;
    }
    auto_step_ = value;
    note_property_change("AutoStep", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::uint32_t Animator::load(InstanceId animation) {
    drop_if_stale();
    const auto* asset = dynamic_cast<const Animation*>(instance(animation));
    if (asset == nullptr || !alive(animation)) {
        return 0;
    }
    if (tracks_.empty()) {
        generation_ = world_generation();
    }
    TrackState track;
    track.id = next_track_++;
    track.clip = asset->clip();
    track.looped = track.clip != nullptr && track.clip->looped;
    tracks_.push_back(std::move(track));
    slots_.push_back(Slot{animation});
    return tracks_.back().id;
}

TrackState* Animator::track(std::uint32_t id) {
    drop_if_stale();
    if (!alive(this->id())) {
        return nullptr;
    }
    for (TrackState& track : tracks_) {
        if (track.id == id) {
            return &track;
        }
    }
    return nullptr;
}

InstanceId Animator::track_animation(std::uint32_t id) const {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == id) {
            return slots_[i].animation;
        }
    }
    return 0;
}

void Animator::destroy_track(std::uint32_t id) {
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == id) {
            tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(i));
            slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
            release_signals(id);
            return;
        }
    }
}

std::vector<std::uint32_t> Animator::playing() {
    drop_if_stale();
    std::vector<std::uint32_t> out;
    for (const TrackState& track : tracks_) {
        if (track.playing) {
            out.push_back(track.id);
        }
    }
    return out;
}

void Animator::step(double dt) {
    drop_if_stale();
    const auto* object = dynamic_cast<const GameObject*>(instance(parent(id())));
    const std::shared_ptr<const Skeleton> skeleton =
        object != nullptr ? prefab_skeleton(*object, object->prefab_guid()) : nullptr;
    const std::uint64_t signature = skeleton != nullptr ? skeleton->signature : 0;
    const std::size_t bones = skeleton != nullptr ? skeleton->bones.size() : 0;
    // Each track's bones matched by name, again whenever the skeleton changes.
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        TrackState& track = tracks_[i];
        if (slots_[i].matched == signature || track.clip == nullptr) {
            continue;
        }
        track.bone_map.assign(track.clip->bones.size(), -1);
        for (std::size_t b = 0; b < track.clip->bones.size() && skeleton != nullptr; ++b) {
            track.bone_map[b] = skeleton->find(track.clip->bones[b]);
        }
        slots_[i].matched = signature;
    }
    events_.clear();
    step_animations(tracks_, static_cast<float>(dt), bones, poses_, touched_, events_);

    std::shared_ptr<const AnimatedPose> made;
    if (skeleton != nullptr) {
        auto pose = std::make_shared<AnimatedPose>();
        pose->revision = next_revision();
        pose->skeleton = signature;
        pose->deltas.resize(bones, matrix4_identity());
        for (std::size_t b = 0; b < bones; ++b) {
            if (touched_[b]) {
                pose->deltas[b] = bone_delta_matrix(poses_[b].position, poses_[b].rotation, poses_[b].scale);
            }
        }
        made = std::move(pose);
    }
    {
        std::lock_guard<std::mutex> lock(animated_mutex_);
        animated_ = std::move(made);
        animated_generation_ = world_generation();
    }

    for (const TrackEvent& event : events_) {
        const auto found = signals_.find({event.track, static_cast<int>(event.kind)});
        if (found == signals_.end()) {
            continue;
        }
        EventArgs args;
        if (event.kind == TrackEvent::Kind::KeyframeReached) {
            LuaSlot name;
            name.kind = LuaSlot::Kind::String;
            name.text = event.name;
            LuaSlot index;
            index.kind = LuaSlot::Kind::Number;
            index.number = event.index + 1;
            args = {name, index};
        }
        events().emit_args(found->second->id(), 0, std::move(args));
    }
}

std::shared_ptr<const AnimatedPose> Animator::animated() const {
    std::lock_guard<std::mutex> lock(animated_mutex_);
    // A Stop since that step made it is no pose at all.
    if (animated_generation_ != world_generation()) {
        return nullptr;
    }
    return animated_;
}

Signal& Animator::track_signal(std::uint32_t id, TrackEvent::Kind kind) {
    std::unique_ptr<Signal>& signal = signals_[{id, static_cast<int>(kind)}];
    if (signal == nullptr) {
        signal = std::make_unique<Signal>();
        events().host_signal(signal.get());
    }
    return *signal;
}

void Animator::release_signals(std::uint32_t id) {
    for (auto it = signals_.begin(); it != signals_.end();) {
        if (it->first.first == id) {
            events().release_signal(*it->second);
            it = signals_.erase(it);
        } else {
            ++it;
        }
    }
}

void Animator::drop_if_stale() {
    if (!tracks_.empty() && generation_ != world_generation()) {
        drop_all();
    }
}

void Animator::drop_all() {
    tracks_.clear();
    slots_.clear();
    for (auto& [key, signal] : signals_) {
        events().release_signal(*signal);
    }
    signals_.clear();
    std::lock_guard<std::mutex> lock(animated_mutex_);
    animated_.reset();
}

void Animator::on_reuse() {
    drop_all();
    auto_step_ = true;
}

namespace {

bool read_auto_step(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* animator = dynamic_cast<const Animator*>(&object);
    if (animator == nullptr) {
        return false;
    }
    out = bool_slot(animator->auto_step());
    return true;
}

bool write_auto_step(DataModel&, DataModel& object, LuaSlot& in) {
    auto* animator = dynamic_cast<Animator*>(&object);
    if (animator == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Bool) {
        in.error = "AutoStep must be a boolean";
        return false;
    }
    if (std::optional<std::string> error = animator->set_auto_step(in.flag)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_animator_lua) {
    const LuaField fields[] = {
        lua_saved_property("AutoStep", "boolean", read_auto_step, write_auto_step, "true"),
    };
    register_lua_class("Animator", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Animator", {"GameObject"});
}

}  // namespace

}  // namespace engine_core
