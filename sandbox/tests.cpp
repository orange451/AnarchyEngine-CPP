#include "Contract.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "SnapshotPump.hpp"
#include "TestTriangle.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"
#include "LuaApi.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

struct InstallContractHandler {
    InstallContractHandler() {
        engine_core::set_contract_handler([](const char* message) { throw engine_core::ContractViolation(message); });
    }
};

InstallContractHandler gInstallContractHandler;

bool near(const engine_core::Transform& a, const engine_core::Transform& b) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > 1e-4f) {
            return false;
        }
    }
    return true;
}

bool is_zero(const engine_core::Transform& value) {
    for (float cell : value.m) {
        if (cell != 0.f) {
            return false;
        }
    }
    return true;
}

template <typename Pred>
void wait_until(Pred pred, std::chrono::milliseconds budget = std::chrono::seconds(3)) {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > budget) {
            FAIL("timed out waiting for the simulation");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

engine_core::Transform T0() { return engine_core::transform_translation(3.f, 4.f, 5.f); }

engine_core::Transform T1() { return engine_core::transform_translation(50.f, 0.f, 0.f); }

engine_core::ColorRgb rgb(float r, float g, float b) {
    engine_core::ColorRgb color;
    color.r = r;
    color.g = g;
    color.b = b;
    color.a = 1.f;
    return color;
}

bool near_color(engine_core::ColorRgb a, engine_core::ColorRgb b) {
    return std::fabs(a.r - b.r) < 1e-4f && std::fabs(a.g - b.g) < 1e-4f && std::fabs(a.b - b.b) < 1e-4f &&
           std::fabs(a.a - b.a) < 1e-4f;
}

const engine_core::VisualInstance* find_instance(const engine_core::VisualSnapshot& snapshot,
                                                  engine_core::InstanceId id) {
    for (const engine_core::VisualInstance& item : snapshot.instances) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

class CountingRenderer : public engine_core::IRenderer {
public:
    void perform(const engine_core::VisualSnapshot&) override {}
    void present() override {}
};

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

}  // namespace

TEST_CASE("simulation and render are different threads", "[T1]") {
    engine_core::Engine engine;
    engine.start();
    engine.resume();
    REQUIRE(engine.simulation_thread_id() != std::thread::id{});
    REQUIRE(engine.render_thread_id() != std::thread::id{});
    REQUIRE(engine.simulation_thread_id() != engine.render_thread_id());
    engine.stop();
}

TEST_CASE("DataModel write during Perform is rejected", "[T2]") {
    engine_core::Engine engine;
    struct Probe : engine_core::IRenderer {
        engine_core::GameObject* object = nullptr;
        std::atomic<int> hits{0};
        std::string message;
        void perform(const engine_core::VisualSnapshot&) override {
            if (hits.load() != 0) {
                return;
            }
            try {
                object->set_color(engine_core::ColorRgb{});
            } catch (const engine_core::ContractViolation& ex) {
                message = ex.what();
                hits.store(1);
            }
        }
        void present() override {}
    } probe;
    probe.object = &engine.datamodel().create_game_object();
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return probe.hits.load() == 1; });
    REQUIRE(probe.message.find("PreRender") != std::string::npos);
    engine.stop();
}

TEST_CASE("heartbeat writes run alongside present", "[T3]") {
    engine_core::Engine engine;
    CountingRenderer renderer;
    engine.set_renderer(&renderer);
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> writes{0};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (writes.load() != 0) {
            return;
        }
        for (int i = 0; i < 10000; ++i) {
            engine_core::ColorRgb color;
            color.r = static_cast<float>(i & 255) / 255.f;
            color.g = 0.25f;
            color.b = 0.5f;
            color.a = 1.f;
            engine.datamodel().game_object(id)->set_color(color);
        }
        writes.store(10000);
    });
    engine.start();
    engine.resume();
    wait_until([&] { return writes.load() == 10000 && engine.present_count() >= 1000; });
    engine.stop();
}

TEST_CASE("a long sim write does not block present", "[T4]") {
    engine_core::Engine engine;
    CountingRenderer renderer;
    engine.set_renderer(&renderer);
    std::atomic<int> stage{0};
    std::atomic<std::uint64_t> presents_at_sleep{0};
    std::atomic<std::uint64_t> presents_after_sleep{0};
    std::atomic<std::uint64_t> frame_at_sleep{0};
    std::atomic<std::uint64_t> frame_after_sleep{0};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() == 0 && engine.present_count() > 2) {
            // Still inside the sim write lock. Render may Present, but it cannot Prepare.
            presents_at_sleep.store(engine.present_count());
            frame_at_sleep.store(engine.published_frame());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            presents_after_sleep.store(engine.present_count());
            frame_after_sleep.store(engine.published_frame());
            stage.store(2);
        }
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 2; });
    REQUIRE(presents_after_sleep.load() > presents_at_sleep.load());
    REQUIRE(frame_after_sleep.load() == frame_at_sleep.load());
    engine.stop();
}

TEST_CASE("heartbeat transform is live and snapshotted", "[T5]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    const engine_core::Transform expected = T0();
    std::atomic<int> ready{0};
    engine_core::Transform snapped{};
    engine_core::WriteOrigin origin = engine_core::WriteOrigin::SnapshotOverride;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        engine.datamodel().game_object(id)->set_transform(expected);
    });
    struct Probe : CountingRenderer {
        engine_core::Engine* engine = nullptr;
        engine_core::InstanceId id = 0;
        const engine_core::Transform* expected = nullptr;
        engine_core::Transform* snapped = nullptr;
        engine_core::WriteOrigin* origin = nullptr;
        std::atomic<int>* ready = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (ready->load() != 0) {
                return;
            }
            const engine_core::VisualInstance* inst = nullptr;
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id == id) {
                    inst = &item;
                }
            }
            if (inst == nullptr || !near(inst->world, *expected)) {
                return;
            }
            *snapped = inst->world;
            *origin = inst->transform_origin;
            ready->store(1);
        }
    } probe;
    probe.engine = &engine;
    probe.id = id;
    probe.expected = &expected;
    probe.snapped = &snapped;
    probe.origin = &origin;
    probe.ready = &ready;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return ready.load() == 1; });
    engine.stop();
    // Threads are joined, so this live read is the DataModel, not the snapshot.
    REQUIRE(near(engine.datamodel().game_object(id)->transform(), expected));
    REQUIRE(near(snapped, expected));
    REQUIRE(origin == engine_core::WriteOrigin::Simulation);
}

TEST_CASE("path C changes pixels for one frame only", "[T6]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    const engine_core::Transform sim = T0();
    const engine_core::Transform flash = T1();
    std::atomic<int> heartbeats{0};
    std::atomic<int> stage{0};
    engine_core::Transform live{};
    engine_core::Transform snap_override{};
    engine_core::Transform snap_next{};
    engine_core::WriteOrigin override_origin = engine_core::WriteOrigin::Simulation;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        engine.datamodel().game_object(id)->set_transform(sim);
        heartbeats.fetch_add(1);
    });
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 0 || heartbeats.load() == 0) {
            if (stage.load() == 2) {
                stage.store(3);
            }
            return;
        }
        engine_core::SnapshotOverride override;
        override.id = id;
        override.field = engine_core::VisualField::Transform;
        override.transform = flash;
        engine.pump().override_visual(override);
        live = engine.datamodel().game_object(id)->transform();
        stage.store(1);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Transform* snap_override = nullptr;
        engine_core::Transform* snap_next = nullptr;
        engine_core::WriteOrigin* override_origin = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            const engine_core::VisualInstance* inst = nullptr;
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id == id) {
                    inst = &item;
                }
            }
            if (inst == nullptr) {
                return;
            }
            if (stage->load() == 1) {
                *snap_override = inst->world;
                *override_origin = inst->transform_origin;
                stage->store(2);
            } else if (stage->load() == 3) {
                *snap_next = inst->world;
                stage->store(4);
            }
        }
    } probe;
    probe.id = id;
    probe.stage = &stage;
    probe.snap_override = &snap_override;
    probe.snap_next = &snap_next;
    probe.override_origin = &override_origin;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 4; });
    engine.stop();
    REQUIRE(near(live, sim));
    REQUIRE(near(snap_override, flash));
    REQUIRE(override_origin == engine_core::WriteOrigin::SnapshotOverride);
    REQUIRE(near(snap_next, sim));
    REQUIRE(near(engine.datamodel().game_object(id)->transform(), sim));
}

TEST_CASE("path B on a visual-only part becomes sim truth", "[T7]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = game.create_game_object().id();
    game.set_visual_only(id, true);
    const engine_core::Transform posed = T1();
    std::atomic<int> stage{0};
    engine_core::Transform live_at_write{};
    engine_core::Transform snapped{};
    engine_core::Transform live_later{};
    engine_core::WriteOrigin origin = engine_core::WriteOrigin::Simulation;
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        game.game_object(id)->set_transform(posed);
        live_at_write = game.game_object(id)->transform();
        stage.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() < 1) {
            return;
        }
        live_later = game.game_object(id)->transform();
        if (stage.load() == 1) {
            return;
        }
        stage.store(3);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Transform* snapped = nullptr;
        engine_core::WriteOrigin* origin = nullptr;
        const engine_core::Transform* posed = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (stage->load() != 1) {
                return;
            }
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id != id || !near(item.world, *posed)) {
                    continue;
                }
                *snapped = item.world;
                *origin = item.transform_origin;
                stage->store(2);
            }
        }
    } probe;
    probe.id = id;
    probe.stage = &stage;
    probe.snapped = &snapped;
    probe.origin = &origin;
    probe.posed = &posed;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 3; });
    REQUIRE(near(live_at_write, posed));
    REQUIRE(near(snapped, posed));
    REQUIRE(origin == engine_core::WriteOrigin::PreRenderDataModel);
    REQUIRE(near(live_later, posed));
    engine.stop();
}

TEST_CASE("path B on a simulated part is rejected unless forced", "[T8]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = game.create_game_object().id();
    game.set_simulated(id, true);
    game.game_object(id)->set_linear_velocity(1.f, 0.f, 0.f);
    const engine_core::Transform posed = T1();
    std::atomic<int> rejected{0};
    std::atomic<int> stage{0};
    engine_core::Transform snapped{};
    engine_core::Transform live_at_write{};
    engine_core::Transform live_after_physics{};
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        game.game_object(id)->set_transform(posed);
        if (game.take_deferred_violation()) {
            rejected.store(1);
        }
        game.game_object(id)->set_transform(posed, engine_core::ForceSimWrite{});
        live_at_write = game.game_object(id)->transform();
        stage.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::PostSimulation, [&](double) {
        if (stage.load() != 2) {
            return;
        }
        live_after_physics = game.game_object(id)->transform();
        stage.store(3);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Transform* snapped = nullptr;
        const engine_core::Transform* posed = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (stage->load() != 1) {
                return;
            }
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id == id && near(item.world, *posed)) {
                    *snapped = item.world;
                    stage->store(2);
                }
            }
        }
    } probe;
    probe.id = id;
    probe.stage = &stage;
    probe.snapped = &snapped;
    probe.posed = &posed;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 3 && rejected.load() == 1; });
    REQUIRE(near(snapped, posed));
    REQUIRE(near(live_at_write, posed));
    REQUIRE(std::fabs(live_after_physics.m[12] - posed.m[12]) > 1e-3f);
    engine.stop();
}

TEST_CASE("physics substeps follow the sim clock, not present", "[T9]") {
    struct FixedClock : engine_core::IClock {
        double delta_seconds() override { return 1.0 / 30.0; }
    } clock;
    engine_core::Engine engine;
    engine.set_clock(&clock);
    engine.set_timing(1.0 / 30.0, 1.0 / 240.0);
    engine.start();
    engine.resume();
    wait_until([&] { return engine.sim_frame_count() > 4; });
    const int substeps = engine.last_substep_count();
    REQUIRE(substeps >= 7);
    REQUIRE(substeps <= 9);
    engine.stop();
}

TEST_CASE("destroy removes the instance from the next snapshot", "[T10]") {
    engine_core::Engine engine;
    engine_core::GameObject& object = engine.datamodel().create_game_object();
    const engine_core::InstanceId id = object.id();
    std::atomic<int> destroyed{0};
    std::atomic<int> live_closed{0};
    std::atomic<int> snap_gone{0};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (engine.sim_frame_count() > 1 && destroyed.load() == 0) {
            engine.datamodel().destroy(id);
            destroyed.store(1);
        }
        if (destroyed.load() == 1) {
            const bool closed = !engine.datamodel().alive(id) && engine.datamodel().game_object(id) == nullptr &&
                                is_zero(object.transform());
            if (closed) {
                live_closed.store(1);
            }
        }
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* destroyed = nullptr;
        std::atomic<int>* snap_gone = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (destroyed->load() == 0) {
                return;
            }
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id == id) {
                    return;
                }
            }
            snap_gone->store(1);
        }
    } probe;
    probe.id = id;
    probe.destroyed = &destroyed;
    probe.snap_gone = &snap_gone;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return live_closed.load() == 1 && snap_gone.load() == 1; });
    engine.stop();
}

TEST_CASE("PreRender cannot spend the prepare budget on simulated parts", "[T11]") {
    struct ZeroClock : engine_core::IClock {
        double delta_seconds() override { return 0; }
    } clock;
    engine_core::Engine engine;
    engine.set_clock(&clock);
    engine.set_pace_hz(60.0);
    engine_core::DataModel& game = engine.datamodel();
    engine_core::InstanceId ids[10000];
    for (int i = 0; i < 10000; ++i) {
        ids[i] = game.create_game_object().id();
        game.set_simulated(ids[i], true);
    }
    // The rejection is the Prepare. Drop the create records so that Prepare
    // does not spend its budget copying parts that were never drawn.
    game.invalidations().clear();
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        // Every one of these parts is simulated. The first write is rejected,
        // so Prepare does not walk the rest of the list.
        for (engine_core::InstanceId id : ids) {
            game.game_object(id)->set_transform(T1());
            if (game.has_deferred_violation()) {
                break;
            }
        }
    });
    engine.start();
    engine.resume();
    wait_until([&] { return engine.contract_count() > 0; });
    REQUIRE(engine.last_contract_prepare_ns() < 2000000);
    engine.stop();
}

TEST_CASE("simulation stays paused until resume", "[pause]") {
    engine_core::Engine engine;
    engine.start();
    REQUIRE(engine.paused());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    REQUIRE(engine.sim_frame_count() == 0);
    engine.resume();
    REQUIRE_FALSE(engine.paused());
    wait_until([&] { return engine.sim_frame_count() > 0; });
    engine.pause();
    // Let the in-flight step finish, then confirm no further steps are published.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const std::uint64_t frames = engine.sim_frame_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    REQUIRE(engine.sim_frame_count() == frames);
    engine.stop();
}

TEST_CASE("heartbeat property change drains before prepare", "[T12]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    const engine_core::ColorRgb tint = rgb(0.15f, 0.25f, 0.35f);
    std::atomic<int> handler_ran{0};
    std::atomic<int> bad{0};
    std::atomic<int> ready{0};
    std::atomic<int> phase{-1};
    std::atomic<int> role{-1};
    std::atomic<int> window{-1};
    engine.datamodel().property_changed(id, engine_core::Field::Color).connect([&](engine_core::InstanceId, engine_core::Field) {
        phase.store(static_cast<int>(engine.scheduler().current_phase()));
        role.store(static_cast<int>(engine_core::thread_role()));
        window.store(engine.datamodel().prerender_window() ? 1 : 0);
        if (std::this_thread::get_id() != engine.simulation_thread_id()) {
            bad.store(1);
        }
        handler_ran.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (handler_ran.load() != 0) {
            return;
        }
        engine.datamodel().game_object(id)->set_color(tint);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        const engine_core::ColorRgb* tint = nullptr;
        std::atomic<int>* handler_ran = nullptr;
        std::atomic<int>* bad = nullptr;
        std::atomic<int>* ready = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (ready->load() != 0 || bad->load() != 0) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            if (inst == nullptr || !near_color(inst->color, *tint)) {
                return;
            }
            if (handler_ran->load() == 0) {
                bad->store(1);
                return;
            }
            ready->store(1);
        }
    } probe;
    probe.id = id;
    probe.tint = &tint;
    probe.handler_ran = &handler_ran;
    probe.bad = &bad;
    probe.ready = &ready;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return ready.load() == 1 || bad.load() == 1; });
    engine.stop();
    REQUIRE(bad.load() == 0);
    REQUIRE(ready.load() == 1);
    REQUIRE(phase.load() == static_cast<int>(engine_core::Phase::Heartbeat));
    REQUIRE(role.load() == static_cast<int>(engine_core::ThreadRole::Simulation));
    REQUIRE(window.load() == 0);
    REQUIRE(near_color(engine.datamodel().game_object(id)->color(), tint));
}

