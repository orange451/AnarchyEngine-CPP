#include "AudioWorld.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "PVInstance.hpp"
#include "SceneService.hpp"
#include "SoundEmitter.hpp"

#pragma warning(push, 0)
#include "miniaudio.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

// Without a device, the mix is read at this rate, in stereo.
constexpr ma_uint32 kOfflineRate = 48000;
constexpr ma_uint32 kOfflineChannels = 2;
// miniaudio ignores a pitch of 0 or less, so 0 is played as this: as good as still.
constexpr float kMinPitch = 0.001f;

ma_attenuation_model attenuation_of(SoundEmitter::RollOff mode) {
    switch (mode) {
    case SoundEmitter::RollOff::Linear:
        return ma_attenuation_model_linear;
    case SoundEmitter::RollOff::Exponential:
        return ma_attenuation_model_exponential;
    case SoundEmitter::RollOff::None:
        return ma_attenuation_model_none;
    case SoundEmitter::RollOff::Inverse:
    default:
        return ma_attenuation_model_inverse;
    }
}

Vec3 unit(Vec3 v, Vec3 fallback) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 1e-6f) || !std::isfinite(length)) {
        return fallback;
    }
    return Vec3{v.x / length, v.y / length, v.z / length};
}

}  // namespace

struct AudioWorld::Impl {
    struct Voice {
        // miniaudio keeps pointers into a ma_sound, so it never moves.
        std::unique_ptr<ma_sound> sound;
        InstanceId asset = 0;
        std::filesystem::path file;
        std::uint32_t serial = 0;
        bool spatial = false;
        std::uint64_t seen = 0;
    };

    explicit Impl(bool use_device) : want_device(use_device) {
        voices.reserve(256);
        sources.reserve(256);
        gone.reserve(256);
    }

    ~Impl() { close(); }

    mutable std::mutex mu;
    bool want_device = true;
    std::unique_ptr<ma_engine> engine;
    bool offline = false;
    double offline_frames = 0;
    std::vector<float> mix;
    std::uint32_t generation = 0;
    std::uint64_t pass = 0;
    std::unordered_map<InstanceId, Voice> voices;
    std::function<void(const std::string&)> warn;

    // Scratch, kept between steps.
    std::vector<InstanceId> sources;
    std::vector<InstanceId> gone;

    void say(const std::string& text) const {
        if (warn) {
            warn(text);
        }
    }

    void close() {
        drop_all();
        if (engine) {
            ma_engine_uninit(engine.get());
            engine.reset();
        }
    }

    // The miniaudio engine, opened the first time a voice needs it.
    bool open() {
        if (engine) {
            return true;
        }
        auto made = std::make_unique<ma_engine>();
        ma_engine_config config = ma_engine_config_init();
        if (want_device && ma_engine_init(&config, made.get()) == MA_SUCCESS) {
            engine = std::move(made);
            offline = false;
            return true;
        }
        if (want_device) {
            say("No audio device could be opened, so sounds play silently");
        }
        config = ma_engine_config_init();
        config.noDevice = MA_TRUE;
        config.channels = kOfflineChannels;
        config.sampleRate = kOfflineRate;
        if (ma_engine_init(&config, made.get()) != MA_SUCCESS) {
            return false;
        }
        engine = std::move(made);
        offline = true;
        return true;
    }

    void drop(Voice& voice) {
        if (voice.sound) {
            ma_sound_uninit(voice.sound.get());
            voice.sound.reset();
        }
    }

    void drop_all() {
        for (auto& [id, voice] : voices) {
            drop(voice);
        }
        voices.clear();
    }

    void erase(InstanceId id) {
        const auto found = voices.find(id);
        if (found != voices.end()) {
            drop(found->second);
            voices.erase(found);
        }
    }

    void suspend() {
        std::lock_guard<std::mutex> guard(mu);
        for (auto& [id, voice] : voices) {
            ma_sound_stop(voice.sound.get());
        }
    }

    void step(DataModel& game, double dt) {
        std::lock_guard<std::mutex> guard(mu);
        if (!game.simulation_running()) {
            drop_all();
            return;
        }
        if (generation != game.world_generation()) {
            drop_all();
            generation = game.world_generation();
        }
        ++pass;
        game.sound_sources(sources);
        for (InstanceId id : sources) {
            if (auto* emitter = dynamic_cast<SoundEmitter*>(game.instance(id))) {
                update(game, id, *emitter);
            }
        }
        // Destroyed, or out of game: stopped now.
        gone.clear();
        for (const auto& [id, voice] : voices) {
            if (voice.seen != pass) {
                gone.push_back(id);
            }
        }
        for (InstanceId id : gone) {
            erase(id);
        }
        place_listener(game);
        if (engine && offline && dt > 0) {
            advance(dt);
        }
    }

