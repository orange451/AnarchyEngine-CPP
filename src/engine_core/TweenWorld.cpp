#include "TweenWorld.hpp"

#include "Animation.hpp"
#include "DataModel.hpp"
#include "Matrix4.hpp"
#include "VectorMath.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {

namespace {

float mix(float from, float to, double alpha) { return vector_math::lerp_component(from, to, alpha); }

}  // namespace

bool tween_kind(LuaSlot::Kind kind) {
    return kind == LuaSlot::Kind::Number || kind == LuaSlot::Kind::Vec2 || kind == LuaSlot::Kind::Vec3 ||
           kind == LuaSlot::Kind::Color || kind == LuaSlot::Kind::Matrix4;
}

bool tween_lerp(const LuaSlot& from, const LuaSlot& to, double alpha, LuaSlot& out) {
    if (from.kind != to.kind || !tween_kind(from.kind)) {
        return false;
    }
    LuaSlot made;
    made.kind = from.kind;
    switch (from.kind) {
    case LuaSlot::Kind::Number:
        made.number = alpha == 1.0 ? to.number : from.number + (to.number - from.number) * alpha;
        break;
    case LuaSlot::Kind::Vec2:
    case LuaSlot::Kind::Vec3:
        made.vec = Vec3{mix(from.vec.x, to.vec.x, alpha), mix(from.vec.y, to.vec.y, alpha),
                        mix(from.vec.z, to.vec.z, alpha)};
        break;
    case LuaSlot::Kind::Color:
        made.color.r = mix(from.color.r, to.color.r, alpha);
        made.color.g = mix(from.color.g, to.color.g, alpha);
        made.color.b = mix(from.color.b, to.color.b, alpha);
        made.color.a = mix(from.color.a, to.color.a, alpha);
        break;
    case LuaSlot::Kind::Matrix4:
        made.transform = alpha == 1.0 ? to.transform : matrix4_lerp(from.transform, to.transform, alpha);
        break;
    default:
        return false;
    }
    out = std::move(made);
    return true;
}

TweenWorld::Tween* TweenWorld::find(std::uint32_t id) {
    const auto found = tweens_.find(id);
    return found != tweens_.end() ? found->second.get() : nullptr;
}

const TweenWorld::Tween* TweenWorld::find(std::uint32_t id) const {
    const auto found = tweens_.find(id);
    return found != tweens_.end() ? found->second.get() : nullptr;
}

std::uint32_t TweenWorld::start(DataModel& world, InstanceId target, std::vector<TweenGoal> goals,
                                const TweenOptions& options) {
    // A property tweens one way at a time: the newer tween takes it.
    const std::vector<std::uint32_t> running = running_;
    for (std::uint32_t other_id : running) {
        Tween* other = find(other_id);
        if (other == nullptr || other->ended || other->plain || other->target != target) {
            continue;
        }
        const std::size_t before = other->goals.size();
        other->goals.erase(std::remove_if(other->goals.begin(), other->goals.end(),
                                          [&](const TweenGoal& held) {
                                              return std::any_of(goals.begin(), goals.end(), [&](const TweenGoal& goal) {
                                                  return goal.field == held.field;
                                              });
                                          }),
                           other->goals.end());
        if (other->goals.size() != before && other->goals.empty()) {
            finish(world, *other, false);
        }
    }
    auto tween = std::make_unique<Tween>();
    tween->id = next_id_++;
    tween->target = target;
    tween->guid = world.guid(target);
    tween->goals = std::move(goals);
    tween->options = options;
    tween->delay_left = std::max(0.0, options.delay);
    tween->repeats_left = options.repeat;
    const std::uint32_t id = tween->id;
    tweens_.emplace(id, std::move(tween));
    running_.push_back(id);
    return id;
}

