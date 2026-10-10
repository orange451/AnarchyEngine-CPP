#include "Contract.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "RunService.hpp"
#include "SelectionService.hpp"
#include "SnapshotPump.hpp"
#include "Engine.hpp"
#include "Enum.hpp"
#include "UserInputService.hpp"
#include "Events.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"
#include "LuaApi.hpp"
#include "ModuleScript.hpp"
#include "PhysicsObject.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TableSnapshot.hpp"
#include "TaskScheduler.hpp"

#include "support.hpp"
#include "ide/PluginLoader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
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

bool near(const engine_core::Matrix4& a, const engine_core::Matrix4& b) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a.m[i] - b.m[i]) > 1e-4f) {
            return false;
        }
    }
    return true;
}

bool is_zero(const engine_core::Matrix4& value) {
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

engine_core::Matrix4 T0() { return engine_core::matrix4_translation(3.f, 4.f, 5.f); }

engine_core::Matrix4 T1() { return engine_core::matrix4_translation(50.f, 0.f, 0.f); }

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
                object->set_transform(T0());
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
            engine.datamodel().game_object(id)->set_transform(
                engine_core::matrix4_translation(static_cast<float>(i & 255) / 255.f, 0.25f, 0.5f));
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
            // Render publishes after it releases the lock, so a Prepare that
            // finished just before this step took it may still publish now.
            // Read the frame once that publish has landed; a later change
            // would need a Prepare inside this lock.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            frame_at_sleep.store(engine.published_frame());
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
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
    const engine_core::InstanceId id = create_part(engine.datamodel()).id();
    const engine_core::Matrix4 expected = T0();
    std::atomic<int> ready{0};
    engine_core::Matrix4 snapped{};
    engine_core::WriteOrigin origin = engine_core::WriteOrigin::SnapshotOverride;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        engine.datamodel().game_object(id)->set_transform(expected);
    });
    struct Probe : CountingRenderer {
        engine_core::Engine* engine = nullptr;
        engine_core::InstanceId id = 0;
        const engine_core::Matrix4* expected = nullptr;
        engine_core::Matrix4* snapped = nullptr;
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
    const engine_core::InstanceId id = create_part(engine.datamodel()).id();
    const engine_core::Matrix4 sim = T0();
    const engine_core::Matrix4 flash = T1();
    std::atomic<int> heartbeats{0};
    std::atomic<int> stage{0};
    engine_core::Matrix4 live{};
    engine_core::Matrix4 snap_override{};
    engine_core::Matrix4 snap_next{};
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
        engine_core::Matrix4* snap_override = nullptr;
        engine_core::Matrix4* snap_next = nullptr;
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
    const engine_core::InstanceId id = create_part(game).id();
    game.set_visual_only(id, true);
    const engine_core::Matrix4 posed = T1();
    std::atomic<int> stage{0};
    engine_core::Matrix4 live_at_write{};
    engine_core::Matrix4 snapped{};
    engine_core::Matrix4 live_later{};
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
        engine_core::Matrix4* snapped = nullptr;
        engine_core::WriteOrigin* origin = nullptr;
        const engine_core::Matrix4* posed = nullptr;
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
    const engine_core::InstanceId id = create_part(game).id();
    game.set_simulated(id, true);
    game.game_object(id)->set_linear_velocity(1.f, 0.f, 0.f);
    const engine_core::Matrix4 posed = T1();
    std::atomic<int> rejected{0};
    std::atomic<int> stage{0};
    engine_core::Matrix4 snapped{};
    engine_core::Matrix4 live_at_write{};
    engine_core::Matrix4 live_after_physics{};
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
        engine_core::Matrix4* snapped = nullptr;
        const engine_core::Matrix4* posed = nullptr;
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
    // Under Workspace, so it has a row for the destroy to remove.
    engine_core::GameObject& object = create_part(engine.datamodel());
    const engine_core::InstanceId id = object.id();
    std::atomic<int> seen{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> live_closed{0};
    std::atomic<int> snap_gone{0};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        // Only once a snapshot has shown the row, so its absence proves the removal.
        if (seen.load() == 1 && destroyed.load() == 0) {
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
        std::atomic<int>* seen = nullptr;
        std::atomic<int>* destroyed = nullptr;
        std::atomic<int>* snap_gone = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            bool present = false;
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id == id) {
                    present = true;
                }
            }
            if (destroyed->load() == 0) {
                if (present) {
                    seen->store(1);
                }
                return;
            }
            if (!present) {
                snap_gone->store(1);
            }
        }
    } probe;
    probe.id = id;
    probe.seen = &seen;
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
    const engine_core::InstanceId id = create_part(engine.datamodel()).id();
    const engine_core::Matrix4 posed = engine_core::matrix4_translation(0.15f, 0.25f, 0.35f);
    std::atomic<int> handler_ran{0};
    std::atomic<int> bad{0};
    std::atomic<int> ready{0};
    std::atomic<int> phase{-1};
    std::atomic<int> role{-1};
    std::atomic<int> window{-1};
    engine.datamodel().property_changed(id, engine_core::Field::Transform).connect([&](engine_core::InstanceId, engine_core::Field) {
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
        engine.datamodel().game_object(id)->set_transform(posed);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        const engine_core::Matrix4* posed = nullptr;
        std::atomic<int>* handler_ran = nullptr;
        std::atomic<int>* bad = nullptr;
        std::atomic<int>* ready = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (ready->load() != 0 || bad->load() != 0) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            if (inst == nullptr || !near(inst->world, *posed)) {
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
    probe.posed = &posed;
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
    REQUIRE(near(engine.datamodel().game_object(id)->transform(), posed));
}

TEST_CASE("handler writes are in the same snapshot", "[T13]") {
    engine_core::Engine engine;
    // The Heartbeat moves the first part; its handler moves the second.
    const engine_core::InstanceId id = create_part(engine.datamodel()).id();
    const engine_core::InstanceId follower = create_part(engine.datamodel()).id();
    const engine_core::Matrix4 moved = T0();
    const engine_core::Matrix4 posed = T1();
    std::atomic<int> ready{0};
    std::atomic<int> bad{0};
    engine.datamodel().property_changed(id, engine_core::Field::Transform).connect([&](engine_core::InstanceId, engine_core::Field) {
        engine.datamodel().game_object(follower)->set_transform(posed);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (ready.load() != 0) {
            return;
        }
        engine.datamodel().game_object(id)->set_transform(moved);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        engine_core::InstanceId follower = 0;
        const engine_core::Matrix4* moved = nullptr;
        const engine_core::Matrix4* posed = nullptr;
        std::atomic<int>* ready = nullptr;
        std::atomic<int>* bad = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (ready->load() != 0) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            const engine_core::VisualInstance* other = find_instance(snapshot, follower);
            if (inst == nullptr || other == nullptr) {
                return;
            }
            const bool moved_ok = near(inst->world, *moved);
            const bool posed_ok = near(other->world, *posed);
            if (!moved_ok && !posed_ok) {
                return;
            }
            if (moved_ok && posed_ok) {
                ready->store(1);
                return;
            }
            bad->store(1);
        }
    } probe;
    probe.id = id;
    probe.follower = follower;
    probe.moved = &moved;
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
    REQUIRE(near(engine.datamodel().game_object(id)->transform(), moved));
    REQUIRE(near(engine.datamodel().game_object(follower)->transform(), posed));
}

TEST_CASE("deferred handler does not re-enter the same drain", "[T14]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_game_object().id();
    std::atomic<int> depth{0};
    std::atomic<int> max_depth{0};
    std::atomic<int> transform_hits{0};
    std::atomic<int> name_hits{0};
    engine.datamodel().changed(id).connect([&](engine_core::InstanceId changed, engine_core::Field field) {
        const int now = depth.fetch_add(1) + 1;
        int seen = max_depth.load();
        while (now > seen && !max_depth.compare_exchange_weak(seen, now)) {
        }
        if (field == engine_core::Field::Transform) {
            transform_hits.fetch_add(1);
            engine.datamodel().set_name(changed, "Renamed");
        } else if (field == engine_core::Field::Name) {
            name_hits.fetch_add(1);
        }
        depth.fetch_sub(1);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (transform_hits.load() != 0) {
            return;
        }
        engine.datamodel().game_object(id)->set_transform(T0());
    });
    engine.start();
    engine.resume();
    wait_until([&] { return transform_hits.load() == 1 && name_hits.load() == 1; });
    engine.stop();
    REQUIRE(max_depth.load() == 1);
    REQUIRE(transform_hits.load() == 1);
    REQUIRE(name_hits.load() == 1);
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
            engine.datamodel().game_object(id)->set_transform(engine_core::matrix4_translation(0.1f, 0.2f, 0.3f));
            stage.store(1);
        } else if (step == 1 && a.load() >= 1) {
            engine.datamodel().game_object(id)->set_transform(engine_core::matrix4_translation(0.4f, 0.5f, 0.6f));
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
            (void)object->transform();
        }
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        game.game_object(id)->set_transform(T1());
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
    const engine_core::InstanceId id = create_part(game).id();
    game.set_visual_only(id, true);
    const engine_core::Matrix4 posed = engine_core::matrix4_translation(0.2f, 0.8f, 0.1f);
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
        game.game_object(id)->set_transform(posed);
        during.store(hits.load());
        stage.store(1);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        const engine_core::Matrix4* posed = nullptr;
        std::atomic<int>* stage = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            if (stage->load() != 1) {
                return;
            }
            const engine_core::VisualInstance* inst = find_instance(snapshot, id);
            if (inst != nullptr && near(inst->world, *posed)) {
                stage->store(2);
            }
        }
    } probe;
    probe.id = id;
    probe.posed = &posed;
    probe.stage = &stage;
    engine.set_renderer(&probe);
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 2 && hits.load() >= 1; });
    engine.stop();
    REQUIRE(during.load() == 0);
    REQUIRE(bad.load() == 0);
    REQUIRE(hits.load() >= 1);
    REQUIRE(near(game.game_object(id)->transform(), posed));
    REQUIRE(game.events().count(engine_core::WriteOrigin::PreRenderDataModel) >= 1);
    REQUIRE(game.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
}

TEST_CASE("path C emits nothing", "[T18]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    // Under Workspace, so the override lands on a row.
    const engine_core::InstanceId id = create_part(game).id();
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int> moved{0};
        void perform(const engine_core::VisualSnapshot& snapshot) override {
            for (const engine_core::VisualInstance& item : snapshot.instances) {
                if (item.id == id && item.transform_origin == engine_core::WriteOrigin::SnapshotOverride &&
                    near(item.world, T1())) {
                    moved.store(1);
                }
            }
        }
    } probe;
    probe.id = id;
    engine.set_renderer(&probe);
    const engine_core::Matrix4 live_transform = game.game_object(id)->transform();
    REQUIRE_FALSE(near(live_transform, T1()));
    std::atomic<int> hits{0};
    std::atomic<int> stage{0};
    game.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    game.property_changed(id, engine_core::Field::Transform)
        .connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        if (stage.load() != 0) {
            return;
        }
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
    REQUIRE(probe.moved.load() == 1);  // the override reached the pixels
    REQUIRE(hits.load() == 0);
    REQUIRE(near(game.game_object(id)->transform(), live_transform));
    REQUIRE(game.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
    REQUIRE(game.events().suppressed_overrides() == 0);
}

TEST_CASE("count of SnapshotOverride is the suppressed overrides", "[events]") {
    engine_core::EventQueue events;
    events.emit(engine_core::SignalId{}, 0, engine_core::Field::Reflected, engine_core::WriteOrigin::SnapshotOverride);
    REQUIRE(events.suppressed_overrides() == 1);
    REQUIRE(events.count(engine_core::WriteOrigin::SnapshotOverride) == events.suppressed_overrides());
}

TEST_CASE("wait resumes on a later simulation phase", "[T19]") {
    if (!engine_core::TaskScheduler::can_suspend()) {
        SKIP("Signal::wait needs the fiber switch, which this platform does not build");
    }
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
            engine.datamodel().property_changed(id, engine_core::Field::Transform).wait();
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
            engine.datamodel().game_object(id)->set_transform(engine_core::matrix4_translation(0.3f, 0.2f, 0.1f));
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
                    engine.datamodel().game_object(changed)->set_transform(
                        engine_core::matrix4_translation(10.f + static_cast<float>(n), 2.f, 3.f));
                }
                depth.fetch_sub(1);
            });
            engine.datamodel().game_object(id)->set_transform(engine_core::matrix4_translation(0.4f, 0.5f, 0.6f));
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

TEST_CASE("plain instances do not carry transform or velocity", "[instance]") {
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
    Spinner& spinner = game.create<Spinner>();
    game.set_parent(spinner.id(), workspace_of(game));
    REQUIRE(game.instance(spinner.id()) == &spinner);
    REQUIRE(game.game_object(spinner.id()) == nullptr);
    REQUIRE(game.parent(spinner.id()) == workspace_of(game));
    REQUIRE(game.first_child(workspace_of(game)) == spinner.id());
    REQUIRE(spinner.degrees() == 0.0);
    spinner.step(1.0);
    REQUIRE(spinner.degrees() == 90.0);

    engine_core::DataModel& plain = game.create();
    REQUIRE(game.parent(plain.id()) == engine_core::DataModel::kNoParent);
    REQUIRE(dynamic_cast<Spinner*>(game.instance(plain.id())) == nullptr);
    REQUIRE(game.first_child(workspace_of(game)) == spinner.id());

    const engine_core::InstanceId id = spinner.id();
    game.destroy(id);
    REQUIRE_FALSE(game.alive(id));
    REQUIRE(game.instance(id) == nullptr);
    REQUIRE(spinner.degrees() == 0.0);
    REQUIRE(game.first_child(workspace_of(game)) == 0);

    Spinner& again = game.create<Spinner>();
    REQUIRE(again.degrees() == 0.0);
    REQUIRE(game.game_object(again.id()) == nullptr);
}

TEST_CASE("Heartbeat steps descendants of the root", "[instance]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    Spinner& spinner = game.create<Spinner>();
    game.set_parent(spinner.id(), workspace_of(game));
    Spinner& nested = game.create<Spinner>();
    game.set_parent(nested.id(), spinner.id());
    Spinner& loose = game.create<Spinner>();
    engine.start();
    REQUIRE(spinner.degrees() == 0.0);
    REQUIRE(nested.degrees() == 0.0);
    engine.resume();
    wait_until([&] { return spinner.degrees() > 1.0 && nested.degrees() > 1.0; });
    REQUIRE(loose.degrees() == 0.0);
    engine.stop();
}

TEST_CASE("RenderStepped writes this frame and PostRender does not", "[T21]") {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId id = create_part(game).id();
    game.set_visual_only(id, true);
    const engine_core::Matrix4 posed = T1();
    const engine_core::Matrix4 refused = T0();

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
    engine_core::Matrix4 snapped{};
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
            game.game_object(id)->set_transform(refused);
        } catch (const engine_core::ContractViolation& ex) {
            post_write = ex.what();
        }
        try {
            engine_core::SnapshotOverride override;
            override.id = id;
            override.field = engine_core::VisualField::Transform;
            override.transform = refused;
            engine.pump().override_visual(override);
        } catch (const engine_core::ContractViolation& ex) {
            post_override = ex.what();
        }
        inst = engine.pump().find(id);
        const bool held = inst != nullptr && near(inst->world, posed);
        front_held.store(held ? 1 : 0);
        stage.store(4);
    });

    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Matrix4* snapped = nullptr;
        engine_core::WriteOrigin* origin = nullptr;
        const engine_core::Matrix4* posed = nullptr;
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
            engine.datamodel().property_changed(id, engine_core::Field::Transform).wait();
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
            engine.datamodel().property_changed(id, engine_core::Field::Transform).wait();
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

