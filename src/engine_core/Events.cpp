#include "Events.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "Ring.hpp"
#include "TaskScheduler.hpp"

#include <algorithm>
#include <utility>

namespace engine_core {
namespace {

constexpr std::uint32_t kNone = 0xffffffffu;
constexpr std::size_t kEventCapacity = 8192;
constexpr std::size_t kConnCapacity = 4096;
constexpr int kImmediateCap = 16;

int origin_index(WriteOrigin origin) {
    return static_cast<int>(origin);
}

struct FlagGuard {
    bool& flag;
    explicit FlagGuard(bool& flag) : flag(flag) { flag = true; }
    ~FlagGuard() { flag = false; }
};

struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& depth) : depth(depth) { ++depth; }
    ~DepthGuard() { --depth; }
};

}  // namespace

EventQueue::EventQueue() {
    signal_slots_.reserve(1024);
    conns_.reserve(kConnCapacity);
    events_.assign(kEventCapacity, Event{});
    invoke_list_.reserve(256);
}

void EventQueue::set_policy(EventPolicy policy) { policy_ = policy; }

void EventQueue::attach_scheduler(TaskScheduler* scheduler) { scheduler_ = scheduler; }

void EventQueue::watch_prerender(const bool* open) { prerender_open_ = open; }

std::uint64_t EventQueue::count(WriteOrigin origin) const {
    const int index = origin_index(origin);
    if (index < 0 || index > 2) {
        return 0;
    }
    return counts_[index];
}

Signal* EventQueue::resolve(SignalId id) {
    if (!id.valid() || id.index >= signal_slots_.size()) {
        return nullptr;
    }
    SignalSlot& slot = signal_slots_[id.index];
    if (slot.generation != id.generation || slot.signal == nullptr) {
        return nullptr;
    }
    return slot.signal;
}

const Signal* EventQueue::resolve(SignalId id) const {
    return const_cast<EventQueue*>(this)->resolve(id);
}

void EventQueue::register_signal(Signal* signal) {
    if (signal == nullptr) {
        contract_fail("null signal");
    }
    std::uint32_t index = 0;
    if (!free_signals_.empty()) {
        index = free_signals_.back();
        free_signals_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(signal_slots_.size());
        signal_slots_.push_back(SignalSlot{});
    }
    SignalSlot& slot = signal_slots_[index];
    slot.signal = signal;
    signal->id_.index = index;
    signal->id_.generation = slot.generation;
    signal->queue_ = this;
}

void EventQueue::unregister_signal(Signal& signal) {
    if (!signal.bound() || signal.queue_ != this) {
        return;
    }
    std::uint32_t index = signal.head_;
    while (index != kNone) {
        const std::uint32_t next = conns_[index].next;
        tombstone(index);
        index = next;
    }
    signal.head_ = kNone;
    signal.tail_ = kNone;
    signal.listeners_ = 0;
    if (signal.id_.index < signal_slots_.size()) {
        SignalSlot& slot = signal_slots_[signal.id_.index];
        if (slot.generation == signal.id_.generation) {
            slot.signal = nullptr;
            if (slot.generation != 0xffffffffu) {
                ++slot.generation;
            } else {
                slot.generation = 1;
            }
            free_signals_.push_back(signal.id_.index);
        }
    }
    signal.id_ = SignalId{};
    signal.queue_ = nullptr;
}

void EventQueue::tombstone(std::uint32_t index) {
    if (index >= conns_.size()) {
        return;
    }
    ConnSlot& slot = conns_[index];
    if (!slot.live) {
        return;
    }
    const std::uint32_t prev = slot.prev;
    const std::uint32_t next = slot.next;
    const std::uint32_t signal_index = slot.signal_index;
    if (signal_index < signal_slots_.size() && signal_slots_[signal_index].signal != nullptr) {
        Signal* signal = signal_slots_[signal_index].signal;
        if (prev != kNone) {
            conns_[prev].next = next;
        } else {
            signal->head_ = next;
        }
        if (next != kNone) {
            conns_[next].prev = prev;
        } else {
            signal->tail_ = prev;
        }
        if (signal->listeners_ > 0) {
            --signal->listeners_;
        }
    }
    slot.live = false;
    slot.once = false;
    slot.handler = nullptr;
    slot.script = 0;
    slot.script_generation = 0;
    slot.kept = false;
    slot.prev = kNone;
    slot.next = kNone;
    slot.signal_index = kNone;
    if (slot.generation != 0xffffffffu) {
        ++slot.generation;
    } else {
        slot.generation = 1;
    }
    free_conns_.push_back(index);
}

void EventQueue::disconnect_slot(std::uint32_t index, std::uint32_t generation) {
    if (index >= conns_.size()) {
        return;
    }
    ConnSlot& slot = conns_[index];
    if (!slot.live || slot.generation != generation) {
        return;
    }
    tombstone(index);
}

