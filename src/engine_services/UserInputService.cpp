#include "UserInputService.hpp"

#include "LuaApi.hpp"

#include <algorithm>
#include <cstddef>

namespace engine_core {
namespace {

// A step that never runs, such as a paused test, must not grow the queue
// without bound. Past this many records, new presses and movement are dropped.
// Releases are always kept, so a key whose press got through never stays down.
constexpr std::size_t kMaxQueued = 256;

bool valid_button(int button) { return button >= 0 && button < 3; }

}  // namespace

int UserInputService::key_code_from_glfw(int glfw_key) {
    // Letters: GLFW has uppercase ASCII, KeyCode lowercase.
    if (glfw_key >= 65 && glfw_key <= 90) {
        return glfw_key + 32;
    }
    switch (glfw_key) {
    // Printable keys other than letters share their ASCII code.
    case 32:  // Space
    case 39:  // Quote
    case 44:  // Comma
    case 45:  // Minus
    case 46:  // Period
    case 47:  // Slash
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:  // Zero to Nine
    case 59:  // Semicolon
    case 61:  // Equals
    case 91:  // LeftBracket
    case 92:  // BackSlash
    case 93:  // RightBracket
    case 96:  // Backquote
        return glfw_key;
    case 256:
        return 27;  // Escape
    case 257:
        return 13;  // Return
    case 258:
        return 9;  // Tab
    case 259:
        return 8;  // Backspace
    case 260:
        return 277;  // Insert
    case 261:
        return 127;  // Delete
    case 262:
        return 275;  // Right
    case 263:
        return 276;  // Left
    case 264:
        return 274;  // Down
    case 265:
        return 273;  // Up
    case 266:
        return 280;  // PageUp
    case 267:
        return 281;  // PageDown
    case 268:
        return 278;  // Home
    case 269:
        return 279;  // End
    case 280:
        return 301;  // CapsLock
    case 281:
        return 302;  // ScrollLock
    case 282:
        return 300;  // NumLock
    case 283:
        return 316;  // Print
    case 284:
        return 19;  // Pause
    case 330:
        return 266;  // KeypadPeriod
    case 331:
        return 267;  // KeypadDivide
    case 332:
        return 268;  // KeypadMultiply
    case 333:
        return 269;  // KeypadMinus
    case 334:
        return 270;  // KeypadPlus
    case 335:
        return 271;  // KeypadEnter
    case 336:
        return 272;  // KeypadEquals
    case 340:
        return 304;  // LeftShift
    case 341:
        return 306;  // LeftControl
    case 342:
        return 308;  // LeftAlt
    case 343:
        return 311;  // LeftSuper
    case 344:
        return 303;  // RightShift
    case 345:
        return 305;  // RightControl
    case 346:
        return 307;  // RightAlt
    case 347:
        return 312;  // RightSuper
    case 348:
        return 319;  // Menu
    default:
        break;
    }
    // F1 to F15.
    if (glfw_key >= 290 && glfw_key <= 304) {
        return glfw_key - 290 + 282;
    }
    // Keypad digits.
    if (glfw_key >= 320 && glfw_key <= 329) {
        return glfw_key - 320 + 256;
    }
    return 0;
}

void UserInputService::set_active(bool active) {
    std::lock_guard<std::mutex> lock(mu_);
    active_ = active;
    queue_.clear();
    posted_keys_.clear();
    std::fill(std::begin(posted_buttons_), std::end(posted_buttons_), false);
    posted_mouse_known_ = false;
}

bool UserInputService::active() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
}

void UserInputService::push_locked(const InputRecord& record) {
    // Movement between two steps arrives as one Changed, as a frame of
    // Roblox mouse movement does. The deltas add up.
    if (record.type == kMouseMovement && !queue_.empty()) {
        InputRecord& last = queue_.back();
        if (last.type == kMouseMovement && last.processed == record.processed) {
            last.delta.x += record.delta.x;
            last.delta.y += record.delta.y;
            last.position = record.position;
            return;
        }
    }
    queue_.push_back(record);
}