TEST_CASE("a paused edit parents a GameObject before the next step", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    REQUIRE(engine.paused());
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::GameObject& part = game.create<engine_core::GameObject>();
        game.set_parent(part.id(), workspace_of(game));
        id = part.id();
    });
    REQUIRE(id != 0);
    REQUIRE(engine.datamodel().alive(id));
    REQUIRE(engine.datamodel().parent(id) == workspace_of(engine.datamodel()));
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
        game.set_parent(part.id(), workspace_of(game));
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

TEST_CASE("a paused edit writes transform and flags at once", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    engine_core::InstanceId id = 0;
    bool seen_inside = false;
    const engine_core::Matrix4 moved = engine_core::matrix4_translation(1.f, 2.f, 3.f);
    engine.on_simulation([&](engine_core::DataModel& game) {
        begin_step(game);
        engine_core::GameObject& part = game.create<engine_core::GameObject>();
        game.set_parent(part.id(), workspace_of(game));
        id = part.id();
        part.set_transform(moved);
        game.set_simulated(id, true);
        game.set_visual_only(id, true);
        end_step(game);
        // A project load reads these back before it captures the place.
        seen_inside = part.transform().m[12] == 1.f && game.simulated(id);
    });
    REQUIRE(seen_inside);
    const engine_core::GameObject* part = engine.datamodel().game_object(id);
    REQUIRE(part != nullptr);
    REQUIRE(part->transform().m[13] == 2.f);
    REQUIRE(engine.datamodel().visual_only(id));
    // Undo of those writes runs as a paused edit too.
    engine.on_simulation([&](engine_core::DataModel& game) { game.history().undo(); });
    REQUIRE_FALSE(engine.datamodel().alive(id));
    engine.on_simulation([&](engine_core::DataModel& game) { game.history().redo(); });
    REQUIRE(engine.datamodel().visual_only(id));
    REQUIRE(engine.datamodel().game_object(id)->transform().m[12] == 1.f);
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
        engine_core::GameObject& part = game.create<engine_core::GameObject>();
        game.set_parent(part.id(), workspace_of(game));
        id.store(part.id());
    });
    wait_until([&] { return id.load() != 0; });
    engine.stop();
    REQUIRE(on_sim.load() == 1);
    REQUIRE(engine.datamodel().alive(id.load()));
    REQUIRE(engine.datamodel().parent(id.load()) == workspace_of(engine.datamodel()));
}

TEST_CASE("a paced simulation steps at the rate it is given", "[pace]") {
    engine_core::Engine engine;
    engine.set_simulation_pace_hz(60.0);
    engine.start();
    engine.resume();
    wait_until([&] { return engine.sim_frame_count() > 0; });
    const auto first = engine.sim_frame_count();
    const auto from = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto steps = engine.sim_frame_count() - first;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
    engine.stop();
    // Each sleep wakes late, by up to a whole 15.6 ms Windows tick. Pacing on a
    // fixed schedule absorbs that; pacing each step on its own ran near 32 Hz.
    const double hz = static_cast<double>(steps) / seconds;
    INFO("steps a second: " << hz);
    REQUIRE(hz > 57.0);
    REQUIRE(hz < 63.0);
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

TEST_CASE("client sync steps an uncapped simulation with each paint", "[pace]") {
    engine_core::Engine engine;
    engine.set_simulation_pace_hz(0.0);
    engine.set_render_pace_hz(0.0);
    engine.set_simulation_client_sync(true);
    engine.set_render_client_sync(true);
    std::mutex dts_mu;
    std::vector<double> dts;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double dt) {
        std::lock_guard<std::mutex> guard(dts_mu);
        dts.push_back(dt);
    });
    engine.start();
    engine.resume();
    wait_until([&] { return engine.sim_frame_count() > 0; });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    // With no paint, the step falls back to about 60 Hz rather than spinning.
    const auto idle_from = engine.sim_frame_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto idle = engine.sim_frame_count() - idle_from;
    INFO("idle steps in 200 ms: " << idle);
    REQUIRE(idle >= 6);
    REQUIRE(idle <= 16);
    {
        std::lock_guard<std::mutex> guard(dts_mu);
        dts.clear();
    }
    // Paints every 4 ms, a 250 Hz window. A 60 Hz step would take 40 ms per 2.4 paints.
    // Paced against a deadline, not sleep_for(4 ms): Windows sleeps in ~15.6 ms
    // ticks, which would make this a 64 Hz window and look like the fallback.
    const auto paced_at = std::chrono::steady_clock::now();
    const auto first = engine.sim_frame_count();
    auto next_paint = paced_at;
    for (int i = 0; i < 60; ++i) {
        next_paint += std::chrono::milliseconds(4);
        while (std::chrono::steady_clock::now() < next_paint) {
            std::this_thread::yield();
        }
        engine.note_client_frame();
    }
    const auto steps = engine.sim_frame_count() - first;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - paced_at).count();
    engine.stop();
    const double hz = static_cast<double>(steps) / seconds;
    INFO("paints a second: " << 60.0 / seconds);
    INFO("steps a second: " << hz);
    REQUIRE(hz > 120.0);
    // Each step's dt is the time it covers, so Heartbeat time keeps up with the clock.
    double total = 0;
    for (double dt : dts) {
        total += dt;
    }
    INFO("Heartbeat time " << total << " over " << seconds << " s");
    REQUIRE(total > seconds * 0.8);
    REQUIRE(total < seconds * 1.2);
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

    part.set_transform(T0());
    REQUIRE(game.invalidations().size() == 1);

    const engine_core::InstanceId dead = plain.id();
    game.destroy(dead);
    REQUIRE(game.name(dead).empty());
    REQUIRE_THROWS_AS(game.set_name(dead, "Nope"), engine_core::ContractViolation);
}

TEST_CASE("N6 the root Changed signal is not the first instance", "[N6]") {
    engine_core::Game game;
    // The first slot holds Workspace, the first scene service Game makes.
    const engine_core::InstanceId first = workspace_of(game);
    REQUIRE((first & 0xffffu) == 0);
    REQUIRE(first != game.id());
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_parent(part.id(), first);

    int root_changed = 0;
    int root_named = 0;
    int root_added = 0;
    int first_changed = 0;
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
    game.changed(first).connect([&](engine_core::InstanceId, engine_core::Field) { ++first_changed; });
    game.changed(part.id()).connect([&](engine_core::InstanceId id, engine_core::Field) {
        REQUIRE(id == part.id());
        ++part_changed;
    });
    game.child_added(game.id()).connect([&](engine_core::InstanceId, engine_core::Field) { ++root_added; });
    game.child_added(first).connect([&](engine_core::InstanceId child, engine_core::Field) {
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
    game.set_parent(extra.id(), first);
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(root_changed == 1);
    REQUIRE(root_named == 1);
    REQUIRE(first_changed == 0);
    REQUIRE(root_added == 0);
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
    REQUIRE(first_changed == 0);
    REQUIRE(part_changed == part_held);
}

TEST_CASE("SG9 ancestry_changed fires on the moved instance and every descendant, once each", "[signals]") {
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    engine_core::Folder& child = game.create<engine_core::Folder>();
    game.set_parent(child.id(), folder.id());
    engine_core::GameObject& grandchild = game.create<engine_core::GameObject>();
    game.set_parent(grandchild.id(), child.id());
    engine_core::Folder& bystander = game.create<engine_core::Folder>();
    game.set_parent(bystander.id(), workspace_of(game));

    int folder_fired = 0, child_fired = 0, grandchild_fired = 0, bystander_fired = 0;
    game.ancestry_changed(folder.id()).connect([&](engine_core::InstanceId id, engine_core::Field field) {
        REQUIRE(id == folder.id());
        REQUIRE(field == engine_core::Field::Parent);
        ++folder_fired;
    });
    game.ancestry_changed(child.id()).connect([&](engine_core::InstanceId id, engine_core::Field) {
        REQUIRE(id == child.id());
        ++child_fired;
    });
    game.ancestry_changed(grandchild.id()).connect([&](engine_core::InstanceId, engine_core::Field) { ++grandchild_fired; });
    game.ancestry_changed(bystander.id()).connect([&](engine_core::InstanceId, engine_core::Field) { ++bystander_fired; });

    game.set_parent(folder.id(), game.scene_service("Storage"));
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(folder_fired == 1);
    REQUIRE(child_fired == 1);
    REQUIRE(grandchild_fired == 1);
    REQUIRE(bystander_fired == 0);

    // A move to the same parent is not a change.
    game.set_parent(folder.id(), game.scene_service("Storage"));
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(folder_fired == 1);
}

TEST_CASE("N2 siblings may share a name and find_first_child returns the first", "[N2]") {
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    game.set_name(folder.id(), "Folder");
    game.set_parent(folder.id(), workspace_of(game));

    engine_core::GameObject& older = game.create<engine_core::GameObject>();
    engine_core::GameObject& newer = game.create<engine_core::GameObject>();
    game.set_name(older.id(), "Wood");
    game.set_name(newer.id(), "Wood");
    game.set_parent(older.id(), folder.id());
    game.set_parent(newer.id(), folder.id());
    // A child goes last, so the one parented first is first.
    REQUIRE(game.first_child(folder.id()) == older.id());
    REQUIRE(game.find_first_child(folder.id(), "Wood") == older.id());
    REQUIRE(game.find_first_child(folder.id(), "Missing") == 0);

    engine_core::GameObject& metal = game.create<engine_core::GameObject>();
    game.set_name(metal.id(), "Metal");
    game.set_parent(metal.id(), folder.id());
    REQUIRE(game.find_first_child(folder.id(), "Wood") == older.id());
    REQUIRE(game.find_first_child(folder.id(), "Metal") == metal.id());

    const std::vector<engine_core::InstanceId> children = game.get_children(folder.id());
    REQUIRE(children.size() == 3);
    REQUIRE(children[0] == older.id());
    REQUIRE(children[1] == newer.id());
    REQUIRE(children[2] == metal.id());
    REQUIRE(game.get_children(0xdeadbeefu).empty());
    REQUIRE(game.find_first_child(0xdeadbeefu, "Wood") == 0);
    REQUIRE(game.get_children(workspace_of(game)).size() == 1);
    REQUIRE(game.get_children(workspace_of(game))[0] == folder.id());
}

TEST_CASE("N3 place restore reverts play and drops session instances", "[N3]") {
    engine_core::Game game;
    engine_core::DataModel& folder = game.create();
    const engine_core::InstanceId folder_id = folder.id();
    game.set_name(folder_id, "Folder");
    game.set_parent(folder_id, workspace_of(game));

    engine_core::GameObject& leaf = game.create<engine_core::GameObject>();
    const engine_core::InstanceId leaf_id = leaf.id();
    game.set_name(leaf_id, "Leaf");
    game.set_parent(leaf_id, folder_id);
    const engine_core::Matrix4 posed = T0();
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
    leaf.set_transform(T1());
    game.set_parent(leaf_id, workspace_of(game));
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
    // A captured instance's slot waits for Stop, so play does not reuse it.
    REQUIRE((recycled_id & 0xffffu) != (sibling_id & 0xffffu));
    game.set_name(recycled_id, "Recycled");
    game.set_parent(recycled_id, workspace_of(game));

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
    REQUIRE(game.parent(folder_id) == workspace_of(game));
    REQUIRE(game.find_first_child(folder_id, "Sibling") == sibling_id);
    REQUIRE(near(game.game_object(leaf_id)->transform(), posed));
    REQUIRE(game.simulated(leaf_id));
    game.integrate_simulated(1.0);
    REQUIRE(near(game.game_object(leaf_id)->transform(), posed));

    const std::vector<engine_core::InstanceId> children = game.get_children(folder_id);
    REQUIRE(children.size() == 2);
    REQUIRE(children[0] == leaf_id);
    REQUIRE(children[1] == sibling_id);

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
    REQUIRE(near(vis->world, posed));
    REQUIRE(pump.find(extra_id) == nullptr);
    REQUIRE(pump.find(recycled_id) == nullptr);
    REQUIRE(pump.find(sibling_id) != nullptr);
    REQUIRE(pump.find(folder_id) == nullptr);
}

TEST_CASE("N4 a second play restores the original place", "[N4]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    game.set_parent(id, workspace_of(game));
    const engine_core::Matrix4 door = T0();
    part.set_transform(door);
    game.set_name(id, "Door");
    game.capture_place();
    const std::uint32_t generation = game.world_generation();

    game.start_simulation();
    part.set_transform(T1());
    game.set_name(id, "Session");
    engine_core::GameObject& extra = game.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    game.stop_simulation();
    REQUIRE(game.world_generation() == generation + 1);
    REQUIRE(game.name(id) == "Door");
    REQUIRE(near(game.game_object(id)->transform(), door));
    REQUIRE_FALSE(game.alive(extra_id));

    game.game_object(id)->set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
    game.set_name(id, "Edited");
    engine_core::GameObject& between = game.create<engine_core::GameObject>();
    const engine_core::InstanceId between_id = between.id();
    game.set_parent(between_id, workspace_of(game));

    game.start_simulation();
    game.game_object(id)->set_transform(T1());
    game.set_name(id, "Again");
    game.stop_simulation();
    REQUIRE(game.world_generation() == generation + 2);
    REQUIRE(game.name(id) == "Door");
    REQUIRE(near(game.game_object(id)->transform(), door));
    REQUIRE(game.parent(id) == workspace_of(game));
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
    game.set_parent(folder_id, workspace_of(game));

    engine_core::Folder& cut = game.create<engine_core::Folder>();
    const engine_core::InstanceId cut_id = cut.id();
    game.set_name(cut_id, "Loose");
    game.set_parent(cut_id, workspace_of(game));
    // Insert while stopped replaces the snapshot, which is why the new
    // folders survive the next Stop.
    game.capture_place();

    game.start_simulation();
    game.stop_simulation();
    REQUIRE(game.parent(folder_id) == workspace_of(game));
    REQUIRE(game.parent(cut_id) == workspace_of(game));

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
    REQUIRE(game.find_first_child(workspace_of(game), "Props") == 0);
    REQUIRE(game.find_first_child(workspace_of(game), "Loose") == 0);
}

TEST_CASE("edit then play captures the place on start", "[N4]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    const engine_core::Matrix4 door = T0();
    part.set_transform(door);
    game.set_name(id, "Door");
    game.set_parent(id, workspace_of(game));
    REQUIRE_FALSE(game.simulation_running());
    game.start_simulation();
    REQUIRE(game.simulation_running());
    part.set_transform(T1());
    game.set_name(id, "Gone");
    engine_core::GameObject& extra = game.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    game.stop_simulation();
    REQUIRE(game.name(id) == "Door");
    REQUIRE(near(game.game_object(id)->transform(), door));
    REQUIRE(game.parent(id) == workspace_of(game));
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

engine_core::GameObject& add_part(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), parent);
    return part;
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
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "A") == first.id());
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
    engine_core::GameObject& part = add_part(rig.game, workspace_of(rig.game), "P");
    engine_core::GameObject& hits = add_part(rig.game, workspace_of(rig.game), "Hits");
    engine_core::GameObject& beat = add_part(rig.game, workspace_of(rig.game), "Beat");
    engine_core::GameObject& other = add_part(rig.game, workspace_of(rig.game), "Other");
    // The find names are the stable names. Counters live on children so renames do not hide them.
    engine_core::GameObject& hit_count = add_part(rig.game, hits.id(), "0");
    engine_core::GameObject& beat_count = add_part(rig.game, beat.id(), "0");
    engine_core::GameObject& other_count = add_part(rig.game, other.id(), "0");
    engine_core::Script& one = add_script(rig.game, "One", R"(
        local part = workspace:FindFirstChild("P")
        local hits = workspace:FindFirstChild("Hits"):GetChildren()[1]
        local beat = workspace:FindFirstChild("Beat"):GetChildren()[1]
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
        local part = workspace:FindFirstChild("P")
        local other = workspace:FindFirstChild("Other"):GetChildren()[1]
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
    part.set_transform(engine_core::matrix4_translation(0.2f, 0.3f, 0.4f));
    rig.game.events().drain();
    const int hits_before = name_number(rig.game, hit_count.id());
    const int other_before = name_number(rig.game, other_count.id());
    REQUIRE(hits_before > 0);
    REQUIRE(other_before > 0);

    one.set_enabled(false);
    part.set_transform(engine_core::matrix4_translation(0.6f, 0.1f, 0.1f));
    rig.game.events().drain();
    REQUIRE(name_number(rig.game, hit_count.id()) == hits_before);
    REQUIRE(name_number(rig.game, other_count.id()) > other_before);
    const int beat_held = name_number(rig.game, beat_count.id());
    rig.frames(2, 0.05);
    REQUIRE(name_number(rig.game, beat_count.id()) == beat_held);

    const int other_held = name_number(rig.game, other_count.id());
    rig.game.destroy(two.id());
    part.set_transform(engine_core::matrix4_translation(0.1f, 0.7f, 0.2f));
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
    rig.game.set_parent(id, workspace_of(rig.game));
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
        made.Parent = workspace
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
    const engine_core::InstanceId from_new = rig.game.find_first_child(workspace_of(rig.game), "FromNew");
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
    engine_core::GameObject& runs = add_part(rig.game, workspace_of(rig.game), "0");
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
    rig.game.set_parent(mod.id(), workspace_of(rig.game));
    engine_core::ModuleScript& cycle = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(cycle.id(), "Cycle");
    cycle.set_source("return require(script)");
    rig.game.set_parent(cycle.id(), workspace_of(rig.game));
    add_script(rig.game, "Main", R"(
        local mod = workspace:FindFirstChild("Mod")
        local a = require(mod)
        local b = require(mod)
        _G.same = (a == b)
        _G.n = a.n
        local ok = pcall(function()
            require(workspace:FindFirstChild("Cycle"))
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
    engine_core::GameObject& door = add_part(rig.game, workspace_of(rig.game), "Door");
    engine_core::Script& maker = add_script(rig.game, "Maker", R"(
        local made = Instance.new("GameObject")
        made.Name = "Session"
        made.Parent = script.Parent
        local door = script.Parent:FindFirstChild("Door")
        door.Name = "Moved"
    )");
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "Maker") == maker.id());
    REQUIRE(std::string(maker.class_name()) == "Script");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.game.name(door.id()) == "Moved");
    const engine_core::InstanceId session = rig.game.find_first_child(workspace_of(rig.game), "Session");
    REQUIRE(session != 0);
    REQUIRE(std::string(rig.game.instance(session)->class_name()) == "GameObject");

    rig.game.stop_simulation();
    REQUIRE(rig.game.name(door.id()) == "Door");
    REQUIRE_FALSE(rig.game.alive(session));
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "Session") == 0);
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "Maker") == maker.id());
}

