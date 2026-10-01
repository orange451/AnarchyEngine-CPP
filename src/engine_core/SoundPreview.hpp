#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace engine_core {

// One sound file played straight to the output device, for the studio to
// audition a Sound while the place is not playing. It has its own miniaudio
// engine, apart from AudioWorld's, opened on the first play and kept until
// the preview goes. Not spatialized; at full volume, once through, unless
// set_mix says otherwise.
// Only this class's source and AudioWorld's include miniaudio.
class SoundPreview {
public:
    enum class State { Stopped, Playing, Paused };

    SoundPreview();
    ~SoundPreview();
    SoundPreview(const SoundPreview&) = delete;
    SoundPreview& operator=(const SoundPreview&) = delete;

    // Plays file from the start, stopping whatever played before. False, with
    // why in error, when no device opens or the file does not play.
    bool play(const std::filesystem::path& file, std::string& error);
    // Holds the sound where it is, and plays on from there.
    void pause();
    void resume();
    void stop();
    // Moves a playing or paused sound to seconds from its start.
    void seek(double seconds);
    // How the sound plays, now and on each later play, as a SoundEmitter's
    // Volume, Pitch, and Looped. The default is 1, 1, and once through.
    void set_mix(double volume, double pitch, bool looped);

    // Playing or Paused from a play until stop. A sound that played to its
    // end is Stopped.
    State state() const;
    // Where the sound is, in seconds; 0 while Stopped.
    double position() const;
    // How long the sound is, in seconds; 0 while Stopped.
    double length() const;
    // The file last played, empty after stop.
    const std::filesystem::path& file() const;

    // How long file plays, in seconds, read without a device; 0 when it does
    // not decode.
    static double length_of(const std::filesystem::path& file);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine_core
