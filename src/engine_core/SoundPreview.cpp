#include "SoundPreview.hpp"

#pragma warning(push, 0)
#include "miniaudio.h"
#pragma warning(pop)

#include <algorithm>

namespace engine_core {

struct SoundPreview::Impl {
    // miniaudio keeps pointers into both, so neither moves.
    std::unique_ptr<ma_engine> engine;
    std::unique_ptr<ma_sound> sound;
    std::filesystem::path file;
    bool paused = false;
    float volume = 1.f;
    float pitch = 1.f;
    bool looped = false;

    void mix() {
        if (sound) {
            ma_sound_set_volume(sound.get(), volume);
            ma_sound_set_pitch(sound.get(), pitch);
            ma_sound_set_looping(sound.get(), looped ? MA_TRUE : MA_FALSE);
        }
    }

    ~Impl() {
        drop();
        if (engine) {
            ma_engine_uninit(engine.get());
        }
    }

    void drop() {
        if (sound) {
            ma_sound_uninit(sound.get());
            sound.reset();
        }
        file.clear();
        paused = false;
    }

    // A sound that is held or still has some to play.
    bool live() const { return sound && (paused || !ma_sound_at_end(sound.get())); }
};

SoundPreview::SoundPreview() : impl_(std::make_unique<Impl>()) {}

SoundPreview::~SoundPreview() = default;

bool SoundPreview::play(const std::filesystem::path& file, std::string& error) {
    impl_->drop();
    if (!impl_->engine) {
        auto made = std::make_unique<ma_engine>();
        ma_engine_config config = ma_engine_config_init();
        if (ma_engine_init(&config, made.get()) != MA_SUCCESS) {
            error = "No audio device could be opened";
            return false;
        }
        impl_->engine = std::move(made);
    }
    auto sound = std::make_unique<ma_sound>();
    // Streamed, so a long file starts at once.
    const ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
#if defined(_WIN32)
    const ma_result result =
        ma_sound_init_from_file_w(impl_->engine.get(), file.c_str(), flags, nullptr, nullptr, sound.get());
#else
    const ma_result result =
        ma_sound_init_from_file(impl_->engine.get(), file.c_str(), flags, nullptr, nullptr, sound.get());
#endif
    if (result != MA_SUCCESS) {
        error = ma_result_description(result);
        return false;
    }
    impl_->sound = std::move(sound);
    impl_->mix();
    if (ma_sound_start(impl_->sound.get()) != MA_SUCCESS) {
        impl_->drop();
        error = "The sound would not start";
        return false;
    }
    impl_->file = file;
    return true;
}

void SoundPreview::set_mix(double volume, double pitch, bool looped) {
    impl_->volume = static_cast<float>(std::max(0.0, volume));
    // miniaudio ignores a pitch of 0 or less, as AudioWorld notes.
    impl_->pitch = static_cast<float>(std::max(pitch, 0.001));
    impl_->looped = looped;
    impl_->mix();
}

void SoundPreview::pause() {
    if (state() == State::Playing) {
        ma_sound_stop(impl_->sound.get());
        impl_->paused = true;
    }
}

void SoundPreview::resume() {
    if (state() == State::Paused) {
        ma_sound_start(impl_->sound.get());
        impl_->paused = false;
    }
}

void SoundPreview::stop() { impl_->drop(); }

void SoundPreview::seek(double seconds) {
    if (impl_->live()) {
        ma_sound_seek_to_second(impl_->sound.get(), static_cast<float>(std::max(0.0, seconds)));
    }
}

SoundPreview::State SoundPreview::state() const {
    if (!impl_->live()) {
        return State::Stopped;
    }
    return impl_->paused ? State::Paused : State::Playing;
}

double SoundPreview::position() const {
    float cursor = 0.f;
    if (!impl_->live() || ma_sound_get_cursor_in_seconds(impl_->sound.get(), &cursor) != MA_SUCCESS) {
        return 0;
    }
    return cursor;
}

double SoundPreview::length() const {
    float seconds = 0.f;
    if (!impl_->live() || ma_sound_get_length_in_seconds(impl_->sound.get(), &seconds) != MA_SUCCESS) {
        return 0;
    }
    return seconds;
}

const std::filesystem::path& SoundPreview::file() const { return impl_->file; }

double SoundPreview::length_of(const std::filesystem::path& file) {
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