TEST_CASE("S7 parenting a script while running starts it after the drain", "[S7]") {
    ScriptRig rig;
    engine_core::GameObject& mark = add_part(rig.game, workspace_of(rig.game), "Mark");
    engine_core::GameObject& token = add_part(rig.game, mark.id(), "hidden");
    engine_core::GameObject& flag = add_part(rig.game, workspace_of(rig.game), "Flag");
    engine_core::GameObject& trigger = add_part(rig.game, workspace_of(rig.game), "Trigger");
    engine_core::Script& script = rig.game.create<engine_core::Script>();
    rig.game.set_name(script.id(), "Late");
    script.set_source(R"(
        local mark = workspace:FindFirstChild("Mark")
        local flag = workspace:FindFirstChild("Flag")
        flag.Name = mark:GetChildren()[1].Name
    )");
    REQUIRE(rig.game.parent(script.id()) == engine_core::DataModel::kNoParent);

    rig.game.start_simulation();
    bool started_inside = false;
    rig.game.changed(trigger.id()).connect([&](engine_core::InstanceId, engine_core::Field) {
        rig.game.set_name(token.id(), "visible");
        rig.game.set_parent(script.id(), workspace_of(rig.game));
        started_inside = rig.game.name(flag.id()) == "visible";
    });
    rig.game.set_name(trigger.id(), "go");
    REQUIRE(rig.game.name(flag.id()) == "Flag");
    rig.game.events().drain();
    REQUIRE_FALSE(started_inside);
    REQUIRE(rig.game.parent(script.id()) == workspace_of(rig.game));
    REQUIRE(rig.game.name(flag.id()) == "visible");
}

TEST_CASE("S8 a script position write is path A", "[S8]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.game, workspace_of(rig.game), "P");
    add_script(rig.game, "Painter", R"(
        local part = workspace:FindFirstChild("P")
        part.Transform = Matrix4.new(0.2, 0.4, 0.6)
    )");
    int hits = 0;
    engine_core::Field seen = engine_core::Field::Count;
    rig.game.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field field) {
        ++hits;
        seen = field;
    });
    rig.game.start_simulation();
    rig.frames(1, 1.0 / 60.0);
    const engine_core::Matrix4 moved = engine_core::matrix4_translation(0.2f, 0.4f, 0.6f);
    REQUIRE(hits >= 1);
    REQUIRE(seen == engine_core::Field::Transform);
    REQUIRE(near(part.transform(), moved));

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
    REQUIRE(near(vis->world, moved));
    REQUIRE(vis->transform_origin == engine_core::WriteOrigin::Simulation);
}

TEST_CASE("S9 binding PreRender from a script errors, and RenderStepped does not", "[S9]") {
    ScriptRig rig;
    add_script(rig.game, "Bad", R"(
        local pre_ok = pcall(function()
            game:GetService("RunService").PreRender:Connect(function() end)
        end)
        local step_ok = pcall(function()
            game:GetService("RunService").RenderStepped:Connect(function() end)
        end)
        _G.pre = not pre_ok
        _G.step = step_ok
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
    engine_core::GameObject& door = add_part(rig.game, workspace_of(rig.game), "Door");
    add_script(rig.game, "Keeper", R"(
        local door = workspace:FindFirstChild("Door")
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
    engine_core::GameObject& part = add_part(game, workspace_of(game), "P");
    add_script(game, "Beat", R"(
        local part = workspace:FindFirstChild("P")
        local n = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            n = n + 1
            part.Transform = Matrix4.new(n / 100, 0.2, 0.3)
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

TEST_CASE("Vector2 is a value with Roblox's API", "[vector2]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        local a = Vector2.new(3, 4)
        local checks = {
            typeof(a) == "Vector2",
            type(a) == "userdata",
            a.X == 3 and a.Y == 4,
            a.Magnitude == 5,
            a.Unit:FuzzyEq(Vector2.new(0.6, 0.8)),
            tostring(a) == "3, 4",
            tostring(Vector2.new(-1.5)) == "-1.5, 0",
            a == Vector2.new(3, 4),
            a ~= Vector2.new(4, 3),
            a + Vector2.one == Vector2.new(4, 5),
            a - Vector2.one == Vector2.new(2, 3),
            -a == Vector2.new(-3, -4),
            a * 2 == Vector2.new(6, 8),
            2 * a == Vector2.new(6, 8),
            a * Vector2.new(2, 3) == Vector2.new(6, 12),
            a / 2 == Vector2.new(1.5, 2),
            12 / a == Vector2.new(4, 3),
            a // 2 == Vector2.new(1, 2),
            Vector2.zero == Vector2.new(0, 0),
            Vector2.xAxis == Vector2.new(1, 0) and Vector2.yAxis == Vector2.new(0, 1),
            Vector2.new(-1.5, 2.5):Abs() == Vector2.new(1.5, 2.5),
            Vector2.new(1.2, -1.2):Ceil() == Vector2.new(2, -1),
            Vector2.new(1.2, -1.2):Floor() == Vector2.new(1, -2),
            Vector2.new(-3, 0):Sign() == Vector2.new(-1, 0),
            Vector2.xAxis:Cross(Vector2.yAxis) == 1,
            a:Dot(Vector2.new(1, 1)) == 7,
            math.abs(Vector2.xAxis:Angle(Vector2.yAxis) - math.pi / 2) < 1e-6,
            math.abs(Vector2.yAxis:Angle(Vector2.xAxis, true) + math.pi / 2) < 1e-6,
            Vector2.yAxis:Angle(Vector2.xAxis) > 0,
            Vector2.zero:Lerp(a, 0.5) == Vector2.new(1.5, 2),
            Vector2.new(1, 5):Max(Vector2.new(2, 1), Vector2.new(0, 7)) == Vector2.new(2, 7),
            Vector2.new(1, 5):Min(Vector2.new(2, 1)) == Vector2.new(1, 1),
            not pcall(function() a.X = 1 end),
            not pcall(function() return a.Z end),
            not pcall(function() return a + 1 end),
            not pcall(function() return Vector2.new(nil) end),
            not pcall(function() return a:Dot(Vector3.one) end),
        }
        for index, ok in checks do
            if not ok then
                error("check " .. index .. " failed")
            end
        end
        _G.vector2_checks = #checks
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    double count = 0;
    REQUIRE(rig.runtime.global_number("vector2_checks", count));
    REQUIRE(count == 37);
}

TEST_CASE("Vector3 is a GameObject's Transform.Position", "[vector3]") {
    ScriptRig rig;
    engine_core::GameObject& part = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(part.id(), "Tri0");
    rig.game.set_parent(part.id(), workspace_of(rig.game));
    part.set_position(engine_core::Vec3{-0.58f, 0.38f, 0.f});
    add_script(rig.game, "Main", R"(
        local tri = workspace:FindFirstChild("Tri0")
        local home = tri.Transform.Position
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
            tri.Transform = {x = 1, y = 2, z = 3}
        end)
        _G.table_rejected = not ok
        _G.table_msg = type(err) == "string" and string.find(err, "Matrix4", 1, true) ~= nil
        local held = tri.Transform.Position
        _G.held = held.X == home.X and held.Y == home.Y and held.Z == home.Z

        tri.Transform = Matrix4.new(vector.create(3, 4, 5))
        local placed = tri.Transform.Position
        _G.vec_x, _G.vec_y, _G.vec_z = placed.X, placed.Y, placed.Z
        tri.Transform = tri.Transform.Rotation + (home + Vector3.new(0.5, 0, 0))
        _G.hop_x = tri.Transform.Position.X
        _G.hop_y = tri.Transform.Position.Y
        tri.Transform = tri.Transform.Rotation + Vector3.new(1.5, -2, 0.25)
        local set = tri.Transform.Position
        _G.set_x, _G.set_y, _G.set_z = set.X, set.Y, set.Z
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

    const engine_core::Vec3 position = part.position();
    REQUIRE(position.x == 1.5f);
    REQUIRE(position.y == -2.f);
    REQUIRE(position.z == 0.25f);
}

TEST_CASE("scene scripts hop a part on task.wait and stop restores the pose", "[scene]") {
    ScriptRig rig;
    engine_core::GameObject& slow = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(slow.id(), "Tri0");
    rig.game.set_parent(slow.id(), workspace_of(rig.game));
    slow.set_position(engine_core::Vec3{-0.58f, 0.38f, 0.f});
    engine_core::GameObject& fast = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(fast.id(), "Tri1");
    rig.game.set_parent(fast.id(), workspace_of(rig.game));
    fast.set_position(engine_core::Vec3{0.58f, 0.38f, 0.15f});
    add_script(rig.game, "HopSlow", R"(
        local tri = workspace:FindFirstChild("Tri0")
        assert(tri)
        local home = tri.Transform
        local n = 0
        while true do
            task.wait(0.5)
            n = n + 1
            local hop = (n % 2 == 1) and 0.45 or 0
            tri.Transform = home + Vector3.new(hop, 0, 0)
        end
    )");
    add_script(rig.game, "HopFast", R"(
        local tri = workspace:FindFirstChild("Tri1")
        assert(tri)
        local home = tri.Transform
        local n = 0
        while true do
            task.wait(0.2)
            n = n + 1
            local hop = (n % 2 == 1) and 0.35 or 0
            tri.Transform = home + Vector3.new(0, hop, 0)
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
    REQUIRE(rig.game.name(rig.game.find_first_child(workspace_of(rig.game), "HopSlow")) == "HopSlow");
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

TEST_CASE("S32 print copies a table so the console can open it", "[S32]") {
    ScriptRig rig;
    rig.runtime.run_chunk("local t = {10, \"a\\nb\", zed = true, name = {x = 1}, [\"two words\"] = 2}\n"
                          "t.self = t\n"
                          "print(\"label\", t)\n"
                          "print(\"plain\", 2)");
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 2);
    const engine_core::ScriptRuntime::OutputLine& line = batch.lines[0];
    REQUIRE(line.text.rfind("label\ttable: ", 0) == 0);
    REQUIRE(line.values.size() == 2);
    REQUIRE(line.values[0].text == "label");
    REQUIRE_FALSE(line.values[0].table);
    REQUIRE(line.values[1].text.rfind("table: ", 0) == 0);
    const std::shared_ptr<const engine_core::TableSnapshot> table = line.values[1].table;
    REQUIRE(table);
    std::vector<std::string> keys;
    for (const engine_core::TableField& field : table->fields) {
        keys.push_back(field.key);
    }
    REQUIRE(keys == std::vector<std::string>{"[1]", "[2]", "[\"two words\"]", "name", "self", "zed"});
    REQUIRE(table->fields[0].value == "10");
    REQUIRE(table->fields[1].value == "\"a\\nb\"");
    REQUIRE(table->fields[5].value == "true");
    REQUIRE(table->fields[3].table);
    REQUIRE(table->fields[3].table->fields.size() == 1);
    REQUIRE(table->fields[3].table->fields[0].key == "x");
    // A table inside itself is named, not copied again.
    REQUIRE(table->fields[4].note == "cycle");
    REQUIRE_FALSE(table->fields[4].table);
    REQUIRE(table->omitted == 0);
    // A print without a table carries only its text.
    REQUIRE(batch.lines[1].values.empty());
    REQUIRE(batch.lines[1].text == "plain\t2\n");
}

TEST_CASE("S35 the command line requires a ModuleScript like a script does", "[S35]") {
    ScriptRig rig;
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_name(folder.id(), "Folder");
    rig.game.set_parent(folder.id(), workspace_of(rig.game));
    engine_core::ModuleScript& config = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(config.id(), "Config");
    config.set_source(R"(
local module = {
    Configs = {
        ValidateTransactions = true,
    },
    Currencies = {
        Gold = "Gold",
        Silver = "Silver",
        Copper = "Copper",
    }
}

return module
)");
    rig.game.set_parent(config.id(), folder.id());
    rig.runtime.drain_output();

    // Stopped: the console runs the module itself.
    rig.runtime.run_chunk("print(require(workspace.Folder.Config))");
    engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Print);
    REQUIRE(batch.lines[0].values.size() == 1);
    const std::shared_ptr<const engine_core::TableSnapshot> table = batch.lines[0].values[0].table;
    REQUIRE(table);
    REQUIRE(table->fields.size() == 2);
    REQUIRE(table->fields[0].key == "Configs");
    REQUIRE(table->fields[1].key == "Currencies");
    REQUIRE(table->fields[1].table);
    REQUIRE(table->fields[1].table->fields.size() == 3);

    // One command gets one copy, as a script does.
    rig.runtime.run_chunk("print(require(workspace.Folder.Config) == require(workspace.Folder.Config), "
                          "require(workspace.Folder.Config).Currencies.Gold)");
    batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].text == "true\tGold\n");

    // The next command reads the module again, so an edit while stopped shows.
    config.set_source("return { Currencies = { Gold = \"Au\" } }");
    rig.runtime.run_chunk("print(require(workspace.Folder.Config).Currencies.Gold)");
    batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].text == "Au\n");

    // An error in the module is the command's error, and the next require still works.
    config.set_source("error(\"broken module\")");
    rig.runtime.run_chunk("print(require(workspace.Folder.Config))");
    batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Error);
    REQUIRE(batch.lines[0].text.find("broken module") != std::string::npos);
    config.set_source("return 7");
    rig.runtime.run_chunk("print(require(workspace.Folder.Config))");
    batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].text == "7\n");

    // Playing: the same, beside a script that requires it in the play VM.
    config.set_source("return { Gold = \"Gold\" }");
    add_script(rig.game, "Main", "_G.gold = require(workspace.Folder.Config).Gold");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_is_nil("gold") == false);
    rig.runtime.drain_output();
    rig.runtime.run_chunk("print(require(workspace.Folder.Config).Gold)");
    batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].text == "Gold\n");
}