Connection EventQueue::connect_to(SignalId id, Handler handler, bool once, InstanceId script,
                                  std::uint32_t script_generation, bool kept) {
    Signal* signal = resolve(id);
    if (signal == nullptr) {
        contract_fail("connect on a dead signal");
    }
    std::uint32_t index = 0;
    if (!free_conns_.empty()) {
        index = free_conns_.back();
        free_conns_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(conns_.size());
        conns_.push_back(ConnSlot{});
    }
    ConnSlot& slot = conns_[index];
    slot.live = true;
    slot.once = once;
    slot.script = script;
    slot.script_generation = script_generation;
    slot.kept = kept;
    slot.signal_index = id.index;
    slot.handler = std::move(handler);
    if (draining_) {
        // Eligible on the next drain() call, not this one, and not an
        // immediate fire that sneaks in before that drain.
        slot.min_drain = drain_serial_ + 1;
        slot.min_invoke = 0;
    } else if (invoke_depth_ > 0) {
        slot.min_drain = 0;
        slot.min_invoke = invoke_epoch_ + 1;
    } else {
        slot.min_drain = 0;
        slot.min_invoke = 0;
    }
    slot.prev = signal->tail_;
    slot.next = kNone;
    if (signal->tail_ != kNone) {
        conns_[signal->tail_].next = index;
    } else {
        signal->head_ = index;
    }
    signal->tail_ = index;
    ++signal->listeners_;

    Connection connection;
    connection.queue_ = this;
    connection.index_ = index;
    connection.generation_ = slot.generation;
    return connection;
}

bool EventQueue::eligible(const ConnSlot& slot) const {
    return slot.live && slot.min_invoke <= invoke_epoch_ && slot.min_drain <= drain_serial_;
}

void EventQueue::invoke(const Event& event) {
    Signal* signal = resolve(event.signal);
    if (signal == nullptr) {
        return;
    }
    // Immediate events nest, so the outer event's payload and values come back
    // after, even when a handler throws.
    struct Restore {
        std::uint64_t& payload;
        const std::uint64_t outer;
        const EventArgs*& args;
        const EventArgs* const outer_args;
        ~Restore() {
            payload = outer;
            args = outer_args;
        }
    } restore{payload_, payload_, current_args_, current_args_};
    payload_ = event.payload;
    current_args_ = event.args.get();
    invoke_connections(*signal, event);
}

void EventQueue::invoke_connections(Signal& signal, const Event& event) {
    // Touch the owner so a destroyed bag faults under ASan if this signal dangles.
    const InstanceId owner = signal.owner_;
    (void)owner;
    const std::size_t begin = invoke_list_.size();
    // A handler that throws would otherwise leave this call's entries behind for good.
    struct Trim {
        std::vector<std::uint32_t>& list;
        const std::size_t size;
        ~Trim() { list.resize(size); }
    } trim{invoke_list_, begin};
    for (std::uint32_t index = signal.head_; index != kNone; index = conns_[index].next) {
        invoke_list_.push_back(index);
    }
    const std::size_t end = invoke_list_.size();
    for (std::size_t cursor = begin; cursor < end; ++cursor) {
        const std::uint32_t index = invoke_list_[cursor];
        if (index >= conns_.size() || !eligible(conns_[index])) {
            continue;
        }
        if (conns_[index].script != 0 && script_gate_ != nullptr &&
            !script_gate_(conns_[index].script, conns_[index].script_generation, script_gate_ud_)) {
            continue;
        }
        Handler handler = conns_[index].handler;
        const bool once = conns_[index].once;
        if (once) {
            tombstone(index);
        }
        if (handler) {
            handler(event.instance, event.field);
        }
    }
}