std::uint32_t TweenWorld::start_value(LuaSlot from, LuaSlot to, const TweenOptions& options) {
    auto tween = std::make_unique<Tween>();
    tween->id = next_id_++;
    tween->plain = true;
    tween->from = std::move(from);
    tween->to = std::move(to);
    tween->options = options;
    tween->delay_left = std::max(0.0, options.delay);
    tween->repeats_left = options.repeat;
    const std::uint32_t id = tween->id;
    tweens_.emplace(id, std::move(tween));
    running_.push_back(id);
    return id;
}

void TweenWorld::pause(std::uint32_t id) {
    if (Tween* tween = find(id); tween != nullptr && !tween->ended) {
        tween->paused = true;
    }
}

void TweenWorld::resume(std::uint32_t id) {
    if (Tween* tween = find(id); tween != nullptr && !tween->ended) {
        tween->paused = false;
    }
}

void TweenWorld::cancel(DataModel& world, std::uint32_t id) {
    if (Tween* tween = find(id); tween != nullptr && !tween->ended) {
        finish(world, *tween, false);
    }
}

bool TweenWorld::known(std::uint32_t id) const { return find(id) != nullptr; }

bool TweenWorld::playing(std::uint32_t id) const {
    const Tween* tween = find(id);
    return tween != nullptr && !tween->ended && !tween->paused;
}

bool TweenWorld::ended(std::uint32_t id) const {
    const Tween* tween = find(id);
    return tween == nullptr || tween->ended;
}

bool TweenWorld::completed(std::uint32_t id) const {
    const Tween* tween = find(id);
    return tween != nullptr && tween->ended && tween->completed;
}

Signal* TweenWorld::completed_signal(DataModel& world, std::uint32_t id) {
    Tween* tween = find(id);
    if (tween == nullptr) {
        return nullptr;
    }
    if (tween->completed_signal == nullptr) {
        tween->completed_signal = std::make_unique<Signal>();
        world.events().host_signal(tween->completed_signal.get());
    }
    return tween->completed_signal.get();
}

Signal* TweenWorld::stepped_signal(DataModel& world, std::uint32_t id) {
    Tween* tween = find(id);
    if (tween == nullptr) {
        return nullptr;
    }
    if (tween->stepped_signal == nullptr) {
        tween->stepped_signal = std::make_unique<Signal>();
        world.events().host_signal(tween->stepped_signal.get());
    }
    return tween->stepped_signal.get();
}

void TweenWorld::retain(std::uint32_t id) {
    if (Tween* tween = find(id)) {
        ++tween->holders;
    }
}

void TweenWorld::release(DataModel&, std::uint32_t id) {
    // The next step forgets it: its Completed may still be queued, as in finish.
    if (Tween* tween = find(id); tween != nullptr && tween->holders > 0) {
        --tween->holders;
    }
}

void TweenWorld::forget(DataModel& world, std::uint32_t id) {
    const auto found = tweens_.find(id);
    if (found == tweens_.end()) {
        return;
    }
    if (found->second->completed_signal != nullptr) {
        world.events().release_signal(*found->second->completed_signal);
    }
    if (found->second->stepped_signal != nullptr) {
        world.events().release_signal(*found->second->stepped_signal);
    }
    tweens_.erase(found);
}

void TweenWorld::finish(DataModel& world, Tween& tween, bool completed) {
    if (tween.ended) {
        return;
    }
    tween.ended = true;
    tween.completed = completed;
    running_.erase(std::remove(running_.begin(), running_.end(), tween.id), running_.end());
    if (tween.completed_signal != nullptr) {
        LuaSlot arg;
        arg.kind = LuaSlot::Kind::Bool;
        arg.flag = completed;
        world.events().emit_args(tween.completed_signal->id(), 0, EventArgs{arg});
    }
    // A handler may still be queued for the signal, so a tween nothing holds
    // is forgotten at the next step, not here.
}