    void update(DataModel& game, InstanceId id, SoundEmitter& emitter) {
        const std::uint32_t dirty = emitter.take_dirty();
        if (!emitter.is_playing()) {
            erase(id);
            return;
        }
        const InstanceId asset = emitter.sound_id();
        const auto* sound = asset != 0 ? dynamic_cast<const Sound*>(game.instance(asset)) : nullptr;
        if (sound == nullptr || sound->path().empty() || game.resources_root().empty()) {
            // No Sound, or it was destroyed: nothing to hear.
            erase(id);
            emitter.store_playback(0.0, false);
            return;
        }
        const std::filesystem::path file = game.resources_root() / std::filesystem::u8path(sound->path());
        auto found = voices.find(id);
        if (found != voices.end() &&
            (found->second.serial != emitter.play_serial() || found->second.asset != asset || found->second.file != file)) {
            erase(id);
            found = voices.end();
        }
        if (found == voices.end()) {
            Voice voice;
            if (!start(game, id, emitter, sound->path(), file, voice)) {
                emitter.store_playback(0.0, false);
                return;
            }
            voice.asset = asset;
            voice.file = file;
            voice.serial = emitter.play_serial();
            found = voices.emplace(id, std::move(voice)).first;
        } else {
            ma_sound* playing = found->second.sound.get();
            if ((dirty & SoundEmitter::kDirtyMix) != 0) {
                mix_into(emitter, *playing);
            }
            if ((dirty & SoundEmitter::kDirtyRollOff) != 0) {
                roll_off_into(emitter, *playing);
            }
            if ((dirty & SoundEmitter::kDirtySeek) != 0) {
                ma_sound_seek_to_second(playing, static_cast<float>(emitter.time_position()));
            }
        }
        Voice& voice = found->second;
        voice.seen = pass;
        place(game, id, voice);
        ma_sound* playing = voice.sound.get();
        if (!emitter.looped() && ma_sound_at_end(playing)) {
            erase(id);
            emitter.store_playback(0.0, false);
            return;
        }
        // Stopped by suspend, or not yet started.
        if (!ma_sound_is_playing(playing)) {
            ma_sound_start(playing);
        }
        float cursor = 0.f;
        if (ma_sound_get_cursor_in_seconds(playing, &cursor) == MA_SUCCESS) {
            emitter.store_playback(cursor, true);
        }
    }

    bool start(DataModel& game, InstanceId id, SoundEmitter& emitter, const std::string& path,
               const std::filesystem::path& file, Voice& voice) {
        if (!open()) {
            return false;
        }
        voice.sound = std::make_unique<ma_sound>();
        // Decoded whole as it loads, so a seek is exact and costs nothing.
        const ma_uint32 flags = MA_SOUND_FLAG_DECODE;
#if defined(_WIN32)
        const ma_result result =
            ma_sound_init_from_file_w(engine.get(), file.c_str(), flags, nullptr, nullptr, voice.sound.get());
#else
        const ma_result result =
            ma_sound_init_from_file(engine.get(), file.c_str(), flags, nullptr, nullptr, voice.sound.get());
#endif
        if (result != MA_SUCCESS) {
            voice.sound.reset();
            if (!emitter.warned_file) {
                emitter.warned_file = true;
                say("SoundEmitter " + game.name(id) + ": could not play " + path + " (" + ma_result_description(result) +
                    ")");
            }
            return false;
        }
        emitter.warned_file = false;
        ma_sound_set_doppler_factor(voice.sound.get(), 0.f);
        mix_into(emitter, *voice.sound);
        roll_off_into(emitter, *voice.sound);
        if (emitter.time_position() > 0) {
            ma_sound_seek_to_second(voice.sound.get(), static_cast<float>(emitter.time_position()));
        }
        return true;
    }

    static void mix_into(const SoundEmitter& emitter, ma_sound& sound) {
        ma_sound_set_volume(&sound, static_cast<float>(emitter.volume()));
        ma_sound_set_pitch(&sound, std::max(static_cast<float>(emitter.pitch()), kMinPitch));
        ma_sound_set_looping(&sound, emitter.looped() ? MA_TRUE : MA_FALSE);
    }

    static void roll_off_into(const SoundEmitter& emitter, ma_sound& sound) {
        ma_sound_set_attenuation_model(&sound, attenuation_of(emitter.roll_off_mode()));
        ma_sound_set_min_distance(&sound, static_cast<float>(emitter.roll_off_min_distance()));
        ma_sound_set_max_distance(&sound, static_cast<float>(emitter.roll_off_max_distance()));
    }