// The render thread's synchronous counterpart to invoke(): no queued Event,
// no drain, no invoke_epoch_ bookkeeping (nothing here is deferred, so there
// is nothing for a later drain to pick up). A host signal's slots are walked
// directly off signal.head_/ConnSlot::next, mirroring invoke's save-restore
// and once-tombstoning, because drain() refuses to run inside the prerender
// window and this is the only other door in.
void EventQueue::invoke_render(Signal& signal, bool include_tagged) {
    if (prerender_open_ == nullptr || !*prerender_open_) {
        contract_fail("invoke_render runs inside RenderStepped or PreRender");
    }
    // Same payload_/current_args_ save-and-restore as invoke(), just with
    // nothing to restore to but zero/null: a host signal carries neither.
    struct Restore {
        std::uint64_t& payload;
        const std::uint64_t outer;
        const EventArgs*& args;
        const EventArgs* const outer_args;
        ~Restore() {
            payload = outer;
            args = outer_args;
        }
    } restore{payload_, payload_, current_args_, current_args_};
    payload_ = 0;
    current_args_ = nullptr;

    // Snapshot (index, generation) pairs from head_ up to the tail as it
    // stood when the walk began, before any handler runs. A connect from
    // inside a handler appends past this captured tail, so it is visited on
    // a later invoke_render call, not this one. Keeping the generation lets
    // the loop below notice a slot that was tombstoned and its index handed
    // to a brand new connection by a handler earlier in this same walk.
    struct Visit {
        std::uint32_t index;
        std::uint32_t generation;
    };
    std::vector<Visit> visits;
    const std::uint32_t tail = signal.tail_;
    std::uint32_t cursor = signal.head_;
    while (cursor != kNone) {
        visits.push_back(Visit{cursor, conns_[cursor].generation});
        if (cursor == tail) {
            break;
        }
        cursor = conns_[cursor].next;
    }

    for (const Visit& visit : visits) {
        if (visit.index >= conns_.size()) {
            continue;
        }
        ConnSlot& slot = conns_[visit.index];
        if (!slot.live || slot.generation != visit.generation) {
            continue;
        }
        if (slot.script != 0) {
            if (!include_tagged) {
                continue;
            }
            if (script_gate_ != nullptr && !script_gate_(slot.script, slot.script_generation, script_gate_ud_)) {
                continue;
            }
        }
        Handler handler = slot.handler;
        const bool once = slot.once;
        if (once) {
            tombstone(visit.index);
        }
        if (handler) {
            // Matches RunService::fire's events.emit(target->id(), 0,
            // Field::Name): a host signal's own field_ is never what it fires
            // with, so this does not read it.
            handler(0, Field::Name);
        }
    }
}

void EventQueue::enqueue(const Event& event) {
    if (size_ == events_.size()) {
        grow();
    }
    events_[tail_] = event;
    tail_ = (tail_ + 1) % events_.size();
    ++size_;
}

// A script can queue more events in one step than the queue first held, as by
// parenting thousands of instances under a parent with a ChildAdded listener.
// The ring doubles. Nothing holds a reference into it across a handler.
void EventQueue::grow() { grow_ring(events_, head_, tail_, size_, kEventCapacity); }

EventQueue::Event EventQueue::pop() {
    Event event = events_[head_];
    events_[head_] = Event{};
    head_ = (head_ + 1) % events_.size();
    --size_;
    return event;
}

void EventQueue::emit(SignalId signal, InstanceId id, Field field, WriteOrigin origin, std::uint64_t payload) {
    if (origin == WriteOrigin::SnapshotOverride) {
        ++suppressed_overrides_;
        return;
    }
    Signal* live = resolve(signal);
    if (live == nullptr || live->listeners_ <= 0) {
        return;
    }
    const int origin_slot = origin_index(origin);
    if (origin_slot >= 0 && origin_slot <= 2) {
        ++counts_[origin_slot];
    }
    Event event;
    event.signal = signal;
    event.instance = id;
    event.owner = live->owner_;
    event.field = field;
    event.origin = origin;
    event.payload = payload;
    event.live = true;
    post(event);
}

void EventQueue::emit_args(SignalId signal, InstanceId id, EventArgs args) {
    Signal* live = resolve(signal);
    if (live == nullptr || live->listeners_ <= 0) {
        return;
    }
    ++counts_[origin_index(WriteOrigin::Simulation)];
    Event event;
    event.signal = signal;
    event.instance = id;
    event.owner = live->owner_;
    event.field = Field::Reflected;
    event.live = true;
    if (!args.empty()) {
        event.args = std::make_shared<const EventArgs>(std::move(args));
    }
    post(event);
}

std::size_t EventQueue::queued_with_args() const {
    std::size_t count = 0;
    std::size_t index = head_;
    for (std::size_t n = 0; n < size_; ++n) {
        if (events_[index].args) {
            ++count;
        }
        index = (index + 1) % events_.size();
    }
    return count;
}

void EventQueue::post(const Event& event) {
    const bool on_sim = thread_role() == ThreadRole::Simulation;
    const bool run_now = policy_ == EventPolicy::Immediate && on_sim && immediate_depth_ < kImmediateCap;
    if (!run_now) {
        enqueue(event);
        return;
    }
    DepthGuard depth(immediate_depth_);
    DepthGuard invoking(invoke_depth_);
    ++invoke_epoch_;
    invoke(event);
}

void EventQueue::drain() {
    if (thread_role() != ThreadRole::Simulation) {
        contract_fail("EventQueue::drain runs on SimulationThread");
    }
    if (prerender_open_ != nullptr && *prerender_open_) {
        contract_fail("EventQueue::drain during RenderStepped or PreRender");
    }
    if (draining_) {
        return;
    }
    FlagGuard draining(draining_);
    ++drain_serial_;
    ++invoke_epoch_;
    const std::size_t batch = size_;
    for (std::size_t n = 0; n < batch; ++n) {
        Event event = pop();
        if (!event.live) {
            continue;
        }
        invoke(event);
    }
    if (after_drain_) {
        after_drain_();
    }
}

