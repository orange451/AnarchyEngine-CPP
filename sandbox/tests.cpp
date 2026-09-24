#include "Contract.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "GameObject.hpp"
#include "SnapshotPump.hpp"
#include "TestTriangle.hpp"
#include "Engine.hpp"
#include "Events.hpp"
#include "IClock.hpp"
#include "IRenderer.hpp"

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