TEST_CASE("S33 a large printed table is copied up to a cap", "[S33]") {
    ScriptRig rig;
    rig.runtime.run_chunk("local t = {} for i = 1, 6000 do t[i] = i end print(t)");
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].values.size() == 1);
    const std::shared_ptr<const engine_core::TableSnapshot> table = batch.lines[0].values[0].table;
    REQUIRE(table);
    REQUIRE(table->omitted > 0);
    REQUIRE(table->fields.size() + table->omitted == 6000);
    REQUIRE(table->fields.front().key == "[1]");
}

TEST_CASE("S34 a table with __tostring prints its name and is not copied", "[S34]") {
    ScriptRig rig;
    rig.runtime.run_chunk("local named = setmetatable({secret = 1}, {__tostring = function() return \"Named\" end})\n"
                          "print(named, {inner = named})");
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    const engine_core::ScriptRuntime::OutputLine& line = batch.lines[0];
    REQUIRE(line.text.rfind("Named\ttable: ", 0) == 0);
    REQUIRE(line.values.size() == 2);
    REQUIRE(line.values[0].text == "Named");
    REQUIRE_FALSE(line.values[0].table);
    const std::shared_ptr<const engine_core::TableSnapshot> table = line.values[1].table;
    REQUIRE(table);
    REQUIRE(table->fields.size() == 1);
    REQUIRE(table->fields[0].key == "inner");
    REQUIRE(table->fields[0].value == "Named");
    REQUIRE_FALSE(table->fields[0].table);
    REQUIRE(table->fields[0].note.empty());
}

TEST_CASE("S18 Heartbeat:Wait yields until the next Heartbeat", "[S18]") {
    ScriptRig rig;
    // The first Heartbeat has already been emitted by the time the script starts,
    // so the first Wait resumes on the following beat and returns that beat's dt.
    add_script(rig.game, "HopSlow", R"(
        local pre_ok = pcall(function()
            game:GetService("RunService").PreRender:Wait()
        end)
        _G.pre = not pre_ok
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
    REQUIRE(rig.runtime.global_boolean("pre", pre));
    REQUIRE(pre);
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
    add_part(rig.game, workspace_of(rig.game), "P");
    add_script(rig.game, "Watch", R"(
        local part = workspace:FindFirstChild("P")
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
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "Next") != 0);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S21 game.Changed reports the root property", "[S21]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.game, workspace_of(rig.game), "P");
    add_script(rig.game, "Watch", R"(
        local part = workspace:FindFirstChild("P")
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
    game.set_parent(props_id, workspace_of(game));

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
        session.Parent = workspace
        local loose = Instance.new("GameObject")
        loose.Name = "Loose"
        loose.Parent = session
        local props = workspace:FindFirstChild("Props")
        props.Name = "Renamed"
        local box = props:FindFirstChild("Inner"):FindFirstChild("Box")
        box.Parent = workspace
        if not session:IsA("Folder") or not session:IsA("DataModel") or session:IsA("GameObject") then
            error("folder class")
        end
    )");
    const engine_core::ScriptRuntime::OutputBatch played = rig.runtime.drain_output();
    for (const engine_core::ScriptRuntime::OutputLine& line : played.lines) {
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
    }
    REQUIRE(rig.runtime.last_error().empty());
    const engine_core::InstanceId session_id = game.find_first_child(workspace_of(game), "Session");
    REQUIRE(session_id != 0);
    REQUIRE(std::string(game.instance(session_id)->class_name()) == "Folder");
    REQUIRE(game.find_first_child(session_id, "Loose") != 0);
    REQUIRE(game.parent(box_id) == workspace_of(game));
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
    REQUIRE(game.parent(props_id) == workspace_of(game));
    REQUIRE(game.parent(inner_id) == props_id);
    REQUIRE(game.parent(box_id) == inner_id);
    REQUIRE(game.find_first_child(workspace_of(game), "Session") == 0);
    REQUIRE(game.game_object(props_id) == nullptr);
    REQUIRE(game.game_object(inner_id) == nullptr);
}

TEST_CASE("S22 a script created during play stays parented to game", "[S22]") {
    ScriptRig rig;
    add_script(rig.game, "Maker", R"lua(
        local made = Instance.new("Script")
        made.Name = "Spawned"
        made.Source = "print('from spawned')"
        made.Parent = workspace
        local also = Instance.new("Script", workspace)
        also.Name = "FromNew"
        also.Source = "print('from new')"
    )lua");
    rig.game.start_simulation();
    rig.frames(2, 0.05);
    const engine_core::InstanceId spawned = rig.game.find_first_child(workspace_of(rig.game), "Spawned");
    const engine_core::InstanceId from_new = rig.game.find_first_child(workspace_of(rig.game), "FromNew");
    REQUIRE(spawned != 0);
    REQUIRE(from_new != 0);
    REQUIRE(std::string(rig.game.instance(spawned)->class_name()) == "Script");
    REQUIRE(rig.game.parent(spawned) == workspace_of(rig.game));
    REQUIRE(rig.game.parent(from_new) == workspace_of(rig.game));
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
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "Spawned") == 0);
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "FromNew") == 0);
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
    game.set_parent(a.id(), workspace_of(game));
    engine_core::Folder& b = game.create<engine_core::Folder>();
    game.set_name(b.id(), "B");
    game.set_parent(b.id(), workspace_of(game));
    const engine_core::InstanceId a_id = a.id();
    const engine_core::InstanceId b_id = b.id();
    REQUIRE(engine_core::lua_service_known("Selection"));
    const std::string definitions = engine_core::lua_analysis_definitions();
    REQUIRE(definitions.find("function Get(self): {Instance}") != std::string::npos);
    REQUIRE(definitions.find("function Set(self, selection: {Instance}): ()") != std::string::npos);

    rig.runtime.run_chunk("local s = game:GetService(\"Selection\")\n"
                          "local a, b = workspace:FindFirstChild(\"A\"), workspace:FindFirstChild(\"B\")\n"
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
        _G.here = workspace:WaitForChild("Waiter").Name == "Waiter"
        local later = workspace:WaitForChild("Later")
        _G.got = later.Name == "Later" and _G.made == true
        local renamed = workspace:WaitForChild("Renamed")
        _G.renamed = renamed.Name == "Renamed"
        _G.timed_out = workspace:WaitForChild("Never", 0.3) == nil
    )");
    add_script(rig.game, "Maker", R"(
        task.wait(0.2)
        local made = Instance.new("Folder")
        made.Name = "Later"
        made.Parent = workspace
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
        local found = workspace:WaitForChild("Target")
        _G.ok = found.Name == "Target" and found.Parent == workspace
    )");
    add_script(rig.game, "Maker", R"(
        task.wait(0.1)
        local other = Instance.new("Folder")
        other.Name = "Other"
        other.Parent = workspace
        -- Matches, then leaves in the same step, before the waiter can resume.
        local brief = Instance.new("Folder")
        brief.Name = "Target"
        brief.Parent = workspace
        brief.Parent = other
        task.wait(0.1)
        local real = Instance.new("Folder")
        real.Name = "Target"
        real.Parent = workspace
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
        workspace:WaitForChild("Nothing")
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

TEST_CASE("S25 WaitForChild from the command line waits for the child", "[S25]") {
    ScriptRig rig;
    rig.game.start_simulation();
    rig.runtime.drain_output();
    rig.runtime.run_chunk("print(workspace:WaitForChild('Missing').Name)");
    rig.frames(1);
    REQUIRE(rig.runtime.drain_output().lines.empty());
    engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
    rig.game.set_name(folder.id(), "Missing");
    rig.game.set_parent(folder.id(), workspace_of(rig.game));
    rig.frames(1);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].text == "Missing\n");
}