TEST_CASE("handler writes are in the same snapshot", "[T13]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    const engine_core::ColorRgb tint = rgb(0.2f, 0.4f, 0.6f);
    const engine_core::Transform posed = T1();
    std::atomic<int> ready{0};
    std::atomic<int> bad{0};
    engine.datamodel().property_changed(id, engine_core::Field::Color).connect([&](engine_core::InstanceId changed, engine_core::Field) {
        engine.datamodel().game_object(changed)->set_transform(posed);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (ready.load() != 0) {
            return;
        }
        engine.datamodel().game_object(id)->set_color(tint);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        const engine_core::ColorRgb* tint = nullptr;
        const engine_core::Transform* posed = nullptr;
        std::atomic<int>* ready = nullptr;
        std::atomic<int>* bad = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (ready->load() != 0) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            if (inst == nullptr) {
                return;
            }
            const bool color_ok = near_color(inst->color, *tint);
            const bool transform_ok = near(inst->world, *posed);
            if (!color_ok && !transform_ok) {
                return;
            }
            if (color_ok && transform_ok) {
                ready->store(1);
                return;
            }
            bad->store(1);
        }
    } probe;
    probe.id = id;
    probe.tint = &tint;
    probe.posed = &posed;
    probe.ready = &ready;
    probe.bad = &bad;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return ready.load() == 1 || bad.load() == 1; });
    engine.stop();
    REQUIRE(bad.load() == 0);
    REQUIRE(ready.load() == 1);
    REQUIRE(near_color(engine.datamodel().game_object(id)->color(), tint));
    REQUIRE(near(engine.datamodel().game_object(id)->transform(), posed));
}

TEST_CASE("deferred handler does not re-enter the same drain", "[T14]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> depth{0};
    std::atomic<int> max_depth{0};
    std::atomic<int> color_hits{0};
    std::atomic<int> size_hits{0};
    engine.datamodel().changed(id).connect([&](engine_core::InstanceId changed, engine_core::Field field) {
        const int now = depth.fetch_add(1) + 1;
        int seen = max_depth.load();
        while (now > seen && !max_depth.compare_exchange_weak(seen, now)) {
        }
        if (field == engine_core::Field::Color) {
            color_hits.fetch_add(1);
            engine.datamodel().game_object(changed)->set_size(4.f, 5.f, 6.f);
        } else if (field == engine_core::Field::Size) {
            size_hits.fetch_add(1);
        }
        depth.fetch_sub(1);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (color_hits.load() != 0) {
            return;
        }
        engine.datamodel().game_object(id)->set_color(rgb(0.7f, 0.1f, 0.2f));
    });
    engine.start();
    engine.resume();
    wait_until([&] { return color_hits.load() == 1 && size_hits.load() == 1; });
    engine.stop();
    REQUIRE(max_depth.load() == 1);
    REQUIRE(color_hits.load() == 1);
    REQUIRE(size_hits.load() == 1);
}

TEST_CASE("disconnect during drain skips that connection", "[T15]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    std::atomic<int> c{0};
    std::atomic<int> late_hits{0};
    std::atomic<int> stage{0};
    engine_core::Connection late;
    engine_core::Signal& signal = engine.datamodel().changed(id);
    engine_core::Connection cb;
    engine_core::Connection ca = signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        a.fetch_add(1);
        cb.disconnect();
        if (late_hits.load() == 0 && a.load() == 1) {
            late = signal.connect([&](engine_core::InstanceId, engine_core::Field) { late_hits.fetch_add(1); });
        }
    });
    cb = signal.connect([&](engine_core::InstanceId, engine_core::Field) { b.fetch_add(1); });
    engine_core::Connection cc = signal.connect([&](engine_core::InstanceId, engine_core::Field) { c.fetch_add(1); });
    (void)ca;
    (void)cc;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        const int step = stage.load();
        if (step == 0) {
            engine.datamodel().game_object(id)->set_color(rgb(0.1f, 0.2f, 0.3f));
            stage.store(1);
        } else if (step == 1 && a.load() >= 1) {
            engine.datamodel().game_object(id)->set_color(rgb(0.4f, 0.5f, 0.6f));
            stage.store(2);
        }
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 2 && late_hits.load() == 1 && c.load() == 2; });
    engine.stop();
    REQUIRE(a.load() == 2);
    REQUIRE(b.load() == 0);
    REQUIRE(c.load() == 2);
    REQUIRE(late_hits.load() == 1);
    REQUIRE(late.connected());
    REQUIRE_FALSE(cb.connected());
}

TEST_CASE("destroy drops queued handlers", "[T16]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = game.create_game_object().id();
    std::atomic<int> hits{0};
    std::atomic<int> stage{0};
    engine_core::Connection conn = game.changed(id).connect([&](engine_core::InstanceId got, engine_core::Field) {
        hits.fetch_add(1);
        if (const engine_core::GameObject* object = game.game_object(got)) {
            (void)object->color();
        }
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        game.game_object(id)->set_color(rgb(0.9f, 0.1f, 0.1f));
        game.destroy(id);
        stage.store(1);
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 1 && engine.sim_frame_count() > 2; });
    engine.stop();
    REQUIRE(hits.load() == 0);
    REQUIRE_FALSE(game.alive(id));
    REQUIRE_FALSE(conn.connected());
    REQUIRE(game.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
}

TEST_CASE("path B enqueues and the snapshot still updates", "[T17]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = game.create_game_object().id();
    game.set_visual_only(id, true);
    const engine_core::ColorRgb tint = rgb(0.2f, 0.8f, 0.1f);
    std::atomic<int> hits{0};
    std::atomic<int> bad{0};
    std::atomic<int> during{0};
    std::atomic<int> stage{0};
    game.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) {
        if (engine_core::thread_role() != engine_core::ThreadRole::Simulation) {
            bad.store(1);
        }
        if (game.prerender_window()) {
            bad.store(1);
        }
        if (std::this_thread::get_id() != engine.simulation_thread_id()) {
            bad.store(1);
        }
        if (engine.scheduler().current_phase() == engine_core::Phase::PreRender) {
            bad.store(1);
        }
        hits.fetch_add(1);
    });
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        game.game_object(id)->set_color(tint);
        during.store(hits.load());
        stage.store(1);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        const engine_core::ColorRgb* tint = nullptr;
        std::atomic<int>* stage = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (stage->load() != 1) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            if (inst != nullptr && near_color(inst->color, *tint)) {
                stage->store(2);
            }
        }
    } probe;
    probe.id = id;
    probe.tint = &tint;
    probe.stage = &stage;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 2 && hits.load() >= 1; });
    engine.stop();
    REQUIRE(during.load() == 0);
    REQUIRE(bad.load() == 0);
    REQUIRE(hits.load() >= 1);
    REQUIRE(near_color(game.game_object(id)->color(), tint));
    REQUIRE(game.events().count(engine_core::WriteOrigin::PreRenderDataModel) >= 1);
    REQUIRE(game.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
}

TEST_CASE("path C emits nothing", "[T18]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = game.create_game_object().id();
    const engine_core::ColorRgb live_color = game.game_object(id)->color();
    const engine_core::Transform live_transform = game.game_object(id)->transform();
    std::atomic<int> hits{0};
    std::atomic<int> stage{0};
    game.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    game.property_changed(id, engine_core::Field::Color)
        .connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    game.property_changed(id, engine_core::Field::Transform)
        .connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        engine_core::SnapshotOverride color;
        color.id = id;
        color.field = engine_core::VisualField::Color;
        color.color = rgb(1.f, 0.f, 0.f);
        engine.pump().override_visual(color);
        engine_core::SnapshotOverride transform;
        transform.id = id;
        transform.field = engine_core::VisualField::Transform;
        transform.transform = T1();
        engine.pump().override_visual(transform);
        stage.store(1);
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 1 && engine.present_count() > 3; });
    engine.stop();
    REQUIRE(hits.load() == 0);
    REQUIRE(near_color(game.game_object(id)->color(), live_color));
    REQUIRE(near(game.game_object(id)->transform(), live_transform));
    REQUIRE(game.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
    REQUIRE(game.events().suppressed_overrides() == 0);
}

TEST_CASE("wait resumes on a later simulation phase", "[T19]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> stage{0};
    std::atomic<int> resume_phase{-1};
    std::atomic<int> resume_role{-1};
    std::atomic<int> resume_on_sim{0};
    std::atomic<std::uint64_t> frame_at_wait{0};
    std::atomic<std::uint64_t> frame_at_resume{0};
    engine.scheduler().bind(
        engine_core::Phase::Heartbeat,
        [&](double) {
            if (stage.load() != 0) {
                return;
            }
            stage.store(1);
            frame_at_wait.store(engine.sim_frame_count());
            engine.datamodel().property_changed(id, engine_core::Field::Color).wait();
            frame_at_resume.store(engine.sim_frame_count());
            resume_phase.store(static_cast<int>(engine.scheduler().current_phase()));
            resume_role.store(static_cast<int>(engine_core::thread_role()));
            resume_on_sim.store(std::this_thread::get_id() == engine.simulation_thread_id() ? 1 : 0);
            stage.store(2);
        },
        3000);
    engine.scheduler().bind(
        engine_core::Phase::Heartbeat,
        [&](double) {
            if (stage.load() != 1) {
                return;
            }
            engine.datamodel().game_object(id)->set_color(rgb(0.3f, 0.2f, 0.1f));
            stage.store(3);
        },
        1000);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 2; });
    engine.stop();
    REQUIRE(resume_on_sim.load() == 1);
    REQUIRE(resume_role.load() == static_cast<int>(engine_core::ThreadRole::Simulation));
    REQUIRE(resume_phase.load() != static_cast<int>(engine_core::Phase::PreRender));
    REQUIRE(resume_phase.load() >= static_cast<int>(engine_core::Phase::PreAnimation));
    REQUIRE(resume_phase.load() <= static_cast<int>(engine_core::Phase::Heartbeat));
    REQUIRE(frame_at_resume.load() > frame_at_wait.load());
}

TEST_CASE("immediate handlers run inside set and cap at 16", "[T20]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> calls{0};
    std::atomic<int> depth{0};
    std::atomic<int> max_depth{0};
    std::atomic<int> calls_at_return{0};
    std::atomic<int> stage{0};
    std::atomic<int> bad_thread{0};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        const int step = stage.load();
        if (step >= 2) {
            return;
        }
        if (step == 0) {
            engine.datamodel().events().set_policy(engine_core::EventPolicy::Immediate);
            engine.datamodel().changed(id).connect([&](engine_core::InstanceId changed, engine_core::Field) {
                if (engine_core::thread_role() != engine_core::ThreadRole::Simulation ||
                    std::this_thread::get_id() != engine.simulation_thread_id()) {
                    bad_thread.store(1);
                }
                const int now = depth.fetch_add(1) + 1;
                int seen = max_depth.load();
                while (now > seen && !max_depth.compare_exchange_weak(seen, now)) {
                }
                const int n = calls.fetch_add(1) + 1;
                if (n <= 16) {
                    engine.datamodel().game_object(changed)->set_size(10.f + static_cast<float>(n), 2.f, 3.f);
                }
                depth.fetch_sub(1);
            });
            engine.datamodel().game_object(id)->set_color(rgb(0.4f, 0.5f, 0.6f));
            calls_at_return.store(calls.load());
            stage.store(1);
            return;
        }
        stage.store(2);
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 2; });
    engine.stop();
    REQUIRE(bad_thread.load() == 0);
    REQUIRE(calls_at_return.load() == 16);
    REQUIRE(calls.load() == 17);
    REQUIRE(max_depth.load() == 16);
}

TEST_CASE("plain instances do not carry transform color size or velocity", "[instance]") {
    static_assert(std::is_base_of<engine_core::DataModel, engine_core::GameObject>::value,
                  "GameObject inherits DataModel");
    engine_core::Game game;
    engine_core::DataModel& plain = game.create();
    REQUIRE(plain.id() != 0);
    REQUIRE(game.alive(plain.id()));
    REQUIRE(game.game_object(plain.id()) == nullptr);
    engine_core::GameObject& object = game.create_game_object();
    REQUIRE(game.game_object(object.id()) == &object);
    REQUIRE(object.transform().m[0] == 1.f);
    REQUIRE(object.transform().m[15] == 1.f);
    int seen = 0;
    game.for_each_game_object([&](const engine_core::GameObject& item) {
        REQUIRE(item.id() == object.id());
        ++seen;
    });
    REQUIRE(seen == 1);
    game.destroy(plain.id());
    REQUIRE_FALSE(game.alive(plain.id()));
    REQUIRE(game.alive(object.id()));
}

TEST_CASE("create<T> makes any subclass", "[instance]") {
    engine_core::Game game;
    engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
    game.set_parent(triangle.id(), game.id());
    REQUIRE(game.instance(triangle.id()) == &triangle);
    REQUIRE(game.game_object(triangle.id()) == nullptr);
    REQUIRE(game.parent(triangle.id()) == game.id());
    REQUIRE(game.first_child(game.id()) == triangle.id());
    REQUIRE(triangle.angle_degrees() == 0.0);
    REQUIRE(triangle.position().x == 0.f);
    REQUIRE(triangle.position().y == 0.f);
    REQUIRE(triangle.position().z == 0.f);
    triangle.set_position(1.f, 2.f, 3.f);
    REQUIRE(triangle.position().x == 1.f);
    REQUIRE(triangle.position().y == 2.f);
    REQUIRE(triangle.position().z == 3.f);
    triangle.step(1.0);
    REQUIRE(triangle.angle_degrees() == 90.0);

    engine_core::DataModel& plain = game.create();
    REQUIRE(game.parent(plain.id()) == engine_core::DataModel::kNoParent);
    REQUIRE(dynamic_cast<engine_core::TestTriangle*>(game.instance(plain.id())) == nullptr);
    REQUIRE(game.first_child(game.id()) == triangle.id());

    const engine_core::InstanceId id = triangle.id();
    game.destroy(id);
    REQUIRE_FALSE(game.alive(id));
    REQUIRE(game.instance(id) == nullptr);
    REQUIRE(triangle.angle_degrees() == 0.0);
    REQUIRE(triangle.position().x == 0.f);
    REQUIRE(game.first_child(game.id()) == 0);

    engine_core::TestTriangle& again = game.create<engine_core::TestTriangle>();
    REQUIRE(again.angle_degrees() == 0.0);
    REQUIRE(again.position().z == 0.f);
    REQUIRE(game.game_object(again.id()) == nullptr);
}

TEST_CASE("Heartbeat steps descendants of the root", "[instance]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
    game.set_parent(triangle.id(), game.id());
    engine_core::TestTriangle& nested = game.create<engine_core::TestTriangle>();
    game.set_parent(nested.id(), triangle.id());
    engine_core::TestTriangle& loose = game.create<engine_core::TestTriangle>();
    engine.start();
    REQUIRE(triangle.angle_degrees() == 0.0);
    REQUIRE(nested.angle_degrees() == 0.0);
    engine.resume();
    wait_until([&] { return triangle.angle_degrees() > 1.0 && nested.angle_degrees() > 1.0; });
    REQUIRE(loose.angle_degrees() == 0.0);
    engine.stop();
}

