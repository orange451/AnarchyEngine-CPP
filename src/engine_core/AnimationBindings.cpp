// Animator's and Animation's Lua methods and the AnimationTrack datatype.
// Every binding runs on SimulationThread, as every script does. A track is a
// userdata naming its Animator and its id there, so a track whose Animator is
// gone, which a Stop dropped, or which was destroyed raises "AnimationTrack
// is gone" at its next use.

#include "ScriptBindings.hpp"

#include "Animation.hpp"
#include "Animator.hpp"
#include "AssetInstances.hpp"
#include "LuaApi.hpp"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>
#include <string>

namespace engine_core {

using namespace script_internal;

namespace {

constexpr const char* kTrackMeta = "AE.AnimationTrack";
// A fade, in seconds, when a call gives none: GrooveAnimator's.
constexpr double kDefaultFade = 0.2;

struct TrackUd {
    InstanceId animator = 0;
    std::uint32_t world = 0;
    std::uint32_t track = 0;
};

void push_track(lua_State* state, InstanceId animator, std::uint32_t world, std::uint32_t track) {
    auto* ud = static_cast<TrackUd*>(lua_newuserdata(state, sizeof(TrackUd)));
    *ud = TrackUd{animator, world, track};
    luaL_getmetatable(state, kTrackMeta);
    lua_setmetatable(state, -2);
}

double finite_arg(lua_State* state, int index, const char* name, double fallback) {
    if (lua_isnoneornil(state, index)) {
        return fallback;
    }
    const double value = luaL_checknumber(state, index);
    if (!std::isfinite(value)) {
        luaL_error(state, "%s must be a finite number", name);
    }
    return value;
}

void push_track_signal(lua_State* state, const TrackUd& track, const char* name) {
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    ud->kind = kSignalTrack;
    ud->id = track.animator;
    ud->world = track.world;
    ud->track = track.track;
    ud->event_name = name;
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
}

int track_tostring(lua_State* state) {
    lua_pushliteral(state, "AnimationTrack");
    return 1;
}

int track_eq(lua_State* state) {
    const auto* a = static_cast<TrackUd*>(test_userdata(state, 1, kTrackMeta));
    const auto* b = static_cast<TrackUd*>(test_userdata(state, 2, kTrackMeta));
    lua_pushboolean(state, a != nullptr && b != nullptr && a->animator == b->animator && a->world == b->world &&
                               a->track == b->track);
    return 1;
}

ANARCHY_LUA_REGISTER(register_animation_methods) {
    const LuaField animator_methods[] = {
        lua_method("LoadAnimation", "AnimationTrack", reinterpret_cast<void*>(&ScriptBindings::animator_load_animation)),
        lua_method("GetPlayingAnimationTracks", "AnimationTrack",
                   reinterpret_cast<void*>(&ScriptBindings::animator_get_playing), false, false, true),
        lua_method("StepAnimations", "nil", reinterpret_cast<void*>(&ScriptBindings::animator_step_animations)),
    };
    register_lua_class("Animator", nullptr, animator_methods,
                       static_cast<int>(sizeof(animator_methods) / sizeof(animator_methods[0])));
    const LuaField animation_methods[] = {
        lua_method("GetKeyframeNames", "string", reinterpret_cast<void*>(&ScriptBindings::animation_get_keyframe_names),
                   false, false, true),
    };
    register_lua_class("Animation", nullptr, animation_methods, 1);

    // The datatype, for completion and the type checker; the metatable does the work.
    static const LuaParam kReachedArgs[] = {{"name", "string"}, {"index", "number"}};
    const LuaField track_fields[] = {
        lua_property("Animation", "Animation", false, nullptr, nullptr),
        lua_property("Length", "number", false, nullptr, nullptr),
        lua_property("Looped", "boolean", true, nullptr, nullptr),
        lua_property("Speed", "number", true, nullptr, nullptr),
        lua_property("TimePosition", "number", true, nullptr, nullptr),
        lua_property("IsPlaying", "boolean", false, nullptr, nullptr),
        lua_property("WeightCurrent", "number", false, nullptr, nullptr),
        lua_property("WeightTarget", "number", false, nullptr, nullptr),
        lua_method("Play", "nil", nullptr),
        lua_method("Stop", "nil", nullptr),
        lua_method("AdjustWeight", "nil", nullptr),
        lua_method("AdjustSpeed", "nil", nullptr),
        lua_method("Destroy", "nil", nullptr),
        lua_event("KeyframeReached", kReachedArgs, 2),
        lua_event("Stopped"),
    };
    register_lua_class("AnimationTrack", nullptr, track_fields,
                       static_cast<int>(sizeof(track_fields) / sizeof(track_fields[0])));
}

}  // namespace

void ScriptBindings::link_animation_methods() {}

Animator& ScriptBindings::animator_self(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* animator = runtime == nullptr ? nullptr : dynamic_cast<Animator*>(runtime->resolve_id(ud->id, ud->world));
    if (animator == nullptr) {
        luaL_error(state, "instance is gone");
    }
    return *animator;
}

int ScriptBindings::animator_load_animation(lua_State* state) {
    return lua_guard(state, [&] {
        Animator& animator = animator_self(state);
        auto* ud = static_cast<InstanceUd*>(test_userdata(state, 2, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        DataModel* object = ud != nullptr ? runtime->resolve_id(ud->id, ud->world) : nullptr;
        if (dynamic_cast<Animation*>(object) == nullptr) {
            luaL_error(state, "LoadAnimation takes an Animation");
        }
        const std::uint32_t track = animator.load(object->id());
        if (track == 0) {
            luaL_error(state, "LoadAnimation takes an Animation");
        }
        auto* self = static_cast<InstanceUd*>(lua_touserdata(state, 1));
        push_track(state, animator.id(), self->world, track);
        return 1;
    });
}

int ScriptBindings::animator_get_playing(lua_State* state) {
    return lua_guard(state, [&] {
        Animator& animator = animator_self(state);
        auto* self = static_cast<InstanceUd*>(lua_touserdata(state, 1));
        const std::vector<std::uint32_t> playing = animator.playing();
        lua_createtable(state, static_cast<int>(playing.size()), 0);
        for (std::size_t i = 0; i < playing.size(); ++i) {
            push_track(state, animator.id(), self->world, playing[i]);
            lua_rawseti(state, -2, static_cast<int>(i) + 1);
        }
        return 1;
    });
}

int ScriptBindings::animator_step_animations(lua_State* state) {
    return lua_guard(state, [&] {
        Animator& animator = animator_self(state);
        const double dt = luaL_checknumber(state, 2);
        if (!std::isfinite(dt)) {
            luaL_error(state, "dt must be a finite number");
        }
        animator.step(dt);
        return 0;
    });
}

int ScriptBindings::animation_get_keyframe_names(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        const auto* animation =
            runtime == nullptr ? nullptr : dynamic_cast<const Animation*>(runtime->resolve_id(ud->id, ud->world));
        if (animation == nullptr) {
            luaL_error(state, "instance is gone");
        }
        const std::shared_ptr<const Clip> clip = animation->clip();
        const int count = clip != nullptr ? static_cast<int>(clip->keyframes.size()) : 0;
        lua_createtable(state, count, 0);
        for (int k = 0; k < count; ++k) {
            const std::string& name = clip->keyframes[static_cast<std::size_t>(k)].name;
            lua_pushlstring(state, name.data(), name.size());
            lua_rawseti(state, -2, k + 1);
        }
        return 1;
    });
}

TrackState& ScriptBindings::track_of(lua_State* state, int index, Animator** owner) {
    auto* ud = static_cast<TrackUd*>(luaL_checkudata(state, index, kTrackMeta));
    ScriptRuntime* runtime = runtime_from(state);
    auto* animator =
        runtime == nullptr ? nullptr : dynamic_cast<Animator*>(runtime->resolve_id(ud->animator, ud->world));
    TrackState* track = animator != nullptr ? animator->track(ud->track) : nullptr;
    if (track == nullptr) {
        luaL_error(state, "AnimationTrack is gone");
    }
    if (owner != nullptr) {
        *owner = animator;
    }
    return *track;
}

int ScriptBindings::track_index(lua_State* state) {
    return lua_guard(state, [&] {
        Animator* animator = nullptr;
        TrackState& track = track_of(state, 1, &animator);
        const char* key = luaL_checkstring(state, 2);
        const auto* ud = static_cast<TrackUd*>(lua_touserdata(state, 1));
        if (std::strcmp(key, "Animation") == 0) {
            const InstanceId animation = animator->track_animation(track.id);
            if (animation != 0 && runtime_from(state)->game_->alive(animation)) {
                runtime_from(state)->push_instance(state, animation);
            } else {
                lua_pushnil(state);
            }
        } else if (std::strcmp(key, "Length") == 0) {
            lua_pushnumber(state, track.clip != nullptr ? track.clip->length : 0.0);
        } else if (std::strcmp(key, "Looped") == 0) {
            lua_pushboolean(state, track.looped);
        } else if (std::strcmp(key, "Speed") == 0) {
            lua_pushnumber(state, track.speed);
        } else if (std::strcmp(key, "TimePosition") == 0) {
            lua_pushnumber(state, track.time);
        } else if (std::strcmp(key, "IsPlaying") == 0) {
            lua_pushboolean(state, track.playing);
        } else if (std::strcmp(key, "WeightCurrent") == 0) {
            lua_pushnumber(state, track.weight_current);
        } else if (std::strcmp(key, "WeightTarget") == 0) {
            lua_pushnumber(state, track.weight_target);
        } else if (std::strcmp(key, "KeyframeReached") == 0) {
            push_track_signal(state, *ud, "KeyframeReached");
        } else if (std::strcmp(key, "Stopped") == 0) {
            push_track_signal(state, *ud, "Stopped");
        } else {
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_pushvalue(state, 2);
            lua_rawget(state, -2);
            if (!lua_isfunction(state, -1)) {
                luaL_error(state, "%s is not a valid member of AnimationTrack", key);
            }
        }
        return 1;
    });
}

int ScriptBindings::track_newindex(lua_State* state) {
    return lua_guard(state, [&] {
        TrackState& track = track_of(state, 1);
        const char* key = luaL_checkstring(state, 2);
        if (std::strcmp(key, "Looped") == 0) {
            luaL_checktype(state, 3, LUA_TBOOLEAN);
            track.looped = lua_toboolean(state, 3) != 0;
        } else if (std::strcmp(key, "Speed") == 0) {
            track.speed = static_cast<float>(finite_arg(state, 3, "Speed", 0.0));
            track.s_dur = -1.f;
        } else if (std::strcmp(key, "TimePosition") == 0) {
            track.time = static_cast<float>(finite_arg(state, 3, "TimePosition", 0.0));
            // A jump: the keyframe search starts over.
            track.key_lo = -1;
        } else {
            luaL_error(state, "%s cannot be assigned to", key);
        }
        return 0;
    });
}

int ScriptBindings::track_play(lua_State* state) {
    return lua_guard(state, [&] {
        TrackState& track = track_of(state, 1);
        const double fade = finite_arg(state, 2, "fade", kDefaultFade);
        const double speed = finite_arg(state, 3, "speed", 1.0);
        const double weight = finite_arg(state, 4, "weight", 1.0);
        engine_core::track_play(track, static_cast<float>(fade), static_cast<float>(speed), static_cast<float>(weight));
        return 0;
    });
}

int ScriptBindings::track_stop(lua_State* state) {
    return lua_guard(state, [&] {
        TrackState& track = track_of(state, 1);
        engine_core::track_stop(track, static_cast<float>(finite_arg(state, 2, "fade", kDefaultFade)));
        return 0;
    });
}

int ScriptBindings::track_adjust_weight(lua_State* state) {
    return lua_guard(state, [&] {
        TrackState& track = track_of(state, 1);
        const double weight = finite_arg(state, 2, "weight", 1.0);
        engine_core::track_adjust_weight(track, static_cast<float>(weight),
                                         static_cast<float>(finite_arg(state, 3, "fade", kDefaultFade)));
        return 0;
    });
}

int ScriptBindings::track_adjust_speed(lua_State* state) {
    return lua_guard(state, [&] {
        TrackState& track = track_of(state, 1);
        const double speed = finite_arg(state, 2, "speed", 1.0);
        engine_core::track_adjust_speed(track, static_cast<float>(speed),
                                        static_cast<float>(finite_arg(state, 3, "fade", kDefaultFade)));
        return 0;
    });
}

int ScriptBindings::track_destroy(lua_State* state) {
    return lua_guard(state, [&] {
        Animator* animator = nullptr;
        const TrackState& track = track_of(state, 1, &animator);
        animator->destroy_track(track.id);
        return 0;
    });
}

void open_animation_track(lua_State* state) {
    static const luaL_Reg kMethods[] = {
        {"Play", &ScriptBindings::track_play},
        {"Stop", &ScriptBindings::track_stop},
        {"AdjustWeight", &ScriptBindings::track_adjust_weight},
        {"AdjustSpeed", &ScriptBindings::track_adjust_speed},
        {"Destroy", &ScriptBindings::track_destroy},
        {nullptr, nullptr},
    };
    luaL_newmetatable(state, kTrackMeta);
    lua_createtable(state, 0, 5);
    for (const luaL_Reg* entry = kMethods; entry->name != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    lua_setreadonly(state, -1, true);
    lua_pushcclosure(state, &ScriptBindings::track_index, "index", 1);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, &ScriptBindings::track_newindex, "newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, track_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, track_eq, "eq");
    lua_setfield(state, -2, "__eq");
    lua_pushliteral(state, "AnimationTrack");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

}  // namespace engine_core
