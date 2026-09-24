#include "Contract.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

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

class CountingRenderer : public engine_core::IRenderer {
public:
    void perform(const engine_core::VisualSnapshot&, int) override {}
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
        engine_core::DataModel* model = nullptr;
        engine_core::InstanceId id = 0;
        std::atomic<int> hits{0};
        std::string message;
        void perform(const engine_core::VisualSnapshot&, int) override {
            if (hits.load() != 0) {
                return;
            }
            try {
                model->set_color(id, engine_core::ColorRgb{});
            } catch (const engine_core::ContractViolation& ex) {
                message = ex.what();
                hits.store(1);
            }
        }
        void present() override {}
    } probe;
    probe.id = engine.datamodel().create_part();
    probe.model = &engine.datamodel();
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
    const engine_core::InstanceId id = engine.datamodel().create_part();
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
            engine.datamodel().set_color(id, color);
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
    const engine_core::InstanceId id = engine.datamodel().create_part();
    const engine_core::Transform expected = T0();
    std::atomic<int> ready{0};
    engine_core::Transform snapped{};
    engine_core::WriteOrigin origin = engine_core::WriteOrigin::SnapshotOverride;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        engine.datamodel().set_transform(id, expected);
    });
    struct Probe : CountingRenderer {
        engine_core::Engine* engine = nullptr;
        engine_core::InstanceId id = 0;
        const engine_core::Transform* expected = nullptr;
        engine_core::Transform* snapped = nullptr;
        engine_core::WriteOrigin* origin = nullptr;
        std::atomic<int>* ready = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot, int) override {
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
    REQUIRE(near(engine.datamodel().transform(id), expected));
    REQUIRE(near(snapped, expected));
    REQUIRE(origin == engine_core::WriteOrigin::Simulation);
}

TEST_CASE("path C changes pixels for one frame only", "[T6]") {
    engine_core::Engine engine;
    const engine_core::InstanceId id = engine.datamodel().create_part();
    const engine_core::Transform sim = T0();
    const engine_core::Transform flash = T1();
    std::atomic<int> heartbeats{0};
    std::atomic<int> stage{0};
    engine_core::Transform live{};
    engine_core::Transform snap_override{};
    engine_core::Transform snap_next{};
    engine_core::WriteOrigin override_origin = engine_core::WriteOrigin::Simulation;
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        engine.datamodel().set_transform(id, sim);
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
        live = engine.datamodel().transform(id);
        stage.store(1);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Transform* snap_override = nullptr;
        engine_core::Transform* snap_next = nullptr;
        engine_core::WriteOrigin* override_origin = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot, int) override {
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
    REQUIRE(near(engine.datamodel().transform(id), sim));
}

TEST_CASE("path B on a visual-only part becomes sim truth", "[T7]") {
    engine_core::Engine engine;
    engine_core::DataModel& model = engine.datamodel();
    const engine_core::InstanceId id = model.create_part();
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
        model.set_transform(id, posed);
        live_at_write = model.transform(id);
        stage.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (stage.load() < 1) {
            return;
        }
        live_later = model.transform(id);
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
        void perform(const engine_core::VisualSnapshot& snapshot, int) override {
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
    const engine_core::InstanceId id = model.create_part();
    model.set_simulated(id, true);
    model.set_linear_velocity(id, 1.f, 0.f, 0.f);
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
        model.set_transform(id, posed);
        if (model.take_deferred_violation()) {
            rejected.store(1);
        }
        model.set_transform(id, posed, engine_core::ForceSimWrite{});
        live_at_write = model.transform(id);
        stage.store(1);
    });
    engine.scheduler().bind(engine_core::Phase::PostSimulation, [&](double) {
        if (stage.load() != 2) {
            return;
        }
        live_after_physics = model.transform(id);
        stage.store(3);
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* stage = nullptr;
        engine_core::Transform* snapped = nullptr;
        const engine_core::Transform* posed = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot, int) override {
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
    const engine_core::InstanceId id = engine.datamodel().create_part();
    std::atomic<int> destroyed{0};
    std::atomic<int> live_closed{0};
    std::atomic<int> snap_gone{0};
    engine.scheduler().bind(engine_core::Phase::Heartbeat, [&](double) {
        if (engine.sim_frame_count() > 1 && destroyed.load() == 0) {
            engine.datamodel().destroy(id);
            destroyed.store(1);
        }
        if (destroyed.load() == 1) {
            const bool closed = !engine.datamodel().alive(id) && is_zero(engine.datamodel().transform(id));
            if (closed) {
                live_closed.store(1);
            }
        }
    });
    struct Probe : CountingRenderer {
        engine_core::InstanceId id = 0;
        std::atomic<int>* destroyed = nullptr;
        std::atomic<int>* snap_gone = nullptr;
        void perform(const engine_core::VisualSnapshot& snapshot, int) override {
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
        ids[i] = model.create_part();
        model.set_simulated(ids[i], true);
    }
    // The rejection is the Prepare. Drop the create records so that Prepare
    // does not spend its budget copying parts that were never drawn.
    model.invalidations().clear();
    engine.scheduler().bind(engine_core::Phase::PreRender, [&](double) {
        // Every one of these parts is simulated. The first write is rejected,
        // so Prepare does not walk the rest of the list.
        for (engine_core::InstanceId id : ids) {
            model.set_transform(id, T1());
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
    REQUIRE(engine.steps().consume() == 0);
    engine.resume();
    REQUIRE_FALSE(engine.paused());
    wait_until([&] { return engine.sim_frame_count() > 0 && engine.steps().consume() > 0; });
    engine.pause();
    // Let the in-flight step finish, then confirm no further steps are published.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const std::uint64_t frames = engine.sim_frame_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    REQUIRE(engine.sim_frame_count() == frames);
    engine.stop();
}