TEST_CASE("RenderStepped writes this frame and PostRender does not", "[T21]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = game.create_game_object().id();
    game.set_visual_only(id, true);
    const engine_core::Transform posed = T1();
    const engine_core::ColorRgb tint = rgb(0.4f, 0.5f, 0.6f);
    const engine_core::ColorRgb original = game.game_object(id)->color();

    std::atomic<int> stage{0};
    std::atomic<int> bad{0};
    std::atomic<int> lead{0};
    std::atomic<int> rs_window{-1};
    std::atomic<int> pre_window{-1};
    std::atomic<int> post_window{-1};
    std::atomic<int> rs_depth{-1};
    std::atomic<int> pre_saw{0};
    std::atomic<int> post_saw{0};
    std::atomic<int> front_held{0};
    std::atomic<int> hits{0};
    std::atomic<int> during{0};
    engine_core::Transform snapped{};
    engine_core::WriteOrigin origin = engine_core::WriteOrigin::Simulation;
    std::string post_write;
    std::string post_override;

    game.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) {
        if (engine_core::thread_role() != engine_core::ThreadRole::Simulation) {
            bad.store(1);
        }
        if (std::this_thread::get_id() != engine.simulation_thread_id()) {
            bad.store(1);
        }
        if (game.prerender_window()) {
            bad.store(1);
        }
        hits.fetch_add(1);
    });
    // Larger priority runs first, same rule as every other phase.
    engine.scheduler().bind(
        engine_core::Phase::RenderStepped,
        [&](double) {
            if (stage.load() != 0) {
                return;
            }
            if (lead.load() != 1) {
                bad.store(1);
            }
            rs_window.store(game.prerender_window() ? 1 : 0);
            rs_depth.store(game.write_depth());
            if (engine_core::thread_role() != engine_core::ThreadRole::Render) {
                bad.store(1);
            }
            game.game_object(id)->set_transform(posed);
            during.store(hits.load());
            stage.store(1);
        },
        100);
    engine.scheduler().bind(
        engine_core::Phase::RenderStepped,
        [&](double) {
            if (stage.load() != 0) {
                return;
            }
            lead.store(1);
        },
        3000);
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 1) {
            return;
        }
        pre_window.store(game.prerender_window() ? 1 : 0);
        pre_saw.store(near(game.game_object(id)->transform(), posed) ? 1 : 0);
        stage.store(2);
    });
    engine.scheduler().bind(engine_core::Phase::PostRender, [&](double) {
        if (stage.load() != 3) {
            return;
        }
        post_window.store(game.prerender_window() ? 1 : 0);
        if (engine_core::thread_role() != engine_core::ThreadRole::Render) {
            bad.store(1);
        }
        const engine_core::VisualInstance* inst = engine.pump().find(id);
        post_saw.store(inst != nullptr && near(inst->world, posed) ? 1 : 0);
        try {
            game.game_object(id)->set_color(tint);
        } catch (const engine_core::ContractViolation& ex) {
            post_write = ex.what();
        }
        try {
            engine_core::SnapshotOverride override;
            override.id = id;
            override.field = engine_core::VisualField::Color;
            override.color = tint;
            engine.pump().override_visual(override);
        } catch (const engine_core::ContractViolation& ex) {
            post_override = ex.what();
        }
        inst = engine.pump().find(id);
        const bool held = inst != nullptr && near(inst->world, posed) && near_color(inst->color, original);
        front_held.store(held ? 1 : 0);
        stage.store(4);
    });

    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Transform* snapped = nullptr;
        engine_core::WriteOrigin* origin = nullptr;
        const engine_core::Transform* posed = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (stage->load() != 2) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            if (inst == nullptr || !near(inst->world, *posed)) {
                return;
            }
            *snapped = inst->world;
            *origin = inst->transform_origin;
            stage->store(3);
        }
    } probe;
    probe.id = id;
    probe.stage = &stage;
    probe.snapped = &snapped;
    probe.origin = &origin;
    probe.posed = &posed;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 4 && hits.load() >= 1; });
    engine.stop();

    REQUIRE(bad.load() == 0);
    REQUIRE(lead.load() == 1);
    REQUIRE(rs_window.load() == 1);
    REQUIRE(pre_window.load() == 1);
    REQUIRE(post_window.load() == 0);
    REQUIRE(rs_depth.load() > 0);
    REQUIRE(pre_saw.load() == 1);
    REQUIRE(post_saw.load() == 1);
    REQUIRE(front_held.load() == 1);
    REQUIRE(during.load() == 0);
    REQUIRE(hits.load() >= 1);
    REQUIRE(near(snapped, posed));
    REQUIRE(origin == engine_core::WriteOrigin::PreRenderDataModel);
    REQUIRE(near(game.game_object(id)->transform(), posed));
    REQUIRE(near_color(game.game_object(id)->color(), original));
    REQUIRE(post_write.find("PreRender") != std::string::npos);
    REQUIRE(post_override.find("PreRender") != std::string::npos);
    REQUIRE(game.events().count(engine_core::WriteOrigin::PreRenderDataModel) >= 1);
}

TEST_CASE("RenderStepped and PostRender run while simulation is paused", "[T22]") {
    engine_core::Engine engine;
    std::atomic<int> stepped{0};
    std::atomic<int> posted{0};
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) { stepped.fetch_add(1); });
    engine.scheduler().bind(engine_core::Phase::PostRender, [&](double) { posted.fetch_add(1); });
    engine.start();
    wait_until([&] { return stepped.load() > 0 && posted.load() > 0; });
    REQUIRE(engine.paused());
    REQUIRE(engine.sim_frame_count() == 0);
    engine.stop();
}

TEST_CASE("render phases do not yield or drain", "[T23]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> stepped_wait{0};
    std::atomic<int> posted_wait{0};
    std::atomic<int> posted_drain{0};
    std::string stepped_message;
    std::string posted_wait_message;
    std::string posted_drain_message;
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) {
        if (stepped_wait.load() != 0) {
            return;
        }
        try {
            engine.datamodel().property_changed(id, engine_core::Field::Color).wait();
        } catch (const engine_core::ContractViolation& ex) {
            stepped_message = ex.what();
            stepped_wait.store(1);
        }
    });
    engine.scheduler().bind(engine_core::Phase::PostRender, [&](double) {
        if (posted_wait.load() != 0) {
            return;
        }
        try {
            engine.datamodel().property_changed(id, engine_core::Field::Color).wait();
        } catch (const engine_core::ContractViolation& ex) {
            posted_wait_message = ex.what();
            posted_wait.store(1);
        }
        try {
            engine.datamodel().events().drain();
        } catch (const engine_core::ContractViolation& ex) {
            posted_drain_message = ex.what();
            posted_drain.store(1);
        }
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stepped_wait.load() == 1 && posted_wait.load() == 1 && posted_drain.load() == 1; });
    engine.stop();
    REQUIRE(stepped_message.find("simulation job") != std::string::npos);
    REQUIRE(posted_wait_message.find("simulation job") != std::string::npos);
    REQUIRE(posted_drain_message.find("SimulationThread") != std::string::npos);
}

TEST_CASE("a RenderStepped contract still runs PreRender", "[T24]") {
    engine_core::Engine engine;
    std::atomic<int> pre{0};
    engine.scheduler().bind(engine_core::Phase::RenderStepped, [&](double) {
        throw engine_core::ContractViolation("RenderStepped stopped");
    });
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) { pre.fetch_add(1); });
    engine.start();
    engine.resume();
    wait_until([&] { return pre.load() > 0 && engine.contract_count() > 0; });
    engine.stop();
    REQUIRE(pre.load() > 0);
    REQUIRE(engine.contract_count() > 0);
}

TEST_CASE("a paused edit parents a triangle before the next step", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    REQUIRE(engine.paused());
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
        game.set_parent(triangle.id(), game.id());
        id = triangle.id();
    });
    REQUIRE(id != 0);
    REQUIRE(engine.datamodel().alive(id));
    REQUIRE(engine.datamodel().parent(id) == engine.datamodel().id());
    REQUIRE(engine.paused());
    REQUIRE(engine.sim_frame_count() == 0);
    engine.stop();
}

TEST_CASE("a paused edit destroys at once", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::GameObject& part = game.create<engine_core::GameObject>();
        game.set_parent(part.id(), game.id());
        id = part.id();
    });
    REQUIRE(engine.datamodel().alive(id));
    // A project load clears the world inside one paused edit, then builds the
    // new tree. The old instances must be gone before that edit returns.
    bool gone_inside = false;
    engine.on_simulation([&](engine_core::DataModel& game) {
        game.destroy(id);
        gone_inside = !game.alive(id);
    });
    REQUIRE(gone_inside);
    REQUIRE_FALSE(engine.datamodel().alive(id));
    REQUIRE(engine.paused());
    engine.stop();
}

TEST_CASE("a paused edit writes transform, color, and flags at once", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    engine_core::InstanceId id = 0;
    bool seen_inside = false;
    const engine_core::Transform moved = engine_core::transform_translation(1.f, 2.f, 3.f);
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::GameObject& part = game.create<engine_core::GameObject>();
        game.set_parent(part.id(), game.id());
        id = part.id();
        engine_core::ColorRgb red;
        red.g = 0.f;
        red.b = 0.f;
        part.set_color(red);
        part.set_transform(moved);
        game.set_simulated(id, true);
        game.set_visual_only(id, true);
        // A project load reads these back before it captures the place.
        seen_inside = part.color().g == 0.f && part.transform().m[12] == 1.f && game.simulated(id);
    });
    REQUIRE(seen_inside);
    const engine_core::GameObject* part = engine.datamodel().game_object(id);
    REQUIRE(part != nullptr);
    REQUIRE(part->color().g == 0.f);
    REQUIRE(part->transform().m[13] == 2.f);
    REQUIRE(engine.datamodel().visual_only(id));
    // Undo of those writes runs as a paused edit too.
    engine.on_simulation([&](engine_core::DataModel& game) {
        game.history().end_gesture();
        game.history().undo();
    });
    REQUIRE_FALSE(engine.datamodel().alive(id));
    engine.on_simulation([&](engine_core::DataModel& game) { game.history().redo(); });
    REQUIRE(engine.datamodel().visual_only(id));
    REQUIRE(engine.datamodel().game_object(id)->color().g == 0.f);
    REQUIRE(engine.paused());
    engine.stop();
}

TEST_CASE("an edit during play runs on the simulation thread", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    engine.resume();
    std::atomic<int> on_sim{0};
    std::atomic<engine_core::InstanceId> id{0};
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (std::this_thread::get_id() == engine.simulation_thread_id()) {
            on_sim.store(1);
        }
        engine_core::TestTriangle& triangle = game.create<engine_core::TestTriangle>();
        game.set_parent(triangle.id(), game.id());
        id.store(triangle.id());
    });
    wait_until([&] { return id.load() != 0; });
    engine.stop();
    REQUIRE(on_sim.load() == 1);
    REQUIRE(engine.datamodel().alive(id.load()));
    REQUIRE(engine.datamodel().parent(id.load()) == engine.datamodel().id());
}

TEST_CASE("client sync keeps an uncapped render loop with the window", "[pace]") {
    engine_core::Engine engine;
    engine.set_simulation_pace_hz(60.0);
    engine.set_render_pace_hz(0.0);
    engine.set_render_client_sync(true);
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto idle = engine.present_count();
    // An empty step with nothing to wait for would present tens of thousands of times.
    REQUIRE(idle < 12);
    const auto paced_at = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        const auto before = engine.present_count();
        engine.note_client_frame();
        wait_until([&] { return engine.present_count() > before; });
    }
    const auto paced_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - paced_at)
                              .count();
    // Each note releases one step. Waiting out the 50 ms fallback twenty times would take a second.
    REQUIRE(paced_ms < 400);
    REQUIRE(engine.present_count() < idle + 50);
    engine.stop();
}

TEST_CASE("N1 default name is the class name and set_name fires Name", "[N1]") {
    engine_core::Game game;
    // The root is game, class Game.
    REQUIRE(std::string(game.class_name()) == "Game");
    REQUIRE(game.name(game.id()) == "Game");
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    REQUIRE(game.name(part.id()) == "GameObject");
    engine_core::DataModel& plain = game.create();
    REQUIRE(game.name(plain.id()) == "DataModel");

    int property_hits = 0;
    int changed_hits = 0;
    engine_core::Field property_field = engine_core::Field::Transform;
    engine_core::Field changed_field = engine_core::Field::Transform;
    std::string seen_name;
    game.property_changed(part.id(), engine_core::Field::Name)
        .connect([&](engine_core::InstanceId, engine_core::Field field) {
            ++property_hits;
            property_field = field;
            seen_name = game.name(part.id());
        });
    game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field field) {
        ++changed_hits;
        changed_field = field;
    });

    game.set_name(part.id(), "GameObject");
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(property_hits == 0);
    REQUIRE(changed_hits == 0);

    REQUIRE(game.invalidations().size() == 0);
    game.set_name(part.id(), "Brick");
    REQUIRE(game.name(part.id()) == "Brick");
    REQUIRE(property_hits == 0);
    REQUIRE(changed_hits == 0);
    REQUIRE(game.invalidations().size() == 0);
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(property_hits == 1);
    REQUIRE(changed_hits == 1);
    REQUIRE(property_field == engine_core::Field::Name);
    REQUIRE(changed_field == engine_core::Field::Name);
    REQUIRE(seen_name == "Brick");

    part.set_color(rgb(0.2f, 0.3f, 0.4f));
    REQUIRE(game.invalidations().size() == 1);

    const engine_core::InstanceId dead = plain.id();
    game.destroy(dead);
    REQUIRE(game.name(dead).empty());
    REQUIRE_THROWS_AS(game.set_name(dead, "Nope"), engine_core::ContractViolation);
}

TEST_CASE("N6 the root Changed signal is not the first instance", "[N6]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    REQUIRE((part.id() & 0xffffu) == 0);
    REQUIRE(part.id() != game.id());

    int root_changed = 0;
    int root_named = 0;
    int part_changed = 0;
    int added = 0;
    engine_core::InstanceId added_id = 0;
    game.changed(game.id()).connect([&](engine_core::InstanceId id, engine_core::Field field) {
        REQUIRE(id == game.id());
        REQUIRE(field == engine_core::Field::Name);
        ++root_changed;
    });
    game.property_changed(game.id(), engine_core::Field::Name)
        .connect([&](engine_core::InstanceId id, engine_core::Field field) {
            REQUIRE(id == game.id());
            REQUIRE(field == engine_core::Field::Name);
            ++root_named;
        });
    game.changed(part.id()).connect([&](engine_core::InstanceId id, engine_core::Field) {
        REQUIRE(id == part.id());
        ++part_changed;
    });
    game.child_added(game.id()).connect([&](engine_core::InstanceId child, engine_core::Field) {
        ++added;
        added_id = child;
    });

    game.set_name(game.id(), "Game");
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(root_changed == 0);
    REQUIRE(root_named == 0);

    game.set_name(game.id(), "Place");
    game.set_name(part.id(), "Brick");
    engine_core::GameObject& extra = game.create<engine_core::GameObject>();
    game.set_parent(extra.id(), game.id());
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(root_changed == 1);
    REQUIRE(root_named == 1);
    REQUIRE(part_changed == 1);
    REQUIRE(added == 1);
    REQUIRE(added_id == extra.id());
    REQUIRE(game.name(game.id()) == "Place");

    const int part_held = part_changed;
    game.destroy(part.id());
    game.set_name(game.id(), "Again");
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(root_changed == 2);
    REQUIRE(root_named == 2);
    REQUIRE(part_changed == part_held);
}

