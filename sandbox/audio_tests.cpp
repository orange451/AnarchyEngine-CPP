// SoundEmitter, which plays a Sound while the place plays, AudioWorld, which
// plays it through miniaudio, and PVInstance, the class of everything with a
// Transform. Importing sound files is tested in tests/TextureImportTest.cpp.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "AudioWorld.hpp"
#include "Camera.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "Light.hpp"
#include "LuaApi.hpp"
#include "PVInstance.hpp"
#include "PhysicsObject.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SceneService.hpp"
#include "SoundEmitter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

using engine_core::GameObject;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::SoundEmitter;
using engine_core::Vec3;

constexpr double kFrame = 1.0 / 60.0;

void put16(std::ofstream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xff), static_cast<char>(value >> 8)};
    out.write(bytes, 2);
}

void put32(std::ofstream& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        const char byte = static_cast<char>((value >> shift) & 0xff);
        out.write(&byte, 1);
    }
}

// A mono 16-bit WAV of a 440 Hz tone, seconds long.
void write_tone(const std::filesystem::path& file, double seconds) {
    constexpr std::uint32_t kRate = 44100;
    const auto frames = static_cast<std::uint32_t>(seconds * kRate);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write("RIFF", 4);
    put32(out, 36 + frames * 2);
    out.write("WAVEfmt ", 8);
    put32(out, 16);
    put16(out, 1);
    put16(out, 1);
    put32(out, kRate);
    put32(out, kRate * 2);
    put16(out, 2);
    put16(out, 16);
    out.write("data", 4);
    put32(out, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double sample = std::sin(2.0 * 3.14159265358979 * 440.0 * i / kRate) * 0.25;
        put16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 32767)));
    }
}

// A Game with a resources folder holding a one-second tone and a two-second
// one, their Sounds, an audio world with no device, and its warnings.
struct AudioRig {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::AudioWorld audio{false};
    std::vector<std::string> warnings;
    InstanceId tone = 0;
    InstanceId long_tone = 0;

    AudioRig() {
        audio.set_warning_sink([this](const std::string& text) { warnings.push_back(text); });
        write_tone(dir / "sounds/tone.wav", 1.0);
        write_tone(dir / "sounds/long.wav", 2.0);
        game.set_resources_root(dir.path);
        tone = sound("Tone", "sounds/tone.wav");
        long_tone = sound("Long", "sounds/long.wav");
    }

    InstanceId sound(const char* name, const char* path) {
        engine_core::Sound& made = game.create<engine_core::Sound>();
        game.set_name(made.id(), name);
        REQUIRE_FALSE(made.set_path(path));
        game.set_parent(made.id(), game.service("Audio"));
        return made.id();
    }

    SoundEmitter& emitter(InstanceId parent, InstanceId plays) {
        SoundEmitter& made = game.create<SoundEmitter>();
        engine_core::LuaSlot slot;
        if (plays != 0) {
            slot.kind = engine_core::LuaSlot::Kind::Instance;
            slot.id = plays;
        }
        REQUIRE_FALSE(made.set_sound(slot));
        game.set_parent(made.id(), parent);
        return made;
    }

    GameObject& part(float x, float y, float z) {
        GameObject& made = create_part(game);
        made.set_transform(engine_core::matrix4_translation(x, y, z));
        return made;
    }

    void play() {
        game.capture_place();
        game.start_simulation();
    }

    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            audio.step(game, kFrame);
        }
    }
};

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

}  // namespace

