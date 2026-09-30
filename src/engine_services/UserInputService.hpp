#pragma once

#include "Events.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace engine_core {

// One keyboard or mouse event, as a script's InputObject reads it. The enum
// fields hold Roblox values: KeyCode, UserInputType, and UserInputState.
struct InputRecord {
    int type = 22;  // UserInputType.None
    int state = 4;  // UserInputState.None
    int key = 0;    // KeyCode.Unknown
    // Pointer position in the scene view, in points from its top-left corner.
    // z is 0, except for MouseWheel, where z of position is the wheel movement.
    Vec3 position{};
    Vec3 delta{};
    // True when the studio, not the game, took the event.
    bool processed = false;
};

// game:GetService("UserInputService"). Keys and the mouse, as Roblox's
// UserInputService gives them: InputBegan, InputChanged, and InputEnded fire
// with an InputObject and gameProcessedEvent, and IsKeyDown and the other
// queries answer from the events delivered so far.
//
// The studio posts from the UI thread. Posts wait in a queue behind their own
// lock, never the DataModel lock, and are only kept while the service is active,
// which is while the place is playing. SimulationThread dispatches the queue once
// per step, before PreAnimation's handlers run, so every script sees one frame's
// input at the same point.
class UserInputService {
public:
    // UserInputType values.
    static constexpr int kMouseButton1 = 0;
    static constexpr int kMouseButton2 = 1;
    static constexpr int kMouseButton3 = 2;
    static constexpr int kMouseWheel = 3;
    static constexpr int kMouseMovement = 4;
    static constexpr int kKeyboard = 8;
    static constexpr int kFocus = 9;
    // UserInputState values.
    static constexpr int kBegin = 0;
    static constexpr int kChange = 1;
    static constexpr int kEnd = 2;
    static constexpr int kCancel = 3;
    // MouseBehavior values.
    static constexpr int kMouseBehaviorDefault = 0;
    static constexpr int kLockCenter = 1;
    static constexpr int kLockCurrentPosition = 2;

    enum class Kind { Began, Changed, Ended };

    UserInputService() = default;
    UserInputService(const UserInputService&) = delete;
    UserInputService& operator=(const UserInputService&) = delete;

    // The KeyCode value for a GLFW key number. JadeFX forwards GLFW's numbers
    // unchanged. KeyCode.Unknown (0) when the key has no KeyCode.
    static int key_code_from_glfw(int glfw_key);

    // Any thread. While inactive, posts are dropped. Either way the queue and
    // what the posts left down are cleared, so nothing carries into a session.
    void set_active(bool active);
    bool active() const;

    // Any thread. A repeated press of a key already down, or a release of one
    // that is not, is dropped. button is 0, 1, or 2 for MouseButton1 to 3.
    void post_key(int key_code, bool down, bool processed = false);
    void post_mouse_button(int button, bool down, float x, float y, bool processed = false);
    void post_mouse_move(float x, float y, bool processed = false);
    // Motion while the pointer is locked: a MouseMovement whose Delta is the
    // motion, at the mouse location as it was. The location does not move.
    void post_mouse_delta(float dx, float dy, bool processed = false);
    void post_wheel(float x, float y, float amount, bool processed = false);
    // The scene view lost keyboard focus: every key and button still down ends, and
    // MouseBehavior goes back to Default, since the view let the pointer go.
    void post_focus_lost();

    // SimulationThread. Between bind and release the signals belong to one
    // world's EventQueue, and they must outlive that use.
    void bind(EventQueue& events);
    void release(EventQueue& events);
    Signal* signal(Kind kind);

    // SimulationThread. Applies every queued record to the state the queries
    // read, then queues one signal event for each. A handler finds its record
    // through record(), keyed by the EventQueue payload it was emitted with.
    void dispatch(EventQueue& events);
    // Null when the payload is not from the latest dispatch.
    const InputRecord* record(std::uint64_t payload) const;

    // SimulationThread. What the dispatched events add up to.
    bool key_down(int key_code) const;
    bool button_down(int button) const;
    // Keys in the order they went down.
    const std::vector<int>& keys_down() const { return keys_down_; }
    Vec3 mouse_location() const { return mouse_; }

    // SimulationThread. The Delta of every MouseMovement in the latest dispatch,
    // added up and not scaled. GetMouseDelta scales it by the sensitivity.
    Vec3 mouse_delta() const { return mouse_delta_; }

    // Any thread. What scripts asked of the pointer. The scene view reads it
    // each paint and locks the pointer while it is not Default.
    int mouse_behavior() const { return mouse_behavior_.load(std::memory_order_relaxed); }
    void set_mouse_behavior(int behavior) { mouse_behavior_.store(behavior, std::memory_order_relaxed); }

    // SimulationThread. Clamped to 0 and up. False, changing nothing, when not finite.
    double mouse_delta_sensitivity() const { return mouse_delta_sensitivity_; }
    bool set_mouse_delta_sensitivity(double value);

    // SimulationThread. Forgets the dispatched state and records. The play
    // session calls this when it starts and stops.
    void reset();

private:
    void push_locked(const InputRecord& record);
    void end_held_locked();

    // Guards everything the studio writes.
    mutable std::mutex mu_;
    bool active_ = false;
    std::vector<InputRecord> queue_;
    // What the posts so far have left down. The studio side of the state, used
    // to drop repeats and to end everything on a focus loss.
    std::vector<int> posted_keys_;
    bool posted_buttons_[3] = {};
    Vec3 posted_mouse_{};
    // False until the session's first move: nothing before it to measure from.
    bool posted_mouse_known_ = false;

    // SimulationThread only.
    std::vector<InputRecord> dispatched_;
    std::uint64_t first_payload_ = 1;
    std::uint64_t next_payload_ = 1;
    std::vector<int> keys_down_;
    bool buttons_down_[3] = {};
    Vec3 mouse_{};
    Vec3 mouse_delta_{};
    double mouse_delta_sensitivity_ = 1.0;

    Signal began_;
    Signal changed_;
    Signal ended_;
    bool bound_ = false;
    std::atomic<int> mouse_behavior_{kMouseBehaviorDefault};
};

}  // namespace engine_core
