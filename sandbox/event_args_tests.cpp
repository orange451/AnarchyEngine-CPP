// Event arguments: values an event carries to its handlers.

#include "support.hpp"

#include "Contract.hpp"
#include "DataModel.hpp"
#include "Enum.hpp"
#include "Events.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <string>
#include <vector>

namespace {

engine_core::LuaSlot number_slot(double value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

// A host signal on its own queue, as RunService and UserInputService keep theirs.
struct QueueRig {
    SimRole role;
    engine_core::EventQueue queue;
    engine_core::Signal signal;

    QueueRig() { queue.host_signal(&signal); }
    ~QueueRig() { queue.release_signal(signal); }
};

// A class whose only purpose is to declare events for these tests.
class EventProbe : public engine_core::DataModel {
public:
    EventProbe(engine_core::DataModel::ChildTag tag, engine_core::DataModel::State& state, engine_core::InstanceId id)
        : engine_core::DataModel(tag, state, id) {}
    const char* class_name() const override { return "EventProbe"; }
};

const engine_core::LuaParam kFiredArgs[] = {
    {"count", "number"}, {"where", "Vector3"}, {"key", "EnumItem"}, {"who", "Instance?"}};
const engine_core::LuaParam kStrictArgs[] = {{"who", "Instance"}};

engine_core::LuaSlot vec3_slot(float x, float y, float z) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Vec3;
    slot.vec = engine_core::Vec3{x, y, z};
    return slot;
}

engine_core::LuaSlot key_slot(int value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Enum;
    slot.enum_type = &engine_core::key_code_enum();
    slot.number = value;
    return slot;
}

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::LuaSlot nil_slot() { return engine_core::LuaSlot{}; }

// The four values Fired declares: 3, (1, 2, 3), KeyCode.W, and who.
engine_core::EventArgs fired_args(engine_core::LuaSlot who) {
    return {number_slot(3), vec3_slot(1, 2, 3), key_slot(119), std::move(who)};
}

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

// A probe named Probe under Workspace, and a script beside it, running in play.
struct ProbeRig {
    ScriptRig rig;
    engine_core::InstanceId probe = 0;

    explicit ProbeRig(const char* source) {
        EventProbe& made = rig.game.create<EventProbe>();
        probe = made.id();
        rig.game.set_name(probe, "Probe");
        rig.game.set_parent(probe, rig.game.scene_service("Workspace"));
        add_script(rig.game, "Listener", source);
        rig.game.start_simulation();
        rig.frames(1);
    }
};

}  // namespace

ANARCHY_LUA_REGISTER(register_event_probe_lua) {
    const engine_core::LuaField fields[] = {
        engine_core::lua_event("Fired", kFiredArgs, 4),
        engine_core::lua_event("Strict", kStrictArgs, 1),
        engine_core::lua_event("Bare"),
    };
    engine_core::register_lua_class("EventProbe", "Instance", fields, static_cast<int>(std::size(fields)));
}

TEST_CASE("EQ1 a handler reads the values its event was emitted with", "[EQ1]") {
    QueueRig rig;
    std::vector<double> seen;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        const engine_core::EventArgs* args = rig.queue.current_args();
        REQUIRE(args != nullptr);
        REQUIRE(args->size() == 2);
        seen.push_back((*args)[0].number);
        seen.push_back((*args)[1].number);
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1), number_slot(2)});
    REQUIRE(rig.queue.queued_with_args() == 1);
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{1, 2});
    REQUIRE(rig.queue.queued_with_args() == 0);
    REQUIRE(rig.queue.current_args() == nullptr);
}

TEST_CASE("EQ2 a handler that emits again keeps its own values, and the next drain delivers the new ones", "[EQ2]") {
    QueueRig rig;
    std::vector<double> seen;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        const double first = (*rig.queue.current_args())[0].number;
        if (first == 1) {
            rig.queue.emit_args(rig.signal.id(), 0, {number_slot(2)});
        }
        seen.push_back((*rig.queue.current_args())[0].number);
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1)});
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{1});
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{1, 2});
}

TEST_CASE("EQ3 two events in one drain each bring their own values", "[EQ3]") {
    QueueRig rig;
    std::vector<double> seen;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        seen.push_back((*rig.queue.current_args())[0].number);
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(5)});
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(6)});
    rig.queue.drain();
    REQUIRE(seen == std::vector<double>{5, 6});
}

TEST_CASE("EQ4 dropped events let their values go, and nothing is kept without a listener", "[EQ4]") {
    QueueRig rig;
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1)});
    REQUIRE(rig.queue.queued_with_args() == 0);

    engine_core::Connection connection = rig.signal.connect([](engine_core::InstanceId, engine_core::Field) {});
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(1)});
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(2)});
    REQUIRE(rig.queue.queued_with_args() == 2);
    rig.queue.drop_pending();
    REQUIRE(rig.queue.queued_with_args() == 0);
    connection.disconnect();
}

TEST_CASE("EQ5 the Immediate policy delivers the values at once", "[EQ5]") {
    QueueRig rig;
    rig.queue.set_policy(engine_core::EventPolicy::Immediate);
    double seen = 0;
    rig.signal.connect([&](engine_core::InstanceId, engine_core::Field) {
        seen = (*rig.queue.current_args())[0].number;
    });
    rig.queue.emit_args(rig.signal.id(), 0, {number_slot(9)});
    REQUIRE(seen == 9);
    REQUIRE(rig.queue.current_args() == nullptr);
}