void EventQueue::seal_instance(InstanceId id) {
    if (events_.empty() || size_ == 0) {
        return;
    }
    std::size_t index = head_;
    for (std::size_t n = 0; n < size_; ++n) {
        Event& event = events_[index];
        if (event.live && (event.owner == id || event.instance == id)) {
            event.live = false;
            event.signal = SignalId{};
            event.args.reset();
        }
        index = (index + 1) % events_.size();
    }
}

void EventQueue::destroy_instance(InstanceId id) {
    seal_instance(id);
    for (SignalSlot& slot : signal_slots_) {
        if (slot.signal == nullptr || slot.signal->owner_ != id) {
            continue;
        }
        unregister_signal(*slot.signal);
    }
}

void EventQueue::drop_pending() {
    if (events_.empty() || size_ == 0) {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
        return;
    }
    std::size_t index = head_;
    for (std::size_t n = 0; n < size_; ++n) {
        events_[index] = Event{};
        index = (index + 1) % events_.size();
    }
    head_ = 0;
    tail_ = 0;
    size_ = 0;
}

void EventQueue::disconnect_all() {
    const std::uint32_t count = static_cast<std::uint32_t>(conns_.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        if (conns_[index].live && !conns_[index].kept) {
            tombstone(index);
        }
    }
}

void EventQueue::disconnect_script(InstanceId script) {
    if (script == 0) {
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(conns_.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        if (conns_[index].live && conns_[index].script == script) {
            tombstone(index);
        }
    }
}

void EventQueue::disconnect_scripted() {
    const std::uint32_t count = static_cast<std::uint32_t>(conns_.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        if (conns_[index].live && conns_[index].script != 0) {
            tombstone(index);
        }
    }
}

void EventQueue::set_after_drain(std::function<void()> hook) { after_drain_ = std::move(hook); }

void EventQueue::set_script_gate(ScriptGate gate, void* userdata) {
    script_gate_ = gate;
    script_gate_ud_ = userdata;
}

void EventQueue::host_signal(Signal* signal) { register_signal(signal); }

void EventQueue::release_signal(Signal& signal) { unregister_signal(signal); }

void EventQueue::shutdown() {
    for (ConnSlot& slot : conns_) {
        slot.live = false;
        slot.handler = nullptr;
    }
    free_conns_.clear();
    conns_.clear();
    for (SignalSlot& slot : signal_slots_) {
        if (slot.signal != nullptr) {
            slot.signal->head_ = kNone;
            slot.signal->tail_ = kNone;
            slot.signal->listeners_ = 0;
            slot.signal->queue_ = nullptr;
            slot.signal->id_ = SignalId{};
            slot.signal = nullptr;
        }
    }
    // Queued events let their values go with them.
    for (Event& event : events_) {
        event = Event{};
    }
    size_ = 0;
    head_ = 0;
    tail_ = 0;
}

Connection Signal::connect(Handler handler) {
    if (!bound()) {
        contract_fail("connect on a dead signal");
    }
    return queue_->connect_to(id_, std::move(handler), false, 0, 0);
}

Connection Signal::once(Handler handler) {
    if (!bound()) {
        contract_fail("connect on a dead signal");
    }
    return queue_->connect_to(id_, std::move(handler), true, 0, 0);
}

Connection Signal::connect_scripted(Handler handler, InstanceId script, std::uint32_t script_generation, bool once) {
    if (!bound()) {
        contract_fail("connect on a dead signal");
    }
    return queue_->connect_to(id_, std::move(handler), once, script, script_generation);
}

Connection Signal::connect_kept(Handler handler, bool once) {
    if (!bound()) {
        contract_fail("connect on a dead signal");
    }
    return queue_->connect_to(id_, std::move(handler), once, 0, 0, true);
}

void Signal::wait() {
    if (!bound() || queue_->scheduler() == nullptr || thread_role() != ThreadRole::Simulation ||
        !queue_->scheduler()->in_job()) {
        contract_fail("Signal::wait() yields the current simulation job only");
    }
    queue_->scheduler()->yield_for(*this);
}

void Connection::disconnect() {
    if (queue_ == nullptr) {
        return;
    }
    queue_->disconnect_slot(index_, generation_);
}

bool Connection::connected() const {
    if (queue_ == nullptr || index_ == kNone || index_ >= queue_->conns_.size()) {
        return false;
    }
    const EventQueue::ConnSlot& slot = queue_->conns_[index_];
    return slot.live && slot.generation == generation_;
}

}  // namespace engine_core