TEST_CASE("A1 every class with a Transform is a PVInstance", "[audio]") {
    for (const char* klass : {"GameObject", "Camera", "PointLight", "SpotLight", "PhysicsObject"}) {
        INFO(klass);
        REQUIRE(engine_core::lua_class_inherits(klass, "PVInstance"));
        REQUIRE(engine_core::lua_class_find(klass, "Transform") != nullptr);
    }
    for (const char* klass : {"Folder", "SoundEmitter", "DirectionalLight", "Sound", "Script"}) {
        INFO(klass);
        REQUIRE_FALSE(engine_core::lua_class_inherits(klass, "PVInstance"));
    }
    REQUIRE(engine_core::lua_class_inherits("PVInstance", "Instance"));
    REQUIRE_FALSE(engine_core::lua_creatable_known("PVInstance"));

    SimRole role;
    engine_core::Game game;
    engine_core::PhysicsObject& body = game.create<engine_core::PhysicsObject>();
    REQUIRE_FALSE(body.set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f)));
    const engine_core::PVInstance* pv = dynamic_cast<engine_core::PVInstance*>(&body);
    REQUIRE(pv != nullptr);
    REQUIRE(pv->transform().m[13] == 2.f);
    REQUIRE(dynamic_cast<engine_core::PVInstance*>(&game.create<engine_core::Camera>()) != nullptr);
}

TEST_CASE("A2 in a PVInstance, a sound plays from its position and follows it", "[audio]") {
    AudioRig rig;
    GameObject& part = rig.part(4.f, 5.f, 6.f);
    SoundEmitter& emitter = rig.emitter(part.id(), rig.tone);
    rig.play();
    rig.frames(1);
    REQUIRE(rig.audio.voice_count() == 0);
    REQUIRE_FALSE(emitter.is_playing());

    emitter.play();
    rig.frames(1);
    REQUIRE(rig.audio.has_voice(emitter.id()));
    REQUIRE(rig.audio.voice_sounding(emitter.id()));
    REQUIRE(rig.audio.spatial(emitter.id()));
    Vec3 at = rig.audio.voice_position(emitter.id());
    REQUIRE((at.x == 4.f && at.y == 5.f && at.z == 6.f));

    part.set_transform(engine_core::matrix4_translation(-1.f, 0.f, 9.f));
    rig.frames(1);
    at = rig.audio.voice_position(emitter.id());
    REQUIRE((at.x == -1.f && at.y == 0.f && at.z == 9.f));

    // A PhysicsObject is a PVInstance too.
    engine_core::PhysicsObject& body = rig.game.create<engine_core::PhysicsObject>();
    REQUIRE_FALSE(body.set_transform(engine_core::matrix4_translation(7.f, 0.f, 0.f)));
    rig.game.set_parent(body.id(), rig.game.service("Storage"));
    rig.game.set_parent(emitter.id(), body.id());
    rig.frames(1);
    REQUIRE(rig.audio.spatial(emitter.id()));
    REQUIRE(rig.audio.voice_position(emitter.id()).x == 7.f);
}

TEST_CASE("A3 outside a PVInstance, a sound plays in 2D", "[audio]") {
    AudioRig rig;
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(folder.id(), workspace_of(rig.game));
    SoundEmitter& in_folder = rig.emitter(folder.id(), rig.tone);
    SoundEmitter& in_workspace = rig.emitter(workspace_of(rig.game), rig.tone);
    GameObject& part = rig.part(0.f, 0.f, 0.f);
    SoundEmitter& moved = rig.emitter(part.id(), rig.tone);
    rig.play();
    in_folder.play();
    in_workspace.play();
    moved.play();
    rig.frames(1);
    REQUIRE(rig.audio.voice_count() == 3);
    REQUIRE_FALSE(rig.audio.spatial(in_folder.id()));
    REQUIRE_FALSE(rig.audio.spatial(in_workspace.id()));
    REQUIRE(rig.audio.spatial(moved.id()));

    // Taken out of its PVInstance, it plays on, in 2D.
    rig.game.set_parent(moved.id(), folder.id());
    rig.frames(1);
    REQUIRE(rig.audio.has_voice(moved.id()));
    REQUIRE_FALSE(rig.audio.spatial(moved.id()));
}