TEST_CASE("EA5 firing with the wrong count or kind is a contract failure, even with nothing listening", "[EA5]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    const engine_core::InstanceId id = probe.id();
    REQUIRE_THROWS_AS(game.fire_event(id, "Fired", {number_slot(3)}), engine_core::ContractViolation);
    engine_core::EventArgs wrong = fired_args(nil_slot());
    wrong[1] = number_slot(1);
    REQUIRE_THROWS_AS(game.fire_event(id, "Fired", wrong), engine_core::ContractViolation);
    // An event declared with arguments, fired without them.
    REQUIRE_THROWS_AS(game.fire_event(id, "Fired"), engine_core::ContractViolation);
    // An event declared without arguments, fired with some.
    REQUIRE_THROWS_AS(game.fire_event(id, "Bare", {number_slot(1)}), engine_core::ContractViolation);
    REQUIRE_NOTHROW(game.fire_event(id, "Fired", fired_args(nil_slot())));
    REQUIRE_NOTHROW(game.fire_event(id, "Bare"));
}

TEST_CASE("EA5b an event the class does not declare does nothing, as before", "[EA5b]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Nope"));
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Nope", {number_slot(1)}));
}

TEST_CASE("EA6 nil passes only where the type ends in ?", "[EA6]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Fired", fired_args(nil_slot())));
    REQUIRE_NOTHROW(game.fire_event(probe.id(), "Strict", {instance_slot(probe.id())}));
    REQUIRE_THROWS_AS(game.fire_event(probe.id(), "Strict", {nil_slot()}), engine_core::ContractViolation);
}

TEST_CASE("EA7 destroying an instance lets its queued values go", "[EA7]") {
    SimRole role;
    engine_core::Game game;
    EventProbe& probe = game.create<EventProbe>();
    const engine_core::InstanceId id = probe.id();
    int calls = 0;
    game.event_signal(id, "Fired").connect([&](engine_core::InstanceId, engine_core::Field) { ++calls; });
    game.fire_event(id, "Fired", fired_args(nil_slot()));
    game.fire_event(id, "Fired", fired_args(nil_slot()));
    REQUIRE(game.events().queued_with_args() == 2);
    game.destroy(id);
    REQUIRE(game.events().queued_with_args() == 0);
    game.events().drain();
    REQUIRE(calls == 0);
}

TEST_CASE("EA8 the script checker types an event's callback from its declaration", "[EA8]") {
    const std::string definitions = engine_core::lua_analysis_definitions();
    INFO(definitions.substr(0, 400));
    REQUIRE(definitions.find("Fired: Signal_EventProbe_Fired") != std::string::npos);
    REQUIRE(definitions.find("count: number, where: Vector3") != std::string::npos);
    REQUIRE(definitions.find("Bare: Signal\n") != std::string::npos);
}

TEST_CASE("EA1 a Connect handler gets each value the event was fired with", "[EA1]") {
    ProbeRig probe(R"(
        workspace.Probe.Fired:Connect(function(count, where, key, who)
            print("fired", count, where.X, where.Y, where.Z, key == Enum.KeyCode.W, who == workspace.Probe, who == nil)
        end)
    )");
    probe.rig.game.fire_event(probe.probe, "Fired", fired_args(instance_slot(probe.probe)));
    probe.rig.frames(1);
    INFO(probe.rig.runtime.last_error());
    const auto output = probe.rig.runtime.drain_output();
    REQUIRE(has_line(output, "fired\t3\t1\t2\t3\ttrue\ttrue\tfalse\n"));

    probe.rig.game.fire_event(probe.probe, "Fired", fired_args(nil_slot()));
    probe.rig.frames(1);
    REQUIRE(has_line(probe.rig.runtime.drain_output(), "fired\t3\t1\t2\t3\ttrue\tfalse\ttrue\n"));
}

TEST_CASE("EA2 Wait returns the fired values", "[EA2]") {
    ProbeRig probe(R"(
        task.spawn(function()
            local count, where = workspace.Probe.Fired:Wait()
            print("waited", count, where.Y)
        end)
    )");
    probe.rig.game.fire_event(probe.probe, "Fired", fired_args(nil_slot()));
    probe.rig.frames(2);
    INFO(probe.rig.runtime.last_error());
    REQUIRE(has_line(probe.rig.runtime.drain_output(), "waited\t3\t2\n"));
}

TEST_CASE("EA3 two events fired in one step each reach Lua with their own values", "[EA3]") {
    ProbeRig probe(R"(
        workspace.Probe.Fired:Connect(function(count)
            print("count", count)
        end)
    )");
    engine_core::EventArgs first = fired_args(nil_slot());
    engine_core::EventArgs second = fired_args(nil_slot());
    second[0] = number_slot(4);
    probe.rig.game.fire_event(probe.probe, "Fired", first);
    probe.rig.game.fire_event(probe.probe, "Fired", second);
    probe.rig.frames(1);
    const auto output = probe.rig.runtime.drain_output();
    REQUIRE(has_line(output, "count\t3\n"));
    REQUIRE(has_line(output, "count\t4\n"));
}

TEST_CASE("EA3b an event without arguments still reaches Lua with none", "[EA3b]") {
    ProbeRig probe(R"(
        workspace.Probe.Bare:Connect(function(...)
            print("bare", select("#", ...))
        end)
    )");
    probe.rig.game.fire_event(probe.probe, "Bare");
    probe.rig.frames(1);
    REQUIRE(has_line(probe.rig.runtime.drain_output(), "bare\t0\n"));
}