TEST_CASE("N2 siblings may share a name and find_first_child returns the first", "[N2]") {
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    game.set_name(folder.id(), "Folder");
    game.set_parent(folder.id(), game.id());

    engine_core::GameObject& older = game.create<engine_core::GameObject>();
    engine_core::GameObject& newer = game.create<engine_core::GameObject>();
    game.set_name(older.id(), "Wood");
    game.set_name(newer.id(), "Wood");
    game.set_parent(older.id(), folder.id());
    game.set_parent(newer.id(), folder.id());
    // link_child inserts at the front, so the later parent is first.
    REQUIRE(game.first_child(folder.id()) == newer.id());
    REQUIRE(game.find_first_child(folder.id(), "Wood") == newer.id());
    REQUIRE(game.find_first_child(folder.id(), "Missing") == 0);

    engine_core::GameObject& metal = game.create<engine_core::GameObject>();
    game.set_name(metal.id(), "Metal");
    game.set_parent(metal.id(), folder.id());
    REQUIRE(game.find_first_child(folder.id(), "Wood") == newer.id());
    REQUIRE(game.find_first_child(folder.id(), "Metal") == metal.id());

    const std::vector<engine_core::InstanceId> children = game.get_children(folder.id());
    REQUIRE(children.size() == 3);
    REQUIRE(children[0] == metal.id());
    REQUIRE(children[1] == newer.id());
    REQUIRE(children[2] == older.id());
    REQUIRE(game.get_children(0xdeadbeefu).empty());
    REQUIRE(game.find_first_child(0xdeadbeefu, "Wood") == 0);
    REQUIRE(game.get_children(game.id()).size() == 1);
    REQUIRE(game.get_children(game.id())[0] == folder.id());
}

TEST_CASE("N3 place restore reverts play and drops session instances", "[N3]") {
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    const engine_core::InstanceId folder_id = folder.id();
    game.set_name(folder_id, "Folder");
    game.set_parent(folder_id, game.id());

    engine_core::GameObject& leaf = game.create<engine_core::GameObject>();
    const engine_core::InstanceId leaf_id = leaf.id();
    game.set_name(leaf_id, "Leaf");
    game.set_parent(leaf_id, folder_id);
    const engine_core::ColorRgb red = rgb(0.8f, 0.1f, 0.1f);
    const engine_core::Transform posed = T0();
    leaf.set_color(red);
    leaf.set_size(2.f, 3.f, 4.f);
    leaf.set_transform(posed);
    game.set_simulated(leaf_id, true);

    engine_core::GameObject& sibling = game.create<engine_core::GameObject>();
    const engine_core::InstanceId sibling_id = sibling.id();
    game.set_name(sibling_id, "Sibling");
    game.set_parent(sibling_id, folder_id);

    const std::uint32_t generation = game.world_generation();
    game.capture_place();
    game.start_simulation();
    REQUIRE(game.simulation_running());
    REQUIRE_THROWS_AS(game.start_simulation(), engine_core::ContractViolation);

    game.set_name(leaf_id, "Moved");
    leaf.set_color(rgb(0.1f, 0.2f, 0.9f));
    leaf.set_size(9.f, 9.f, 9.f);
    leaf.set_transform(T1());
    game.set_parent(leaf_id, game.id());
    leaf.set_linear_velocity(10.f, 0.f, 0.f);
    game.integrate_simulated(1.0);
    REQUIRE_FALSE(near(game.game_object(leaf_id)->transform(), posed));

    engine_core::GameObject& extra = game.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    game.set_name(extra_id, "Session");
    game.set_parent(extra_id, folder_id);

    game.destroy(sibling_id);
    REQUIRE_FALSE(game.alive(sibling_id));
    engine_core::GameObject& recycled = game.create<engine_core::GameObject>();
    const engine_core::InstanceId recycled_id = recycled.id();
    REQUIRE(recycled_id != sibling_id);
    REQUIRE((recycled_id & 0xffffu) == (sibling_id & 0xffffu));
    game.set_name(recycled_id, "Recycled");
    game.set_parent(recycled_id, game.id());

    game.stop_simulation();
    REQUIRE_FALSE(game.simulation_running());
    REQUIRE(game.world_generation() == generation + 1);
    REQUIRE(game.name(game.id()) == "Game");
    REQUIRE(game.alive(leaf_id));
    REQUIRE(game.alive(sibling_id));
    REQUIRE(game.alive(folder_id));
    REQUIRE_FALSE(game.alive(extra_id));
    REQUIRE_FALSE(game.alive(recycled_id));
    REQUIRE(game.name(leaf_id) == "Leaf");
    REQUIRE(game.name(sibling_id) == "Sibling");
    REQUIRE(game.name(folder_id) == "Folder");
    REQUIRE(game.parent(leaf_id) == folder_id);
    REQUIRE(game.parent(sibling_id) == folder_id);
    REQUIRE(game.parent(folder_id) == game.id());
    REQUIRE(game.find_first_child(folder_id, "Sibling") == sibling_id);
    REQUIRE(near_color(game.game_object(leaf_id)->color(), red));
    float size[3] = {};
    REQUIRE(game.game_object(leaf_id)->copy_size(size));
    REQUIRE(size[0] == 2.f);
    REQUIRE(size[1] == 3.f);
    REQUIRE(size[2] == 4.f);
    REQUIRE(near(game.game_object(leaf_id)->transform(), posed));
    REQUIRE(game.simulated(leaf_id));
    game.integrate_simulated(1.0);
    REQUIRE(near(game.game_object(leaf_id)->transform(), posed));

    const std::vector<engine_core::InstanceId> children = game.get_children(folder_id);
    REQUIRE(children.size() == 2);
    REQUIRE(children[0] == sibling_id);
    REQUIRE(children[1] == leaf_id);

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Write);
        pump.begin_prerender_window(game);
        pump.end_prerender_window(game);
        pump.prepare_copy(game);
    }
    pump.publish();
    const engine_core::VisualInstance* vis = pump.find(leaf_id);
    REQUIRE(vis != nullptr);
    REQUIRE(near_color(vis->color, red));
    REQUIRE(near(vis->world, posed));
    REQUIRE(vis->size[0] == 2.f);
    REQUIRE(pump.find(extra_id) == nullptr);
    REQUIRE(pump.find(recycled_id) == nullptr);
    REQUIRE(pump.find(sibling_id) != nullptr);
    REQUIRE(pump.find(folder_id) == nullptr);
}

TEST_CASE("N4 a second play restores the original place", "[N4]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    game.set_parent(id, game.id());
    const engine_core::ColorRgb red = rgb(0.7f, 0.0f, 0.0f);
    part.set_color(red);
    game.set_name(id, "Door");
    game.capture_place();
    const std::uint32_t generation = game.world_generation();

    game.start_simulation();
    part.set_color(rgb(0.f, 0.f, 1.f));
    game.set_name(id, "Session");
    engine_core::GameObject& extra = game.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    game.stop_simulation();
    REQUIRE(game.world_generation() == generation + 1);
    REQUIRE(game.name(id) == "Door");
    REQUIRE(near_color(game.game_object(id)->color(), red));
    REQUIRE_FALSE(game.alive(extra_id));

    game.game_object(id)->set_color(rgb(0.f, 1.f, 0.f));
    game.set_name(id, "Edited");
    engine_core::GameObject& between = game.create<engine_core::GameObject>();
    const engine_core::InstanceId between_id = between.id();
    game.set_parent(between_id, game.id());

    game.start_simulation();
    game.game_object(id)->set_color(rgb(0.f, 0.f, 1.f));
    game.set_name(id, "Again");
    game.stop_simulation();
    REQUIRE(game.world_generation() == generation + 2);
    REQUIRE(game.name(id) == "Door");
    REQUIRE(near_color(game.game_object(id)->color(), red));
    REQUIRE(game.parent(id) == game.id());
    REQUIRE_FALSE(game.alive(between_id));

    game.stop_simulation();
    REQUIRE_FALSE(game.simulation_running());
    REQUIRE(game.world_generation() == generation + 2);
}

TEST_CASE("N4 a folder removed in edit mode stays removed after the next stop", "[N4]") {
    engine_core::Game game;

    game.start_simulation();
    game.stop_simulation();

    engine_core::Folder& folder = game.create<engine_core::Folder>();
    const engine_core::InstanceId folder_id = folder.id();
    game.set_name(folder_id, "Props");
    game.set_parent(folder_id, game.id());

    engine_core::Folder& cut = game.create<engine_core::Folder>();
    const engine_core::InstanceId cut_id = cut.id();
    game.set_name(cut_id, "Loose");
    game.set_parent(cut_id, game.id());
    // Insert while stopped replaces the snapshot, which is why the new
    // folders survive the next Stop.
    game.capture_place();

    game.start_simulation();
    game.stop_simulation();
    REQUIRE(game.parent(folder_id) == game.id());
    REQUIRE(game.parent(cut_id) == game.id());

    game.destroy(folder_id);
    game.set_parent(cut_id, engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(game.alive(folder_id));
    REQUIRE(game.parent(cut_id) == engine_core::DataModel::kNoParent);

    // Test freezes this edit-mode tree before play.
    game.capture_place();
    game.start_simulation();
    game.stop_simulation();
    REQUIRE_FALSE(game.alive(folder_id));
    REQUIRE(game.alive(cut_id));
    REQUIRE(game.parent(cut_id) == engine_core::DataModel::kNoParent);
    REQUIRE(game.find_first_child(game.id(), "Props") == 0);
    REQUIRE(game.find_first_child(game.id(), "Loose") == 0);
}

TEST_CASE("edit then play captures the place on start", "[N4]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    const engine_core::ColorRgb red = rgb(0.4f, 0.1f, 0.1f);
    part.set_color(red);
    game.set_name(id, "Door");
    game.set_parent(id, game.id());
    REQUIRE_FALSE(game.simulation_running());
    game.start_simulation();
    REQUIRE(game.simulation_running());
    part.set_color(rgb(0.1f, 0.1f, 0.8f));
    game.set_name(id, "Gone");
    engine_core::GameObject& extra = game.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    game.stop_simulation();
    REQUIRE(game.name(id) == "Door");
    REQUIRE(near_color(game.game_object(id)->color(), red));
    REQUIRE(game.parent(id) == game.id());
    REQUIRE_FALSE(game.alive(extra_id));
}

TEST_CASE("N5 stop drops session events and session jobs", "[N5]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    game.set_name(id, "Door");
    game.capture_place();

    int hits = 0;
    engine_core::Connection early =
        game.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) { ++hits; });
    engine_core::Connection named = game.property_changed(id, engine_core::Field::Name)
                                         .connect([&](engine_core::InstanceId, engine_core::Field) { ++hits; });

    int session_jobs = 0;
    int permanent_jobs = 0;
    engine.scheduler().bind_session(engine_core::Phase::Heartbeat, [&](double) { ++session_jobs; });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) { ++permanent_jobs; });

    game.start_simulation();
    game.set_name(id, "Session");
    REQUIRE(hits == 0);
    REQUIRE(game.name(id) == "Session");
    game.stop_simulation();
    REQUIRE(game.name(id) == "Door");
    REQUIRE_FALSE(early.connected());
    REQUIRE_FALSE(named.connected());
    {
        SimRole role;
        game.events().drain();
        engine.scheduler().run_phase(engine_core::Phase::Heartbeat, 0.0);
    }
    REQUIRE(hits == 0);
    REQUIRE(session_jobs == 0);
    REQUIRE(permanent_jobs == 1);

    int after = 0;
    game.property_changed(id, engine_core::Field::Name)
        .connect([&](engine_core::InstanceId, engine_core::Field field) {
            if (field == engine_core::Field::Name) {
                ++after;
            }
        });
    game.set_name(id, "After");
    REQUIRE(after == 0);
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(after == 1);
    REQUIRE(game.name(id) == "After");
}

int name_number(engine_core::DataModel& game, engine_core::InstanceId id) {
    try {
        return std::stoi(game.name(id));
    } catch (const std::exception&) {
        return -1;
    }
}

struct ScriptRig {
    SimRole role;
    engine_core::Game game;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;

    ScriptRig() {
        scheduler.reserve(16);
        game.attach_scheduler(&scheduler);
        runtime.attach(game, scheduler);
    }

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            game.events().drain();
            runtime.heartbeat(dt);
            game.events().drain();
        }
    }
};

engine_core::GameObject& add_part(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), parent);
    return part;
}

engine_core::Script& add_script(engine_core::DataModel& game, const char* name, const char* source) {
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), name);
    script.set_source(source);
    game.set_parent(script.id(), game.id());
    return script;
}

TEST_CASE("S1 two scripts wait without blocking each other", "[S1]") {
    ScriptRig rig;
    engine_core::Script& first = add_script(rig.game, "A", R"(
        local box = script:GetChildren()[1]
        while true do
            task.wait(0.05)
            local n = tonumber(box.Name) or 0
            box.Name = tostring(n + 1)
        end
    )");
    engine_core::Script& second = add_script(rig.game, "B", R"(
        local box = script:GetChildren()[1]
        while true do
            task.wait(0.05)
            local n = tonumber(box.Name) or 0
            box.Name = tostring(n + 1)
        end
    )");
    engine_core::GameObject& count_a = add_part(rig.game, first.id(), "0");
    engine_core::GameObject& count_b = add_part(rig.game, second.id(), "0");
    REQUIRE(rig.game.find_first_child(rig.game.id(), "A") == first.id());
    REQUIRE(std::string(first.class_name()) == "Script");

    rig.game.start_simulation();
    rig.frames(4, 0.05);
    const int a = name_number(rig.game, count_a.id());
    const int b = name_number(rig.game, count_b.id());
    REQUIRE(a > 0);
    REQUIRE(b > 0);
    REQUIRE(std::fabs(rig.runtime.sim_clock() - 0.2) < 1e-9);
}

TEST_CASE("S2 a script timeout leaves the other Heartbeat running", "[S2]") {
    ScriptRig rig;
    add_script(rig.game, "Spin", "while true do end");
    engine_core::Script& live = add_script(rig.game, "Live", R"(
        local box = script:GetChildren()[1]
        game:GetService("RunService").Heartbeat:Connect(function()
            local n = tonumber(box.Name) or 0
            box.Name = tostring(n + 1)
        end)
    )");
    engine_core::GameObject& count = add_part(rig.game, live.id(), "0");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    const int mid = name_number(rig.game, count.id());
    REQUIRE(mid > 0);
    REQUIRE(rig.runtime.last_error().find("ScriptTimeout") != std::string::npos);
    rig.frames(2, 0.05);
    REQUIRE(name_number(rig.game, count.id()) > mid);
}

TEST_CASE("S3 stop aborts a waiting script and the next start runs from the top", "[S3]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "Main", R"(
        _G.marker = 1
        local n = 0
        local flag = script:GetChildren()[1]
        flag.Name = tostring(n)
        task.wait(10)
        n = n + 1
        flag.Name = tostring(n)
        _G.marker = 2
    )");
    engine_core::GameObject& flag = add_part(rig.game, script.id(), "start");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    REQUIRE(rig.game.name(flag.id()) == "0");
    double marker = 0;
    REQUIRE(rig.runtime.global_number("marker", marker));
    REQUIRE(marker == 1);

    rig.game.stop_simulation();
    REQUIRE(rig.game.name(flag.id()) == "start");
    REQUIRE_FALSE(rig.runtime.vm_open());
    REQUIRE(rig.game.find_first_child(script.id(), "start") == flag.id());

    rig.game.start_simulation();
    REQUIRE(rig.runtime.vm_open());
    REQUIRE(rig.runtime.global_is_nil("marker"));
    rig.frames(3, 0.05);
    REQUIRE(rig.game.name(flag.id()) == "0");
    REQUIRE(rig.runtime.global_number("marker", marker));
    REQUIRE(marker == 1);
    REQUIRE(rig.runtime.sim_clock() < 10);
}