TEST_CASE("A4 destroying the PVInstance, the SoundEmitter, or the Sound stops it at once", "[audio]") {
    AudioRig rig;
    GameObject& part = rig.part(1.f, 0.f, 0.f);
    SoundEmitter& under_part = rig.emitter(part.id(), rig.long_tone);
    SoundEmitter& alone = rig.emitter(workspace_of(rig.game), rig.long_tone);
    SoundEmitter& of_tone = rig.emitter(workspace_of(rig.game), rig.tone);
    const InstanceId under_part_id = under_part.id();
    rig.play();
    under_part.play();
    alone.play();
    of_tone.play();
    rig.frames(2);
    REQUIRE(rig.audio.voice_count() == 3);

    rig.game.destroy(part.id());
    rig.frames(1);
    REQUIRE_FALSE(rig.audio.has_voice(under_part_id));
    REQUIRE(rig.audio.voice_count() == 2);

    const InstanceId alone_id = alone.id();
    rig.game.destroy(alone_id);
    rig.frames(1);
    REQUIRE_FALSE(rig.audio.has_voice(alone_id));

    rig.game.destroy(rig.tone);
    rig.frames(1);
    REQUIRE_FALSE(rig.audio.has_voice(of_tone.id()));
    REQUIRE_FALSE(of_tone.is_playing());
    REQUIRE(rig.audio.voice_count() == 0);
}

TEST_CASE("A5 a sound ends unless Looped; TimePosition follows it, and Stop resets it", "[audio]") {
    AudioRig rig;
    SoundEmitter& once = rig.emitter(workspace_of(rig.game), rig.tone);
    SoundEmitter& looped = rig.emitter(workspace_of(rig.game), rig.tone);
    looped.set_looped(true);
    rig.play();
    once.play();
    looped.play();
    rig.frames(31);
    INFO(once.time_position());
    REQUIRE(near(static_cast<float>(once.time_position()), 0.5f, 0.05f));
    REQUIRE(once.is_playing());

    rig.frames(40);
    REQUIRE_FALSE(once.is_playing());
    REQUIRE(once.time_position() == 0.0);
    REQUIRE_FALSE(rig.audio.has_voice(once.id()));
    REQUIRE(looped.is_playing());
    REQUIRE(rig.audio.has_voice(looped.id()));
    REQUIRE(looped.time_position() < 0.5);

    looped.stop();
    REQUIRE(looped.time_position() == 0.0);
    rig.frames(1);
    REQUIRE_FALSE(rig.audio.has_voice(looped.id()));

    // Play starts from TimePosition; a write while playing seeks.
    REQUIRE_FALSE(once.set_time_position(0.75));
    once.play();
    rig.frames(1);
    REQUIRE(once.time_position() >= 0.75);
    REQUIRE_FALSE(once.set_time_position(0.25));
    rig.frames(1);
    REQUIRE(once.time_position() < 0.5);
    // Play while playing starts over.
    rig.frames(10);
    once.play();
    REQUIRE(once.time_position() == 0.0);
    rig.frames(1);
    REQUIRE(once.time_position() < 0.1);
}