void UserInputService::post_key(int key_code, bool down, bool processed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_ || key_code == 0 || (down && queue_.size() >= kMaxQueued)) {
        return;
    }
    const auto held = std::find(posted_keys_.begin(), posted_keys_.end(), key_code);
    if (down == (held != posted_keys_.end())) {
        return;
    }
    if (down) {
        posted_keys_.push_back(key_code);
    } else {
        posted_keys_.erase(held);
    }
    InputRecord record;
    record.type = kKeyboard;
    record.state = down ? kBegin : kEnd;
    record.key = key_code;
    record.position = posted_mouse_;
    record.processed = processed;
    push_locked(record);
}

void UserInputService::post_mouse_button(int button, bool down, float x, float y, bool processed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_ || !valid_button(button) || (down && queue_.size() >= kMaxQueued)) {
        return;
    }
    posted_mouse_ = Vec3{x, y, 0.f};
    if (posted_buttons_[button] == down) {
        return;
    }
    posted_buttons_[button] = down;
    InputRecord record;
    record.type = kMouseButton1 + button;
    record.state = down ? kBegin : kEnd;
    record.position = posted_mouse_;
    record.processed = processed;
    push_locked(record);
}

void UserInputService::post_mouse_move(float x, float y, bool processed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return;
    }
    const Vec3 next{x, y, 0.f};
    if (posted_mouse_known_ && next.x == posted_mouse_.x && next.y == posted_mouse_.y) {
        return;
    }
    InputRecord record;
    record.type = kMouseMovement;
    record.state = kChange;
    record.position = next;
    record.delta = posted_mouse_known_ ? Vec3{next.x - posted_mouse_.x, next.y - posted_mouse_.y, 0.f} : Vec3{};
    record.processed = processed;
    // A merge does not grow the queue, so only a new record is capped.
    const bool merges = !queue_.empty() && queue_.back().type == kMouseMovement && queue_.back().processed == processed;
    if (!merges && queue_.size() >= kMaxQueued) {
        return;
    }
    posted_mouse_ = next;
    posted_mouse_known_ = true;
    push_locked(record);
}

void UserInputService::post_wheel(float x, float y, float amount, bool processed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_ || amount == 0.f || queue_.size() >= kMaxQueued) {
        return;
    }
    posted_mouse_ = Vec3{x, y, 0.f};
    InputRecord record;
    record.type = kMouseWheel;
    record.state = kChange;
    // Roblox puts the wheel in Position.Z.
    record.position = Vec3{x, y, amount};
    record.processed = processed;
    push_locked(record);
}

void UserInputService::end_held_locked() {
    // Not capped: these ends are what keeps a key from staying down forever.
    for (int key : posted_keys_) {
        InputRecord record;
        record.type = kKeyboard;
        record.state = kEnd;
        record.key = key;
        record.position = posted_mouse_;
        queue_.push_back(record);
    }
    posted_keys_.clear();
    for (int button = 0; button < 3; ++button) {
        if (!posted_buttons_[button]) {
            continue;
        }
        posted_buttons_[button] = false;
        InputRecord record;
        record.type = kMouseButton1 + button;
        record.state = kEnd;
        record.position = posted_mouse_;
        queue_.push_back(record);
    }
}

void UserInputService::post_focus_lost() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return;
    }
    end_held_locked();
}

void UserInputService::bind(EventQueue& events) {
    if (bound_) {
        return;
    }
    events.host_signal(&began_);
    events.host_signal(&changed_);
    events.host_signal(&ended_);
    bound_ = true;
}

void UserInputService::release(EventQueue& events) {
    if (!bound_) {
        return;
    }
    events.release_signal(began_);
    events.release_signal(changed_);
    events.release_signal(ended_);
    bound_ = false;
}

Signal* UserInputService::signal(Kind kind) {
    switch (kind) {
    case Kind::Began:
        return &began_;
    case Kind::Changed:
        return &changed_;
    case Kind::Ended:
        return &ended_;
    }
    return nullptr;
}