TEST_CASE("S27 every handle to an instance is the same value", "[S27]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.game, "Main", R"(
        local box = script:GetChildren()[1]
        local same = script:FindFirstChild("Box")
        local seen = {}
        seen[box] = true
        _G.eq = box == same and rawequal(box, same) and seen[same] == true
        _G.parent_is_game = script.Parent == workspace and box.Parent == script
        _G.differs = box ~= script and box ~= game and box ~= nil
        local made = Instance.new("Folder")
        made.Parent = workspace
        _G.made_eq = workspace:FindFirstChild(made.Name) == made
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
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    rig.runtime.run_chunk(
        "local box = workspace:FindFirstChild('Box') assert(box == workspace:GetChildren()[1] and box.Parent == workspace)");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    rig.runtime.run_chunk("assert(workspace:FindFirstChild('Box') ~= game)");
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S29 a dot reads a child by name", "[S29]") {
    ScriptRig rig;
    engine_core::GameObject& door = add_part(rig.game, workspace_of(rig.game), "Door");
    engine_core::Folder& box = rig.game.create<engine_core::Folder>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    engine_core::GameObject& inner = add_part(rig.game, box.id(), "Inner");
    add_part(rig.game, box.id(), "Twin");
    add_part(rig.game, box.id(), "Twin");
    // A child named like a property: the property wins.
    add_part(rig.game, box.id(), "Name");
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Mod");
    module.set_source("return 42\n");
    rig.game.set_parent(module.id(), workspace_of(rig.game));
    add_script(rig.game, "Reader", R"(
        _G.door = workspace.Door == workspace:FindFirstChild("Door")
        _G.nested = workspace.Box.Inner.Name == "Inner"
        _G.parent = script.Parent.Box.Inner.Parent == workspace.Box
        _G.first = workspace.Box.Twin == workspace.Box:FindFirstChild("Twin")
        _G.property = workspace.Box.Name == "Box"
        _G.required = require(script.Parent.Mod) == 42
        local ok, message = pcall(function()
            return workspace.Box.Missing
        end)
        _G.missing_errors = not ok
        _G.missing_message = type(message) == "string"
            and string.find(message, "Missing is not a valid member of Folder \"Box\"", 1, true) ~= nil
        workspace.Box.Inner.Name = "Renamed"
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
    for (const char* name : {"Folder", "GameObject", "Camera", "PointLight", "SpotLight", "DirectionalLight", "Script",
                             "ModuleScript"}) {
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
        local box = Instance.new("Folder", workspace)
        _G.box_isa = box:IsA("Folder") and box:IsA("Instance") and box:IsA("DataModel") and not box:IsA("Game")
        _G.parent = box.Parent == workspace
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
    rig.game.set_parent(module.id(), workspace_of(rig.game));
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

namespace {

// One step as Engine runs it: PreAnimation delivers the frame's input, then Heartbeat.
void input_frame(ScriptRig& rig) {
    rig.scheduler.run_phase(engine_core::Phase::PreAnimation, 1.0 / 60.0);
    rig.game.events().drain();
    rig.frames(1);
}

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("UserInputService maps GLFW keys to Roblox KeyCode values", "[input]") {
    using engine_core::UserInputService;
    REQUIRE(UserInputService::key_code_from_glfw(87) == 119);   // W
    REQUIRE(UserInputService::key_code_from_glfw(65) == 97);    // A
    REQUIRE(UserInputService::key_code_from_glfw(32) == 32);    // Space
    REQUIRE(UserInputService::key_code_from_glfw(49) == 49);    // One
    REQUIRE(UserInputService::key_code_from_glfw(256) == 27);   // Escape
    REQUIRE(UserInputService::key_code_from_glfw(257) == 13);   // Return
    REQUIRE(UserInputService::key_code_from_glfw(265) == 273);  // Up
    REQUIRE(UserInputService::key_code_from_glfw(294) == 286);  // F5
    REQUIRE(UserInputService::key_code_from_glfw(340) == 304);  // LeftShift
    REQUIRE(UserInputService::key_code_from_glfw(320) == 256);  // KeypadZero
    REQUIRE(UserInputService::key_code_from_glfw(-1) == 0);
    REQUIRE(UserInputService::key_code_from_glfw(161) == 0);
}

TEST_CASE("UserInputService keeps posts only while active and ends held input", "[input]") {
    SimRole role;
    engine_core::EventQueue events;
    engine_core::UserInputService input;
    input.bind(events);

    // Inactive: dropped.
    input.post_key(119, true);
    input.dispatch(events);
    REQUIRE_FALSE(input.key_down(119));

    input.set_active(true);
    input.post_key(119, true);
    input.post_key(119, true);  // a repeat of a key already down
    input.post_key(97, false);  // a release of a key never pressed
    input.post_mouse_button(1, true, 4.f, 5.f);
    input.post_mouse_move(10.f, 20.f);
    input.post_mouse_move(15.f, 22.f);
    input.dispatch(events);
    REQUIRE(input.key_down(119));
    REQUIRE_FALSE(input.key_down(97));
    REQUIRE(input.keys_down() == std::vector<int>{119});
    REQUIRE(input.button_down(1));
    REQUIRE(input.mouse_location().x == 15.f);
    REQUIRE(input.mouse_location().y == 22.f);

    // Losing focus ends everything still down.
    input.post_focus_lost();
    input.dispatch(events);
    REQUIRE_FALSE(input.key_down(119));
    REQUIRE_FALSE(input.button_down(1));

    // Turning it off drops what was queued.
    input.post_key(120, true);
    input.set_active(false);
    input.dispatch(events);
    REQUIRE_FALSE(input.key_down(120));
    input.release(events);
}

TEST_CASE("UserInputService signals give scripts an InputObject", "[input]") {
    ScriptRig rig;
    engine_core::DataModel& model = rig.game;
    REQUIRE(engine_core::lua_service_known("UserInputService"));
    const std::string definitions = engine_core::lua_analysis_definitions();
    REQUIRE(definitions.find("declare extern type UserInputService with") != std::string::npos);
    REQUIRE(definitions.find("read InputBegan: Signal") != std::string::npos);
    REQUIRE(definitions.find("function IsKeyDown(self, keyCode: any): boolean") != std::string::npos);
    REQUIRE(definitions.find("function GetKeysPressed(self): {InputObject}") != std::string::npos);
    REQUIRE(definitions.find("declare extern type InputObject with") != std::string::npos);

    add_script(model, "Input", R"(
        local input = game:GetService("UserInputService")
        input.InputBegan:Connect(function(io, processed)
            print("began", io.KeyCode, io.UserInputType, io.UserInputState, processed,
                input:IsKeyDown(Enum.KeyCode.W), io.KeyCode == Enum.KeyCode.W)
            if io.KeyCode == Enum.KeyCode.E then
                local keys = input:GetKeysPressed()
                print("keys", #keys, keys[1].KeyCode.Name, keys[2].KeyCode.Name, input:IsKeyDown("E"))
            end
            if io.UserInputType == Enum.UserInputType.MouseButton1 then
                print("click", io.Position.X, io.Position.Y, input:IsMouseButtonPressed(Enum.UserInputType.MouseButton1),
                    #input:GetMouseButtonsPressed())
            end
        end)
        input.InputChanged:Connect(function(io)
            local location = input:GetMouseLocation()
            print("changed", io.UserInputType.Name, io.Position.X, io.Position.Y, io.Position.Z, io.Delta.X, io.Delta.Y,
                location.X, location.Y, typeof(location))
        end)
        input.InputEnded:Connect(function(io)
            print("ended", io.KeyCode.Name, io.UserInputType.Name, io.UserInputState.Name)
        end)
        task.spawn(function()
            local io, processed = input.InputBegan:Wait()
            print("waited", io.KeyCode.Name, processed)
        end)
        print("ready", input.KeyboardEnabled, input.MouseEnabled, input.TouchEnabled,
            (pcall(function() return input:IsKeyDown(Enum.NormalId.Top) end)))
    )");
    // Input before Test is not the session's.
    model.input().post_key(119, true);
    rig.game.start_simulation();
    REQUIRE(model.input().active());
    rig.frames(1);
    INFO(rig.runtime.last_error());
    const engine_core::ScriptRuntime::OutputBatch ready = rig.runtime.drain_output();
    REQUIRE(has_line(ready, "ready\ttrue\ttrue\tfalse\tfalse\n"));

    model.input().post_key(119, true);
    input_frame(rig);
    const engine_core::ScriptRuntime::OutputBatch began = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(began,
                     "began\tEnum.KeyCode.W\tEnum.UserInputType.Keyboard\tEnum.UserInputState.Begin\tfalse\ttrue\ttrue\n"));
    REQUIRE(has_line(began, "waited\tW\tfalse\n"));

    model.input().post_key(101, true);
    input_frame(rig);
    REQUIRE(has_line(rig.runtime.drain_output(), "keys\t2\tW\tE\ttrue\n"));

    model.input().post_mouse_move(10.f, 20.f);
    model.input().post_mouse_move(15.f, 22.f);
    input_frame(rig);
    const engine_core::ScriptRuntime::OutputBatch moved = rig.runtime.drain_output();
    REQUIRE(moved.lines.size() == 1);
    // The session's first move has nothing before it, so only the second one moves.
    REQUIRE(has_line(moved, "changed\tMouseMovement\t15\t22\t0\t5\t2\t15\t22\tVector2\n"));

    model.input().post_mouse_button(0, true, 15.f, 22.f);
    input_frame(rig);
    REQUIRE(has_line(rig.runtime.drain_output(), "click\t15\t22\ttrue\t1\n"));

    model.input().post_wheel(15.f, 22.f, -1.f);
    input_frame(rig);
    REQUIRE(has_line(rig.runtime.drain_output(), "changed\tMouseWheel\t15\t22\t-1\t0\t0\t15\t22\tVector2\n"));

    model.input().post_key(119, false);
    model.input().post_focus_lost();
    input_frame(rig);
    const engine_core::ScriptRuntime::OutputBatch ended = rig.runtime.drain_output();
    REQUIRE(has_line(ended, "ended\tW\tKeyboard\tEnd\n"));
    REQUIRE(has_line(ended, "ended\tE\tKeyboard\tEnd\n"));
    REQUIRE(has_line(ended, "ended\tUnknown\tMouseButton1\tEnd\n"));
    REQUIRE(model.input().keys_down().empty());

    // Stop keeps the service active, for the plugins, but forgets what it held.
    model.input().post_key(119, true);
    rig.game.stop_simulation();
    REQUIRE(model.input().active());
    REQUIRE_FALSE(model.input().key_down(119));
}

TEST_CASE("UserInputService keeps a release when the queue is full", "[input]") {
    SimRole role;
    engine_core::EventQueue events;
    engine_core::UserInputService input;
    input.set_active(true);
    input.post_key(119, true);
    // A paused test never dispatches. Movement fills the queue.
    for (int i = 0; i < 2000; ++i) {
        input.post_mouse_button(1, i % 2 == 0, 0.f, 0.f);
    }
    input.post_key(97, true);  // dropped: the queue is full
    input.post_key(119, false);
    input.dispatch(events);
    REQUIRE_FALSE(input.key_down(119));
    REQUIRE_FALSE(input.key_down(97));
    REQUIRE_FALSE(input.button_down(1));
}

TEST_CASE("S36 a finished listener's thread is released", "[S36]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        local seen = setmetatable({}, {__mode = "k"})
        local fires = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            seen[coroutine.running()] = true
            fires += 1
        end)
        while fires < 200 do
            task.wait()
        end
        -- Enough garbage that the collector finishes several cycles.
        for _ = 1, 400 do
            local junk = string.rep("x", 65536)
        end
        local kept = 0
        for _ in pairs(seen) do
            kept += 1
        end
        _G.kept = kept
    )");
    rig.game.start_simulation();
    rig.frames(260);
    double kept = -1;
    REQUIRE(rig.runtime.global_number("kept", kept));
    // Each fire ran on its own thread and finished. Only one still running could stay.
    REQUIRE(kept < 10);
}

TEST_CASE("S37 Disconnect releases the callback", "[S37]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        local held = setmetatable({}, {__mode = "k"})
        local heartbeat = game:GetService("RunService").Heartbeat
        for _ = 1, 200 do
            local calls = 0
            local callback = function()
                calls += 1
            end
            held[callback] = true
            heartbeat:Connect(callback):Disconnect()
        end
        for _ = 1, 400 do
            local junk = string.rep("x", 65536)
        end
        local kept = 0
        for _ in pairs(held) do
            kept += 1
        end
        _G.kept_callbacks = kept
    )");
    rig.game.start_simulation();
    rig.frames(2);
    double kept = -1;
    REQUIRE(rig.runtime.global_number("kept_callbacks", kept));
    REQUIRE(kept < 10);
}

TEST_CASE("S38 running out of Lua memory outside a script stops the scripts instead of throwing", "[S38]") {
    ScriptRig rig;
    engine_core::Script& hoard = add_script(rig.game, "Hoard", R"(
        _G.hoard = false
        _G.used = 0
        _G.full = false
        -- Luau interns strings, so each piece gets a count to make it a new one.
        local count = 0
        local function grow(size)
            while true do
                count += 1
                _G.hoard = {_G.hoard, string.rep("x", size) .. count}
            end
        end
        -- Big pieces first, so the loops fit in one step's budget, then smaller ones for the rest.
        for _, size in ipairs({1048576, 65536, 4096, 256, 0}) do
            pcall(grow, size)
        end
        _G.used = gcinfo()
        _G.full = true
        while true do
            task.wait(1)
        end
    )");
    rig.game.start_simulation();
    rig.frames(3);
    bool full = false;
    double used = 0;
    INFO("last error: " << rig.runtime.last_error());
    REQUIRE(rig.runtime.global_boolean("full", full));
    REQUIRE(full);
    REQUIRE(rig.runtime.global_number("used", used));
    REQUIRE(used > 65000);  // KB of the 64 MB the VM may use
    rig.runtime.drain_output();

    // Starting a script makes its thread from C++, outside any Lua call. Each of
    // these keeps its thread, so one of them finds the memory gone, even after the
    // collector frees the fill's temporary strings.
    std::vector<engine_core::Script*> late;
    for (int i = 0; i < 2000; ++i) {
        late.push_back(&add_script(rig.game, "Late", "while true do task.wait(1) end"));
    }
    REQUIRE_NOTHROW(rig.frames(3));
    bool reported = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : rig.runtime.drain_output().lines) {
        reported = reported || (line.kind == engine_core::ScriptRuntime::OutputKind::Error &&
                                line.text.rfind("Scripts stopped: ", 0) == 0 &&
                                line.text.find("memory") != std::string::npos);
    }
    REQUIRE(reported);
    REQUIRE_NOTHROW(rig.frames(3));

    // The next play session starts with a fresh VM, and scripts run again.
    rig.game.stop_simulation();
    hoard.set_enabled(false);
    for (engine_core::Script* script : late) {
        script->set_enabled(false);
    }
    add_script(rig.game, "Again", "_G.again = true");
    rig.game.start_simulation();
    rig.frames(2);
    bool again = false;
    REQUIRE(rig.runtime.global_boolean("again", again));
    REQUIRE(again);
}

TEST_CASE("T25 an exception on the simulation thread is reported, and stepping goes on", "[T25]") {
    engine_core::Engine engine;
    engine.start();
    engine.resume();
    engine.on_simulation([](engine_core::DataModel&) { throw std::runtime_error("edit failed on purpose"); });
    const std::uint64_t frames = engine.sim_frame_count();
    wait_until([&] { return engine.sim_frame_count() > frames + 3; });
    bool reported = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : engine.scripts().output_since(0, 64).lines) {
        reported = reported || line.text.find("edit failed on purpose") != std::string::npos;
    }
    engine.stop();
    REQUIRE(reported);
}

TEST_CASE("E1 one drain delivers more events than the queue first held", "[E1]") {
    engine_core::Game game;
    game.history().set_enabled(false);
    int added = 0;
    game.child_added(workspace_of(game)).connect([&](engine_core::InstanceId, engine_core::Field) { ++added; });
    for (int i = 0; i < 10000; ++i) {
        game.set_parent(game.create().id(), workspace_of(game));
    }
    {
        SimRole role;
        game.events().drain();
    }
    REQUIRE(added == 10000);
}

TEST_CASE("E2 more worker writes than the command queue first held all apply", "[E2]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create_game_object();
    std::thread::id simulation;
    std::thread::id render;
    std::thread([&simulation] { simulation = std::this_thread::get_id(); }).join();
    std::thread([&render] { render = std::this_thread::get_id(); }).join();
    game.set_thread_ids(simulation, render);
    game.set_threads_running(true);
    // This thread is neither engine thread, so each write waits as a command.
    for (int i = 1; i <= 5000; ++i) {
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(i), 0.f, 0.f));
    }
    game.set_thread_ids(std::this_thread::get_id(), render);
    {
        SimRole role;
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Write);
        game.drain_commands();
    }
    game.set_threads_running(false);
    REQUIRE(near(part.transform(), engine_core::matrix4_translation(5000.f, 0.f, 0.f)));
}

TEST_CASE("S39 Color3.fromHSV reads a hue that is not finite as 0", "[S39]") {
    ScriptRig rig;
    add_script(rig.game, "Colors", R"(
        local red = Color3.new(1, 0, 0)
        _G.nan = Color3.fromHSV(0 / 0, 1, 1) == red
        _G.inf = Color3.fromHSV(math.huge, 1, 1) == red
        _G.ninf = Color3.fromHSV(-math.huge, 1, 1) == red
    )");
    rig.game.start_simulation();
    rig.frames(1);
    for (const char* name : {"nan", "inf", "ninf"}) {
        bool red = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, red));
        REQUIRE(red);
    }
}

TEST_CASE("T26 a renderer that throws is reported, and frames go on", "[T26]") {
    struct Throwing : CountingRenderer {
        void perform(const engine_core::VisualSnapshot&) override { throw std::runtime_error("render failed on purpose"); }
    } renderer;
    engine_core::Engine engine;
    engine.set_renderer(&renderer);
    engine.start();
    engine.resume();
    const std::uint64_t presents = engine.present_count();
    wait_until([&] { return engine.present_count() > presents + 3; });
    bool reported = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : engine.scripts().output_since(0, 64).lines) {
        reported = reported || line.text.find("render failed on purpose") != std::string::npos;
    }
    engine.stop();
    REQUIRE(reported);
}

TEST_CASE("T27 an edit that throws does not drop the edits queued after it", "[T27]") {
    engine_core::Engine engine;
    std::atomic<bool> hold{true};
    std::atomic<bool> held{false};
    // Holding a step open lets both edits below land in the same batch.
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        held.store(true);
        while (hold.load()) {
            std::this_thread::yield();
        }
    });
    engine.start();
    engine.resume();
    wait_until([&] { return held.load(); });
    std::atomic<bool> second{false};
    engine.on_simulation([](engine_core::DataModel&) { throw std::runtime_error("first edit failed on purpose"); });
    engine.on_simulation([&second](engine_core::DataModel&) { second.store(true); });
    hold.store(false);
    wait_until([&] { return second.load(); });
    engine.stop();
}

TEST_CASE("E3 a handler that throws leaves the queue as it was", "[E3]") {
    SimRole role;
    engine_core::EventQueue events;
    engine_core::Signal signal;
    events.host_signal(&signal);
    signal.connect([](engine_core::InstanceId, engine_core::Field) { throw std::runtime_error("handler failed"); });
    events.emit(signal.id(), 0, engine_core::Field::Reflected, engine_core::WriteOrigin::Simulation, 7);
    REQUIRE_THROWS_AS(events.drain(), std::runtime_error);
    REQUIRE(events.payload() == 0);
    engine_core::LuaSlot value;
    value.kind = engine_core::LuaSlot::Kind::Number;
    events.emit_args(signal.id(), 0, {value});
    REQUIRE_THROWS_AS(events.drain(), std::runtime_error);
    REQUIRE(events.current_args() == nullptr);
    events.release_signal(signal);
}

TEST_CASE("S40 cancelling a task that finished long ago does nothing", "[S40]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        local finished = task.spawn(function() end)
        task.wait()
        task.wait()
        task.cancel(finished)
        _G.after = true
    )");
    rig.game.start_simulation();
    rig.frames(4);
    bool after = false;
    REQUIRE(rig.runtime.global_boolean("after", after));
    REQUIRE(after);
}

TEST_CASE("S41 a cancelled Wait does not resume when its signal fires later", "[S41]") {
    ScriptRig rig;
    add_script(rig.game, "Main", R"(
        local watched = Instance.new("Folder")
        watched.Parent = workspace
        local waiter = task.spawn(function()
            watched.Changed:Wait()
            _G.woke = true
        end)
        task.wait()
        task.cancel(waiter)
        task.wait()
        watched.Name = "Renamed"
        task.wait()
        task.wait()
        _G.done = true
    )");
    rig.game.start_simulation();
    rig.frames(6);
    bool done = false;
    REQUIRE(rig.runtime.global_boolean("done", done));
    REQUIRE(done);
    REQUIRE(rig.runtime.global_is_nil("woke"));
}

TEST_CASE("L1 a thread that holds one world's lock still locks another", "[L1]") {
    engine_core::Game first;
    engine_core::Game second;
    engine_core::DataModelLock outer(first, engine_core::DataModelLock::Write);
    engine_core::DataModelLock inner(second, engine_core::DataModelLock::Write);
    bool other_got_it = true;
    std::thread other([&] {
        engine_core::DataModelLock attempt(second, engine_core::DataModelLock::Write, std::chrono::milliseconds(20));
        other_got_it = attempt.owns();
    });
    other.join();
    REQUIRE_FALSE(other_got_it);
}

