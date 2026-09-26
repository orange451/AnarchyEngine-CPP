#include "Contract.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "SnapshotPump.hpp"
#include "TestTriangle.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"
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
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_game_object().id();
    model.set_visual_only(id, true);
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
        model.game_object(id)->set_transform(posed);
        live_at_write = model.game_object(id)->transform();
        stage.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() < 1) {
            return;
        }
        live_later = model.game_object(id)->transform();
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
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_game_object().id();
    model.set_simulated(id, true);
    model.game_object(id)->set_linear_velocity(1.f, 0.f, 0.f);
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
        model.game_object(id)->set_transform(posed);
        if (model.take_deferred_violation()) {
            rejected.store(1);
        }
        model.game_object(id)->set_transform(posed, engine_core::ForceSimWrite{});
        live_at_write = model.game_object(id)->transform();
        stage.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::PostSimulation, [&](double) {
        if (stage.load() != 2) {
            return;
        }
        live_after_physics = model.game_object(id)->transform();
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
    engine_core::DataModel& model = engine.datamodel();
    engine_core::InstanceId ids[10000];
    for (int i = 0; i < 10000; ++i) {
        ids[i] = model.create_game_object().id();
        model.set_simulated(ids[i], true);
    }
    // The rejection is the Prepare. Drop the create records so that Prepare
    // does not spend its budget copying parts that were never drawn.
    model.invalidations().clear();
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        // Every one of these parts is simulated. The first write is rejected,
        // so Prepare does not walk the rest of the list.
        for (engine_core::InstanceId id : ids) {
            model.game_object(id)->set_transform(T1());
            if (model.has_deferred_violation()) {
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
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_game_object().id();
    std::atomic<int> hits{0};
    std::atomic<int> stage{0};
    engine_core::Connection conn = model.changed(id).connect([&](engine_core::InstanceId got, engine_core::Field) {
        hits.fetch_add(1);
        if (const engine_core::GameObject* object = model.game_object(got)) {
            (void)object->color();
        }
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() != 0) {
            return;
        }
        model.game_object(id)->set_color(rgb(0.9f, 0.1f, 0.1f));
        model.destroy(id);
        stage.store(1);
    });
    engine.start();
    engine.resume();
    wait_until([&] { return stage.load() == 1 && engine.sim_frame_count() > 2; });
    engine.stop();
    REQUIRE(hits.load() == 0);
    REQUIRE_FALSE(model.alive(id));
    REQUIRE_FALSE(conn.connected());
    REQUIRE(model.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
}

TEST_CASE("path B enqueues and the snapshot still updates", "[T17]") {
    engine_core::Engine engine;
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_game_object().id();
    model.set_visual_only(id, true);
    const engine_core::ColorRgb tint = rgb(0.2f, 0.8f, 0.1f);
    std::atomic<int> hits{0};
    std::atomic<int> bad{0};
    std::atomic<int> during{0};
    std::atomic<int> stage{0};
    model.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) {
        if (engine_core::thread_role() != engine_core::ThreadRole::Simulation) {
            bad.store(1);
        }
        if (model.prerender_window()) {
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
        model.game_object(id)->set_color(tint);
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
    REQUIRE(near_color(model.game_object(id)->color(), tint));
    REQUIRE(model.events().count(engine_core::WriteOrigin::PreRenderDataModel) >= 1);
    REQUIRE(model.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
}

TEST_CASE("path C emits nothing", "[T18]") {
    engine_core::Engine engine;
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_game_object().id();
    const engine_core::ColorRgb live_color = model.game_object(id)->color();
    const engine_core::Transform live_transform = model.game_object(id)->transform();
    std::atomic<int> hits{0};
    std::atomic<int> stage{0};
    model.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    model.property_changed(id, engine_core::Field::Color)
        .connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    model.property_changed(id, engine_core::Field::Transform)
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
    REQUIRE(near_color(model.game_object(id)->color(), live_color));
    REQUIRE(near(model.game_object(id)->transform(), live_transform));
    REQUIRE(model.events().count(engine_core::WriteOrigin::SnapshotOverride) == 0);
    REQUIRE(model.events().suppressed_overrides() == 0);
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
    engine_core::DataModel model;
    engine_core::DataModel& plain = model.create();
    REQUIRE(plain.id() != 0);
    REQUIRE(model.alive(plain.id()));
    REQUIRE(model.game_object(plain.id()) == nullptr);
    engine_core::GameObject& object = model.create_game_object();
    REQUIRE(model.game_object(object.id()) == &object);
    REQUIRE(object.transform().m[0] == 1.f);
    REQUIRE(object.transform().m[15] == 1.f);
    int seen = 0;
    model.for_each_game_object([&](const engine_core::GameObject& item) {
        REQUIRE(item.id() == object.id());
        ++seen;
    });
    REQUIRE(seen == 1);
    model.destroy(plain.id());
    REQUIRE_FALSE(model.alive(plain.id()));
    REQUIRE(model.alive(object.id()));
}

TEST_CASE("create<T> makes any subclass", "[instance]") {
    engine_core::DataModel model;
    engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
    model.set_parent(triangle.id(), model.id());
    REQUIRE(model.instance(triangle.id()) == &triangle);
    REQUIRE(model.game_object(triangle.id()) == nullptr);
    REQUIRE(model.parent(triangle.id()) == model.id());
    REQUIRE(model.first_child(model.id()) == triangle.id());
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

    engine_core::DataModel& plain = model.create();
    REQUIRE(model.parent(plain.id()) == engine_core::DataModel::kNoParent);
    REQUIRE(dynamic_cast<engine_core::TestTriangle*>(model.instance(plain.id())) == nullptr);
    REQUIRE(model.first_child(model.id()) == triangle.id());

    const engine_core::InstanceId id = triangle.id();
    model.destroy(id);
    REQUIRE_FALSE(model.alive(id));
    REQUIRE(model.instance(id) == nullptr);
    REQUIRE(triangle.angle_degrees() == 0.0);
    REQUIRE(triangle.position().x == 0.f);
    REQUIRE(model.first_child(model.id()) == 0);

    engine_core::TestTriangle& again = model.create<engine_core::TestTriangle>();
    REQUIRE(again.angle_degrees() == 0.0);
    REQUIRE(again.position().z == 0.f);
    REQUIRE(model.game_object(again.id()) == nullptr);
}

TEST_CASE("Heartbeat steps descendants of the root", "[instance]") {
    engine_core::Engine engine;
    engine_core::DataModel& model = engine.datamodel();
    engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
    model.set_parent(triangle.id(), model.id());
    engine_core::TestTriangle& nested = model.create<engine_core::TestTriangle>();
    model.set_parent(nested.id(), triangle.id());
    engine_core::TestTriangle& loose = model.create<engine_core::TestTriangle>();
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
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_game_object().id();
    model.set_visual_only(id, true);
    const engine_core::Transform posed = T1();
    const engine_core::ColorRgb tint = rgb(0.4f, 0.5f, 0.6f);
    const engine_core::ColorRgb original = model.game_object(id)->color();

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

    model.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) {
        if (engine_core::thread_role() != engine_core::ThreadRole::Simulation) {
            bad.store(1);
        }
        if (std::this_thread::get_id() != engine.simulation_thread_id()) {
            bad.store(1);
        }
        if (model.prerender_window()) {
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
            rs_window.store(model.prerender_window() ? 1 : 0);
            rs_depth.store(model.write_depth());
            if (engine_core::thread_role() != engine_core::ThreadRole::Render) {
                bad.store(1);
            }
            model.game_object(id)->set_transform(posed);
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
        pre_window.store(model.prerender_window() ? 1 : 0);
        pre_saw.store(near(model.game_object(id)->transform(), posed) ? 1 : 0);
        stage.store(2);
    });
    engine.scheduler().bind(engine_core::Phase::PostRender, [&](double) {
        if (stage.load() != 3) {
            return;
        }
        post_window.store(model.prerender_window() ? 1 : 0);
        if (engine_core::thread_role() != engine_core::ThreadRole::Render) {
            bad.store(1);
        }
        const engine_core::VisualInstance* inst = engine.pump().find(id);
        post_saw.store(inst != nullptr && near(inst->world, posed) ? 1 : 0);
        try {
            model.game_object(id)->set_color(tint);
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
    REQUIRE(near(model.game_object(id)->transform(), posed));
    REQUIRE(near_color(model.game_object(id)->color(), original));
    REQUIRE(post_write.find("PreRender") != std::string::npos);
    REQUIRE(post_override.find("PreRender") != std::string::npos);
    REQUIRE(model.events().count(engine_core::WriteOrigin::PreRenderDataModel) >= 1);
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
    engine.on_simulation([&](engine_core::DataModel& model) {
        engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
        model.set_parent(triangle.id(), model.id());
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
    engine.on_simulation([&](engine_core::DataModel& model) {
        engine_core::GameObject& part = model.create<engine_core::GameObject>();
        model.set_parent(part.id(), model.id());
        id = part.id();
    });
    REQUIRE(engine.datamodel().alive(id));
    // A project load clears the world inside one paused edit, then builds the
    // new tree. The old instances must be gone before that edit returns.
    bool gone_inside = false;
    engine.on_simulation([&](engine_core::DataModel& model) {
        model.destroy(id);
        gone_inside = !model.alive(id);
    });
    REQUIRE(gone_inside);
    REQUIRE_FALSE(engine.datamodel().alive(id));
    REQUIRE(engine.paused());
    engine.stop();
}

TEST_CASE("an edit during play runs on the simulation thread", "[edit]") {
    engine_core::Engine engine;
    engine.start();
    engine.resume();
    std::atomic<int> on_sim{0};
    std::atomic<engine_core::InstanceId> id{0};
    engine.on_simulation([&](engine_core::DataModel& model) {
        if (std::this_thread::get_id() == engine.simulation_thread_id()) {
            on_sim.store(1);
        }
        engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
        model.set_parent(triangle.id(), model.id());
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
    engine_core::DataModel model;
    REQUIRE(model.name(model.id()) == "DataModel");
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    REQUIRE(model.name(part.id()) == "GameObject");
    engine_core::DataModel& plain = model.create();
    REQUIRE(model.name(plain.id()) == "DataModel");

    int property_hits = 0;
    int changed_hits = 0;
    engine_core::Field property_field = engine_core::Field::Transform;
    engine_core::Field changed_field = engine_core::Field::Transform;
    std::string seen_name;
    model.property_changed(part.id(), engine_core::Field::Name)
        .connect([&](engine_core::InstanceId, engine_core::Field field) {
            ++property_hits;
            property_field = field;
            seen_name = model.name(part.id());
        });
    model.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field field) {
        ++changed_hits;
        changed_field = field;
    });

    model.set_name(part.id(), "GameObject");
    {
        SimRole role;
        model.events().drain();
    }
    REQUIRE(property_hits == 0);
    REQUIRE(changed_hits == 0);

    REQUIRE(model.invalidations().size() == 0);
    model.set_name(part.id(), "Brick");
    REQUIRE(model.name(part.id()) == "Brick");
    REQUIRE(property_hits == 0);
    REQUIRE(changed_hits == 0);
    REQUIRE(model.invalidations().size() == 0);
    {
        SimRole role;
        model.events().drain();
    }
    REQUIRE(property_hits == 1);
    REQUIRE(changed_hits == 1);
    REQUIRE(property_field == engine_core::Field::Name);
    REQUIRE(changed_field == engine_core::Field::Name);
    REQUIRE(seen_name == "Brick");

    part.set_color(rgb(0.2f, 0.3f, 0.4f));
    REQUIRE(model.invalidations().size() == 1);

    const engine_core::InstanceId dead = plain.id();
    model.destroy(dead);
    REQUIRE(model.name(dead).empty());
    REQUIRE_THROWS_AS(model.set_name(dead, "Nope"), engine_core::ContractViolation);
}

TEST_CASE("N6 the root Changed signal is not the first instance", "[N6]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    REQUIRE((part.id() & 0xffffu) == 0);
    REQUIRE(part.id() != model.id());

    int root_changed = 0;
    int root_named = 0;
    int part_changed = 0;
    int added = 0;
    engine_core::InstanceId added_id = 0;
    model.changed(model.id()).connect([&](engine_core::InstanceId id, engine_core::Field field) {
        REQUIRE(id == model.id());
        REQUIRE(field == engine_core::Field::Name);
        ++root_changed;
    });
    model.property_changed(model.id(), engine_core::Field::Name)
        .connect([&](engine_core::InstanceId id, engine_core::Field field) {
            REQUIRE(id == model.id());
            REQUIRE(field == engine_core::Field::Name);
            ++root_named;
        });
    model.changed(part.id()).connect([&](engine_core::InstanceId id, engine_core::Field) {
        REQUIRE(id == part.id());
        ++part_changed;
    });
    model.child_added(model.id()).connect([&](engine_core::InstanceId child, engine_core::Field) {
        ++added;
        added_id = child;
    });

    model.set_name(model.id(), "DataModel");
    {
        SimRole role;
        model.events().drain();
    }
    REQUIRE(root_changed == 0);
    REQUIRE(root_named == 0);

    model.set_name(model.id(), "Place");
    model.set_name(part.id(), "Brick");
    engine_core::GameObject& extra = model.create<engine_core::GameObject>();
    model.set_parent(extra.id(), model.id());
    {
        SimRole role;
        model.events().drain();
    }
    REQUIRE(root_changed == 1);
    REQUIRE(root_named == 1);
    REQUIRE(part_changed == 1);
    REQUIRE(added == 1);
    REQUIRE(added_id == extra.id());
    REQUIRE(model.name(model.id()) == "Place");

    const int part_held = part_changed;
    model.destroy(part.id());
    model.set_name(model.id(), "Again");
    {
        SimRole role;
        model.events().drain();
    }
    REQUIRE(root_changed == 2);
    REQUIRE(root_named == 2);
    REQUIRE(part_changed == part_held);
}

TEST_CASE("N2 siblings may share a name and find_first_child returns the first", "[N2]") {
    engine_core::DataModel model;
    engine_core::DataModel& folder = model.create();
    model.set_name(folder.id(), "Folder");
    model.set_parent(folder.id(), model.id());

    engine_core::GameObject& older = model.create<engine_core::GameObject>();
    engine_core::GameObject& newer = model.create<engine_core::GameObject>();
    model.set_name(older.id(), "Wood");
    model.set_name(newer.id(), "Wood");
    model.set_parent(older.id(), folder.id());
    model.set_parent(newer.id(), folder.id());
    // link_child inserts at the front, so the later parent is first.
    REQUIRE(model.first_child(folder.id()) == newer.id());
    REQUIRE(model.find_first_child(folder.id(), "Wood") == newer.id());
    REQUIRE(model.find_first_child(folder.id(), "Missing") == 0);

    engine_core::GameObject& metal = model.create<engine_core::GameObject>();
    model.set_name(metal.id(), "Metal");
    model.set_parent(metal.id(), folder.id());
    REQUIRE(model.find_first_child(folder.id(), "Wood") == newer.id());
    REQUIRE(model.find_first_child(folder.id(), "Metal") == metal.id());

    const std::vector<engine_core::InstanceId> children = model.get_children(folder.id());
    REQUIRE(children.size() == 3);
    REQUIRE(children[0] == metal.id());
    REQUIRE(children[1] == newer.id());
    REQUIRE(children[2] == older.id());
    REQUIRE(model.get_children(0xdeadbeefu).empty());
    REQUIRE(model.find_first_child(0xdeadbeefu, "Wood") == 0);
    REQUIRE(model.get_children(model.id()).size() == 1);
    REQUIRE(model.get_children(model.id())[0] == folder.id());
}

TEST_CASE("N3 place restore reverts play and drops session instances", "[N3]") {
    engine_core::DataModel model;
    engine_core::DataModel& folder = model.create();
    const engine_core::InstanceId folder_id = folder.id();
    model.set_name(folder_id, "Folder");
    model.set_parent(folder_id, model.id());

    engine_core::GameObject& leaf = model.create<engine_core::GameObject>();
    const engine_core::InstanceId leaf_id = leaf.id();
    model.set_name(leaf_id, "Leaf");
    model.set_parent(leaf_id, folder_id);
    const engine_core::ColorRgb red = rgb(0.8f, 0.1f, 0.1f);
    const engine_core::Transform posed = T0();
    leaf.set_color(red);
    leaf.set_size(2.f, 3.f, 4.f);
    leaf.set_transform(posed);
    model.set_simulated(leaf_id, true);

    engine_core::GameObject& sibling = model.create<engine_core::GameObject>();
    const engine_core::InstanceId sibling_id = sibling.id();
    model.set_name(sibling_id, "Sibling");
    model.set_parent(sibling_id, folder_id);

    const std::uint32_t generation = model.world_generation();
    model.capture_place();
    model.start_simulation();
    REQUIRE(model.simulation_running());
    REQUIRE_THROWS_AS(model.start_simulation(), engine_core::ContractViolation);

    model.set_name(leaf_id, "Moved");
    leaf.set_color(rgb(0.1f, 0.2f, 0.9f));
    leaf.set_size(9.f, 9.f, 9.f);
    leaf.set_transform(T1());
    model.set_parent(leaf_id, model.id());
    leaf.set_linear_velocity(10.f, 0.f, 0.f);
    model.integrate_simulated(1.0);
    REQUIRE_FALSE(near(model.game_object(leaf_id)->transform(), posed));

    engine_core::GameObject& extra = model.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    model.set_name(extra_id, "Session");
    model.set_parent(extra_id, folder_id);

    model.destroy(sibling_id);
    REQUIRE_FALSE(model.alive(sibling_id));
    engine_core::GameObject& recycled = model.create<engine_core::GameObject>();
    const engine_core::InstanceId recycled_id = recycled.id();
    REQUIRE(recycled_id != sibling_id);
    REQUIRE((recycled_id & 0xffffu) == (sibling_id & 0xffffu));
    model.set_name(recycled_id, "Recycled");
    model.set_parent(recycled_id, model.id());

    model.stop_simulation();
    REQUIRE_FALSE(model.simulation_running());
    REQUIRE(model.world_generation() == generation + 1);
    REQUIRE(model.name(model.id()) == "DataModel");
    REQUIRE(model.alive(leaf_id));
    REQUIRE(model.alive(sibling_id));
    REQUIRE(model.alive(folder_id));
    REQUIRE_FALSE(model.alive(extra_id));
    REQUIRE_FALSE(model.alive(recycled_id));
    REQUIRE(model.name(leaf_id) == "Leaf");
    REQUIRE(model.name(sibling_id) == "Sibling");
    REQUIRE(model.name(folder_id) == "Folder");
    REQUIRE(model.parent(leaf_id) == folder_id);
    REQUIRE(model.parent(sibling_id) == folder_id);
    REQUIRE(model.parent(folder_id) == model.id());
    REQUIRE(model.find_first_child(folder_id, "Sibling") == sibling_id);
    REQUIRE(near_color(model.game_object(leaf_id)->color(), red));
    float size[3] = {};
    REQUIRE(model.game_object(leaf_id)->copy_size(size));
    REQUIRE(size[0] == 2.f);
    REQUIRE(size[1] == 3.f);
    REQUIRE(size[2] == 4.f);
    REQUIRE(near(model.game_object(leaf_id)->transform(), posed));
    REQUIRE(model.simulated(leaf_id));
    model.integrate_simulated(1.0);
    REQUIRE(near(model.game_object(leaf_id)->transform(), posed));

    const std::vector<engine_core::InstanceId> children = model.get_children(folder_id);
    REQUIRE(children.size() == 2);
    REQUIRE(children[0] == sibling_id);
    REQUIRE(children[1] == leaf_id);

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    {
        engine_core::DataModelLock lock(model, engine_core::DataModelLock::Write);
        pump.begin_prerender_window(model);
        pump.end_prerender_window(model);
        pump.prepare_copy(model);
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
    engine_core::DataModel model;
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    model.set_parent(id, model.id());
    const engine_core::ColorRgb red = rgb(0.7f, 0.0f, 0.0f);
    part.set_color(red);
    model.set_name(id, "Door");
    model.capture_place();
    const std::uint32_t generation = model.world_generation();

    model.start_simulation();
    part.set_color(rgb(0.f, 0.f, 1.f));
    model.set_name(id, "Session");
    engine_core::GameObject& extra = model.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    model.stop_simulation();
    REQUIRE(model.world_generation() == generation + 1);
    REQUIRE(model.name(id) == "Door");
    REQUIRE(near_color(model.game_object(id)->color(), red));
    REQUIRE_FALSE(model.alive(extra_id));

    model.game_object(id)->set_color(rgb(0.f, 1.f, 0.f));
    model.set_name(id, "Edited");
    engine_core::GameObject& between = model.create<engine_core::GameObject>();
    const engine_core::InstanceId between_id = between.id();
    model.set_parent(between_id, model.id());

    model.start_simulation();
    model.game_object(id)->set_color(rgb(0.f, 0.f, 1.f));
    model.set_name(id, "Again");
    model.stop_simulation();
    REQUIRE(model.world_generation() == generation + 2);
    REQUIRE(model.name(id) == "Door");
    REQUIRE(near_color(model.game_object(id)->color(), red));
    REQUIRE(model.parent(id) == model.id());
    REQUIRE_FALSE(model.alive(between_id));

    model.stop_simulation();
    REQUIRE_FALSE(model.simulation_running());
    REQUIRE(model.world_generation() == generation + 2);
}

TEST_CASE("N4 a folder removed in edit mode stays removed after the next stop", "[N4]") {
    engine_core::DataModel model;

    model.start_simulation();
    model.stop_simulation();

    engine_core::Folder& folder = model.create<engine_core::Folder>();
    const engine_core::InstanceId folder_id = folder.id();
    model.set_name(folder_id, "Props");
    model.set_parent(folder_id, model.id());

    engine_core::Folder& cut = model.create<engine_core::Folder>();
    const engine_core::InstanceId cut_id = cut.id();
    model.set_name(cut_id, "Loose");
    model.set_parent(cut_id, model.id());
    // Insert while stopped replaces the snapshot, which is why the new
    // folders survive the next Stop.
    model.capture_place();

    model.start_simulation();
    model.stop_simulation();
    REQUIRE(model.parent(folder_id) == model.id());
    REQUIRE(model.parent(cut_id) == model.id());

    model.destroy(folder_id);
    model.set_parent(cut_id, engine_core::DataModel::kNoParent);
    REQUIRE_FALSE(model.alive(folder_id));
    REQUIRE(model.parent(cut_id) == engine_core::DataModel::kNoParent);

    // Test freezes this edit-mode tree before play.
    model.capture_place();
    model.start_simulation();
    model.stop_simulation();
    REQUIRE_FALSE(model.alive(folder_id));
    REQUIRE(model.alive(cut_id));
    REQUIRE(model.parent(cut_id) == engine_core::DataModel::kNoParent);
    REQUIRE(model.find_first_child(model.id(), "Props") == 0);
    REQUIRE(model.find_first_child(model.id(), "Loose") == 0);
}

TEST_CASE("edit then play captures the place on start", "[N4]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    const engine_core::ColorRgb red = rgb(0.4f, 0.1f, 0.1f);
    part.set_color(red);
    model.set_name(id, "Door");
    model.set_parent(id, model.id());
    REQUIRE_FALSE(model.simulation_running());
    model.start_simulation();
    REQUIRE(model.simulation_running());
    part.set_color(rgb(0.1f, 0.1f, 0.8f));
    model.set_name(id, "Gone");
    engine_core::GameObject& extra = model.create<engine_core::GameObject>();
    const engine_core::InstanceId extra_id = extra.id();
    model.stop_simulation();
    REQUIRE(model.name(id) == "Door");
    REQUIRE(near_color(model.game_object(id)->color(), red));
    REQUIRE(model.parent(id) == model.id());
    REQUIRE_FALSE(model.alive(extra_id));
}

TEST_CASE("N5 stop drops session events and session jobs", "[N5]") {
    engine_core::Engine engine;
    engine_core::DataModel& model = engine.datamodel();
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    model.set_name(id, "Door");
    model.capture_place();

    int hits = 0;
    engine_core::Connection early =
        model.changed(id).connect([&](engine_core::InstanceId, engine_core::Field) { ++hits; });
    engine_core::Connection named = model.property_changed(id, engine_core::Field::Name)
                                         .connect([&](engine_core::InstanceId, engine_core::Field) { ++hits; });

    int session_jobs = 0;
    int permanent_jobs = 0;
    engine.scheduler().bind_session(engine_core::Phase::Heartbeat, [&](double) { ++session_jobs; });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) { ++permanent_jobs; });

    model.start_simulation();
    model.set_name(id, "Session");
    REQUIRE(hits == 0);
    REQUIRE(model.name(id) == "Session");
    model.stop_simulation();
    REQUIRE(model.name(id) == "Door");
    REQUIRE_FALSE(early.connected());
    REQUIRE_FALSE(named.connected());
    {
        SimRole role;
        model.events().drain();
        engine.scheduler().run_phase(engine_core::Phase::Heartbeat, 0.0);
    }
    REQUIRE(hits == 0);
    REQUIRE(session_jobs == 0);
    REQUIRE(permanent_jobs == 1);

    int after = 0;
    model.property_changed(id, engine_core::Field::Name)
        .connect([&](engine_core::InstanceId, engine_core::Field field) {
            if (field == engine_core::Field::Name) {
                ++after;
            }
        });
    model.set_name(id, "After");
    REQUIRE(after == 0);
    {
        SimRole role;
        model.events().drain();
    }
    REQUIRE(after == 1);
    REQUIRE(model.name(id) == "After");
}

int name_number(engine_core::DataModel& model, engine_core::InstanceId id) {
    try {
        return std::stoi(model.name(id));
    } catch (const std::exception&) {
        return -1;
    }
}

struct ScriptRig {
    SimRole role;
    engine_core::DataModel model;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;

    ScriptRig() {
        scheduler.reserve(16);
        model.attach_scheduler(&scheduler);
        runtime.attach(model, scheduler);
    }

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            model.events().drain();
            runtime.heartbeat(dt);
            model.events().drain();
        }
    }
};

engine_core::GameObject& add_part(engine_core::DataModel& model, engine_core::InstanceId parent, const char* name) {
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    model.set_name(part.id(), name);
    model.set_parent(part.id(), parent);
    return part;
}

engine_core::Script& add_script(engine_core::DataModel& model, const char* name, const char* source) {
    engine_core::Script& script = model.create<engine_core::Script>();
    model.set_name(script.id(), name);
    script.set_source(source);
    model.set_parent(script.id(), model.id());
    return script;
}

TEST_CASE("S1 two scripts wait without blocking each other", "[S1]") {
    ScriptRig rig;
    engine_core::Script& first = add_script(rig.model, "A", R"(
        local box = script:GetChildren()[1]
        while true do
            task.wait(0.05)
            local n = tonumber(box.Name) or 0
            box.Name = tostring(n + 1)
        end
    )");
    engine_core::Script& second = add_script(rig.model, "B", R"(
        local box = script:GetChildren()[1]
        while true do
            task.wait(0.05)
            local n = tonumber(box.Name) or 0
            box.Name = tostring(n + 1)
        end
    )");
    engine_core::GameObject& count_a = add_part(rig.model, first.id(), "0");
    engine_core::GameObject& count_b = add_part(rig.model, second.id(), "0");
    REQUIRE(rig.model.find_first_child(rig.model.id(), "A") == first.id());
    REQUIRE(std::string(first.class_name()) == "Script");

    rig.model.start_simulation();
    rig.frames(4, 0.05);
    const int a = name_number(rig.model, count_a.id());
    const int b = name_number(rig.model, count_b.id());
    REQUIRE(a > 0);
    REQUIRE(b > 0);
    REQUIRE(std::fabs(rig.runtime.sim_clock() - 0.2) < 1e-9);
}

TEST_CASE("S2 a script timeout leaves the other Heartbeat running", "[S2]") {
    ScriptRig rig;
    add_script(rig.model, "Spin", "while true do end");
    engine_core::Script& live = add_script(rig.model, "Live", R"(
        local box = script:GetChildren()[1]
        game:GetService("RunService").Heartbeat:Connect(function()
            local n = tonumber(box.Name) or 0
            box.Name = tostring(n + 1)
        end)
    )");
    engine_core::GameObject& count = add_part(rig.model, live.id(), "0");
    rig.model.start_simulation();
    rig.frames(3, 0.05);
    const int mid = name_number(rig.model, count.id());
    REQUIRE(mid > 0);
    REQUIRE(rig.runtime.last_error().find("ScriptTimeout") != std::string::npos);
    rig.frames(2, 0.05);
    REQUIRE(name_number(rig.model, count.id()) > mid);
}

TEST_CASE("S3 stop aborts a waiting script and the next start runs from the top", "[S3]") {
    ScriptRig rig;
    engine_core::Script& script = add_script(rig.model, "Main", R"(
        _G.marker = 1
        local n = 0
        local flag = script:GetChildren()[1]
        flag.Name = tostring(n)
        task.wait(10)
        n = n + 1
        flag.Name = tostring(n)
        _G.marker = 2
    )");
    engine_core::GameObject& flag = add_part(rig.model, script.id(), "start");
    rig.model.start_simulation();
    rig.frames(3, 0.05);
    REQUIRE(rig.model.name(flag.id()) == "0");
    double marker = 0;
    REQUIRE(rig.runtime.global_number("marker", marker));
    REQUIRE(marker == 1);

    rig.model.stop_simulation();
    REQUIRE(rig.model.name(flag.id()) == "start");
    REQUIRE_FALSE(rig.runtime.vm_open());
    REQUIRE(rig.model.find_first_child(script.id(), "start") == flag.id());

    rig.model.start_simulation();
    REQUIRE(rig.runtime.vm_open());
    REQUIRE(rig.runtime.global_is_nil("marker"));
    rig.frames(3, 0.05);
    REQUIRE(rig.model.name(flag.id()) == "0");
    REQUIRE(rig.runtime.global_number("marker", marker));
    REQUIRE(marker == 1);
    REQUIRE(rig.runtime.sim_clock() < 10);
}

TEST_CASE("S4 disabling or destroying a script drops only its connections", "[S4]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.model, rig.model.id(), "P");
    engine_core::GameObject& hits = add_part(rig.model, rig.model.id(), "Hits");
    engine_core::GameObject& beat = add_part(rig.model, rig.model.id(), "Beat");
    engine_core::GameObject& other = add_part(rig.model, rig.model.id(), "Other");
    // The find names are the stable names. Counters live on children so renames do not hide them.
    engine_core::GameObject& hit_count = add_part(rig.model, hits.id(), "0");
    engine_core::GameObject& beat_count = add_part(rig.model, beat.id(), "0");
    engine_core::GameObject& other_count = add_part(rig.model, other.id(), "0");
    engine_core::Script& one = add_script(rig.model, "One", R"(
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
    engine_core::Script& two = add_script(rig.model, "Two", R"(
        local part = game:FindFirstChild("P")
        local other = game:FindFirstChild("Other"):GetChildren()[1]
        part.Changed:Connect(function()
            local n = tonumber(other.Name) or 0
            other.Name = tostring(n + 1)
        end)
    )");
    (void)one;
    rig.model.start_simulation();
    rig.frames(3, 0.05);
    const int beat_before = name_number(rig.model, beat_count.id());
    REQUIRE(beat_before > 0);
    part.set_color(rgb(0.2f, 0.3f, 0.4f));
    rig.model.events().drain();
    const int hits_before = name_number(rig.model, hit_count.id());
    const int other_before = name_number(rig.model, other_count.id());
    REQUIRE(hits_before > 0);
    REQUIRE(other_before > 0);

    one.set_enabled(false);
    part.set_color(rgb(0.6f, 0.1f, 0.1f));
    rig.model.events().drain();
    REQUIRE(name_number(rig.model, hit_count.id()) == hits_before);
    REQUIRE(name_number(rig.model, other_count.id()) > other_before);
    const int beat_held = name_number(rig.model, beat_count.id());
    rig.frames(2, 0.05);
    REQUIRE(name_number(rig.model, beat_count.id()) == beat_held);

    const int other_held = name_number(rig.model, other_count.id());
    rig.model.destroy(two.id());
    part.set_color(rgb(0.1f, 0.7f, 0.2f));
    rig.model.events().drain();
    REQUIRE(name_number(rig.model, other_count.id()) == other_held);
    REQUIRE(name_number(rig.model, hit_count.id()) == hits_before);
}

TEST_CASE("a new ModuleScript returns an empty table", "[module]") {
    constexpr const char* starter = "local module = {}\n\nreturn module\n";
    ScriptRig rig;

    engine_core::ModuleScript& created = rig.model.create<engine_core::ModuleScript>();
    REQUIRE(created.source() == starter);
    REQUIRE(created.enabled());
    engine_core::Script& script = rig.model.create<engine_core::Script>();
    REQUIRE(script.source().empty());
    rig.model.destroy(script.id());

    created.set_source("return 1");
    rig.model.destroy(created.id());
    engine_core::ModuleScript& module = rig.model.create<engine_core::ModuleScript>();
    REQUIRE(module.source() == starter);

    const char* kept = "return { kept = true }";
    module.set_source(kept);
    const engine_core::InstanceId id = module.id();
    rig.model.set_name(id, "Kept");
    rig.model.set_parent(id, rig.model.id());
    rig.model.capture_place();

    rig.model.start_simulation();
    module.set_source("return { session = true }");
    rig.model.destroy(id);
    engine_core::ModuleScript& recycled = rig.model.create<engine_core::ModuleScript>();
    const engine_core::InstanceId recycled_id = recycled.id();
    REQUIRE(recycled.source() == starter);
    add_script(rig.model, "Check", R"lua(
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
    const engine_core::InstanceId from_new = rig.model.find_first_child(rig.model.id(), "FromNew");
    REQUIRE(from_new != 0);
    const auto* made = dynamic_cast<const engine_core::ModuleScript*>(rig.model.instance(from_new));
    REQUIRE(made != nullptr);
    REQUIRE(made->source() == starter);

    rig.model.stop_simulation();
    REQUIRE(rig.model.alive(id));
    REQUIRE_FALSE(rig.model.alive(recycled_id));
    REQUIRE_FALSE(rig.model.alive(from_new));
    auto* restored = dynamic_cast<engine_core::ModuleScript*>(rig.model.instance(id));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->source() == kept);

    restored->set_source("");
    rig.model.capture_place();
    rig.model.start_simulation();
    restored->set_source(starter);
    rig.model.stop_simulation();
    restored = dynamic_cast<engine_core::ModuleScript*>(rig.model.instance(id));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->source().empty());
}

TEST_CASE("S5 require caches one return and drops it when the simulation stops", "[S5]") {
    ScriptRig rig;
    engine_core::GameObject& runs = add_part(rig.model, rig.model.id(), "0");
    rig.model.set_name(runs.id(), "Runs");
    engine_core::GameObject& run_count = add_part(rig.model, runs.id(), "0");
    engine_core::ModuleScript& mod = rig.model.create<engine_core::ModuleScript>();
    rig.model.set_name(mod.id(), "Mod");
    mod.set_source(R"(
        local flag = script.Parent:FindFirstChild("Runs"):GetChildren()[1]
        local n = tonumber(flag.Name) or 0
        flag.Name = tostring(n + 1)
        return { n = n + 1 }
    )");
    rig.model.set_parent(mod.id(), rig.model.id());
    engine_core::ModuleScript& cycle = rig.model.create<engine_core::ModuleScript>();
    rig.model.set_name(cycle.id(), "Cycle");
    cycle.set_source("return require(script)");
    rig.model.set_parent(cycle.id(), rig.model.id());
    add_script(rig.model, "Main", R"(
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
    rig.model.start_simulation();
    rig.frames(1, 0.05);
    bool same = false;
    bool cycled = false;
    double n = 0;
    REQUIRE(rig.runtime.global_boolean("same", same));
    REQUIRE(same);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
    REQUIRE(name_number(rig.model, run_count.id()) == 1);
    REQUIRE(rig.runtime.global_boolean("cycle", cycled));
    REQUIRE(cycled);

    rig.model.stop_simulation();
    REQUIRE(rig.model.name(run_count.id()) == "0");
    rig.model.start_simulation();
    REQUIRE(rig.runtime.global_is_nil("same"));
    REQUIRE(rig.runtime.global_is_nil("n"));
    rig.frames(1, 0.05);
    REQUIRE(name_number(rig.model, run_count.id()) == 1);
    REQUIRE(rig.runtime.global_boolean("same", same));
    REQUIRE(same);
    REQUIRE(rig.runtime.global_number("n", n));
    REQUIRE(n == 1);
}

TEST_CASE("S6 stop drops a script-created GameObject and restores an authored name", "[S6]") {
    ScriptRig rig;
    engine_core::GameObject& door = add_part(rig.model, rig.model.id(), "Door");
    engine_core::Script& maker = add_script(rig.model, "Maker", R"(
        local made = Instance.new("GameObject")
        made.Name = "Session"
        made.Parent = script.Parent
        local door = script.Parent:FindFirstChild("Door")
        door.Name = "Moved"
    )");
    REQUIRE(rig.model.find_first_child(rig.model.id(), "Maker") == maker.id());
    REQUIRE(std::string(maker.class_name()) == "Script");
    rig.model.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.model.name(door.id()) == "Moved");
    const engine_core::InstanceId session = rig.model.find_first_child(rig.model.id(), "Session");
    REQUIRE(session != 0);
    REQUIRE(std::string(rig.model.instance(session)->class_name()) == "GameObject");

    rig.model.stop_simulation();
    REQUIRE(rig.model.name(door.id()) == "Door");
    REQUIRE_FALSE(rig.model.alive(session));
    REQUIRE(rig.model.find_first_child(rig.model.id(), "Session") == 0);
    REQUIRE(rig.model.find_first_child(rig.model.id(), "Maker") == maker.id());
}

TEST_CASE("S7 parenting a script while running starts it after the drain", "[S7]") {
    ScriptRig rig;
    engine_core::GameObject& mark = add_part(rig.model, rig.model.id(), "Mark");
    engine_core::GameObject& token = add_part(rig.model, mark.id(), "hidden");
    engine_core::GameObject& flag = add_part(rig.model, rig.model.id(), "Flag");
    engine_core::GameObject& trigger = add_part(rig.model, rig.model.id(), "Trigger");
    engine_core::Script& script = rig.model.create<engine_core::Script>();
    rig.model.set_name(script.id(), "Late");
    script.set_source(R"(
        local mark = game:FindFirstChild("Mark")
        local flag = game:FindFirstChild("Flag")
        flag.Name = mark:GetChildren()[1].Name
    )");
    REQUIRE(rig.model.parent(script.id()) == engine_core::DataModel::kNoParent);

    rig.model.start_simulation();
    bool started_inside = false;
    rig.model.changed(trigger.id()).connect([&](engine_core::InstanceId, engine_core::Field) {
        rig.model.set_name(token.id(), "visible");
        rig.model.set_parent(script.id(), rig.model.id());
        started_inside = rig.model.name(flag.id()) == "visible";
    });
    rig.model.set_name(trigger.id(), "go");
    REQUIRE(rig.model.name(flag.id()) == "Flag");
    rig.model.events().drain();
    REQUIRE_FALSE(started_inside);
    REQUIRE(rig.model.parent(script.id()) == rig.model.id());
    REQUIRE(rig.model.name(flag.id()) == "visible");
}

TEST_CASE("S8 a script color write is path A", "[S8]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.model, rig.model.id(), "P");
    add_script(rig.model, "Painter", R"(
        local part = game:FindFirstChild("P")
        part.Color = {r = 0.2, g = 0.4, b = 0.6, a = 1}
    )");
    int hits = 0;
    engine_core::Field seen = engine_core::Field::Count;
    rig.model.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field field) {
        ++hits;
        seen = field;
    });
    rig.model.start_simulation();
    rig.frames(1, 1.0 / 60.0);
    const engine_core::ColorRgb tint = rgb(0.2f, 0.4f, 0.6f);
    REQUIRE(hits >= 1);
    REQUIRE(seen == engine_core::Field::Color);
    REQUIRE(near_color(part.color(), tint));

    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    {
        engine_core::DataModelLock lock(rig.model, engine_core::DataModelLock::Write);
        pump.begin_prerender_window(rig.model);
        pump.end_prerender_window(rig.model);
        pump.prepare_copy(rig.model);
    }
    pump.publish();
    const engine_core::VisualInstance* vis = pump.find(part.id());
    REQUIRE(vis != nullptr);
    REQUIRE(near_color(vis->color, tint));
    REQUIRE(vis->color_origin == engine_core::WriteOrigin::Simulation);
}

TEST_CASE("S9 binding PreRender from a script errors", "[S9]") {
    ScriptRig rig;
    add_script(rig.model, "Bad", R"(
        local pre_ok = pcall(function()
            game:GetService("RunService").PreRender:Connect(function() end)
        end)
        local step_ok = pcall(function()
            game:GetService("RunService").RenderStepped:Connect(function() end)
        end)
        _G.pre = not pre_ok
        _G.step = not step_ok
    )");
    add_script(rig.model, "Other", "_G.other = true");
    rig.model.start_simulation();
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
    engine_core::GameObject& door = add_part(rig.model, rig.model.id(), "Door");
    add_script(rig.model, "Keeper", R"(
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
    rig.model.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.model.name(door.id()) == "Live");
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

    const std::uint32_t generation = rig.model.world_generation();
    rig.model.stop_simulation();
    REQUIRE(rig.model.world_generation() == generation + 1);
    REQUIRE(rig.model.alive(door.id()));
    REQUIRE(rig.model.name(door.id()) == "Door");
    REQUIRE(rig.runtime.resolve_watch(watch) == nullptr);

    rig.model.start_simulation();
    REQUIRE(rig.runtime.global_is_nil("door"));
    REQUIRE(rig.runtime.resolve_watch(watch) == nullptr);
    rig.frames(1, 0.05);
    REQUIRE(rig.model.name(door.id()) == "Live");
    REQUIRE(rig.runtime.resolve_watch(watch) == nullptr);
    const engine_core::ScriptRuntime::Watch fresh = rig.runtime.watch_global("door");
    REQUIRE(fresh.valid);
    REQUIRE(fresh.world != watch.world);
    REQUIRE(rig.runtime.resolve_watch(fresh) == &door);
}

TEST_CASE("S11 spawned wait(0) threads both resume", "[S11]") {
    ScriptRig rig;
    add_script(rig.model, "Main", R"(
        task.spawn(function()
            task.wait(0)
            _G.a = (_G.a or 0) + 1
        end)
        task.spawn(function()
            task.wait(0)
            _G.b = (_G.b or 0) + 1
        end)
    )");
    rig.model.start_simulation();
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
    engine_core::DataModel& model = engine.datamodel();
    engine_core::GameObject& part = add_part(model, model.id(), "P");
    add_script(model, "Beat", R"(
        local part = game:FindFirstChild("P")
        local n = 0
        game:GetService("RunService").Heartbeat:Connect(function()
            n = n + 1
            part.Color = {r = n / 100, g = 0.2, b = 0.3, a = 1}
        end)
    )");
    std::atomic<int> hits{0};
    model.changed(part.id()).connect([&](engine_core::InstanceId, engine_core::Field) { hits.fetch_add(1); });
    model.start_simulation();
    engine.start();
    engine.resume();
    wait_until([&] { return hits.load() >= 3; });
    engine.stop();
    REQUIRE(hits.load() >= 3);
}

TEST_CASE("Vector3 is the triangle position", "[vector3]") {
    ScriptRig rig;
    engine_core::TestTriangle& triangle = rig.model.create<engine_core::TestTriangle>();
    rig.model.set_name(triangle.id(), "Tri0");
    rig.model.set_parent(triangle.id(), rig.model.id());
    triangle.set_position(-0.58f, 0.38f, 0.f);
    add_script(rig.model, "Main", R"(
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

    rig.model.start_simulation();
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
    engine_core::TestTriangle& slow = rig.model.create<engine_core::TestTriangle>();
    rig.model.set_name(slow.id(), "Tri0");
    rig.model.set_parent(slow.id(), rig.model.id());
    slow.set_position(-0.58f, 0.38f, 0.f);
    engine_core::TestTriangle& fast = rig.model.create<engine_core::TestTriangle>();
    rig.model.set_name(fast.id(), "Tri1");
    rig.model.set_parent(fast.id(), rig.model.id());
    fast.set_position(0.58f, 0.38f, 0.15f);
    add_script(rig.model, "HopSlow", R"(
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
    add_script(rig.model, "HopFast", R"(
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

    rig.model.start_simulation();
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

    rig.model.stop_simulation();
    const engine_core::Vec3 restored_slow = slow.position();
    const engine_core::Vec3 restored_fast = fast.position();
    REQUIRE(std::fabs(restored_slow.x - (-0.58f)) < 1e-4f);
    REQUIRE(std::fabs(restored_slow.y - 0.38f) < 1e-4f);
    REQUIRE(std::fabs(restored_slow.z) < 1e-4f);
    REQUIRE(std::fabs(restored_fast.x - 0.58f) < 1e-4f);
    REQUIRE(std::fabs(restored_fast.y - 0.38f) < 1e-4f);
    REQUIRE(std::fabs(restored_fast.z - 0.15f) < 1e-4f);
    REQUIRE(rig.model.name(rig.model.find_first_child(rig.model.id(), "HopSlow")) == "HopSlow");
}

TEST_CASE("S13 print and errors reach the log and a new start clears it", "[S13]") {
    ScriptRig rig;
    add_script(rig.model, "Main", R"(
        print("hello", 2)
        pcall(function() error("hidden") end)
        error("boom")
    )");
    rig.runtime.append_output(engine_core::ScriptRuntime::OutputKind::Print, "stale-before");
    rig.model.start_simulation();
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

    rig.model.stop_simulation();
    rig.model.start_simulation();
    rig.frames(1);
    rig.model.stop_simulation();
    rig.model.start_simulation();
    const engine_core::ScriptRuntime::OutputBatch wiped = rig.runtime.drain_output();
    REQUIRE(wiped.lines.empty());
    REQUIRE(wiped.epoch > first.epoch);
}

TEST_CASE("S15 a console chunk uses the play VM", "[S15]") {
    ScriptRig rig;
    rig.model.start_simulation();
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
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Print && line.text == "DataModel\n") {
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
    add_script(rig.model, "Bad", "print(");
    rig.model.start_simulation();
    rig.frames(1);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(!batch.lines.empty());
    REQUIRE(batch.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Error);
    REQUIRE(!rig.runtime.last_error().empty());
}

TEST_CASE("S16 the console sees game while the simulation is stopped", "[S16]") {
    ScriptRig rig;
    rig.model.set_name(0, "Place");
    rig.runtime.run_chunk("print(game)");
    rig.runtime.run_chunk("print(game.Name)");
    const engine_core::ScriptRuntime::OutputBatch before = rig.runtime.drain_output();
    REQUIRE(before.lines.size() == 2);
    REQUIRE(before.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Print);
    REQUIRE(before.lines[0].text == "Place\n");
    REQUIRE(before.lines[1].text == "Place\n");
    REQUIRE_FALSE(rig.runtime.vm_open());

    rig.model.start_simulation();
    rig.runtime.run_chunk("print(game.Name)");
    const engine_core::ScriptRuntime::OutputBatch playing = rig.runtime.drain_output();
    REQUIRE(playing.lines.size() == 1);
    REQUIRE(playing.lines[0].text == "Place\n");

    rig.model.stop_simulation();
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
    add_script(rig.model, "HopSlow", R"(
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
    add_script(rig.model, "Nested", R"(
        local rs = game:GetService("RunService")
        rs.Heartbeat:Connect(function()
            local seen = _G.inside or 0
            _G.inside = seen + 1
            if seen == 0 then
                _G.from_inside = rs.Heartbeat:Wait()
            end
        end)
    )");
    rig.model.start_simulation();
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
    add_script(rig.model, "Sub", R"(
        _G.dt = game:GetService("RunService").PreSimulation:Wait()
    )");
    rig.model.start_simulation();
    rig.frames(1, 0.05);
    double dt = 0;
    REQUIRE_FALSE(rig.runtime.global_number("dt", dt));
    rig.scheduler.run_phase(engine_core::Phase::PreSimulation, 0.01);
    rig.model.events().drain();
    REQUIRE(rig.runtime.global_number("dt", dt));
    REQUIRE(std::fabs(dt - 0.01) < 1e-9);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S20 Changed:Wait returns the property name", "[S20]") {
    ScriptRig rig;
    add_part(rig.model, rig.model.id(), "P");
    add_script(rig.model, "Watch", R"(
        local part = game:FindFirstChild("P")
        task.spawn(function()
            part.Name = "Next"
        end)
        local field = part.Changed:Wait()
        _G.ok = (field == "Name")
    )");
    rig.model.start_simulation();
    rig.frames(1, 0.05);
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
    REQUIRE(rig.model.find_first_child(rig.model.id(), "Next") != 0);
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("S21 game.Changed reports the root property", "[S21]") {
    ScriptRig rig;
    engine_core::GameObject& part = add_part(rig.model, rig.model.id(), "P");
    add_script(rig.model, "Watch", R"(
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
    rig.model.start_simulation();
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
    REQUIRE(rig.model.name(rig.model.id()) == "Place");
    REQUIRE(rig.model.name(part.id()) == "Q");
    REQUIRE(rig.runtime.last_error().empty());

    rig.model.stop_simulation();
    REQUIRE(rig.model.name(rig.model.id()) == "DataModel");
    REQUIRE(rig.model.name(part.id()) == "P");
    rig.model.start_simulation();
    rig.frames(1, 0.05);
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
    REQUIRE(rig.runtime.global_boolean("waited", waited));
    REQUIRE(waited);
    REQUIRE(rig.model.name(rig.model.id()) == "Place");
    REQUIRE(rig.runtime.last_error().empty());
}

TEST_CASE("Folder stores other instances", "[folder]") {
    ScriptRig rig;
    engine_core::DataModel& model = rig.model;
    REQUIRE(engine_core::lua_class_known("Folder"));
    REQUIRE(engine_core::lua_class_inherits("Folder", "DataModel"));

    engine_core::Folder& props = model.create<engine_core::Folder>();
    const engine_core::InstanceId props_id = props.id();
    REQUIRE(std::string(props.class_name()) == "Folder");
    REQUIRE(model.name(props_id) == "Folder");
    REQUIRE(model.game_object(props_id) == nullptr);
    model.set_name(props_id, "Props");
    model.set_parent(props_id, model.id());

    engine_core::Folder& inner = model.create<engine_core::Folder>();
    const engine_core::InstanceId inner_id = inner.id();
    model.set_name(inner_id, "Inner");
    model.set_parent(inner_id, props_id);

    engine_core::GameObject& box = model.create<engine_core::GameObject>();
    const engine_core::InstanceId box_id = box.id();
    model.set_name(box_id, "Box");
    model.set_parent(box_id, inner_id);
    REQUIRE(model.parent(box_id) == inner_id);
    REQUIRE(model.parent(inner_id) == props_id);
    REQUIRE(model.find_first_child(inner_id, "Box") == box_id);

    int bodies = 0;
    model.for_each_game_object([&](const engine_core::GameObject& item) {
        REQUIRE(item.id() == box_id);
        ++bodies;
    });
    REQUIRE(bodies == 1);

    model.capture_place();
    model.start_simulation();
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
    const engine_core::InstanceId session_id = model.find_first_child(model.id(), "Session");
    REQUIRE(session_id != 0);
    REQUIRE(std::string(model.instance(session_id)->class_name()) == "Folder");
    REQUIRE(model.find_first_child(session_id, "Loose") != 0);
    REQUIRE(model.parent(box_id) == model.id());
    REQUIRE(model.name(props_id) == "Renamed");

    model.stop_simulation();
    REQUIRE(model.alive(props_id));
    REQUIRE(model.alive(inner_id));
    REQUIRE(model.alive(box_id));
    REQUIRE_FALSE(model.alive(session_id));
    REQUIRE(std::string(model.instance(props_id)->class_name()) == "Folder");
    REQUIRE(std::string(model.instance(inner_id)->class_name()) == "Folder");
    REQUIRE(model.name(props_id) == "Props");
    REQUIRE(model.name(inner_id) == "Inner");
    REQUIRE(model.parent(props_id) == model.id());
    REQUIRE(model.parent(inner_id) == props_id);
    REQUIRE(model.parent(box_id) == inner_id);
    REQUIRE(model.find_first_child(model.id(), "Session") == 0);
    REQUIRE(model.game_object(props_id) == nullptr);
    REQUIRE(model.game_object(inner_id) == nullptr);
}

TEST_CASE("S22 a script created during play stays parented to game", "[S22]") {
    ScriptRig rig;
    add_script(rig.model, "Maker", R"lua(
        local made = Instance.new("Script")
        made.Name = "Spawned"
        made.Source = "print('from spawned')"
        made.Parent = game
        local also = Instance.new("Script", game)
        also.Name = "FromNew"
        also.Source = "print('from new')"
    )lua");
    rig.model.start_simulation();
    rig.frames(2, 0.05);
    const engine_core::InstanceId spawned = rig.model.find_first_child(rig.model.id(), "Spawned");
    const engine_core::InstanceId from_new = rig.model.find_first_child(rig.model.id(), "FromNew");
    REQUIRE(spawned != 0);
    REQUIRE(from_new != 0);
    REQUIRE(std::string(rig.model.instance(spawned)->class_name()) == "Script");
    REQUIRE(rig.model.parent(spawned) == rig.model.id());
    REQUIRE(rig.model.parent(from_new) == rig.model.id());
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

    rig.model.stop_simulation();
    REQUIRE_FALSE(rig.model.alive(spawned));
    REQUIRE_FALSE(rig.model.alive(from_new));
    REQUIRE(rig.model.find_first_child(rig.model.id(), "Spawned") == 0);
    REQUIRE(rig.model.find_first_child(rig.model.id(), "FromNew") == 0);
}