void UserInputService::dispatch(EventQueue& events) {
    // The previous step's records were delivered by the drains that followed it.
    dispatched_.clear();
    {
        std::lock_guard<std::mutex> lock(mu_);
        dispatched_.swap(queue_);
    }
    first_payload_ = next_payload_;
    next_payload_ += dispatched_.size();
    for (std::size_t index = 0; index < dispatched_.size(); ++index) {
        const InputRecord& record = dispatched_[index];
        mouse_ = record.position;
        Kind kind = Kind::Changed;
        if (record.state == kBegin) {
            kind = Kind::Began;
        } else if (record.state == kEnd || record.state == kCancel) {
            kind = Kind::Ended;
        }
        if (record.type == kKeyboard) {
            const auto held = std::find(keys_down_.begin(), keys_down_.end(), record.key);
            if (kind == Kind::Began && held == keys_down_.end()) {
                keys_down_.push_back(record.key);
            } else if (kind == Kind::Ended && held != keys_down_.end()) {
                keys_down_.erase(held);
            }
        } else if (valid_button(record.type - kMouseButton1) && kind != Kind::Changed) {
            buttons_down_[record.type - kMouseButton1] = kind == Kind::Began;
        }
        Signal* target = signal(kind);
        if (target != nullptr && target->id().valid()) {
            events.emit_payload(target->id(), first_payload_ + index);
        }
    }
}

const InputRecord* UserInputService::record(std::uint64_t payload) const {
    if (payload < first_payload_ || payload >= first_payload_ + dispatched_.size()) {
        return nullptr;
    }
    return &dispatched_[static_cast<std::size_t>(payload - first_payload_)];
}

bool UserInputService::key_down(int key_code) const {
    return std::find(keys_down_.begin(), keys_down_.end(), key_code) != keys_down_.end();
}

bool UserInputService::button_down(int button) const { return valid_button(button) && buttons_down_[button]; }

void UserInputService::reset() {
    dispatched_.clear();
    first_payload_ = next_payload_;
    keys_down_.clear();
    std::fill(std::begin(buttons_down_), std::end(buttons_down_), false);
    mouse_ = Vec3{};
}

namespace {

bool read_true(DataModel&, DataModel&, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Bool;
    out.flag = true;
    return true;
}

bool read_false(DataModel&, DataModel&, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Bool;
    out.flag = false;
    return true;
}

// Every UserInputService signal passes the InputObject and whether the studio took it.
const LuaParam kInputSignalArgs[] = {{"input", "InputObject"}, {"gameProcessedEvent", "boolean"}};

LuaField input_signal(const char* name, UserInputService::Kind kind) {
    LuaField field;
    field.name = name;
    field.type_name = "Signal";
    field.tag = static_cast<int>(kind);
    field.params = kInputSignalArgs;
    field.param_count = 2;
    return field;
}

// ScriptRuntime adds the methods, since those calls need the script VM, and
// reads the InputObject fields off its own userdata.
ANARCHY_LUA_REGISTER(register_input_service_lua) {
    const LuaField fields[] = {
        input_signal("InputBegan", UserInputService::Kind::Began),
        input_signal("InputChanged", UserInputService::Kind::Changed),
        input_signal("InputEnded", UserInputService::Kind::Ended),
        lua_property("KeyboardEnabled", "boolean", false, read_true, nullptr),
        lua_property("MouseEnabled", "boolean", false, read_true, nullptr),
        lua_property("TouchEnabled", "boolean", false, read_false, nullptr),
    };
    register_lua_class("UserInputService", nullptr, fields, static_cast<int>(sizeof(fields) / sizeof(fields[0])));
    register_lua_service("UserInputService");

    const LuaField input_object[] = {
        lua_property("KeyCode", "EnumItem", false, nullptr, nullptr),
        lua_property("UserInputType", "EnumItem", false, nullptr, nullptr),
        lua_property("UserInputState", "EnumItem", false, nullptr, nullptr),
        lua_property("Position", "Vector3", false, nullptr, nullptr),
        lua_property("Delta", "Vector3", false, nullptr, nullptr),
    };
    register_lua_class("InputObject", nullptr, input_object,
                       static_cast<int>(sizeof(input_object) / sizeof(input_object[0])));
}

}  // namespace

}  // namespace engine_core