    // From the parent PVInstance's position, or 2D when the parent is none.
    void place(DataModel& game, InstanceId id, Voice& voice) {
        const InstanceId parent = game.parent(id);
        const auto* anchor = parent != DataModel::kNoParent ? dynamic_cast<const PVInstance*>(game.instance(parent)) : nullptr;
        voice.spatial = anchor != nullptr;
        ma_sound_set_spatialization_enabled(voice.sound.get(), voice.spatial ? MA_TRUE : MA_FALSE);
        if (voice.spatial) {
            const Matrix4 at = anchor->transform();
            ma_sound_set_position(voice.sound.get(), at.m[12], at.m[13], at.m[14]);
        }
    }

    void place_listener(DataModel& game) {
        if (!engine) {
            return;
        }
        const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.service("Workspace")));
        const InstanceId camera = workspace != nullptr ? workspace->current_camera() : 0;
        const auto* eye = camera != 0 ? dynamic_cast<const PVInstance*>(game.instance(camera)) : nullptr;
        if (eye == nullptr) {
            return;
        }
        const Matrix4 at = eye->transform();
        const Vec3 forward = unit(Vec3{-at.m[8], -at.m[9], -at.m[10]}, Vec3{0.f, 0.f, -1.f});
        const Vec3 up = unit(Vec3{at.m[4], at.m[5], at.m[6]}, Vec3{0.f, 1.f, 0.f});
        ma_engine_listener_set_position(engine.get(), 0, at.m[12], at.m[13], at.m[14]);
        ma_engine_listener_set_direction(engine.get(), 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(engine.get(), 0, up.x, up.y, up.z);
    }

    // No device pulls the mix, so dt's worth is read here and thrown away.
    void advance(double dt) {
        offline_frames += dt * kOfflineRate;
        const auto frames = static_cast<ma_uint64>(offline_frames);
        offline_frames -= static_cast<double>(frames);
        if (frames == 0) {
            return;
        }
        mix.resize(static_cast<std::size_t>(frames) * kOfflineChannels);
        ma_engine_read_pcm_frames(engine.get(), mix.data(), frames, nullptr);
    }

    const Voice* voice_of(InstanceId id) const {
        const auto found = voices.find(id);
        return found != voices.end() && found->second.sound ? &found->second : nullptr;
    }
};

AudioWorld::AudioWorld(bool device) : impl_(std::make_unique<Impl>(device)) {}

AudioWorld::~AudioWorld() = default;

void AudioWorld::step(DataModel& game, double dt) { impl_->step(game, dt); }

void AudioWorld::suspend() { impl_->suspend(); }

std::size_t AudioWorld::voice_count() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->voices.size();
}

bool AudioWorld::has_voice(InstanceId emitter) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    return impl_->voice_of(emitter) != nullptr;
}

bool AudioWorld::spatial(InstanceId emitter) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const Impl::Voice* voice = impl_->voice_of(emitter);
    return voice != nullptr && voice->spatial;
}

Vec3 AudioWorld::voice_position(InstanceId emitter) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const Impl::Voice* voice = impl_->voice_of(emitter);
    if (voice == nullptr) {
        return Vec3{};
    }
    const ma_vec3f at = ma_sound_get_position(voice->sound.get());
    return Vec3{at.x, at.y, at.z};
}

float AudioWorld::voice_volume(InstanceId emitter) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const Impl::Voice* voice = impl_->voice_of(emitter);
    return voice != nullptr ? ma_sound_get_volume(voice->sound.get()) : 0.f;
}

float AudioWorld::voice_pitch(InstanceId emitter) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const Impl::Voice* voice = impl_->voice_of(emitter);
    return voice != nullptr ? ma_sound_get_pitch(voice->sound.get()) : 0.f;
}

bool AudioWorld::voice_sounding(InstanceId emitter) const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    const Impl::Voice* voice = impl_->voice_of(emitter);
    return voice != nullptr && ma_sound_is_playing(voice->sound.get());
}

Vec3 AudioWorld::listener_position() const {
    std::lock_guard<std::mutex> guard(impl_->mu);
    if (!impl_->engine) {
        return Vec3{};
    }
    const ma_vec3f at = ma_engine_listener_get_position(impl_->engine.get(), 0);
    return Vec3{at.x, at.y, at.z};
}

void AudioWorld::set_warning_sink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> guard(impl_->mu);
    impl_->warn = std::move(sink);
}

double audio_file_seconds(const std::filesystem::path& file) {
    ma_decoder decoder;
#if defined(_WIN32)
    if (ma_decoder_init_file_w(file.c_str(), nullptr, &decoder) != MA_SUCCESS) {
#else
    if (ma_decoder_init_file(file.c_str(), nullptr, &decoder) != MA_SUCCESS) {
#endif
        return 0;
    }
    ma_uint64 frames = 0;
    double seconds = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && decoder.outputSampleRate > 0) {
        seconds = static_cast<double>(frames) / decoder.outputSampleRate;
    }
    ma_decoder_uninit(&decoder);
    return seconds;
}

}  // namespace engine_core