TEST_CASE("L2 the same thread takes one world's lock again without blocking", "[L2]") {
    engine_core::Game game;
    engine_core::DataModelLock outer(game, engine_core::DataModelLock::Write);
    engine_core::DataModelLock inner(game, engine_core::DataModelLock::Write, std::chrono::milliseconds(20));
    REQUIRE(inner.owns());
    REQUIRE(game.write_depth() == 2);
}

namespace {

// The service on its own world, stepped by hand: posts, then one dispatch.
struct InputRig {
    SimRole role;
    engine_core::Game game;
    engine_core::UserInputService& input = game.input();
    std::vector<engine_core::InputRecord> seen;
    std::vector<engine_core::Connection> connections;

    InputRig() {
        input.bind(game.events());
        for (auto kind : {engine_core::UserInputService::Kind::Began, engine_core::UserInputService::Kind::Changed,
                          engine_core::UserInputService::Kind::Ended}) {
            connections.push_back(input.signal(kind)->connect([this](engine_core::InstanceId, engine_core::Field) {
                const engine_core::EventArgs* args = game.events().current_args();
                if (args != nullptr && !args->empty() && (*args)[0].kind == engine_core::LuaSlot::Kind::InputObject) {
                    seen.push_back((*args)[0].input);
                }
            }));
        }
        input.set_active(true);
    }

    void step() {
        input.dispatch(game.events());
        game.events().drain();
    }
};

}  // namespace

TEST_CASE("U1 a session's first mouse move has no delta from before it", "[U1][input]") {
    InputRig rig;
    rig.input.post_mouse_move(100.f, 100.f);
    rig.step();
    REQUIRE(rig.seen.size() == 1);
    REQUIRE(rig.seen[0].delta.x == 0.f);
    REQUIRE(rig.seen[0].delta.y == 0.f);
    rig.input.post_mouse_move(103.f, 104.f);
    rig.step();
    REQUIRE(rig.seen.size() == 2);
    REQUIRE(rig.seen[1].delta.x == 3.f);
    REQUIRE(rig.seen[1].delta.y == 4.f);

    rig.input.set_active(false);
    rig.input.reset();
    rig.input.set_active(true);
    rig.input.post_mouse_move(10.f, 10.f);
    rig.step();
    REQUIRE(rig.seen.size() == 3);
    REQUIRE(rig.seen[2].position.x == 10.f);
    REQUIRE(rig.seen[2].delta.x == 0.f);
    REQUIRE(rig.seen[2].delta.y == 0.f);
}

TEST_CASE("U2 keys begin once, end once, and read as held between", "[U2][input]") {
    InputRig rig;
    const int w = 119;
    rig.input.post_key(w, true);
    rig.input.post_key(w, true);
    rig.step();
    REQUIRE(rig.seen.size() == 1);
    REQUIRE(rig.seen[0].state == engine_core::UserInputService::kBegin);
    REQUIRE(rig.input.key_down(w));
    rig.input.post_key(w, false);
    rig.step();
    REQUIRE(rig.seen.size() == 2);
    REQUIRE(rig.seen[1].state == engine_core::UserInputService::kEnd);
    REQUIRE_FALSE(rig.input.key_down(w));
}

TEST_CASE("U3 losing focus ends what is held, and an inactive service keeps nothing", "[U3][input]") {
    InputRig rig;
    rig.input.post_key(119, true);
    rig.input.post_mouse_button(0, true, 5.f, 5.f);
    rig.step();
    REQUIRE(rig.input.key_down(119));
    REQUIRE(rig.input.button_down(0));
    rig.input.post_focus_lost();
    rig.step();
    REQUIRE_FALSE(rig.input.key_down(119));
    REQUIRE_FALSE(rig.input.button_down(0));

    const std::size_t before = rig.seen.size();
    rig.input.set_active(false);
    rig.input.post_key(119, true);
    rig.step();
    REQUIRE(rig.seen.size() == before);
}

TEST_CASE("U4 moves between steps arrive as one change with the deltas added", "[U4][input]") {
    InputRig rig;
    rig.input.post_mouse_move(1.f, 1.f);
    rig.step();
    rig.input.post_mouse_move(2.f, 3.f);
    rig.input.post_mouse_move(4.f, 4.f);
    rig.step();
    REQUIRE(rig.seen.size() == 2);
    REQUIRE(rig.seen[1].position.x == 4.f);
    REQUIRE(rig.seen[1].delta.x == 3.f);
    REQUIRE(rig.seen[1].delta.y == 3.f);
}

// A script runs only under Workspace, Scripts, or Gui. Out of the tree, it stops at
// its next yield.
TEST_CASE("S42 a script that takes itself out of the tree stops", "[S42]") {
    ScriptRig rig;
    add_script(rig.game, "Hider", R"(
        _G.count = 0
        script.Parent = nil
        while true do
            _G.count += 1
            task.wait(0.05)
        end
    )");
    rig.game.start_simulation();
    rig.frames(4, 0.05);
    INFO(rig.runtime.last_error());
    double count = 0;
    REQUIRE(rig.runtime.global_number("count", count));
    REQUIRE(count == 1);
}

TEST_CASE("S43 moving a running script does not run it again", "[S43]") {
    ScriptRig rig;
    add_script(rig.game, "Mover", R"(
        _G.runs = (_G.runs or 0) + 1
        local folder = Instance.new("Folder")
        folder.Parent = workspace
        script.Parent = folder
        _G.after = true
    )");
    rig.game.start_simulation();
    rig.frames(4, 0.05);
    INFO(rig.runtime.last_error());
    double runs = 0;
    REQUIRE(rig.runtime.global_number("runs", runs));
    REQUIRE(runs == 1);
    bool after = false;
    REQUIRE(rig.runtime.global_boolean("after", after));
    REQUIRE(after);
}

TEST_CASE("T20 an unbound job stops running", "[T20]") {
    SimRole role;
    engine_core::TaskScheduler scheduler;
    scheduler.reserve(4);
    int kept = 0;
    int dropped = 0;
    scheduler.bind(engine_core::Phase::Heartbeat, [&kept](double) { ++kept; });
    const engine_core::TaskScheduler::JobId id =
        scheduler.bind(engine_core::Phase::Heartbeat, [&dropped](double) { ++dropped; });
    scheduler.run_phase(engine_core::Phase::Heartbeat, 1.0 / 60.0);
    scheduler.unbind(id);
    scheduler.run_phase(engine_core::Phase::Heartbeat, 1.0 / 60.0);
    scheduler.cancel_session_jobs();
    scheduler.run_phase(engine_core::Phase::Heartbeat, 1.0 / 60.0);
    REQUIRE(kept == 3);
    REQUIRE(dropped == 1);
}

TEST_CASE("S44 a runtime attached again fires each phase once", "[S44]") {
    ScriptRig rig;
    rig.runtime.detach();
    rig.runtime.attach(rig.game, rig.scheduler);
    add_script(rig.game, "Counter", R"(
        _G.beats = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            _G.beats += 1
        end)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    double before = 0;
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.global_number("beats", before));
    rig.frames(3);
    double after = 0;
    REQUIRE(rig.runtime.global_number("beats", after));
    REQUIRE(after - before == 3);
}

TEST_CASE("R1 reflection answers on two threads at once", "[R1][reflect]") {
    std::atomic<int> wrong{0};
    auto work = [&] {
        for (int i = 0; i < 300; ++i) {
            std::vector<engine_core::LuaSymbol> math;
            if (!engine_core::lua_library_members("math", math) || math.size() < 10) {
                ++wrong;
            }
            std::vector<engine_core::LuaSymbol> text;
            if (!engine_core::lua_value_members("string", text) || text.size() < 10) {
                ++wrong;
            }
        }
    };
    std::thread other(work);
    work();
    other.join();
    REQUIRE(wrong.load() == 0);
}

TEST_CASE("S45 a long loop that ends is not stopped as a runaway", "[S45]") {
    ScriptRig rig;
    add_script(rig.game, "Grid", R"(
        local cells = 0
        for x = 1, 450 do
            for y = 1, 450 do
                cells += 1
            end
        end
        _G.cells = cells
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    double cells = 0;
    REQUIRE(rig.runtime.global_number("cells", cells));
    REQUIRE(cells == 450 * 450);
}

TEST_CASE("W1 children keep their order through removals at either end", "[W1]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Folder& parent = game.create<engine_core::Folder>();
    game.set_parent(parent.id(), workspace_of(game));
    engine_core::Folder& away = game.create<engine_core::Folder>();
    game.set_parent(away.id(), workspace_of(game));
    auto child = [&](const char* name) {
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), name);
        game.set_parent(folder.id(), parent.id());
        return folder.id();
    };
    auto names = [&](engine_core::InstanceId under) {
        std::string out;
        for (engine_core::InstanceId id = game.first_child(under); id != 0; id = game.next_sibling(id)) {
            out += game.name(id);
        }
        return out;
    };
    child("A");
    const engine_core::InstanceId b = child("B");
    const engine_core::InstanceId c = child("C");
    REQUIRE(names(parent.id()) == "ABC");
    game.set_parent(c, away.id());
    child("D");
    REQUIRE(names(parent.id()) == "ABD");
    game.set_parent(game.first_child(parent.id()), away.id());
    child("E");
    REQUIRE(names(parent.id()) == "BDE");
    game.destroy(game.next_sibling(b));
    child("F");
    REQUIRE(names(parent.id()) == "BEF");
    game.set_parent(b, away.id());
    REQUIRE(names(away.id()) == "CAB");
    while (game.first_child(parent.id()) != 0) {
        game.set_parent(game.first_child(parent.id()), away.id());
    }
    child("G");
    REQUIRE(names(parent.id()) == "G");
    REQUIRE(names(away.id()) == "CABEF");
    // The root's children keep their order the same way.
    game.set_parent(parent.id(), away.id());
    engine_core::Folder& last = game.create<engine_core::Folder>();
    game.set_parent(last.id(), workspace_of(game));
    REQUIRE(game.next_sibling(away.id()) == last.id());
}

TEST_CASE("K1 every KeyCode but Unknown comes from exactly one GLFW key", "[K1][input]") {
    const engine_core::EnumType& codes = engine_core::key_code_enum();
    std::vector<int> from(512, 0);
    for (int glfw = -1; glfw <= 400; ++glfw) {
        const int code = engine_core::UserInputService::key_code_from_glfw(glfw);
        if (code == 0) {
            continue;
        }
        INFO("GLFW key " << glfw << " gives " << code);
        REQUIRE(engine_core::enum_item_name(codes, code) != nullptr);
        REQUIRE(code < static_cast<int>(from.size()));
        ++from[static_cast<std::size_t>(code)];
    }
    for (int index = 0; index < codes.count; ++index) {
        const engine_core::EnumEntry& item = codes.items[index];
        if (item.value == 0) {
            continue;
        }
        INFO(item.name);
        REQUIRE(from[static_cast<std::size_t>(item.value)] == 1);
    }
}

TEST_CASE("V1 Vector3 Max and Min take any number of vectors, as Vector2's do", "[V1]") {
    ScriptRig rig;
    add_script(rig.game, "Extremes", R"(
        local high = Vector3.new(1, 5, 3):Max(Vector3.new(2, 1, 1), Vector3.new(0, 0, 9))
        local low = Vector3.new(1, 5, 3):Min(Vector3.new(2, 1, 1), Vector3.new(0, 0, 9))
        _G.high = high.X * 100 + high.Y * 10 + high.Z
        _G.low = low.X * 100 + low.Y * 10 + low.Z
        _G.one = Vector3.new(1, 2, 3):Max(Vector3.new(3, 2, 1)).X
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    double high = 0;
    double low = 0;
    double one = 0;
    REQUIRE(rig.runtime.global_number("high", high));
    REQUIRE(rig.runtime.global_number("low", low));
    REQUIRE(rig.runtime.global_number("one", one));
    REQUIRE(high == 259);
    REQUIRE(low == 1);
    REQUIRE(one == 3);
}

TEST_CASE("W2 the tree revision moves on names and the hierarchy, and not on properties", "[W2]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    std::uint64_t last = game.tree_revision();
    auto moved = [&] {
        const std::uint64_t now = game.tree_revision();
        const bool changed = now != last;
        last = now;
        return changed;
    };
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    REQUIRE_FALSE(moved());
    game.set_parent(part.id(), workspace_of(game));
    REQUIRE(moved());
    game.set_name(part.id(), "Brick");
    REQUIRE(moved());
    part.set_transform(T0());
    REQUIRE_FALSE(moved());
    game.set_name(0, "Place");
    REQUIRE(moved());
    // During play too.
    game.start_simulation();
    REQUIRE_FALSE(moved());
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), workspace_of(game));
    REQUIRE(moved());
    game.destroy(folder.id());
    REQUIRE(moved());
    game.stop_simulation();
    REQUIRE(moved());
}

namespace {

// The studio installs no contract handler: a contract failure there aborts.
// These tests run as the studio does, and put the sandbox's handler back after.
struct StudioContracts {
    StudioContracts() { engine_core::set_contract_handler(nullptr); }
    ~StudioContracts() {
        engine_core::set_contract_handler([](const char* message) { throw engine_core::ContractViolation(message); });
    }
};

}  // namespace