TEST_CASE("S4 disabling or destroying a script drops only its connections", "[S4]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.game, rig.game.id(), "P");
    engine_core::GameObject& hits = add_part(rig.game, rig.game.id(), "Hits");
    engine_core::GameObject& beat = add_part(rig.game, rig.game.id(), "Beat");
    engine_core::GameObject& other = add_part(rig.game, rig.game.id(), "Other");
    // The find names are the stable names. Counters live on children so renames do not hide them.
    engine_core::GameObject& hit_count = add_part(rig.game, hits.id(), "0");
    engine_core::GameObject& beat_count = add_part(rig.game, beat.id(), "0");
    engine_core::GameObject& other_count = add_part(rig.game, other.id(), "0");
    engine_core::Script& one = add_script(rig.game, "One", R"(
        local part = game:FindFirstChild("P")
        local hits = game:FindFirstChild("Hits"):GetChildren()[1]
        local beat = game:FindFirstChild("Beat"):GetChildren()[1]
        part.Changed:Connect(function()
            local n = tonumber(hits.Name) or 0
            hits.Name = tostring(n + 1)
        end)
        game:GetService("RunService").Heartbeat:Connect(function()
            local n = tonumber(beat.Name) or 0
            beat.Name = tostring(n + 1)
        end)
    )");
    engine_core::Script& two = add_script(rig.game, "Two", R"(
        local part = game:FindFirstChild("P")
        local other = game:FindFirstChild("Other"):GetChildren()[1]
        part.Changed:Connect(function()
            local n = tonumber(other.Name) or 0
            other.Name = tostring(n + 1)
        end)
    )");
    (void)one;
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    const int beat_before = name_number(rig.game, beat_count.id());
    REQUIRE(beat_before > 0);
    part.set_color(rgb(0.2f, 0.3f, 0.4f));
    rig.game.events().drain();
    const int hits_before = name_number(rig.game, hit_count.id());
    const int other_before = name_number(rig.game, other_count.id());
    REQUIRE(hits_before > 0);
    REQUIRE(other_before > 0);

    one.set_enabled(false);
    part.set_color(rgb(0.6f, 0.1f, 0.1f));
    rig.game.events().drain();
    REQUIRE(name_number(rig.game, hit_count.id()) == hits_before);
    REQUIRE(name_number(rig.game, other_count.id()) > other_before);
    const int beat_held = name_number(rig.game, beat_count.id());
    rig.frames(2, 0.05);
    REQUIRE(name_number(rig.game, beat_count.id()) == beat_held);

    const int other_held = name_number(rig.game, other_count.id());
    rig.game.destroy(two.id());
    part.set_color(rgb(0.1f, 0.7f, 0.2f));
    rig.game.events().drain();
    REQUIRE(name_number(rig.game, other_count.id()) == other_held);
    REQUIRE(name_number(rig.game, hit_count.id()) == hits_before);
}

TEST_CASE("a new ModuleScript returns an empty table", "[module]") {
    constexpr const char* starter = "local module = {}\n\nreturn module\n";
    ScriptRig rig;

    engine_core::ModuleScript& created = rig.game.create<engine_core::ModuleScript>();
    REQUIRE(created.source() == starter);
    engine_core::Script& script = rig.game.create<engine_core::Script>();
    REQUIRE(script.source().empty());
    rig.game.destroy(script.id());

    created.set_source("return 1");
    rig.game.destroy(created.id());
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    REQUIRE(module.source() == starter);

    const char* kept = "return { kept = true }";
    module.set_source(kept);
    const engine_core::InstanceId id = module.id();
    rig.game.set_name(id, "Kept");
    rig.game.set_parent(id, rig.game.id());
    rig.game.capture_place();

    rig.game.start_simulation();
    module.set_source("return { session = true }");
    rig.game.destroy(id);
    engine_core::ModuleScript& recycled = rig.game.create<engine_core::ModuleScript>();
    const engine_core::InstanceId recycled_id = recycled.id();
    REQUIRE(recycled.source() == starter);
    add_script(rig.game, "Check", R"lua(
        local made = Instance.new("ModuleScript")
        made.Name = "FromNew"
        made.Parent = game
        if made.Source ~= "local module = {}\n\nreturn module\n" then
            error("bad source")
        end
        local value = require(made)
        if type(value) ~= "table" or next(value) ~= nil then
            error("module did not return an empty table")
        end
    )lua");
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const engine_core::ScriptRuntime::OutputBatch played = rig.runtime.drain_output();
    for (const engine_core::ScriptRuntime::OutputLine& line : played.lines) {
        INFO(line.text);
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
    }
    const engine_core::InstanceId from_new = rig.game.find_first_child(rig.game.id(), "FromNew");
    REQUIRE(from_new != 0);
    const auto* made = dynamic_cast<const engine_core::ModuleScript*>(rig.game.instance(from_new));
    REQUIRE(made != nullptr);
    REQUIRE(made->source() == starter);

    rig.game.stop_simulation();
    REQUIRE(rig.game.alive(id));
    REQUIRE_FALSE(rig.game.alive(recycled_id));
    REQUIRE_FALSE(rig.game.alive(from_new));
    auto* restored = dynamic_cast<engine_core::ModuleScript*>(rig.game.instance(id));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->source() == kept);

    restored->set_source("");
    rig.game.capture_place();
    rig.game.start_simulation();
    restored->set_source(starter);
    rig.game.stop_simulation();
    restored = dynamic_cast<engine_core::ModuleScript*>(rig.game.instance(id));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->source().empty());
}

TEST_CASE("S5 require caches one return and drops it when the simulation stops", "[S5]") {
    ScriptRig rig;
    engine_core::GameObject& runs = add_part(rig.game, rig.game.id(), "0");
    rig.game.set_name(runs.id(), "Runs");
    engine_core::GameObject& run_count = add_part(rig.game, runs.id(), "0");
    engine_core::ModuleScript& mod = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(mod.id(), "Mod");
    mod.set_source(R"(
        local flag = script.Parent:FindFirstChild("Runs"):GetChildren()[1]
        local n = tonumber(flag.Name) or 0
        flag.Name = tostring(n + 1)
        return { n = n + 1 }
    )");
    rig.game.set_parent(mod.id(), rig.game.id());
    engine_core::ModuleScript& cycle = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(cycle.id(), "Cycle");
    cycle.set_source("return require(script)");
    rig.game.set_parent(cycle.id(), rig.game.id());
    add_script(rig.game, "Main", R"(
        local mod = game:FindFirstChild("Mod")
        local a = require(mod)
        local b = require(mod)
        _G.same = (a == b)
        _G.n = a.n
        local ok = pcall(function()
            require(game:FindFirstChild("Cycle"))
        end)
        _G.cycle = not ok
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    bool same = false;
    bool cycled = false;
    double n = 0;
    REQUIRE(rig.runtime.global_boolean("same", same));
    REQUIRE(same);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
    REQUIRE(name_number(rig.game, run_count.id()) == 1);
    REQUIRE(rig.runtime.global_boolean("cycle", cycled));
    REQUIRE(cycled);

    rig.game.stop_simulation();
    REQUIRE(rig.game.name(run_count.id()) == "0");
    rig.game.start_simulation();
    REQUIRE(rig.runtime.global_is_nil("same"));
    REQUIRE(rig.runtime.global_is_nil("n"));
    rig.frames(1, 0.05);
    REQUIRE(name_number(rig.game, run_count.id()) == 1);
    REQUIRE(rig.runtime.global_boolean("same", same));
    REQUIRE(same);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
}

TEST_CASE("S6 stop drops a script-created GameObject and restores an authored name", "[S6]") {
    ScriptRig rig;
    engine_core::GameObject& door = add_part(rig.game, rig.game.id(), "Door");
    engine_core::Script& maker = add_script(rig.game, "Maker", R"(
        local made = Instance.new("GameObject")
        made.Name = "Session"
        made.Parent = script.Parent
        local door = script.Parent:FindFirstChild("Door")
        door.Name = "Moved"
    )");
    REQUIRE(rig.game.find_first_child(rig.game.id(), "Maker") == maker.id());
    REQUIRE(std::string(maker.class_name()) == "Script");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.game.name(door.id()) == "Moved");
    const engine_core::InstanceId session = rig.game.find_first_child(rig.game.id(), "Session");
    REQUIRE(session != 0);
    REQUIRE(std::string(rig.game.instance(session)->class_name()) == "GameObject");

    rig.game.stop_simulation();
    REQUIRE(rig.game.name(door.id()) == "Door");
    REQUIRE_FALSE(rig.game.alive(session));
    REQUIRE(rig.game.find_first_child(rig.game.id(), "Session") == 0);
    REQUIRE(rig.game.find_first_child(rig.game.id(), "Maker") == maker.id());
}

TEST_CASE("S7 parenting a script while running starts it after the drain", "[S7]") {
    ScriptRig rig;
    engine_core::GameObject& mark = add_part(rig.game, rig.game.id(), "Mark");
    engine_core::GameObject& token = add_part(rig.game, mark.id(), "hidden");
    engine_core::GameObject& flag = add_part(rig.game, rig.game.id(), "Flag");
    engine_core::GameObject& trigger = add_part(rig.game, rig.game.id(), "Trigger");
    engine_core::Script& script = rig.game.create<engine_core::Script>();
    rig.game.set_name(script.id(), "Late");
    script.set_source(R"(
        local mark = game:FindFirstChild("Mark")
        local flag = game:FindFirstChild("Flag")
        flag.Name = mark:GetChildren()[1].Name
    )");
    REQUIRE(rig.game.parent(script.id()) == engine_core::DataModel::kNoParent);

    rig.game.start_simulation();
    bool started_inside = false;
    rig.game.changed(trigger.id()).connect([&](engine_core::InstanceId, engine_core::Field) {
        rig.game.set_name(token.id(), "visible");
        rig.game.set_parent(script.id(), rig.game.id());
        started_inside = rig.game.name(flag.id()) == "visible";
    });
    rig.game.set_name(trigger.id(), "go");
    REQUIRE(rig.game.name(flag.id()) == "Flag");
    rig.game.events().drain();
    REQUIRE_FALSE(started_inside);
    REQUIRE(rig.game.parent(script.id()) == rig.game.id());
    REQUIRE(rig.game.name(flag.id()) == "visible");
}

TEST_CASE("S8 a script color write is path A", "[S8]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.game, rig.game.id(), "P");
    add_script(rig.game, "Painter", R"(
        local part = game:FindFirstChild("P")
        part.Color = {r = 0.2, g = 0.4, b = 0.6, a = 1}
    )");
    int hits = 0;
    engine_core::Field seen = engine_core::Field::Count;
    rig.game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field field) {
        ++hits;
        seen = field;
    });
    rig.game.start_simulation();
    rig.frames(1, 1.0 / 60.0);
    const engine_core::ColorRgb tint = rgb(0.2f, 0.4f, 0.6f);
    REQUIRE(hits >= 1);
    REQUIRE(seen == engine_core::Field::Color);
    REQUIRE(near_color(part.color(), tint));

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    {
        engine_core::DataModelLock lock(rig.game, engine_core::DataModelLock::Write);
        pump.begin_prerender_window(rig.game);
        pump.end_prerender_window(rig.game);
        pump.prepare_copy(rig.game);
    }
    pump.publish();
    const engine_core::VisualInstance* vis = pump.find(part.id());
    REQUIRE(vis != nullptr);
    REQUIRE(near_color(vis->color, tint));
    REQUIRE(vis->color_origin == engine_core::WriteOrigin::Simulation);
}

TEST_CASE("S9 binding PreRender from a script errors", "[S9]") {
    ScriptRig rig;
    add_script(rig.game, "Bad", R"(
        local pre_ok = pcall(function()
            game:GetService("RunService").PreRender:Connect(function() end)
        end)
        local step_ok = pcall(function()
            game:GetService("RunService").RenderStepped:Connect(function() end)
        end)
        _G.pre = not pre_ok
        _G.step = not step_ok
    )");
    add_script(rig.game, "Other", "_G.other = true");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    bool pre = false;
    bool step = false;
    bool other = false;
    REQUIRE(rig.runtime.global_boolean("pre", pre));
    REQUIRE(pre);
    REQUIRE(rig.runtime.global_boolean("step", step));
    REQUIRE(step);
    REQUIRE(rig.runtime.global_boolean("other", other));
    REQUIRE(other);
}

TEST_CASE("S10 stale userdata after stop does not address the restored tree", "[S10]") {
    ScriptRig rig;
    engine_core::GameObject& door = add_part(rig.game, rig.game.id(), "Door");
    add_script(rig.game, "Keeper", R"(
        local door = game:FindFirstChild("Door")
        _G.door = door
        door.Name = "Live"
        local temp = Instance.new("GameObject")
        temp.Name = "Temp"
        temp:Destroy()
        _G.read_nil = (temp.Name == nil)
        local ok = pcall(function()
            temp.Name = "nope"
        end)
        _G.set_failed = not ok
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.game.name(door.id()) == "Live");
    bool read_nil = false;
    bool set_failed = false;
    REQUIRE(rig.runtime.global_boolean("read_nil", read_nil));
    REQUIRE(read_nil);
    REQUIRE(rig.runtime.global_boolean("set_failed", set_failed));
    REQUIRE(set_failed);
    const engine_core::ScriptRuntime::Watch watch = rig.runtime.watch_global("door");
    REQUIRE(watch.valid);
    REQUIRE(watch.id == door.id());
    REQUIRE(rig.runtime.resolve_watch(watch) == &door);

    const std::uint32_t generation = rig.game.world_generation();
    rig.game.stop_simulation();
    REQUIRE(rig.game.world_generation() == generation + 1);
    REQUIRE(rig.game.alive(door.id()));
    REQUIRE(rig.game.name(door.id()) == "Door");
    REQUIRE(rig.runtime.resolve_watch(watch) == nullptr);

    rig.game.start_simulation();
    REQUIRE(rig.runtime.global_is_nil("door"));
    REQUIRE(rig.runtime.resolve_watch(watch) == nullptr);
    rig.frames(1, 0.05);
    REQUIRE(rig.game.name(door.id()) == "Live");
    REQUIRE(rig.runtime.resolve_watch(watch) == nullptr);
    const engine_core::ScriptRuntime::Watch fresh = rig.runtime.watch_global("door");
    REQUIRE(fresh.valid);
    REQUIRE(fresh.world != watch.world);
    REQUIRE(rig.runtime.resolve_watch(fresh) == &door);
}

TEST_CASE("S11 spawned wait(0) threads both resume", "[S11]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        task.spawn(function()
            task.wait(0)
            _G.a = (_G.a or 0) + 1
        end)
        task.spawn(function()
            task.wait(0)
            _G.b = (_G.b or 0) + 1
        end)
    )");
    rig.game.start_simulation();
    rig.frames(2, 0.05);
    double a = 0;
    double b = 0;
    REQUIRE(rig.runtime.global_number("a", a));
    REQUIRE(rig.runtime.global_number("b", b));
    REQUIRE(a == 1);
    REQUIRE(b == 1);
}

TEST_CASE("S12 a Luau Heartbeat connection runs on the simulation thread", "[S12]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::GameObject& part = add_part(game, game.id(), "P");
    add_script(game, "Beat", R"(
        local part = game:FindFirstChild("P")
        local n = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            n = n + 1
            part.Color = {r = n / 100, g = 0.2, b = 0.3, a = 1}
        end)
    )");
    std::atomic<int> hits{0};
    game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    game.start_simulation();
    engine.start();
    engine.resume();
    wait_until([&] { return hits.load() >= 3; });
    engine.stop();
    REQUIRE(hits.load() >= 3);
}

