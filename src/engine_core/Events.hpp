#pragma once

#include "types.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace engine_core {

class TaskScheduler;
class EventQueue;

// Property identity for signals. Distinct from VisualField, which is the
// snapshot dirty mask. Parent and Name are not visual fields.
enum class Field : std::uint8_t {
    Transform = 0,
    Color,
    Size,
    LinearVelocity,
    Simulated,
    VisualOnly,
    Parent,
    Name,
    Source,
    Enabled,
    Count
};

enum class SignalKind : std::uint8_t {
    Changed = 0,
    PropertyChanged,
    ChildAdded,
    ChildRemoved,
    AncestryChanged
};

// Deferred: emit enqueues, handlers run at the next SimulationThread drain.
// Immediate: handlers run inside set_ on the simulation thread, cap 16.
enum class EventPolicy { Deferred, Immediate };

struct SignalId {
    std::uint32_t index = 0xffffffffu;
    std::uint32_t generation = 0;

    bool valid() const { return index != 0xffffffffu; }
};

using Handler = std::function<void(InstanceId, Field)>;

// A live subscription. Copying copies the handle. disconnect() is safe
// during drain: the slot is tombstoned and the generation bumps.
class Connection {
public:
    Connection() = default;

    void disconnect();
    bool connected() const;

private:
    friend class EventQueue;
    EventQueue* queue_ = nullptr;
    std::uint32_t index_ = 0xffffffffu;
    std::uint32_t generation_ = 0;
};

// One signal on one instance. The object inside the instance bag is canonical.
// Copies are handles and resolve through SignalId.
class Signal {
public:
    Connection connect(Handler handler);
    Connection once(Handler handler);
    // script == 0 is an ordinary C++ connection. A non-zero script tags the
    // slot with that Script instance and the start_generation captured here.
    Connection connect_scripted(Handler handler, InstanceId script, std::uint32_t script_generation, bool once);
    // Yields the current simulation job until the next firing.
    // Resumes at a later simulation phase. RenderThread cannot call this.
    void wait();

    SignalId id() const { return id_; }

private:
    friend class EventQueue;
    friend class DataModel;

    EventQueue* queue_ = nullptr;
    SignalId id_{};
    InstanceId owner_ = 0;
    SignalKind kind_ = SignalKind::Changed;
    Field field_ = Field::Transform;
    std::uint32_t head_ = 0xffffffffu;
    std::uint32_t tail_ = 0xffffffffu;
    int listeners_ = 0;

    bool bound() const { return queue_ != nullptr && id_.valid(); }
};

// SimulationThread drains. RenderThread may only enqueue (path B).
// Path C never calls emit. WriteOrigin is stored on each event.
class EventQueue {
public:
    EventQueue();

    void set_policy(EventPolicy policy);
    EventPolicy policy() const { return policy_; }

    void attach_scheduler(TaskScheduler* scheduler);
    TaskScheduler* scheduler() const { return scheduler_; }
    void watch_prerender(const bool* open);

    // 3-argument form matches the public contract. Origin defaults to
    // Simulation. SnapshotOverride is recorded and dropped.
    void emit(SignalId signal, InstanceId id, Field field, WriteOrigin origin = WriteOrigin::Simulation);

    // Runs handlers for events already queued. SimulationThread only.
    // Events enqueued by those handlers wait for the next drain.
    void drain();

    // Drops queued events for this id and disconnects its signals.
    void destroy_instance(InstanceId id);

    // Drops every queued event. Connections stay until disconnect_all().
    void drop_pending();
    // Tombstones every connection. Signal objects stay so a later connect works.
    void disconnect_all();
    // Tombstones connections tagged with this Script instance.
    void disconnect_script(InstanceId script);
    // Tombstones every script-tagged connection. Untagged C++ connections stay.
    void disconnect_scripted();

    // Called after a drain batch, still on SimulationThread, while draining_ is set.
    // Used to resume scripts that the batch made Ready. Must not call drain().
    void set_after_drain(std::function<void()> hook);

    // Tagged slots fire only when this returns true. Null accepts every tag.
    using ScriptGate = bool (*)(InstanceId script, std::uint32_t script_generation, void* userdata);
    void set_script_gate(ScriptGate gate, void* userdata);

    // Host-owned signals (RunService). The Signal object must outlive the queue's use of it.
    void host_signal(Signal* signal);
    void release_signal(Signal& signal);

    std::uint64_t count(WriteOrigin origin) const;
    std::uint64_t suppressed_overrides() const { return suppressed_overrides_; }

    void shutdown();

private:
    friend class Signal;
    friend class Connection;
    friend class DataModel;

    struct Event {
        SignalId signal{};
        InstanceId instance = 0;
        InstanceId owner = 0;
        Field field = Field::Transform;
        WriteOrigin origin = WriteOrigin::Simulation;
        bool live = true;
    };

    struct ConnSlot {
        std::uint32_t generation = 1;
        bool live = false;
        bool once = false;
        std::uint32_t signal_index = 0xffffffffu;
        std::uint32_t prev = 0xffffffffu;
        std::uint32_t next = 0xffffffffu;
        std::uint64_t min_invoke = 0;
        std::uint64_t min_drain = 0;
        InstanceId script = 0;
        std::uint32_t script_generation = 0;
        Handler handler;
    };

    struct SignalSlot {
        Signal* signal = nullptr;
        std::uint32_t generation = 1;
    };

    void register_signal(Signal* signal);
    void unregister_signal(Signal& signal);
    Connection connect_to(SignalId id, Handler handler, bool once, InstanceId script, std::uint32_t script_generation);
    void disconnect_slot(std::uint32_t index, std::uint32_t generation);
    void tombstone(std::uint32_t index);
    Signal* resolve(SignalId id);
    const Signal* resolve(SignalId id) const;
    bool eligible(const ConnSlot& slot) const;
    void invoke(const Event& event);
    void enqueue(const Event& event);
    Event pop();
    void seal_instance(InstanceId id);
    void invoke_connections(Signal& signal, const Event& event);

    EventPolicy policy_ = EventPolicy::Deferred;
    TaskScheduler* scheduler_ = nullptr;
    const bool* prerender_open_ = nullptr;

    std::vector<SignalSlot> signal_slots_;
    std::vector<std::uint32_t> free_signals_;
    std::vector<ConnSlot> conns_;
    std::vector<std::uint32_t> free_conns_;
    std::vector<Event> events_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;

    bool draining_ = false;
    std::function<void()> after_drain_;
    ScriptGate script_gate_ = nullptr;
    void* script_gate_ud_ = nullptr;
    int invoke_depth_ = 0;
    int immediate_depth_ = 0;
    std::uint64_t invoke_epoch_ = 0;
    std::uint64_t drain_serial_ = 0;
    std::uint64_t counts_[3] = {};
    std::uint64_t suppressed_overrides_ = 0;
    // Snapshot of connection indices for the active invoke. Nested invokes append.
    std::vector<std::uint32_t> invoke_list_;
};

}  // namespace engine_core