TEST_CASE("A6 Volume, Pitch, and roll-off go into the voice; numbers are clamped", "[audio]") {
    AudioRig rig;
    SoundEmitter& emitter = rig.emitter(workspace_of(rig.game), rig.long_tone);
    REQUIRE(emitter.volume() == SoundEmitter::kDefaultVolume);
    REQUIRE(emitter.roll_off_mode() == SoundEmitter::RollOff::Inverse);
    REQUIRE_FALSE(emitter.set_volume(9.0));
    REQUIRE(emitter.volume() == 5.0);
    REQUIRE_FALSE(emitter.set_pitch(-1.0));
    REQUIRE(emitter.pitch() == 0.0);
    REQUIRE_FALSE(emitter.set_roll_off_max_distance(1000.0));
    REQUIRE(emitter.roll_off_max_distance() == 512.0);
    REQUIRE_FALSE(emitter.set_roll_off_min_distance(-3.0));
    REQUIRE(emitter.roll_off_min_distance() == 0.0);
    REQUIRE(*emitter.set_volume(std::nan("")) == "Volume must be a finite number");
    REQUIRE(emitter.set_roll_off_mode(7).has_value());

    rig.play();
    REQUIRE_FALSE(emitter.set_volume(2.0));
    REQUIRE_FALSE(emitter.set_pitch(1.5));
    emitter.play();
    rig.frames(1);
    REQUIRE(rig.audio.voice_volume(emitter.id()) == 2.f);
    REQUIRE(rig.audio.voice_pitch(emitter.id()) == 1.5f);
    REQUIRE_FALSE(emitter.set_volume(0.25));
    rig.frames(1);
    REQUIRE(rig.audio.voice_volume(emitter.id()) == 0.25f);
}

TEST_CASE("A7 Stop of the place drops every voice; suspend silences them until the next step", "[audio]") {
    AudioRig rig;
    SoundEmitter& emitter = rig.emitter(workspace_of(rig.game), rig.long_tone);
    rig.play();
    emitter.play();
    rig.frames(1);
    REQUIRE(rig.audio.voice_sounding(emitter.id()));

    rig.audio.suspend();
    REQUIRE(rig.audio.has_voice(emitter.id()));
    REQUIRE_FALSE(rig.audio.voice_sounding(emitter.id()));
    const double at = emitter.time_position();
    rig.frames(1);
    REQUIRE(rig.audio.voice_sounding(emitter.id()));
    REQUIRE(emitter.time_position() >= at);

    rig.game.stop_simulation();
    rig.frames(1);
    REQUIRE(rig.audio.voice_count() == 0);
    REQUIRE_FALSE(emitter.is_playing());
    REQUIRE(emitter.time_position() == 0.0);
}

TEST_CASE("A8 a file that cannot be played warns once and stops the SoundEmitter", "[audio]") {
    AudioRig rig;
    const InstanceId missing = rig.sound("Missing", "sounds/missing.wav");
    SoundEmitter& emitter = rig.emitter(workspace_of(rig.game), missing);
    SoundEmitter& silent = rig.emitter(workspace_of(rig.game), 0);
    rig.play();
    emitter.play();
    silent.play();
    rig.frames(1);
    REQUIRE(rig.audio.voice_count() == 0);
    REQUIRE_FALSE(emitter.is_playing());
    REQUIRE_FALSE(silent.is_playing());
    REQUIRE(rig.warnings.size() == 1);
    REQUIRE(rig.warnings[0].find("sounds/missing.wav") != std::string::npos);
    emitter.play();
    rig.frames(1);
    REQUIRE(rig.warnings.size() == 1);
}

TEST_CASE("A9 the listener is Workspace.CurrentCamera", "[audio]") {
    AudioRig rig;
    engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
    camera.set_transform(engine_core::matrix4_translation(3.f, 2.f, 1.f));
    rig.game.set_parent(camera.id(), workspace_of(rig.game));
    auto* workspace = dynamic_cast<engine_core::Workspace*>(rig.game.instance(workspace_of(rig.game)));
    REQUIRE(workspace != nullptr);
    REQUIRE(workspace->set_current_camera(camera.id()));
    SoundEmitter& emitter = rig.emitter(workspace_of(rig.game), rig.tone);
    rig.play();
    emitter.play();
    rig.frames(1);
    const Vec3 at = rig.audio.listener_position();
    REQUIRE((at.x == 3.f && at.y == 2.f && at.z == 1.f));
}

