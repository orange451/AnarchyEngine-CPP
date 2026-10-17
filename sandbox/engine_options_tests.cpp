// Engine options a headless server needs
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Engine.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("SC38 an Engine made without an audio device still steps a playing place", "[server-client][engine-options]") {
    SimRole role;
    engine_core::EngineOptions options;
    options.audio_device = false;
    engine_core::Engine engine(options);
    REQUIRE_FALSE(engine.has_audio_device());
    engine_core::Engine standard;
    REQUIRE(standard.has_audio_device());
    engine_core::DataModel& game = engine.datamodel();
    add_script(game, "Probe", "_G.stepped = true");
    game.start_simulation();
    engine.resume();
    for (int i = 0; i < 3; ++i) engine.run_frame(1.0 / 60.0);
    REQUIRE(game.simulation_running());
}
