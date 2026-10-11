#include "Animation.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace engine_core {
namespace {

namespace aanim = anarchy::aanim;

constexpr float kPi = 3.14159265358979f;
// How long an unlooped track takes to fade out at its end: Play's and Stop's default.
constexpr float kEndFade = 0.2f;

float lerp(float a, float b, float t) { return a + (b - a) * t; }

Vec3 lerp(Vec3 a, Vec3 b, float t) { return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)}; }

BonePose blend(const BonePose& a, const BonePose& b, float t) {
    return {lerp(a.position, b.position, t), quat_slerp(a.rotation, b.rotation, t), lerp(a.scale, b.scale, t)};
}

float bounce_out(float t) {
    constexpr float n = 7.5625f;
    constexpr float d = 2.75f;
    if (t < 1.f / d) {
        return n * t * t;
    }
    if (t < 2.f / d) {
        t -= 1.5f / d;
        return n * t * t + 0.75f;
    }
    if (t < 2.5f / d) {
        t -= 2.25f / d;
        return n * t * t + 0.9375f;
    }
    t -= 2.625f / d;
    return n * t * t + 0.984375f;
}

// The In curve of each style; Out and InOut are made from it.
float ease_in(aanim::EasingStyle style, float t) {
    using S = aanim::EasingStyle;
    switch (style) {
    case S::Linear:
        return t;
    case S::Constant:
        return t >= 1.f ? 1.f : 0.f;
    case S::Sine:
        return 1.f - std::cos(t * kPi / 2.f);
    case S::Quad:
        return t * t;
    case S::Cubic:
        return t * t * t;
    case S::Quart:
        return t * t * t * t;
    case S::Quint:
        return t * t * t * t * t;
    case S::Exponential:
        return t <= 0.f ? 0.f : std::pow(2.f, 10.f * t - 10.f);
    case S::Circular:
        return 1.f - std::sqrt(std::max(0.f, 1.f - t * t));
    case S::Back: {
        constexpr float c1 = 1.70158f;
        constexpr float c3 = c1 + 1.f;
        return c3 * t * t * t - c1 * t * t;
    }
    case S::Elastic: {
        if (t <= 0.f || t >= 1.f) {
            return t <= 0.f ? 0.f : 1.f;
        }
        constexpr float c4 = 2.f * kPi / 3.f;
        return -std::pow(2.f, 10.f * t - 10.f) * std::sin((t * 10.f - 10.75f) * c4);
    }
    case S::Bounce:
        return 1.f - bounce_out(1.f - t);
    }
    return t;
}

// Walks the count of keyframes before time from its last value, or searches
// when there is none: time moves a little each step, so it rarely walks far.
int keyframes_before(const Clip& clip, float time, int last) {
    const int count = static_cast<int>(clip.keyframes.size());
    if (last < 0 || last > count) {
        int low = 0;
        int high = count;
        while (low < high) {
            const int middle = (low + high) / 2;
            if (clip.keyframes[static_cast<std::size_t>(middle)].time < time) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        return low;
    }
    int walk = last;
    while (walk < count && clip.keyframes[static_cast<std::size_t>(walk)].time < time) {
        ++walk;
    }
    while (walk > 0 && clip.keyframes[static_cast<std::size_t>(walk - 1)].time >= time) {
        --walk;
    }
    return walk;
}

void advance_transitions(TrackState& track, float dt) {
    if (track.w_dur >= 0.f) {
        track.w_t += dt;
        if (track.w_t >= track.w_dur) {
            track.weight_current = track.w_to;
            track.w_dur = -1.f;
            if (track.stop_on_fade) {
                track.stop_on_fade = false;
                track.playing = false;
                track.stopped_pending = true;
            }
        } else {
            track.weight_current = lerp(track.w_from, track.w_to, track.w_t / track.w_dur);
        }
    }
    if (track.s_dur >= 0.f) {
        track.s_t += dt;
        if (track.s_t >= track.s_dur) {
            track.speed = track.s_to;
            track.s_dur = -1.f;
        } else {
            track.speed = lerp(track.s_from, track.s_to, track.s_t / track.s_dur);
        }
    }
}

// Back into the clip: a looped track wraps, either way along; an unlooped one
// clamps. True when an unlooped track sits on its end.
bool wrap_time(TrackState& track, float length, bool& wrapped) {
    wrapped = false;
    if (track.time > length) {
        if (track.looped && length > 0.f) {
            track.time = std::fmod(track.time, length);
            wrapped = true;
            return false;
        }
        track.time = length;
        return true;
    }
    if (track.time < 0.f) {
        if (track.looped && length > 0.f) {
            track.time = std::fmod(track.time, length);
            if (track.time < 0.f) {
                track.time += length;
            }
        } else {
            track.time = 0.f;
        }
    }
    return !track.looped && track.time >= length && length > 0.f;
}

void set_weight(TrackState& track, float weight, float fade) {
    track.weight_target = weight;
    if (fade <= 0.f) {
        track.w_dur = -1.f;
        track.weight_current = weight;
        return;
    }
    track.w_from = track.weight_current;
    track.w_to = weight;
    track.w_t = 0.f;
    track.w_dur = fade;
}

void set_speed(TrackState& track, float speed, float fade) {
    if (fade <= 0.f) {
        track.s_dur = -1.f;
        track.speed = speed;
        return;
    }
    track.s_from = track.speed;
    track.s_to = speed;
    track.s_t = 0.f;
    track.s_dur = fade;
}

}  // namespace