TEST_CASE("A10 scripts play and stop a SoundEmitter and set its properties", "[audio]") {
    ScriptRig rig;
    TempDir dir;
    write_tone(dir / "sounds/tone.wav", 1.0);
    rig.game.set_resources_root(dir.path);
    engine_core::AudioWorld audio(false);
    engine_core::Sound& sound = rig.game.create<engine_core::Sound>();
    rig.game.set_name(sound.id(), "Tone");
    REQUIRE_FALSE(sound.set_path("sounds/tone.wav"));
    rig.game.set_parent(sound.id(), rig.game.service("Audio"));
    add_script(rig.game, "Player", R"(
        local part = Instance.new("GameObject", workspace)
        local emitter = Instance.new("SoundEmitter")
        _G.default = emitter.Volume == 0.5 and emitter.Pitch == 1 and emitter.Looped == false
            and emitter.RollOffMode == Enum.RollOffMode.Inverse and emitter.RollOffMinDistance == 10
            and emitter.RollOffMaxDistance == 100 and emitter.TimePosition == 0 and emitter.Sound == nil
            and emitter.IsPlaying == false
        emitter.Sound = game:GetService("Assets"):FindFirstChild("Audio"):FindFirstChild("Tone")
        emitter.RollOffMode = "Linear"
        _G.mode = emitter.RollOffMode == Enum.RollOffMode.Linear
        emitter.RollOffMode = Enum.RollOffMode.Exponential
        _G.mode = _G.mode and emitter.RollOffMode == Enum.RollOffMode.Exponential
            and not pcall(function() emitter.RollOffMode = "Tapered" end)
            and not pcall(function() emitter.IsPlaying = true end)
            and not pcall(function() emitter.Sound = part end)
        emitter.Volume = 10
        _G.clamped = emitter.Volume == 5
        _G.pv = part:IsA("PVInstance") and not emitter:IsA("PVInstance")
        emitter.Parent = part
        emitter:Play()
        _G.playing = emitter.IsPlaying
        _G.emitter = emitter
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"default", "mode", "clamped", "pv", "playing"}) {
        INFO(name);
        bool value = false;
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    audio.step(rig.game, kFrame);
    REQUIRE(audio.voice_count() == 1);

    add_script(rig.game, "Stopper", "_G.emitter:Stop() _G.stopped = not _G.emitter.IsPlaying");
    rig.frames(1, 0.05);
    bool stopped = false;
    REQUIRE(rig.runtime.global_boolean("stopped", stopped));
    REQUIRE(stopped);
    audio.step(rig.game, kFrame);
    REQUIRE(audio.voice_count() == 0);
}

TEST_CASE("A11 SoundEmitter properties save, load, and come back at Stop", "[audio]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("SoundEmitter"));
    SoundEmitter& emitter = game.create<SoundEmitter>();
    game.set_parent(emitter.id(), workspace_of(game));
    engine_core::PropertyBag saved;
    emitter.save_properties(saved);
    REQUIRE(saved.empty());

    REQUIRE_FALSE(emitter.set_roll_off_mode(static_cast<int>(SoundEmitter::RollOff::None)));
    emitter.set_looped(true);
    engine_core::PropertyBag changed;
    emitter.save_properties(changed);
    const engine_core::JsonValue* mode = engine_core::bag_find(changed, "RollOffMode");
    REQUIRE(mode != nullptr);
    REQUIRE(mode->as_string() == "None");
    REQUIRE(engine_core::bag_find(changed, "IsPlaying") == nullptr);

    SoundEmitter& copy = game.create<SoundEmitter>();
    std::string error;
    REQUIRE(copy.load_property("RollOffMode", engine_core::JsonValue::string("Linear"), error));
    REQUIRE(copy.roll_off_mode() == SoundEmitter::RollOff::Linear);

    game.capture_place();
    game.start_simulation();
    emitter.set_looped(false);
    REQUIRE_FALSE(emitter.set_volume(3.0));
    game.stop_simulation();
    REQUIRE(emitter.looped());
    REQUIRE(emitter.volume() == SoundEmitter::kDefaultVolume);
}