TEST_CASE("S47 a full place is a script error in the studio, not an abort", "[S47]") {
    StudioContracts studio;
    ScriptRig rig;
    add_script(rig.game, "Flood", R"(
        local ok, message = pcall(function()
            for _ = 1, 20000 do
                Instance.new("Folder")
            end
        end)
        _G.stopped = not ok
        _G.said_full = string.find(tostring(message), "full", 1, true) ~= nil
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    bool stopped = false;
    REQUIRE(rig.runtime.global_boolean("stopped", stopped));
    REQUIRE(stopped);
    bool said_full = false;
    REQUIRE(rig.runtime.global_boolean("said_full", said_full));
    REQUIRE(said_full);
    rig.game.stop_simulation();
}

TEST_CASE("S48 creating past the instance cap throws and changes nothing", "[S48]") {
    StudioContracts studio;
    SimRole role;
    engine_core::Game game;
    std::vector<engine_core::InstanceId> made;
    while (game.room_left() > 0) {
        made.push_back(game.create<engine_core::Folder>().id());
    }
    const std::uint64_t tree = game.tree_revision();
    REQUIRE_THROWS_AS(game.create<engine_core::Folder>(), engine_core::InstanceCapacityError);
    REQUIRE_THROWS_AS(game.create<engine_core::Script>(), engine_core::InstanceCapacityError);
    REQUIRE(game.room_left() == 0);
    REQUIRE(game.tree_revision() == tree);
    // Room made again is room to create in.
    game.destroy(made.back());
    REQUIRE(game.room_left() == 1);
    REQUIRE_NOTHROW(game.create<engine_core::Script>());
}

TEST_CASE("S49 a change watch hears its instances' changes and no others", "[S49]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& watched = game.create_game_object();
    engine_core::GameObject& other = game.create_game_object();
    game.set_parent(watched.id(), workspace_of(game));
    game.set_parent(other.id(), workspace_of(game));
    int heard = 0;
    const std::uint64_t watch = game.watch_changes([&heard] { ++heard; });
    game.set_watched(watch, {watched.id()});
    // A watch with nothing in it hears nothing, not even Stop.
    int idle_heard = 0;
    const std::uint64_t idle = game.watch_changes([&idle_heard] { ++idle_heard; });

    game.set_name(other.id(), "Other");
    other.set_transform(T0());
    REQUIRE(heard == 0);

    game.set_name(watched.id(), "Watched");
    REQUIRE(heard == 1);
    watched.set_transform(T1());
    REQUIRE(heard == 2);

    // Physics moves bodies without their setters.
    game.start_simulation();
    game.set_simulated(watched.id(), true);
    const int before_step = heard;
    watched.set_linear_velocity(1.f, 0.f, 0.f);
    const int before_move = heard;
    game.integrate_simulated(1.0 / 60.0);
    REQUIRE(heard > before_move);
    REQUIRE(before_step <= before_move);

    // Stop restores the place without setters, so every watcher hears it.
    const int before_stop = heard;
    game.stop_simulation();
    REQUIRE(heard > before_stop);

    // Destroying a watched instance is a change too.
    const int before_destroy = heard;
    game.destroy(watched.id());
    REQUIRE(heard > before_destroy);

    REQUIRE(idle_heard == 0);
    game.unwatch_changes(idle);

    game.unwatch_changes(watch);
    const int after = heard;
    game.set_name(other.id(), "Again");
    REQUIRE(heard == after);
}

TEST_CASE("S50 undo and redo reach a change watch", "[S50]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = game.create_game_object();
    game.set_parent(part.id(), workspace_of(game));
    game.capture_place();
    int heard = 0;
    const std::uint64_t watch = game.watch_changes([&heard] { ++heard; });
    game.set_watched(watch, {part.id()});
    begin_step(game, "Move");
    part.set_transform(T0());
    end_step(game);
    const int edited = heard;
    REQUIRE(edited > 0);
    REQUIRE(game.history().can_undo().first);
    game.history().undo();
    REQUIRE(heard > edited);
    game.unwatch_changes(watch);
}

TEST_CASE("S46 Instance.new past the instance cap is a script error, not an abort", "[S46]") {
    ScriptRig rig;
    add_script(rig.game, "Flood", R"(
        _G.made = 0
        local ok, message = pcall(function()
            for _ = 1, 20000 do
                Instance.new("Folder")
                _G.made += 1
            end
        end)
        _G.stopped = not ok
        _G.message = tostring(message)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    bool stopped = false;
    REQUIRE(rig.runtime.global_boolean("stopped", stopped));
    REQUIRE(stopped);
    double made = 0;
    REQUIRE(rig.runtime.global_number("made", made));
    REQUIRE(made > 16000);
    REQUIRE(made < 16384);
    rig.game.stop_simulation();
}

namespace {

// One play step as the engine runs it: PreAnimation and its drain, then Heartbeat.
void play_step(ScriptRig& rig, double dt) {
    rig.scheduler.run_phase(engine_core::Phase::PreAnimation, dt);
    rig.game.events().drain();
    rig.frames(1, dt);
}

}  // namespace

TEST_CASE("S51 a play RenderStepped handler runs in the window, once per frame, with the frame's dt",
          "[S51]") {
    ScriptRig rig;
    add_script(rig.game, "Watch", R"(
        game:GetService("RunService").RenderStepped:Connect(function(dt)
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
            print("from the window")
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    REQUIRE(rig.runtime.global_is_nil("n"));  // no frame drawn yet

    rig.render(0.004);
    double n = 0;
    double dt = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);  // delivered in the window, before any sim step
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.004) < 1e-9);

    rig.render(0.008);
    rig.render(0.016);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 3);  // once per frame, not summed into a step
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.016) < 1e-9);

    play_step(rig, 0.05);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 3);  // the step adds nothing
    REQUIRE(rig.runtime.last_error().empty());
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    int prints = 0;
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        REQUIRE(line.kind != engine_core::ScriptRuntime::OutputKind::Error);
        if (line.text.find("from the window") != std::string::npos) {
            ++prints;
        }
    }
    REQUIRE(prints == 3);  // append_output survived the render thread
}

TEST_CASE("RW3 a handler that errors reports every frame and stays connected", "[RW3]") {
    ScriptRig rig;
    add_script(rig.game, "Bad", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.n = (_G.n or 0) + 1
            error("window boom")
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    rig.render(0.016);
    double n = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 2);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    int errors = 0;
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error &&
            line.text.find("window boom") != std::string::npos) {
            ++errors;
        }
    }
    REQUIRE(errors == 2);
}

TEST_CASE("S52 RenderStepped:Wait resumes in the window with the frame's dt", "[S52]") {
    ScriptRig rig;
    add_script(rig.game, "Waiter", R"(
        while true do
            local dt = game:GetService("RunService").RenderStepped:Wait()
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
        end
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    play_step(rig, 0.05);
    REQUIRE(rig.runtime.global_is_nil("n"));
    rig.render(0.004);
    rig.render(0.008);
    double n = 0;
    double dt = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 2);  // one resume per frame, no sim step between
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.008) < 1e-9);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("RW4 task.wait in a window handler resumes the continuation on the sim side", "[RW4]") {
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Mark");
    // A listener, so each Transform write is counted by its origin.
    rig.game.changed(part.id()).connect([](engine_core::InstanceId, engine_core::Field) {});
    add_script(rig.game, "Yielder", R"(
        local part = workspace:FindFirstChild("Mark")
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.before = (_G.before or 0) + 1
            part.Transform = Matrix4.new(1, 0, 0)
            task.wait(0.01)
            _G.after = (_G.after or 0) + 1
            part.Transform = Matrix4.new(2, 0, 0)
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    const std::uint64_t window_writes = rig.game.events().count(engine_core::WriteOrigin::PreRenderDataModel);
    rig.render(0.016);
    double before = 0;
    REQUIRE(rig.runtime.global_number("before", before));
    REQUIRE(before == 1);
    REQUIRE(rig.runtime.global_is_nil("after"));  // parked, not run in the window
    // The write before the yield is a window write.
    REQUIRE(rig.game.events().count(engine_core::WriteOrigin::PreRenderDataModel) == window_writes + 1);
    const std::uint64_t sim_writes = rig.game.events().count(engine_core::WriteOrigin::Simulation);
    play_step(rig, 0.05);  // the step wakes the sleep
    double after = 0;
    REQUIRE(rig.runtime.global_number("after", after));
    REQUIRE(after == 1);
    // The continuation's write is an ordinary sim write: path A, not path B.
    REQUIRE(rig.game.events().count(engine_core::WriteOrigin::PreRenderDataModel) == window_writes + 1);
    REQUIRE(rig.game.events().count(engine_core::WriteOrigin::Simulation) == sim_writes + 1);
    REQUIRE(near(part.transform(), engine_core::matrix4_translation(2.f, 0.f, 0.f)));
    REQUIRE(rig.render_violations == 0);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("RW5 a continuation parked at Stop never resumes into the restored world", "[RW5]") {
    ScriptRig rig;
    add_script(rig.game, "Yielder", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            task.wait(0.01)
            _G.leaked = true
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    rig.game.stop_simulation();
    rig.frames(4, 0.05);
    rig.render(0.016);
    REQUIRE(rig.runtime.global_is_nil("leaked"));
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("RW6 a window handler's visual write lands in that frame's snapshot", "[RW6]") {
    // T21 (sandbox/tests.cpp, the Engine-driven "RenderStepped writes this frame
    // and PostRender does not" case) is the model for this path: a render-step
    // write needs visual_only (or ForceSimWrite) to pass authorize, and the
    // write lock must be held across begin/end-window and take_changes, exactly
    // as Engine::render_once holds it (Engine.cpp around line 400), for the
    // snapshot to pick up the change in the same take_changes call. ScriptRig's
    // plain rig.render() does not hold that lock or drive a pump, so this test
    // builds the window by hand instead of reusing rig.render().
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Mover");
    rig.game.set_visual_only(part.id(), true);
    add_script(rig.game, "Push", R"(
        local part = workspace:FindFirstChild("Mover")
        game:GetService("RunService").RenderStepped:Connect(function()
            part.Transform = Matrix4.new(5, 0, 0)
        end)
    )");
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    rig.game.start_simulation();
    play_step(rig, 0.05);
    std::thread render([&] {
        engine_core::set_thread_role(engine_core::ThreadRole::Render);
        rig.game.set_thread_ids(std::thread::id(), std::this_thread::get_id());
        rig.game.set_threads_running(true);
        {
            engine_core::DataModelLock lock(rig.game, engine_core::DataModelLock::Write);
            pump.begin_prerender_window(rig.game);
            rig.scheduler.run_phase(engine_core::Phase::RenderStepped, 0.016);
            pump.end_prerender_window(rig.game);
            pump.take_changes(rig.game);
        }
        rig.game.set_threads_running(false);
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    });
    render.join();
    pump.finish_copy();
    pump.publish();
    const engine_core::VisualInstance* vis = pump.find(part.id());
    REQUIRE(vis != nullptr);
    REQUIRE(std::fabs(vis->world.m[12] - 5.0f) < 1e-4f);  // this frame, not the next
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("RW9 a window script write authorizes as a sim write, untagged or not", "[RW9]") {
    // Live use (the SceneCamera plugin writing camera.Transform with threads
    // running) found what RW6/RW7's sandbox rigs could not: authorize's
    // render-thread branch required visual_only (or ForceSimWrite) for every
    // window write, including ones a script makes, so an ordinary untagged
    // part's Transform raised "cannot be written in a render step" for real.
    // The ruling (see the design doc's Writes row): a write a window SCRIPT
    // makes authorizes like a sim write, any instance, regardless of the
    // visual_only tag. Same harness as RW6 (pump-driven window, write lock held
    // across begin/end-window and take_changes) so the same-frame snapshot
    // claim is checked for real, not just the DataModel's own copy.
    ScriptRig rig;
    engine_core::GameObject& loose = create_part(rig.game);
    rig.game.set_name(loose.id(), "Loose");
    engine_core::GameObject& loose2 = create_part(rig.game);
    rig.game.set_name(loose2.id(), "Loose2");
    // Neither part is tagged visual_only: before this fix, authorize's
    // render-thread branch would refuse both.
    add_script(rig.game, "Push", R"(
        local a = workspace:FindFirstChild("Loose")
        local b = workspace:FindFirstChild("Loose2")
        game:GetService("RunService").RenderStepped:Connect(function()
            a.Transform = Matrix4.new(5, 0, 0)
            -- A second untagged instance: authorize's window-script bypass is
            -- not a one-instance exception, it is the render-thread branch's
            -- own rule while a script runs.
            b.Transform = Matrix4.new(0, 7, 0)
        end)
    )");
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    rig.game.start_simulation();
    play_step(rig, 0.05);
    bool deferred_left = true;
    std::thread render([&] {
        engine_core::set_thread_role(engine_core::ThreadRole::Render);
        rig.game.set_thread_ids(std::thread::id(), std::this_thread::get_id());
        rig.game.set_threads_running(true);
        {
            engine_core::DataModelLock lock(rig.game, engine_core::DataModelLock::Write);
            pump.begin_prerender_window(rig.game);
            rig.scheduler.run_phase(engine_core::Phase::RenderStepped, 0.016);
            pump.end_prerender_window(rig.game);
            deferred_left = rig.game.has_deferred_violation();
            pump.take_changes(rig.game);
        }
        rig.game.set_threads_running(false);
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    });
    render.join();
    pump.finish_copy();
    pump.publish();

    // No Lua error: pcall was never needed because authorize let both through.
    REQUIRE(rig.runtime.last_error().empty());
    // No refusal left for the engine to count.
    REQUIRE_FALSE(deferred_left);

    // The DataModel itself shows the new transforms (print(part.Transform)
    // reads this, per the README's path table).
    const engine_core::Matrix4 expected_a = engine_core::matrix4_translation(5.f, 0.f, 0.f);
    const engine_core::Matrix4 expected_b = engine_core::matrix4_translation(0.f, 7.f, 0.f);
    REQUIRE(near(rig.game.game_object(loose.id())->transform(), expected_a));
    REQUIRE(near(rig.game.game_object(loose2.id())->transform(), expected_b));

    // And this frame's published snapshot shows them too: the same-frame
    // ForceSimWrite-style route, not a write that waits for the next Prepare.
    const engine_core::VisualInstance* vis_a = pump.find(loose.id());
    const engine_core::VisualInstance* vis_b = pump.find(loose2.id());
    REQUIRE(vis_a != nullptr);
    REQUIRE(vis_b != nullptr);
    REQUIRE(std::fabs(vis_a->world.m[12] - 5.0f) < 1e-4f);
    REQUIRE(std::fabs(vis_b->world.m[13] - 7.0f) < 1e-4f);
}

TEST_CASE("RW7 a C++ job's refusal in the window is counted with its reason, not blamed on a handler", "[RW7]") {
    // rig.render() runs the window as the engine does (threads running, write
    // lock held), so authorize's render-thread branch is the real one. A window
    // SCRIPT's write authorizes like a sim write (RW9), so no script write can be
    // refused here any more. A C++ RenderStepped job still can: it runs with
    // window_script false. Bound above the script runtime's job, it refuses
    // first and the violation is deferred. The handler's write after it must
    // not raise for that deferral (it was not the handler's), and the engine
    // must still count it, with authorize's reason kept.
    ScriptRig rig;
    engine_core::GameObject& part = create_part(rig.game);
    rig.game.set_name(part.id(), "Solid");
    engine_core::GameObject& other = create_part(rig.game);
    const engine_core::Matrix4 other_before = other.transform();
    add_script(rig.game, "Writer", R"(
        local part = workspace:FindFirstChild("Solid")
        game:GetService("RunService").RenderStepped:Connect(function()
            local ok, err = pcall(function()
                part.Transform = Matrix4.new(1, 2, 3)
            end)
            _G.ok = ok
            _G.err = tostring(err)
        end)
    )");
    rig.scheduler.bind(
        engine_core::Phase::RenderStepped,
        [&](double) { rig.game.game_object(other.id())->set_transform(engine_core::matrix4_translation(9.f, 9.f, 9.f)); },
        3000);
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    INFO(rig.runtime.last_error());
    REQUIRE(ok);  // the handler's own write went through
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(near(rig.game.game_object(part.id())->transform(), engine_core::matrix4_translation(1.f, 2.f, 3.f)));
    // The C++ job's write was refused before it changed anything, and counted.
    REQUIRE(near(rig.game.game_object(other.id())->transform(), other_before));
    REQUIRE(rig.render_violations == 1);
    // The reason the binding puts in a refused script write's Lua error
    // ("<key> cannot be written in a render step: <reason>").
    REQUIRE(rig.last_render_violation == "render-step DataModel write requires visual_only or ForceSimWrite");
    rig.game.stop_simulation();
}

TEST_CASE("RW11 a window handler creates, reparents, renames, destroys, and sets Velocity", "[RW11]") {
    // As the studio runs: no contract handler, so a SimulationThread guard that
    // refused the render thread would abort the run here, not fail a REQUIRE.
    StudioContracts studio;
    ScriptRig rig;
    const engine_core::InstanceId ws = workspace_of(rig.game);
    engine_core::Folder& doomed = rig.game.create<engine_core::Folder>();
    rig.game.set_name(doomed.id(), "Doomed");
    rig.game.set_parent(doomed.id(), ws);
    engine_core::Folder& mover = rig.game.create<engine_core::Folder>();
    rig.game.set_name(mover.id(), "Mover");
    rig.game.set_parent(mover.id(), ws);
    engine_core::PhysicsObject& body = rig.game.create<engine_core::PhysicsObject>();
    rig.game.set_name(body.id(), "Body");
    rig.game.set_parent(body.id(), ws);
    add_script(rig.game, "Ops", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            if _G.ran then return end
            _G.ran = true
            local made = Instance.new("Folder", workspace)
            made.Name = "Made"
            local mover = workspace:FindFirstChild("Mover")
            mover.Parent = made
            mover.Name = "Moved"
            workspace:FindFirstChild("Doomed"):Destroy()
            workspace:FindFirstChild("Body").Velocity = Vector3.new(1, 2, 3)
            _G.ok = true
        end)
    )");
    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
    REQUIRE(rig.render_violations == 0);

    // Visible afterward, to C++ and to the next sim step's scripts.
    const engine_core::InstanceId made = rig.game.find_first_child(ws, "Made");
    REQUIRE(made != 0);
    REQUIRE(rig.game.parent(mover.id()) == made);
    REQUIRE(rig.game.name(mover.id()) == "Moved");
    REQUIRE_FALSE(rig.game.alive(doomed.id()));
    REQUIRE(body.velocity().x == 1.f);
    REQUIRE(body.velocity().y == 2.f);
    REQUIRE(body.velocity().z == 3.f);
    rig.runtime.drain_output();
    rig.runtime.run_chunk(R"(
        local made = workspace:FindFirstChild("Made")
        print(string.format("%s %s", tostring(made ~= nil and made:FindFirstChild("Moved") ~= nil),
                            tostring(workspace:FindFirstChild("Doomed") == nil)))
    )");
    play_step(rig, 0.05);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].text.rfind("true true", 0) == 0);
    rig.game.stop_simulation();
}

TEST_CASE("RW11 a plugin's window handler makes an instance in edit mode", "[RW11]") {
    StudioContracts studio;
    ScriptRig rig;
    ide::PluginLoader loader;
    ide::PluginFile file;
    file.name = "Maker";
    file.source = R"(
        local made = false
        game:GetService("RunService").RenderStepped:Connect(function()
            if made then return end
            made = true
            local folder = Instance.new("Folder")
            folder.Name = "FromPlugin"
            folder.Parent = workspace
        end)
    )";
    REQUIRE(loader.load(rig.game, rig.runtime, {file}) == 1);
    rig.render(0.016);
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.render_violations == 0);
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "FromPlugin") != 0);
}