Quat quat_slerp(Quat a, Quat b, float t) {
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosine < 0.f) {
        cosine = -cosine;
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    float wa = 1.f - t;
    float wb = t;
    if (cosine < 0.9995f) {
        const float angle = std::acos(std::min(cosine, 1.f));
        const float sine = std::sin(angle);
        wa = std::sin((1.f - t) * angle) / sine;
        wb = std::sin(t * angle) / sine;
    }
    Quat out{wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
    const float length = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (length > 0.f) {
        out = {out.x / length, out.y / length, out.z / length, out.w / length};
    }
    return out;
}

Matrix4 bone_delta_matrix(Vec3 position, Quat rotation, Vec3 scale) {
    Matrix4 out = matrix4_from_quaternion(rotation.x, rotation.y, rotation.z, rotation.w);
    const float factors[3] = {scale.x, scale.y, scale.z};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            out.m[column * 4 + row] *= factors[column];
        }
    }
    out.m[12] = position.x;
    out.m[13] = position.y;
    out.m[14] = position.z;
    return out;
}

float ease(aanim::EasingStyle style, aanim::EasingDirection direction, float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (style == aanim::EasingStyle::Constant) {
        return t >= 1.f ? 1.f : 0.f;
    }
    switch (direction) {
    case aanim::EasingDirection::In:
        return ease_in(style, t);
    case aanim::EasingDirection::Out:
        return 1.f - ease_in(style, 1.f - t);
    case aanim::EasingDirection::InOut:
        return t < 0.5f ? ease_in(style, 2.f * t) / 2.f : 1.f - ease_in(style, 2.f - 2.f * t) / 2.f;
    }
    return t;
}

std::shared_ptr<const Clip> make_clip(const aanim::Data& data) {
    auto clip = std::make_shared<Clip>();
    clip->name = data.name;
    clip->looped = data.looped;
    clip->length = data.keyframes.empty() ? 0.f : data.keyframes.back().time;
    std::map<std::string, std::uint16_t> bones;
    for (const aanim::Keyframe& from : data.keyframes) {
        Clip::Keyframe keyframe;
        keyframe.time = from.time;
        keyframe.name = from.name;
        for (const aanim::Pose& pose : from.poses) {
            auto found = bones.find(pose.bone);
            if (found == bones.end()) {
                found = bones.emplace(pose.bone, static_cast<std::uint16_t>(clip->bones.size())).first;
                clip->bones.push_back(pose.bone);
            }
            Clip::KeyPose key;
            key.bone = found->second;
            key.pose.position = {pose.position[0], pose.position[1], pose.position[2]};
            key.pose.rotation = {pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]};
            key.pose.scale = {pose.scale[0], pose.scale[1], pose.scale[2]};
            key.style = pose.style;
            key.direction = pose.direction;
            key.weight = pose.weight;
            keyframe.poses.push_back(key);
        }
        clip->keyframes.push_back(std::move(keyframe));
    }
    // Each pose's partner in the next keyframe, found once.
    for (std::size_t k = 0; k < clip->keyframes.size(); ++k) {
        Clip::Keyframe& keyframe = clip->keyframes[k];
        const Clip::Keyframe* after = k + 1 < clip->keyframes.size() ? &clip->keyframes[k + 1] : nullptr;
        for (std::size_t p = 0; p < keyframe.poses.size(); ++p) {
            int partner = -1;
            for (std::size_t q = 0; after != nullptr && q < after->poses.size(); ++q) {
                if (after->poses[q].bone == keyframe.poses[p].bone) {
                    partner = static_cast<int>(q);
                    break;
                }
            }
            keyframe.next.emplace_back(static_cast<int>(p), partner);
        }
    }
    return clip;
}

void track_play(TrackState& track, float fade, float speed, float weight) {
    if (!track.playing) {
        track.weight_current = 0.f;
        // An unlooped track that ran to its end plays again from its start.
        if (track.clip != nullptr && !track.looped) {
            if (speed >= 0.f && track.time >= track.clip->length) {
                track.time = 0.f;
            } else if (speed < 0.f && track.time <= 0.f) {
                track.time = track.clip->length;
            }
            track.key_lo = -1;
        }
    }
    track.playing = true;
    track.stop_on_fade = false;
    track.stopped_pending = false;
    set_speed(track, speed, fade);
    set_weight(track, weight, fade);
}