TEST_CASE("Vector3 is the triangle position", "[vector3]") {
    ScriptRig rig;
    engine_core::TestTriangle& triangle = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(triangle.id(), "Tri0");
    rig.game.set_parent(triangle.id(), rig.game.id());
    triangle.set_position(-0.58f, 0.38f, 0.f);
    add_script(rig.game, "Main", R"(
        local tri = game:FindFirstChild("Tri0")
        local home = tri.Position
        _G.read_x = home.X
        _G.read_y = home.Y
        _G.read_z = home.Z
        _G.lower = home.x == home.X and home.y == home.Y and home.z == home.Z
        _G.typeof_ok = typeof(home) == "Vector3" and typeof(vector.create(1, 2, 3)) == "Vector3"
        _G.type_ok = type(home) == "vector" and type(Vector3.new()) == "vector"
        _G.text = tostring(Vector3.new(5, 2, 10)) == "5, 2, 10"
        _G.text_neg = tostring(Vector3.new(-1, -2, -3)) == "-1, -2, -3"
        _G.text_neg0 = tostring(-Vector3.xAxis) == "-1, -0, -0"

        local empty = Vector3.new()
        _G.empty = empty.X == 0 and empty.Y == 0 and empty.Z == 0
        local partial = Vector3.new(4)
        _G.partial = partial.X == 4 and partial.Y == 0 and partial.Z == 0
        _G.nil_rejected = not pcall(function() return Vector3.new(nil) end)
        _G.bad_new = not pcall(function() return Vector3.new("no") end)

        _G.zero = Vector3.zero.Magnitude == 0 and Vector3.zero == Vector3.new()
        _G.one = Vector3.one == Vector3.new(1, 1, 1)
        _G.axes = Vector3.xAxis == Vector3.new(1, 0, 0) and Vector3.yAxis == Vector3.new(0, 1, 0) and
            Vector3.zAxis == Vector3.new(0, 0, 1)

        local sum = Vector3.new(1, 2, 3) + Vector3.new(4, 5, 6)
        _G.sum_x, _G.sum_y, _G.sum_z = sum.X, sum.Y, sum.Z
        local diff = Vector3.new(4, 5, 6) - Vector3.new(1, 2, 3)
        _G.diff_x = diff.X
        local scaled = Vector3.new(1, 2, 3) * 2
        local left = 2 * Vector3.xAxis
        _G.scaled_y = scaled.Y
        _G.left_x = left.X
        local product = Vector3.new(2, 3, 4) * Vector3.new(5, 6, 7)
        _G.prod_x, _G.prod_y, _G.prod_z = product.X, product.Y, product.Z
        local quotient = Vector3.new(4, 6, 8) / 2
        _G.quot_z = quotient.Z
        local divided = Vector3.new(4, 9, 8) / Vector3.new(2, 3, 4)
        _G.div_y = divided.Y
        local floored = Vector3.new(5, 7, 9) // 2
        _G.floor_x, _G.floor_y, _G.floor_z = floored.X, floored.Y, floored.Z
        local neg = -Vector3.new(1, -2, 3)
        _G.neg_x, _G.neg_y, _G.neg_z = neg.X, neg.Y, neg.Z
        _G.eq = Vector3.new(1, 2, 3) == Vector3.new(1, 2, 3)
        _G.neq = Vector3.new(1, 2, 3) ~= Vector3.new(1, 2, 4)

        _G.mag = Vector3.new(3, 4, 0).Magnitude
        local unit = Vector3.new(3, 4, 0).Unit
        _G.ux, _G.uy, _G.uz, _G.umag = unit.X, unit.Y, unit.Z, unit.Magnitude
        local nan = Vector3.zero.Unit
        _G.unit_nan = nan.X ~= nan.X and nan.Y ~= nan.Y and nan.Z ~= nan.Z

        local absolute = Vector3.new(-2, 4, -6):Abs()
        _G.abs_x, _G.abs_y, _G.abs_z = absolute.X, absolute.Y, absolute.Z
        local ceiled = Vector3.new(-2.6, 5.1, 8.8):Ceil()
        _G.ceil_x, _G.ceil_y, _G.ceil_z = ceiled.X, ceiled.Y, ceiled.Z
        local flo = Vector3.new(-2.6, 5.1, 8.8):Floor()
        _G.flo_x, _G.flo_y, _G.flo_z = flo.X, flo.Y, flo.Z
        local signed = Vector3.new(-2.6, 5.1, 0):Sign()
        _G.sign_x, _G.sign_y, _G.sign_z = signed.X, signed.Y, signed.Z

        local cross = Vector3.xAxis:Cross(Vector3.yAxis)
        _G.cross_x, _G.cross_y, _G.cross_z = cross.X, cross.Y, cross.Z
        local back = Vector3.yAxis:Cross(Vector3.xAxis)
        _G.back_z = back.Z
        _G.dot = Vector3.new(1, 2, 3):Dot(Vector3.new(4, 5, 6))
        _G.perp = Vector3.xAxis:Dot(Vector3.yAxis)
        _G.angle = Vector3.xAxis:Angle(Vector3.yAxis)
        _G.signed_angle = Vector3.xAxis:Angle(Vector3.yAxis, Vector3.zAxis)
        _G.neg_angle = Vector3.yAxis:Angle(Vector3.xAxis, Vector3.zAxis)
        _G.dot_missing = not pcall(function() return Vector3.xAxis:Dot() end)

        local near = Vector3.new(1, 2, 3)
        _G.fuzzy_same = near:FuzzyEq(Vector3.new(1, 2, 3))
        _G.fuzzy_near = near:FuzzyEq(near + Vector3.new(1e-6, 0, 0))
        _G.fuzzy_tight = near:FuzzyEq(near + Vector3.new(1e-6, 0, 0), 1e-8) == false
        _G.fuzzy_far = Vector3.zero:FuzzyEq(Vector3.xAxis) == false
        _G.fuzzy_scale = Vector3.new(100, 0, 0):FuzzyEq(Vector3.new(109, 0, 0), 10)

        local lerped = Vector3.zero:Lerp(Vector3.new(10, 0, 0), 0.25)
        _G.lerp = lerped.X
        _G.lerp0 = Vector3.new(3, 4, 5):Lerp(Vector3.new(9, 9, 9), 0) == Vector3.new(3, 4, 5)
        _G.lerp1 = Vector3.zero:Lerp(Vector3.one, 1) == Vector3.one
        _G.lerp2 = Vector3.zero:Lerp(Vector3.new(10, 0, 0), 2).X
        local high = Vector3.new(1, 2, 1):Max(Vector3.new(2, 1, 2))
        _G.max_x, _G.max_y, _G.max_z = high.X, high.Y, high.Z
        local low = Vector3.new(1, 2, 1):Min(Vector3.new(2, 1, 2))
        _G.min_x, _G.min_y, _G.min_z = low.X, low.Y, low.Z

        local frozen = Vector3.new(1, 2, 3)
        _G.write_rejected = not pcall(function() frozen.X = 9 end) and frozen.X == 1
        _G.missing = not pcall(function() return frozen.Nope end)

        _G.right = Vector3.FromNormalId(Enum.NormalId.Right) == Vector3.xAxis
        _G.left = Vector3.FromNormalId(Enum.NormalId.Left) == -Vector3.xAxis
        _G.top = Vector3.FromNormalId(Enum.NormalId.Top) == Vector3.yAxis
        _G.bottom = Vector3.FromNormalId(Enum.NormalId.Bottom) == -Vector3.yAxis
        _G.back_n = Vector3.FromNormalId(Enum.NormalId.Back) == Vector3.zAxis
        _G.front = Vector3.FromNormalId(Enum.NormalId.Front) == -Vector3.zAxis
        _G.axis_x = Vector3.FromAxis(Enum.Axis.X) == Vector3.xAxis
        _G.axis_y = Vector3.FromAxis(Enum.Axis.Y) == Vector3.yAxis
        _G.axis_z = Vector3.FromAxis(Enum.Axis.Z) == Vector3.zAxis
        _G.enum_name = Enum.NormalId.Front.Name == "Front"
        _G.enum_value = Enum.NormalId.Front.Value == 5
        _G.enum_text = tostring(Enum.NormalId.Front) == "Enum.NormalId.Front"
        _G.enum_type = typeof(Enum.NormalId.Front) == "EnumItem"
        _G.enum_same = Enum.NormalId.Front.EnumType == Enum.NormalId
        _G.wrong_enum = not pcall(function() return Vector3.FromNormalId(Enum.Axis.X) end)
        _G.wrong_axis = not pcall(function() return Vector3.FromAxis(Enum.NormalId.Top) end)

        local ok, err = pcall(function()
            tri.Position = {x = 1, y = 2, z = 3}
        end)
        _G.table_rejected = not ok
        _G.table_msg = type(err) == "string" and string.find(err, "Vector3", 1, true) ~= nil
        _G.held = tri.Position.X == home.X and tri.Position.Y == home.Y and tri.Position.Z == home.Z

        tri.Position = vector.create(3, 4, 5)
        _G.vec_x, _G.vec_y, _G.vec_z = tri.Position.X, tri.Position.Y, tri.Position.Z
        tri.Position = home + Vector3.new(0.5, 0, 0)
        _G.hop_x = tri.Position.X
        _G.hop_y = tri.Position.Y
        tri.Position = Vector3.new(1.5, -2, 0.25)
        _G.set_x, _G.set_y, _G.set_z = tri.Position.X, tri.Position.Y, tri.Position.Z
    )");

    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());

    auto flag = [&](const char* name) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
        return value;
    };
    auto number = [&](const char* name) {
        double value = 0;
        INFO(name);
        REQUIRE(rig.runtime.global_number(name, value));
        return value;
    };

    REQUIRE(number("read_x") == static_cast<double>(-0.58f));
    REQUIRE(number("read_y") == static_cast<double>(0.38f));
    REQUIRE(number("read_z") == 0.0);
    flag("lower");
    flag("typeof_ok");
    flag("type_ok");
    flag("text");
    flag("text_neg");
    flag("text_neg0");
    flag("empty");
    flag("partial");
    flag("nil_rejected");
    flag("bad_new");
    flag("zero");
    flag("one");
    flag("axes");
    REQUIRE(number("sum_x") == 5);
    REQUIRE(number("sum_y") == 7);
    REQUIRE(number("sum_z") == 9);
    REQUIRE(number("diff_x") == 3);
    REQUIRE(number("scaled_y") == 4);
    REQUIRE(number("left_x") == 2);
    REQUIRE(number("prod_x") == 10);
    REQUIRE(number("prod_y") == 18);
    REQUIRE(number("prod_z") == 28);
    REQUIRE(number("quot_z") == 4);
    REQUIRE(number("div_y") == 3);
    REQUIRE(number("floor_x") == 2);
    REQUIRE(number("floor_y") == 3);
    REQUIRE(number("floor_z") == 4);
    REQUIRE(number("neg_x") == -1);
    REQUIRE(number("neg_y") == 2);
    REQUIRE(number("neg_z") == -3);
    flag("eq");
    flag("neq");
    REQUIRE(number("mag") == 5);
    REQUIRE(std::fabs(number("ux") - 0.6) < 1e-5);
    REQUIRE(std::fabs(number("uy") - 0.8) < 1e-5);
    REQUIRE(number("uz") == 0);
    REQUIRE(std::fabs(number("umag") - 1.0) < 1e-5);
    flag("unit_nan");
    REQUIRE(number("abs_x") == 2);
    REQUIRE(number("abs_y") == 4);
    REQUIRE(number("abs_z") == 6);
    REQUIRE(number("ceil_x") == -2);
    REQUIRE(number("ceil_y") == 6);
    REQUIRE(number("ceil_z") == 9);
    REQUIRE(number("flo_x") == -3);
    REQUIRE(number("flo_y") == 5);
    REQUIRE(number("flo_z") == 8);
    REQUIRE(number("sign_x") == -1);
    REQUIRE(number("sign_y") == 1);
    REQUIRE(number("sign_z") == 0);
    REQUIRE(number("cross_x") == 0);
    REQUIRE(number("cross_y") == 0);
    REQUIRE(number("cross_z") == 1);
    REQUIRE(number("back_z") == -1);
    REQUIRE(number("dot") == 32);
    REQUIRE(number("perp") == 0);
    REQUIRE(std::fabs(number("angle") - 1.5707963267948966) < 1e-5);
    REQUIRE(std::fabs(number("signed_angle") - 1.5707963267948966) < 1e-5);
    REQUIRE(std::fabs(number("neg_angle") + 1.5707963267948966) < 1e-5);
    flag("dot_missing");
    flag("fuzzy_same");
    flag("fuzzy_near");
    flag("fuzzy_tight");
    flag("fuzzy_far");
    flag("fuzzy_scale");
    REQUIRE(number("lerp") == 2.5);
    flag("lerp0");
    flag("lerp1");
    REQUIRE(number("lerp2") == 20);
    REQUIRE(number("max_x") == 2);
    REQUIRE(number("max_y") == 2);
    REQUIRE(number("max_z") == 2);
    REQUIRE(number("min_x") == 1);
    REQUIRE(number("min_y") == 1);
    REQUIRE(number("min_z") == 1);
    flag("write_rejected");
    flag("missing");
    flag("right");
    flag("left");
    flag("top");
    flag("bottom");
    flag("back_n");
    flag("front");
    flag("axis_x");
    flag("axis_y");
    flag("axis_z");
    flag("enum_name");
    flag("enum_value");
    flag("enum_text");
    flag("enum_type");
    flag("enum_same");
    flag("wrong_enum");
    flag("wrong_axis");
    flag("table_rejected");
    flag("table_msg");
    flag("held");
    REQUIRE(number("vec_x") == 3);
    REQUIRE(number("vec_y") == 4);
    REQUIRE(number("vec_z") == 5);
    REQUIRE(number("hop_x") == static_cast<double>(-0.58f + 0.5f));
    REQUIRE(number("hop_y") == static_cast<double>(0.38f));
    REQUIRE(number("set_x") == 1.5);
    REQUIRE(number("set_y") == -2);
    REQUIRE(number("set_z") == 0.25);

    const engine_core::Vec3 position = triangle.position();
    REQUIRE(position.x == 1.5f);
    REQUIRE(position.y == -2.f);
    REQUIRE(position.z == 0.25f);
}

TEST_CASE("scene scripts hop a triangle on task.wait and stop restores the pose", "[scene]") {
    ScriptRig rig;
    engine_core::TestTriangle& slow = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(slow.id(), "Tri0");
    rig.game.set_parent(slow.id(), rig.game.id());
    slow.set_position(-0.58f, 0.38f, 0.f);
    engine_core::TestTriangle& fast = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(fast.id(), "Tri1");
    rig.game.set_parent(fast.id(), rig.game.id());
    fast.set_position(0.58f, 0.38f, 0.15f);
    add_script(rig.game, "HopSlow", R"(
        local tri = game:FindFirstChild("Tri0")
        assert(tri)
        local home = tri.Position
        local n = 0
        while true do
            task.wait(0.5)
            n = n + 1
            local hop = (n % 2 == 1) and 0.45 or 0
            tri.Position = home + Vector3.new(hop, 0, 0)
        end
    )");
    add_script(rig.game, "HopFast", R"(
        local tri = game:FindFirstChild("Tri1")
        assert(tri)
        local home = tri.Position
        local n = 0
        while true do
            task.wait(0.2)
            n = n + 1
            local hop = (n % 2 == 1) and 0.35 or 0
            tri.Position = home + Vector3.new(0, hop, 0)
        end
    )");

    rig.game.start_simulation();
    bool slow_moved = false;
    bool fast_moved = false;
    for (int frame = 0; frame < 12; ++frame) {
        rig.frames(1, 0.1);
        const engine_core::Vec3 slow_now = slow.position();
        const engine_core::Vec3 fast_now = fast.position();
        if (std::fabs(slow_now.x - (-0.58f)) > 0.2f) {
            slow_moved = true;
            REQUIRE(std::fabs(slow_now.y - 0.38f) < 1e-4f);
        }
        if (std::fabs(fast_now.y - 0.38f) > 0.2f) {
            fast_moved = true;
            REQUIRE(std::fabs(fast_now.x - 0.58f) < 1e-4f);
        }
    }
    REQUIRE(slow_moved);
    REQUIRE(fast_moved);

    rig.game.stop_simulation();
    const engine_core::Vec3 restored_slow = slow.position();
    const engine_core::Vec3 restored_fast = fast.position();
    REQUIRE(std::fabs(restored_slow.x - (-0.58f)) < 1e-4f);
    REQUIRE(std::fabs(restored_slow.y - 0.38f) < 1e-4f);
    REQUIRE(std::fabs(restored_slow.z) < 1e-4f);
    REQUIRE(std::fabs(restored_fast.x - 0.58f) < 1e-4f);
    REQUIRE(std::fabs(restored_fast.y - 0.38f) < 1e-4f);
    REQUIRE(std::fabs(restored_fast.z - 0.15f) < 1e-4f);
    REQUIRE(rig.game.name(rig.game.find_first_child(rig.game.id(), "HopSlow")) == "HopSlow");
}