TEST_CASE("RW12 a refusal a window script reaches is a Lua error, with no contract handler", "[RW12]") {
    // Every operation a window handler can reach is admitted now (RW11), so the
    // refusal is made on purpose: a C++ RenderStepped job, which runs in the
    // window with window_script false and keeps the SimulationThread guards,
    // enters a VM itself, and the chunk asks for Instance.new. spawn's guard
    // refuses the render thread; with no handler installed, contract_fail would
    // abort. Inside the VM it throws instead, and pcall catches it.
    StudioContracts studio;
    ScriptRig rig;
    rig.runtime.drain_output();
    bool ran = false;
    rig.scheduler.bind(engine_core::Phase::RenderStepped, [&](double) {
        if (ran) {
            return;
        }
        ran = true;
        rig.runtime.run_chunk(R"(
            local ok, err = pcall(function() Instance.new("Folder") end)
            print(ok, err)
        )");
    });
    rig.render(0.016);
    REQUIRE(ran);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    INFO(batch.lines[0].text);
    REQUIRE(batch.lines[0].text.rfind("false", 0) == 0);
    REQUIRE(batch.lines[0].text.find("create runs on SimulationThread") != std::string::npos);
    REQUIRE(rig.game.get_children(workspace_of(rig.game)).empty());

    // The scope is Lua's only: outside one, the handler (or abort) decides.
    REQUIRE_FALSE(engine_core::in_script_contract_scope());
    {
        engine_core::ScriptContractScope scope;
        REQUIRE(engine_core::in_script_contract_scope());
        REQUIRE_THROWS_AS(engine_core::contract_fail("in a script"), engine_core::ContractViolation);
    }
    REQUIRE_FALSE(engine_core::in_script_contract_scope());
}

TEST_CASE("RW13 frames that miss the Prepare lock carry their time to the next RenderStepped", "[RW13]") {
    // Engine::render_loop adds every pass's time and takes it only when it
    // holds the lock, so RenderStepped's dt covers the frames it skipped.
    engine_core::RenderStepTime time;
    time.add(0.016);  // missed the 2 ms lock
    time.add(0.016);  // missed again
    time.add(0.016);  // prepared
    REQUIRE(std::fabs(time.take(1.0 / 60.0) - 0.048) < 1e-12);
    // Taken: the next frame starts from nothing.
    time.add(0.008);
    REQUIRE(std::fabs(time.take(1.0 / 60.0) - 0.008) < 1e-12);
    // The sum clamps at 0.1, as one frame's dt did.
    time.add(0.06);
    time.add(0.06);
    REQUIRE(time.take(1.0 / 60.0) == engine_core::RenderStepTime::kMaxDt);
    REQUIRE(engine_core::RenderStepTime::kMaxDt == 0.1);
    // A clock that went back adds nothing; no time at all is the fallback.
    time.add(-0.5);
    REQUIRE(time.take(0.02) == 0.02);
}

TEST_CASE("RW8 pause silences play handlers, Stop removes them, plugins run through", "[RW8]") {
    ScriptRig rig;
    // A real plugin, loaded as the studio loads SceneCamera: its connection is
    // the plugin VM's, delivered in the window in edit mode, through pause, and
    // after Stop.
    ide::PluginLoader loader;
    ide::PluginFile file;
    file.name = "Counter";
    file.source = R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            print("plugin frame")
        end)
    )";
    REQUIRE(loader.load(rig.game, rig.runtime, {file}) == 1);
    add_script(rig.game, "Watch", R"(
        game:GetService("RunService").RenderStepped:Connect(function()
            _G.n = (_G.n or 0) + 1
            print("play frame")
        end)
    )");
    rig.runtime.drain_output();
    int plugin = 0;
    int play = 0;
    const auto count = [&] {
        for (const engine_core::ScriptRuntime::OutputLine& line : rig.runtime.drain_output().lines) {
            if (line.text.rfind("plugin frame", 0) == 0) {
                ++plugin;
            } else if (line.text.rfind("play frame", 0) == 0) {
                ++play;
            }
        }
    };

    rig.render(0.016);  // edit mode
    count();
    REQUIRE(plugin == 1);
    REQUIRE(play == 0);

    rig.game.start_simulation();
    play_step(rig, 0.05);
    rig.render(0.016);
    count();
    REQUIRE(plugin == 2);
    REQUIRE(play == 1);
    double n = 0;
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);

    rig.runtime.set_render_paused(true);
    rig.render(0.016);
    count();
    REQUIRE(play == 1);    // paused: the play handler is silent
    REQUIRE(plugin == 3);  // the plugin is not
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);

    rig.runtime.set_render_paused(false);
    rig.render(0.016);
    count();
    REQUIRE(play == 2);
    REQUIRE(plugin == 4);

    rig.game.stop_simulation();
    rig.render(0.016);
    rig.frames(1, 0.05);
    rig.render(0.016);
    count();
    REQUIRE(play == 2);    // Stop removed the play connection: frozen across renders
    REQUIRE(plugin == 6);  // the plugin's stays
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.render_violations == 0);
}

TEST_CASE("RW14 window delivery for play handlers is paused exactly when the engine is", "[RW14]") {
    // The engine starts paused; its play handlers must start paused with it, so
    // none can run between a start_simulation and the first resume(). Stop
    // clears paused_, and delivery with it, so a later start() matches.
    engine_core::Engine engine;
    REQUIRE(engine.paused());
    REQUIRE(engine.scripts().render_paused());
    engine.start();
    REQUIRE(engine.scripts().render_paused());
    engine.resume();
    REQUIRE_FALSE(engine.paused());
    REQUIRE_FALSE(engine.scripts().render_paused());
    engine.pause();
    REQUIRE(engine.scripts().render_paused());
    engine.stop();
    REQUIRE_FALSE(engine.paused());
    REQUIRE_FALSE(engine.scripts().render_paused());
}

TEST_CASE("RW10 RenderStepped and Heartbeat handlers share state with both threads live", "[RW10]") {
    // A real engine, both loops running: the play VM is entered by the render
    // thread (RenderStepped, in the Prepare window) and by the sim thread
    // (Heartbeat, in the step), turn about under the write lock. Both handlers
    // bump shared _G counters and write the same part's Transform, and each
    // checks on entry that the part still holds the last value either one
    // wrote. Run under ThreadSanitizer as well.
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine_core::GameObject& part = create_part(game);
    game.set_name(part.id(), "Shared");
    add_script(game, "Both", R"(
        local RunService = game:GetService("RunService")
        local part = workspace:FindFirstChild("Shared")
        _G.rs = 0
        _G.hb = 0
        _G.total = 0
        _G.bad = 0
        local function touch(counter)
            if part.Transform.X ~= _G.total then
                _G.bad += 1
            end
            _G[counter] += 1
            _G.total += 1
            part.Transform = Matrix4.new(_G.total, 0, 0)
        end
        RunService.RenderStepped:Connect(function() touch("rs") end)
        RunService.Heartbeat:Connect(function() touch("hb") end)
    )");
    // Paced so neither loop starves the other of the lock: many turns each way.
    engine.set_simulation_pace_hz(240);
    engine.set_render_pace_hz(480);
    engine.start();
    engine.on_simulation([](engine_core::DataModel& g) { g.start_simulation(); });
    engine.resume();
    const std::uint64_t presents = engine.present_count();
    const std::uint64_t steps = engine.sim_frame_count();
    wait_until([&] { return engine.present_count() >= presents + 120 && engine.sim_frame_count() >= steps + 60; },
               std::chrono::seconds(30));
    engine.stop();

    double rs = 0;
    double hb = 0;
    double total = 0;
    double bad = -1;
    REQUIRE(engine.scripts().global_number("rs", rs));
    REQUIRE(engine.scripts().global_number("hb", hb));
    REQUIRE(engine.scripts().global_number("total", total));
    REQUIRE(engine.scripts().global_number("bad", bad));
    INFO("rs=" << rs << " hb=" << hb << " total=" << total);
    REQUIRE(rs > 0);
    REQUIRE(hb > 0);
    REQUIRE(rs + hb == total);
    REQUIRE(bad == 0);
    REQUIRE(part.transform().m[12] == static_cast<float>(total));
    REQUIRE(engine.scripts().last_error().empty());
    REQUIRE(engine.contract_count() == 0);
}

TEST_CASE("S53 a console connection falls back to sim-side delivery with summed dt", "[S53]") {
    ScriptRig rig;
    rig.runtime.drain_output();
    rig.runtime.run_chunk(R"(
        game:GetService("RunService").RenderStepped:Connect(function(dt)
            _G.n = (_G.n or 0) + 1
            _G.dt = dt
        end)
    )");
    rig.render(0.01);
    rig.render(0.02);
    rig.frames(1, 0.05);  // step_tools fires the fallback with the summed time
    // Console globals live in the console VM's own lua_State, not the play VM's,
    // so global_number (which only reads play_.state) cannot see them; read them
    // back by printing, as S15 does.
    rig.runtime.run_chunk(R"(print(string.format("%d,%.6f", _G.n, _G.dt)))");
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    bool saw = false;
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Print) {
            const std::size_t comma = line.text.find(',');
            REQUIRE(comma != std::string::npos);
            REQUIRE(line.text.substr(0, comma) == "1");
            REQUIRE(std::fabs(std::stod(line.text.substr(comma + 1)) - 0.03) < 1e-6);
            saw = true;
        }
    }
    REQUIRE(saw);
}

// Pin: a console-VM RenderStepped:Wait() also stays on sim-side delivery. It
// must resume on a step (step_tools' fallback), never inside the render
// window, since render_window_routed excludes VmKind::Console from the start.
TEST_CASE("S53 a console RenderStepped:Wait() also stays on sim-side delivery", "[S53]") {
    ScriptRig rig;
    rig.runtime.drain_output();
    rig.runtime.run_chunk(R"(
        local dt = game:GetService("RunService").RenderStepped:Wait()
        print(string.format("resumed %.6f", dt))
    )");
    rig.render(0.016);  // a window frame must not resume a console Wait
    const engine_core::ScriptRuntime::OutputBatch mid = rig.runtime.drain_output();
    REQUIRE(mid.lines.empty());
    rig.frames(1, 0.03);  // step_tools fires the fallback, which resumes it
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Print);
    REQUIRE(batch.lines[0].text.rfind("resumed ", 0) == 0);
}

TEST_CASE("RW1 invoke_render runs a host signal's handlers in the window", "[RW1]") {
    SimRole role;
    engine_core::Game game;
    engine_core::EventQueue& events = game.events();
    engine_core::Signal signal;
    events.host_signal(&signal);
    int kept_runs = 0;
    int tagged_runs = 0;
    int once_runs = 0;
    engine_core::Connection self_made;
    engine_core::Connection self_gone =
        signal.connect_kept([&](engine_core::InstanceId, engine_core::Field) { self_gone.disconnect(); }, false);
    signal.connect_kept(
        [&](engine_core::InstanceId, engine_core::Field) {
            ++kept_runs;
            if (kept_runs == 1) {
                // A connect from inside a handler joins next frame, not this walk.
                self_made = signal.connect_kept(
                    [&](engine_core::InstanceId, engine_core::Field) { ++once_runs; }, false);
            }
        },
        false);
    signal.connect_scripted([&](engine_core::InstanceId, engine_core::Field) { ++tagged_runs; }, 7, 1, false);

    std::thread render([&] {
        engine_core::set_thread_role(engine_core::ThreadRole::Render);
        // Right thread, window still closed: the window-flag guard must fire.
        REQUIRE_THROWS_AS(events.invoke_render(signal, false), engine_core::ContractViolation);
        game.set_prerender_window(true);
        events.invoke_render(signal, false);  // paused: tagged skipped
        REQUIRE(kept_runs == 1);
        REQUIRE(tagged_runs == 0);
        REQUIRE(once_runs == 0);
        events.invoke_render(signal, true);
        REQUIRE(kept_runs == 2);
        REQUIRE(tagged_runs == 1);
        REQUIRE(once_runs == 1);
        game.set_prerender_window(false);
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    });
    render.join();
    // Window open, wrong thread (this test case runs under SimRole): the
    // thread-role guard must fire even though the window flag would pass.
    game.set_prerender_window(true);
    REQUIRE_THROWS_AS(events.invoke_render(signal, false), engine_core::ContractViolation);
    game.set_prerender_window(false);
    events.release_signal(signal);
}

TEST_CASE("RW2 the window signal binds with RunService and carries its own dt", "[RW2]") {
    SimRole role;
    engine_core::Game game;
    engine_core::RunService service;
    service.bind(game.events());
    REQUIRE(service.window_signal() != nullptr);
    REQUIRE(service.window_signal()->id().valid());
    service.set_window_dt(0.004);
    REQUIRE(service.dt(engine_core::Phase::RenderStepped) == 0.004);
    service.release(game.events());
    REQUIRE_FALSE(service.window_signal()->id().valid());
}

TEST_CASE("SCH1 a render-phase job's entry stays until shutdown, so a rig binds one and reuses it", "[scheduler]") {
    engine_core::Engine engine;
    engine_core::TaskScheduler& scheduler = engine.scheduler();
    // A render entry is kept after unbind (the render thread may be inside its
    // closure), so each bind-unbind cycle uses a slot for good. Measure what
    // Engine already bound, then fill the phase to exactly its capacity; one
    // more cycle would abort.
    const engine_core::Phase phase = engine_core::Phase::RenderStepped;
    const std::size_t kFree = scheduler.phase_capacity(phase) - scheduler.bound_count(phase);
    std::vector<engine_core::TaskScheduler::JobId> ids;
    for (std::size_t cycle = 0; cycle < kFree; ++cycle) {
        ids.push_back(scheduler.bind(phase, [](double) {}));
        scheduler.unbind(ids.back());
    }
    REQUIRE(ids.size() == kFree);
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}