void track_stop(TrackState& track, float fade) {
    if (!track.playing) {
        return;
    }
    set_weight(track, 0.f, fade);
    if (fade <= 0.f) {
        track.playing = false;
        track.stopped_pending = true;
        track.stop_on_fade = false;
    } else {
        track.stop_on_fade = true;
    }
}

void track_adjust_weight(TrackState& track, float weight, float fade) { set_weight(track, weight, fade); }

void track_adjust_speed(TrackState& track, float speed, float fade) { set_speed(track, speed, fade); }

void step_animations(std::vector<TrackState>& tracks, float dt, std::size_t bone_count, std::vector<BonePose>& out,
                     std::vector<bool>& touched, std::vector<TrackEvent>& events, AnimationScratch* scratch) {
    out.assign(bone_count, BonePose{});
    touched.assign(bone_count, false);
    // Each bone's contributions, folded after every track has given its own.
    AnimationScratch local;
    std::vector<std::vector<std::pair<BonePose, float>>>& gathered = (scratch != nullptr ? *scratch : local).parts;
    if (gathered.size() < bone_count) {
        gathered.resize(bone_count);
    }
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
        gathered[bone].clear();
    }

    for (TrackState& track : tracks) {
        advance_transitions(track, dt);
        if (track.stopped_pending) {
            track.stopped_pending = false;
            events.push_back({TrackEvent::Kind::Stopped, track.id, std::string(), 0});
        }
        if (!track.playing || track.clip == nullptr) {
            continue;
        }
        const Clip& clip = *track.clip;
        track.time += dt * track.speed;
        bool wrapped = false;
        // A track that adds nothing keeps its time, so it stays in step with the rest.
        if (track.weight_current <= 0.f && track.w_dur < 0.f) {
            wrap_time(track, clip.length, wrapped);
            continue;
        }
        const bool at_end = wrap_time(track, clip.length, wrapped);
        // An unlooped track at its end, in the way it plays, holds there and fades out.
        const bool finished = !track.looped && ((track.speed > 0.f && track.time >= clip.length) ||
                                                (track.speed < 0.f && track.time <= 0.f));
        if (finished && !track.stop_on_fade) {
            track_stop(track, kEndFade);
        }
        if (clip.keyframes.empty()) {
            continue;
        }
        const int last = static_cast<int>(clip.keyframes.size()) - 1;
        if (wrapped) {
            events.push_back({TrackEvent::Kind::KeyframeReached, track.id, clip.keyframes.back().name, last});
            track.key_lo = -1;
        }
        const int before = keyframes_before(clip, track.time, track.key_lo);
        track.key_lo = before;
        int left = before - 1;
        int right = before;
        if (left < 0) {
            left = right;
        }
        if (right > last) {
            right = left;
        }
        const int reached = at_end ? right : left;
        if (reached != track.keyframe_index) {
            track.keyframe_index = reached;
            events.push_back({TrackEvent::Kind::KeyframeReached, track.id,
                              clip.keyframes[static_cast<std::size_t>(reached)].name, reached});
        }
        if (track.weight_current <= 0.f) {
            continue;
        }
        const Clip::Keyframe& from = clip.keyframes[static_cast<std::size_t>(left)];
        const Clip::Keyframe& to = clip.keyframes[static_cast<std::size_t>(right)];
        const float span = to.time - from.time;
        const float fraction = left != right && span > 0.f ? (track.time - from.time) / span : 0.f;
        for (const auto& [own, partner] : from.next) {
            const Clip::KeyPose& a = from.poses[static_cast<std::size_t>(own)];
            const Clip::KeyPose& b =
                left != right && partner >= 0 ? to.poses[static_cast<std::size_t>(partner)] : a;
            if (a.bone >= track.bone_map.size()) {
                continue;
            }
            const int bone = track.bone_map[a.bone];
            if (bone < 0 || static_cast<std::size_t>(bone) >= bone_count) {
                continue;
            }
            const float eased = ease(a.style, a.direction, fraction);
            const float weight = track.weight_current * lerp(a.weight, b.weight, eased);
            if (weight <= 0.f) {
                continue;
            }
            gathered[static_cast<std::size_t>(bone)].emplace_back(blend(a.pose, b.pose, eased), weight);
        }
    }

    for (std::size_t bone = 0; bone < bone_count; ++bone) {
        auto& parts = gathered[bone];
        if (parts.empty()) {
            continue;
        }
        touched[bone] = true;
        // From the heaviest, toward each other by its share of the weight so far.
        std::size_t heaviest = 0;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            if (parts[i].second > parts[heaviest].second) {
                heaviest = i;
            }
        }
        BonePose result = parts[heaviest].first;
        float total = parts[heaviest].second;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i == heaviest) {
                continue;
            }
            total += parts[i].second;
            result = blend(result, parts[i].first, parts[i].second / total);
        }
        out[bone] = total >= 1.f ? result : blend(BonePose{}, result, total);
    }
}

}  // namespace engine_core