TEST_CASE("S13 print and errors reach the log and a new start clears it", "[S13]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        print("hello", 2)
        pcall(function() error("hidden") end)
        error("boom")
    )");
    rig.runtime.append_output(engine_core::ScriptRuntime::OutputKind::Print, "stale-before");
    rig.game.start_simulation();
    const engine_core::ScriptRuntime::OutputBatch cleared = rig.runtime.drain_output();
    REQUIRE(cleared.lines.empty());
    REQUIRE(cleared.epoch >= 1);

    rig.frames(1);
    const engine_core::ScriptRuntime::OutputBatch first = rig.runtime.drain_output();
    bool saw_print = false;
    bool saw_boom = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : first.lines) {
        REQUIRE(line.text.find("hidden") == std::string::npos);
        REQUIRE(line.text.find("stale-before") == std::string::npos);
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Print && line.text == "hello\t2\n") {
            saw_print = true;
        }
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error && line.text.find("boom") != std::string::npos) {
            saw_boom = true;
        }
    }
    REQUIRE(saw_print);
    REQUIRE(saw_boom);
    REQUIRE(rig.runtime.last_error().find("boom") != std::string::npos);

    rig.game.stop_simulation();
    rig.game.start_simulation();
    rig.frames(1);
    rig.game.stop_simulation();
    rig.game.start_simulation();
    const engine_core::ScriptRuntime::OutputBatch wiped = rig.runtime.drain_output();
    REQUIRE(wiped.lines.empty());
    REQUIRE(wiped.epoch > first.epoch);
}

TEST_CASE("S15 a console chunk uses the play VM", "[S15]") {
    ScriptRig rig;
    rig.game.start_simulation();
    rig.runtime.drain_output();
    rig.runtime.run_chunk("print(game.Name)");
    rig.runtime.run_chunk("error(\"nope\")");
    rig.runtime.run_chunk("pcall(function() error(\"hidden\") end)\nprint(\"after\")");
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    bool saw_name = false;
    bool saw_nope = false;
    bool saw_after = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        REQUIRE(line.text.find("hidden") == std::string::npos);
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Print && line.text == "Game\n") {
            saw_name = true;
        }
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error && line.text.find("nope") != std::string::npos) {
            saw_nope = true;
        }
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Print && line.text == "after\n") {
            saw_after = true;
        }
    }
    REQUIRE(saw_name);
    REQUIRE(saw_nope);
    REQUIRE(saw_after);
}

TEST_CASE("S14 a script that does not compile is logged", "[S14]") {
    ScriptRig rig;
    add_script(rig.game, "Bad", "print(");
    rig.game.start_simulation();
    rig.frames(1);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(!batch.lines.empty());
    REQUIRE(batch.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Error);
    REQUIRE(!rig.runtime.last_error().empty());
}

TEST_CASE("S16 the console sees game while the simulation is stopped", "[S16]") {
    ScriptRig rig;
    rig.game.set_name(0, "Place");
    rig.runtime.run_chunk("print(game)");
    rig.runtime.run_chunk("print(game.Name)");
    const engine_core::ScriptRuntime::OutputBatch before = rig.runtime.drain_output();
    REQUIRE(before.lines.size() == 2);
    REQUIRE(before.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Print);
    REQUIRE(before.lines[0].text == "Place\n");
    REQUIRE(before.lines[1].text == "Place\n");
    REQUIRE_FALSE(rig.runtime.vm_open());

    rig.game.start_simulation();
    rig.runtime.run_chunk("print(game.Name)");
    const engine_core::ScriptRuntime::OutputBatch playing = rig.runtime.drain_output();
    REQUIRE(playing.lines.size() == 1);
    REQUIRE(playing.lines[0].text == "Place\n");

    rig.game.stop_simulation();
    REQUIRE_FALSE(rig.runtime.vm_open());
    rig.runtime.run_chunk("print(game)");
    rig.runtime.run_chunk("print(game.Name)");
    const engine_core::ScriptRuntime::OutputBatch after = rig.runtime.drain_output();
    REQUIRE(after.lines.size() == 2);
    REQUIRE(after.lines[0].text == "Place\n");
    REQUIRE(after.lines[1].text == "Place\n");
}

TEST_CASE("S17 output lines remember when they were written", "[S17]") {
    ScriptRig rig;
    const auto before = std::chrono::system_clock::now();
    rig.runtime.append_output(engine_core::ScriptRuntime::OutputKind::Command, "print(1)");
    rig.runtime.append_output(engine_core::ScriptRuntime::OutputKind::Print, "1");
    const auto after = std::chrono::system_clock::now();
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 2);
    REQUIRE(batch.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Command);
    REQUIRE(batch.lines[0].text == "print(1)\n");
    REQUIRE(batch.lines[1].kind == engine_core::ScriptRuntime::OutputKind::Print);
    REQUIRE(batch.lines[1].text == "1\n");
    REQUIRE(batch.lines[0].time >= before);
    REQUIRE(batch.lines[0].time <= after);
    REQUIRE(batch.lines[1].time >= batch.lines[0].time);
    REQUIRE(batch.lines[1].time <= after);
}

TEST_CASE("S18 Heartbeat:Wait yields until the next Heartbeat", "[S18]") {
    ScriptRig rig;
    // The first Heartbeat has already been emitted by the time the script starts,
    // so the first Wait resumes on the following beat and returns that beat's dt.
    add_script(rig.game, "HopSlow", R"(
        local pre_ok = pcall(function()
            game:GetService("RunService").PreRender:Wait()
        end)
        local step_ok = pcall(function()
            game:GetService("RunService").RenderStepped:Wait()
        end)
        _G.pre = not pre_ok
        _G.step = not step_ok
        while true do
            local dt = game:GetService("RunService").Heartbeat:Wait()
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
        end
    )");
    add_script(rig.game, "Nested", R"(
        local rs = game:GetService("RunService")
        rs.Heartbeat:Connect(function()
            local seen = _G.inside or 0
            _G.inside = seen + 1
            if seen == 0 then
                _G.from_inside = rs.Heartbeat:Wait()
            end
        end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    bool pre = false;
    bool step = false;
    REQUIRE(rig.runtime.global_boolean("pre", pre));
    REQUIRE(pre);
    REQUIRE(rig.runtime.global_boolean("step", step));
    REQUIRE(step);
    REQUIRE(rig.runtime.global_is_nil("n"));
    REQUIRE(rig.runtime.global_is_nil("inside"));
    REQUIRE(rig.runtime.last_error().empty());

    rig.frames(1, 0.02);
    double n = 0;
    double dt = 0;
    double inside = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.02) < 1e-9);
    REQUIRE(rig.runtime.global_number("inside", inside));
    REQUIRE(inside == 1);
    REQUIRE_FALSE(rig.runtime.global_number("from_inside", dt));

    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 2);
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.05) < 1e-9);
    REQUIRE(rig.runtime.global_number("inside", inside));
    REQUIRE(inside == 2);
    REQUIRE(rig.runtime.global_number("from_inside", dt));
    REQUIRE(std::fabs(dt - 0.05) < 1e-9);
    REQUIRE(rig.runtime.last_error().empty());
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
    }
}

TEST_CASE("S19 PreSimulation:Wait returns that step", "[S19]") {
    ScriptRig rig;
    add_script(rig.game, "Sub", R"(
        _G.dt = game:GetService("RunService").PreSimulation:Wait()
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    double dt = 0;
    REQUIRE_FALSE(rig.runtime.global_number("dt", dt));
    rig.scheduler.run_phase(engine_core::Phase::PreSimulation, 0.01);
    rig.game.events().drain();
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.01) < 1e-9);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S20 Changed:Wait returns the property name", "[S20]") {
    ScriptRig rig;
    add_part(rig.game, rig.game.id(), "P");
    add_script(rig.game, "Watch", R"(
        local part = game:FindFirstChild("P")
        task.spawn(function()
            part.Name = "Next"
        end)
        local field = part.Changed:Wait()
        _G.ok = (field == "Name")
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
    REQUIRE(rig.game.find_first_child(rig.game.id(), "Next") != 0);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S21 game.Changed reports the root property", "[S21]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.game, rig.game.id(), "P");
    add_script(rig.game, "Watch", R"(
        local part = game:FindFirstChild("P")
        game.Changed:Connect(function(property)
            _G.hits = (_G.hits or 0) + 1
            _G.ok = (_G.hits == 1 and property == "Name")
        end)
        part.Name = "Q"
        game.Name = "Place"
        local field = game.Changed:Wait()
        _G.waited = (field == "Name")
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    bool ok = false;
    bool waited = false;
    double hits = 0;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
    REQUIRE(rig.runtime.global_boolean("waited", waited));
    REQUIRE(waited);
    REQUIRE(rig.runtime.global_number("hits", hits));
    REQUIRE(hits == 1);
    REQUIRE(rig.game.name(rig.game.id()) == "Place");
    REQUIRE(rig.game.name(part.id()) == "Q");
    REQUIRE(rig.runtime.last_error().empty());

    rig.game.stop_simulation();
    REQUIRE(rig.game.name(rig.game.id()) == "Game");
    REQUIRE(rig.game.name(part.id()) == "P");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
    REQUIRE(rig.runtime.global_boolean("waited", waited));
    REQUIRE(waited);
    REQUIRE(rig.game.name(rig.game.id()) == "Place");
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("Folder stores other instances", "[folder]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    REQUIRE(engine_core::lua_class_known("Folder"));
    REQUIRE(engine_core::lua_class_inherits("Folder", "DataModel"));

    engine_core::Folder& props = game.create<engine_core::Folder>();
    const engine_core::InstanceId props_id = props.id();
    REQUIRE(std::string(props.class_name()) == "Folder");
    REQUIRE(game.name(props_id) == "Folder");
    REQUIRE(game.game_object(props_id) == nullptr);
    game.set_name(props_id, "Props");
    game.set_parent(props_id, game.id());

    engine_core::Folder& inner = game.create<engine_core::Folder>();
    const engine_core::InstanceId inner_id = inner.id();
    game.set_name(inner_id, "Inner");
    game.set_parent(inner_id, props_id);

    engine_core::GameObject& box = game.create<engine_core::GameObject>();
    const engine_core::InstanceId box_id = box.id();
    game.set_name(box_id, "Box");
    game.set_parent(box_id, inner_id);
    REQUIRE(game.parent(box_id) == inner_id);
    REQUIRE(game.parent(inner_id) == props_id);
    REQUIRE(game.find_first_child(inner_id, "Box") == box_id);

    int bodies = 0;
    game.for_each_game_object([&](const engine_core::GameObject& item) {
        REQUIRE(item.id() == box_id);
        ++bodies;
    });
    REQUIRE(bodies == 1);

    game.capture_place();
    game.start_simulation();
    rig.runtime.run_chunk(R"(
        local session = Instance.new("Folder")
        session.Name = "Session"
        session.Parent = game
        local loose = Instance.new("GameObject")
        loose.Name = "Loose"
        loose.Parent = session
        local props = game:FindFirstChild("Props")
        props.Name = "Renamed"
        local box = props:FindFirstChild("Inner"):FindFirstChild("Box")
        box.Parent = game
        if not session:IsA("Folder") or not session:IsA("DataModel") or session:IsA("GameObject") then
            error("folder class")
        end
    )");
    const engine_core::ScriptRuntime::OutputBatch played = rig.runtime.drain_output();
    for (const engine_core::ScriptRuntime::OutputLine& line : played.lines) {
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
    }
    REQUIRE(rig.runtime.last_error().empty());
    const engine_core::InstanceId session_id = game.find_first_child(game.id(), "Session");
    REQUIRE(session_id != 0);
    REQUIRE(std::string(game.instance(session_id)->class_name()) == "Folder");
    REQUIRE(game.find_first_child(session_id, "Loose") != 0);
    REQUIRE(game.parent(box_id) == game.id());
    REQUIRE(game.name(props_id) == "Renamed");

    game.stop_simulation();
    REQUIRE(game.alive(props_id));
    REQUIRE(game.alive(inner_id));
    REQUIRE(game.alive(box_id));
    REQUIRE_FALSE(game.alive(session_id));
    REQUIRE(std::string(game.instance(props_id)->class_name()) == "Folder");
    REQUIRE(std::string(game.instance(inner_id)->class_name()) == "Folder");
    REQUIRE(game.name(props_id) == "Props");
    REQUIRE(game.name(inner_id) == "Inner");
    REQUIRE(game.parent(props_id) == game.id());
    REQUIRE(game.parent(inner_id) == props_id);
    REQUIRE(game.parent(box_id) == inner_id);
    REQUIRE(game.find_first_child(game.id(), "Session") == 0);
    REQUIRE(game.game_object(props_id) == nullptr);
    REQUIRE(game.game_object(inner_id) == nullptr);
}

TEST_CASE("S22 a script created during play stays parented to game", "[S22]") {
    ScriptRig rig;
    add_script(rig.game, "Maker", R"lua(
        local made = Instance.new("Script")
        made.Name = "Spawned"
        made.Source = "print('from spawned')"
        made.Parent = game
        local also = Instance.new("Script", game)
        also.Name = "FromNew"
        also.Source = "print('from new')"
    )lua");
    rig.game.start_simulation();
    rig.frames(2, 0.05);
    const engine_core::InstanceId spawned = rig.game.find_first_child(rig.game.id(), "Spawned");
    const engine_core::InstanceId from_new = rig.game.find_first_child(rig.game.id(), "FromNew");
    REQUIRE(spawned != 0);
    REQUIRE(from_new != 0);
    REQUIRE(std::string(rig.game.instance(spawned)->class_name()) == "Script");
    REQUIRE(rig.game.parent(spawned) == rig.game.id());
    REQUIRE(rig.game.parent(from_new) == rig.game.id());
    const engine_core::ScriptRuntime::OutputBatch played = rig.runtime.drain_output();
    bool spawned_printed = false;
    bool new_printed = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : played.lines) {
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error) {
            INFO(line.text);
        }
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
        if (line.text.find("from spawned") != std::string::npos) {
            spawned_printed = true;
        }
        if (line.text.find("from new") != std::string::npos) {
            new_printed = true;
        }
    }
    REQUIRE(spawned_printed);
    REQUIRE(new_printed);
    REQUIRE(rig.runtime.last_error().empty());

    rig.game.stop_simulation();
    REQUIRE_FALSE(rig.game.alive(spawned));
    REQUIRE_FALSE(rig.game.alive(from_new));
    REQUIRE(rig.game.find_first_child(rig.game.id(), "Spawned") == 0);
    REQUIRE(rig.game.find_first_child(rig.game.id(), "FromNew") == 0);
}

TEST_CASE("Selection keeps an ordered list without repeats or the root", "[selection]") {
    engine_core::SelectionService selection;
    REQUIRE(selection.get().empty());
    const std::uint64_t start = selection.revision();
    REQUIRE(selection.set({7, 0, 3, 7}));
    REQUIRE(selection.get() == std::vector<engine_core::InstanceId>{7, 3});
    REQUIRE(selection.revision() == start + 1);
    REQUIRE_FALSE(selection.set({7, 3}));
    REQUIRE(selection.revision() == start + 1);
    std::uint64_t seen = 0;
    REQUIRE(selection.get(seen) == std::vector<engine_core::InstanceId>{7, 3});
    REQUIRE(seen == start + 1);
    REQUIRE(selection.set({}));
    REQUIRE(selection.get().empty());
}

