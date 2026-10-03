// Event arguments: values an event carries to its handlers.

#include "support.hpp"

#include "Contract.hpp"
#include "Events.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

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

}  // namespace

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