bool TweenWorld::apply(DataModel& world, Tween& tween, double alpha, std::vector<std::string>& refusals) {
    if (tween.plain) {
        LuaSlot value;
        if (!tween_lerp(tween.from, tween.to, alpha, value)) {
            return false;
        }
        if (tween.stepped_signal != nullptr) {
            world.events().emit_args(tween.stepped_signal->id(), 0, EventArgs{std::move(value)});
        }
        return true;
    }
    if (!world.alive(tween.target) || !world.has_guid(tween.target, tween.guid)) {
        return false;
    }
    DataModel* object = world.instance(tween.target);
    if (object == nullptr) {
        return false;
    }
    for (const TweenGoal& goal : tween.goals) {
        LuaSlot value;
        if (!tween_lerp(goal.from, goal.to, alpha, value)) {
            return false;
        }
        if (goal.field == nullptr || goal.field->write == nullptr) {
            return false;
        }
        if (!goal.field->write(world, *object, value)) {
            // Back and Elastic overshoot: a property with a range takes its end instead.
            const bool outside = alpha < 0.0 || alpha > 1.0;
            if (!outside || !tween_lerp(goal.from, goal.to, std::clamp(alpha, 0.0, 1.0), value) ||
                !goal.field->write(world, *object, value)) {
                refusals.push_back(std::string("Tween of ") + goal.field->name + " cancelled" +
                                   (value.error.empty() ? std::string() : ": " + value.error));
                return false;
            }
        }
        // The write may have destroyed the target through a Changed handler's reach.
        if (!world.alive(tween.target)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> TweenWorld::step(DataModel& world, double dt) {
    std::vector<std::string> refusals;
    // Ended tweens no script holds, whose Completed handlers have run by now.
    for (auto it = tweens_.begin(); it != tweens_.end();) {
        if (it->second->ended && it->second->holders == 0) {
            const std::uint32_t id = it->first;
            ++it;
            forget(world, id);
        } else {
            ++it;
        }
    }
    if (dt < 0 || !std::isfinite(dt)) {
        dt = 0;
    }
    const std::vector<std::uint32_t> running = running_;
    for (std::uint32_t id : running) {
        Tween* tween = find(id);
        if (tween == nullptr || tween->ended) {
            continue;
        }
        if (tween->paused) {
            // A paused tween still ends when its instance does, so Wait returns.
            if (!tween->plain && (!world.alive(tween->target) || !world.has_guid(tween->target, tween->guid))) {
                finish(world, *tween, false);
            }
            continue;
        }
        double left = dt;
        if (tween->delay_left > 0) {
            if (tween->delay_left > left) {
                tween->delay_left -= left;
                // Still waiting, but an instance that died ends the tween now.
                if (!tween->plain && !world.alive(tween->target)) {
                    finish(world, *tween, false);
                }
                continue;
            }
            left -= tween->delay_left;
            tween->delay_left = 0;
        }
        tween->elapsed += left;
        const double length = tween->options.time;
        const double t = length > 0 ? std::min(1.0, tween->elapsed / length) : 1.0;
        const float eased = ease(tween->options.style, tween->options.direction,
                                 static_cast<float>(tween->returning ? 1.0 - t : t));
        // The ends are exact whatever the curve's rounding.
        double alpha = eased;
        if (t >= 1.0) {
            alpha = tween->returning ? 0.0 : 1.0;
        }
        if (!apply(world, *tween, alpha, refusals)) {
            finish(world, *tween, false);
            continue;
        }
        if (t < 1.0) {
            continue;
        }
        tween->elapsed = 0;
        if (tween->options.reverses && !tween->returning) {
            tween->returning = true;
            continue;
        }
        tween->returning = false;
        if (tween->repeats_left != 0) {
            if (tween->repeats_left > 0) {
                --tween->repeats_left;
            }
            // A tween of no length that repeats forever would never leave this frame's work.
            continue;
        }
        finish(world, *tween, true);
    }
    return refusals;
}

void TweenWorld::clear(DataModel& world) {
    for (auto& [id, tween] : tweens_) {
        if (tween->completed_signal != nullptr) {
            world.events().release_signal(*tween->completed_signal);
        }
        if (tween->stepped_signal != nullptr) {
            world.events().release_signal(*tween->stepped_signal);
        }
    }
    tweens_.clear();
    running_.clear();
}

}  // namespace engine_core