TEST_CASE("Selection Get and Set reach the same list the explorer reads", "[selection]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    engine_core::Folder& a = game.create<engine_core::Folder>();
    game.set_name(a.id(), "A");
    game.set_parent(a.id(), game.id());
    engine_core::Folder& b = game.create<engine_core::Folder>();
    game.set_name(b.id(), "B");
    game.set_parent(b.id(), game.id());
    const engine_core::InstanceId a_id = a.id();
    const engine_core::InstanceId b_id = b.id();
    REQUIRE(engine_core::lua_service_known("Selection"));
    const std::string definitions = engine_core::lua_analysis_definitions();
    REQUIRE(definitions.find("function Get(self): {Instance}") != std::string::npos);
    REQUIRE(definitions.find("function Set(self, selection: {Instance}): ()") != std::string::npos);

    rig.runtime.run_chunk("local s = game:GetService(\"Selection\")\n"
                          "local a, b = game:FindFirstChild(\"A\"), game:FindFirstChild(\"B\")\n"
                          "s:Set({b, a, b, game})\n"
                          "local got = s:Get()\n"
                          "print(#got, got[1].Name, got[2].Name)");
    const engine_core::ScriptRuntime::OutputBatch set = rig.runtime.drain_output();
    REQUIRE(set.lines.size() == 1);
    REQUIRE(set.lines[0].text == "2\tB\tA\n");
    REQUIRE(game.selection().get() == std::vector<engine_core::InstanceId>{b_id, a_id});

    // The studio side writes the list; the script reads it back.
    game.selection().set({a_id});
    rig.runtime.run_chunk("local got = game:GetService(\"Selection\"):Get()\nprint(#got, got[1].Name)");
    const engine_core::ScriptRuntime::OutputBatch read = rig.runtime.drain_output();
    REQUIRE(read.lines.size() == 1);
    REQUIRE(read.lines[0].text == "1\tA\n");

    // A destroyed instance is skipped, and a non-instance is an error.
    game.selection().set({a_id, b_id});
    game.destroy(a_id);
    rig.runtime.run_chunk("local got = game:GetService(\"Selection\"):Get()\nprint(#got, got[1].Name)");
    rig.runtime.run_chunk("game:GetService(\"Selection\"):Set({1})");
    rig.runtime.run_chunk("game:GetService(\"Selection\"):Set({})\nprint(#game:GetService(\"Selection\"):Get())");
    const engine_core::ScriptRuntime::OutputBatch rest = rig.runtime.drain_output();
    REQUIRE(rest.lines.size() == 3);
    REQUIRE(rest.lines[0].text == "1\tB\n");
    REQUIRE(rest.lines[1].kind == engine_core::ScriptRuntime::OutputKind::Error);
    REQUIRE(rest.lines[1].text.find("list of instances") != std::string::npos);
    REQUIRE(rest.lines[2].text == "0\n");
    REQUIRE(game.selection().get().empty());

    // RunService still resolves its signals through the shared service userdata.
    rig.runtime.run_chunk("print(typeof(game:GetService(\"RunService\").Heartbeat.Connect))");
    const engine_core::ScriptRuntime::OutputBatch run = rig.runtime.drain_output();
    REQUIRE(run.lines.size() == 1);
    REQUIRE(run.lines[0].text == "function\n");
}

TEST_CASE("S23 WaitForChild yields until the child exists", "[S23]") {
    ScriptRig rig;
    add_script(rig.game, "Waiter", R"(
        _G.here = game:WaitForChild("Waiter").Name == "Waiter"
        local later = game:WaitForChild("Later")
        _G.got = later.Name == "Later" and _G.made == true
        local renamed = game:WaitForChild("Renamed")
        _G.renamed = renamed.Name == "Renamed"
        _G.timed_out = game:WaitForChild("Never", 0.3) == nil
    )");
    add_script(rig.game, "Maker", R"(
        task.wait(0.2)
        local made = Instance.new("Folder")
        made.Name = "Later"
        made.Parent = game
        _G.made = true
        task.wait(0.1)
        -- A child already there that is renamed to the name wakes the wait too.
        made.Name = "Renamed"
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.runtime.global_is_nil("got"));
    // The frame that adds the child also resumes the waiter.
    rig.frames(1, 0.05);
    bool got = false;
    REQUIRE(rig.runtime.global_boolean("got", got));
    REQUIRE(got);
    REQUIRE(rig.runtime.global_is_nil("renamed"));
    rig.frames(3, 0.05);
    bool renamed = false;
    REQUIRE(rig.runtime.global_boolean("renamed", renamed));
    REQUIRE(renamed);
    REQUIRE(rig.runtime.global_is_nil("timed_out"));
    rig.frames(8, 0.05);
    bool timed_out = false;
    REQUIRE(rig.runtime.global_boolean("timed_out", timed_out));
    REQUIRE(timed_out);
    bool here = false;
    REQUIRE(rig.runtime.global_boolean("here", here));
    REQUIRE(here);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S26 WaitForChild ignores other names and a match that leaves before it resumes", "[S26]") {
    ScriptRig rig;
    add_script(rig.game, "Waiter", R"(
        local found = game:WaitForChild("Target")
        _G.ok = found.Name == "Target" and found.Parent == game
    )");
    add_script(rig.game, "Maker", R"(
        task.wait(0.1)
        local other = Instance.new("Folder")
        other.Name = "Other"
        other.Parent = game
        -- Matches, then leaves in the same step, before the waiter can resume.
        local brief = Instance.new("Folder")
        brief.Name = "Target"
        brief.Parent = game
        brief.Parent = other
        task.wait(0.1)
        local real = Instance.new("Folder")
        real.Name = "Target"
        real.Parent = game
    )");
    rig.game.start_simulation();
    rig.frames(3, 0.05);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.runtime.global_is_nil("ok"));
    rig.frames(3, 0.05);
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
}

TEST_CASE("S24 WaitForChild without a timeout notes a possible infinite yield", "[S24]") {
    ScriptRig rig;
    add_script(rig.game, "Stuck", R"(
        game:WaitForChild("Nothing")
        _G.after = true
    )");
    rig.game.start_simulation();
    rig.runtime.drain_output();
    rig.frames(90, 0.05);
    const engine_core::ScriptRuntime::OutputBatch early = rig.runtime.drain_output();
    for (const engine_core::ScriptRuntime::OutputLine& line : early.lines) {
        REQUIRE(line.text.find("Infinite yield") == std::string::npos);
    }
    rig.frames(20, 0.05);
    int notices = 0;
    for (const engine_core::ScriptRuntime::OutputLine& line : rig.runtime.drain_output().lines) {
        if (line.text.find("Infinite yield possible on") != std::string::npos &&
            line.text.find(":WaitForChild(\"Nothing\")") != std::string::npos) {
            ++notices;
        }
    }
    REQUIRE(notices == 1);
    REQUIRE(rig.runtime.global_is_nil("after"));

    // Stop drops the waiting thread, and the next start waits again from the top.
    rig.game.stop_simulation();
    REQUIRE_FALSE(rig.runtime.vm_open());
    rig.game.start_simulation();
    rig.frames(2, 0.05);
    REQUIRE(rig.runtime.global_is_nil("after"));
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S25 WaitForChild from the command line only returns a child that is there", "[S25]") {
    ScriptRig rig;
    rig.game.start_simulation();
    rig.runtime.run_chunk("_G.found = game:WaitForChild('Missing') ~= nil");
    REQUIRE(rig.runtime.last_error().find("WaitForChild yields the running script thread") != std::string::npos);
}

TEST_CASE("S27 every handle to an instance is the same value", "[S27]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "Main", R"(
        local box = script:GetChildren()[1]
        local same = script:FindFirstChild("Box")
        local seen = {}
        seen[box] = true
        _G.eq = box == same and rawequal(box, same) and seen[same] == true
        _G.parent_is_game = script.Parent == game and box.Parent == script
        _G.differs = box ~= script and box ~= game and box ~= nil
        local made = Instance.new("Folder")
        made.Parent = game
        _G.made_eq = game:FindFirstChild(made.Name) == made
        local weak = setmetatable({}, { __mode = "k" })
        weak[box] = 1
        _G.keyed = weak[script:FindFirstChild("Box")] == 1
    )");
    engine_core::GameObject& box = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), script.id());
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    for (const char* name : {"eq", "parent_is_game", "differs", "made_eq", "keyed"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }

    // A fresh session hands out fresh handles that still compare equal.
    rig.game.stop_simulation();
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    bool eq = false;
    REQUIRE(rig.runtime.global_boolean("eq", eq));
    REQUIRE(eq);
}

TEST_CASE("S28 the command line gets one handle per instance too", "[S28]") {
    ScriptRig rig;
    engine_core::GameObject& box = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), rig.game.id());
    rig.runtime.run_chunk(
        "local box = game:FindFirstChild('Box') assert(box == game:GetChildren()[1] and box.Parent == game)");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    rig.runtime.run_chunk("assert(game:FindFirstChild('Box') ~= game)");
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S29 a dot reads a child by name", "[S29]") {
    ScriptRig rig;
    engine_core::GameObject& door = add_part(rig.game, rig.game.id(), "Door");
    engine_core::Folder& box = rig.game.create<engine_core::Folder>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), rig.game.id());
    engine_core::GameObject& inner = add_part(rig.game, box.id(), "Inner");
    add_part(rig.game, box.id(), "Twin");
    add_part(rig.game, box.id(), "Twin");
    // A child named like a property: the property wins.
    add_part(rig.game, box.id(), "Name");
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Mod");
    module.set_source("return 42\n");
    rig.game.set_parent(module.id(), rig.game.id());
    add_script(rig.game, "Reader", R"(
        _G.door = game.Door == game:FindFirstChild("Door")
        _G.nested = game.Box.Inner.Name == "Inner"
        _G.parent = script.Parent.Box.Inner.Parent == game.Box
        _G.first = game.Box.Twin == game.Box:FindFirstChild("Twin")
        _G.property = game.Box.Name == "Box"
        _G.required = require(script.Parent.Mod) == 42
        local ok, message = pcall(function()
            return game.Box.Missing
        end)
        _G.missing_errors = not ok
        _G.missing_message = type(message) == "string"
            and string.find(message, "Missing is not a valid member of Folder \"Box\"", 1, true) ~= nil
        game.Box.Inner.Name = "Renamed"
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"door", "nested", "parent", "first", "property", "required", "missing_errors",
                             "missing_message"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    REQUIRE(rig.game.name(inner.id()) == "Renamed");
    REQUIRE(rig.game.alive(door.id()));
}

// DataModel is everything in the tree. Instance is what Instance.new makes.
// game is a Game: a DataModel, not an Instance, and the only one with GetService.
TEST_CASE("S30 game is a Game, a DataModel but not an Instance", "[S30]") {
    ScriptRig rig;
    REQUIRE(std::string(rig.game.class_name()) == "Game");
    // Only a Game makes a world. Any other DataModel is an instance inside one.
    STATIC_REQUIRE(std::is_default_constructible<engine_core::Game>::value);
    STATIC_REQUIRE_FALSE(std::is_default_constructible<engine_core::DataModel>::value);
    REQUIRE(engine_core::lua_class_inherits("Game", "DataModel"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("Game", "Instance"));
    for (const char* name : {"Folder", "GameObject", "TestTriangle", "Script", "ModuleScript"}) {
        INFO(name);
        REQUIRE(engine_core::lua_class_inherits(name, "Instance"));
        REQUIRE(engine_core::lua_class_inherits(name, "DataModel"));
    }
    REQUIRE_FALSE(engine_core::lua_creatable_known("Game"));
    REQUIRE(engine_core::lua_class_find("Game", "GetService") != nullptr);
    REQUIRE(engine_core::lua_class_find("DataModel", "GetService") == nullptr);
    REQUIRE(engine_core::lua_class_find("Folder", "GetService") == nullptr);

    add_script(rig.game, "Classes", R"(
        _G.class = game.ClassName == "Game"
        _G.game_isa = game:IsA("Game") and game:IsA("DataModel") and not game:IsA("Instance")
        local box = Instance.new("Folder", game)
        _G.box_isa = box:IsA("Folder") and box:IsA("Instance") and box:IsA("DataModel") and not box:IsA("Game")
        _G.parent = box.Parent == game
        _G.service = game:GetService("RunService") ~= nil
        local ok, message = pcall(function()
            return box:GetService("RunService")
        end)
        _G.box_service = not ok and string.find(message, "GetService is not a valid member of Folder", 1, true) ~= nil
        _G.no_game = not pcall(function()
            return Instance.new("Game")
        end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"class", "game_isa", "box_isa", "parent", "service", "box_service", "no_game"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

// Script and ModuleScript are separate classes. Both are a LuaSource, which
// holds Source. Enabled is Script's alone: a ModuleScript runs only through
// require, so it has none.
TEST_CASE("S31 Script and ModuleScript share LuaSource, and only Script has Enabled", "[S31]") {
    STATIC_REQUIRE(std::is_base_of<engine_core::LuaSource, engine_core::Script>::value);
    STATIC_REQUIRE(std::is_base_of<engine_core::LuaSource, engine_core::ModuleScript>::value);
    STATIC_REQUIRE_FALSE(std::is_base_of<engine_core::Script, engine_core::ModuleScript>::value);
    STATIC_REQUIRE_FALSE(std::is_base_of<engine_core::ModuleScript, engine_core::Script>::value);
    REQUIRE(engine_core::lua_class_inherits("LuaSource", "Instance"));
    REQUIRE(engine_core::lua_class_inherits("Script", "LuaSource"));
    REQUIRE(engine_core::lua_class_inherits("ModuleScript", "LuaSource"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("ModuleScript", "Script"));
    REQUIRE_FALSE(engine_core::lua_class_inherits("Script", "ModuleScript"));
    REQUIRE_FALSE(engine_core::lua_creatable_known("LuaSource"));
    REQUIRE(engine_core::lua_class_find("LuaSource", "Source") != nullptr);
    REQUIRE(engine_core::lua_class_find("ModuleScript", "Source") != nullptr);
    REQUIRE(engine_core::lua_class_find("Script", "Enabled") != nullptr);
    REQUIRE(engine_core::lua_class_find("ModuleScript", "Enabled") == nullptr);
    REQUIRE(engine_core::lua_class_find("LuaSource", "Enabled") == nullptr);

    ScriptRig rig;
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Mod");
    module.set_source("return 7\n");
    rig.game.set_parent(module.id(), rig.game.id());
    add_script(rig.game, "Reader", R"(
        local mod = script.Parent.Mod
        _G.module_isa = mod:IsA("LuaSource") and mod:IsA("ModuleScript") and not mod:IsA("Script")
        _G.script_isa = script:IsA("LuaSource") and script:IsA("Script") and not script:IsA("ModuleScript")
        _G.source = mod.Source == "return 7\n" and script.Enabled == true
        local ok, message = pcall(function()
            return mod.Enabled
        end)
        _G.no_enabled = not ok and string.find(message, "Enabled is not a valid member of ModuleScript", 1, true) ~= nil
        _G.required = require(mod) == 7
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"module_isa", "script_isa", "source", "no_enabled", "required"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }

    // Stop puts back a disabled Script's Enabled and a module's source.
    rig.game.stop_simulation();
    engine_core::Script& off = add_script(rig.game, "Off", "_G.off_ran = true\n");
    off.set_enabled(false);
    rig.game.capture_place();
    rig.game.start_simulation();
    off.set_enabled(true);
    module.set_source("return 8\n");
    rig.game.stop_simulation();
    REQUIRE_FALSE(off.enabled());
    REQUIRE(module.source() == "return 7\n");
}
