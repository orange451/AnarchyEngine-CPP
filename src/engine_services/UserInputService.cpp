#include "UserInputService.hpp"

#include "Contract.hpp"
#include "DataModel.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace engine_core {
namespace {

// A step that never runs, such as a paused test, must not grow the queue
// without bound. Past this many records, new presses and movement are dropped.
// Releases are always kept, so a key whose press got through never stays down.
constexpr std::size_t kMaxQueued = 256;

bool valid_button(int button) { return button >= 0 && button < 3; }

// GLFW key numbers, by the KeyCode item each one is. `count` covers GLFW's
// runs, such as A to Z, whose KeyCode items are consecutive too. The values
// come from the KeyCode enum, so this table holds no Roblox numbers.
struct GlfwKeys {
    int glfw;
    const char* first;
    int count;
};

const GlfwKeys kGlfwKeys[] = {
    {32, "Space", 1},         {39, "Quote", 1},         {44, "Comma", 1},         {45, "Minus", 1},
    {46, "Period", 1},        {47, "Slash", 1},         {48, "Zero", 10},         {59, "Semicolon", 1},
    {61, "Equals", 1},        {65, "A", 26},            {91, "LeftBracket", 1},   {92, "BackSlash", 1},
    {93, "RightBracket", 1},  {96, "Backquote", 1},     {256, "Escape", 1},       {257, "Return", 1},
    {258, "Tab", 1},          {259, "Backspace", 1},    {260, "Insert", 1},       {261, "Delete", 1},
    {262, "Right", 1},        {263, "Left", 1},         {264, "Down", 1},         {265, "Up", 1},
    {266, "PageUp", 1},       {267, "PageDown", 1},     {268, "Home", 1},         {269, "End", 1},
    {280, "CapsLock", 1},     {281, "ScrollLock", 1},   {282, "NumLock", 1},      {283, "Print", 1},
    {284, "Pause", 1},        {290, "F1", 15},          {320, "KeypadZero", 17},  {340, "LeftShift", 1},
    {341, "LeftControl", 1},  {342, "LeftAlt", 1},      {343, "LeftSuper", 1},    {344, "RightShift", 1},
    {345, "RightControl", 1}, {346, "RightAlt", 1},     {347, "RightSuper", 1},   {348, "Menu", 1},
};

// GLFW key number -> KeyCode value, 0 for keys KeyCode does not have.
std::vector<int> build_key_table() {
    const EnumType& codes = key_code_enum();
    std::vector<int> table;
    for (const GlfwKeys& run : kGlfwKeys) {
        int first = -1;
        for (int index = 0; index < codes.count; ++index) {
            if (std::strcmp(codes.items[index].name, run.first) == 0) {
                first = index;
                break;
            }
        }
        if (first < 0 || first + run.count > codes.count) {
            contract_fail("GLFW key table names a KeyCode the enum does not have");
        }
        const std::size_t last = static_cast<std::size_t>(run.glfw + run.count);
        if (table.size() < last) {
            table.resize(last, 0);
        }
        for (int offset = 0; offset < run.count; ++offset) {
            table[static_cast<std::size_t>(run.glfw + offset)] = codes.items[first + offset].value;
        }
    }
    return table;
}

}  // namespace

int UserInputService::key_code_from_glfw(int glfw_key) {
    static const std::vector<int> table = build_key_table();
    if (glfw_key < 0 || static_cast<std::size_t>(glfw_key) >= table.size()) {
        return 0;
    }
    return table[static_cast<std::size_t>(glfw_key)];
}

int UserInputService::glfw_key_named(std::string_view name) {
    const EnumType& codes = key_code_enum();
    for (int index = 0; index < codes.count; ++index) {
        const std::string_view item = codes.items[index].name;
        const bool same = item.size() == name.size() &&
                          std::equal(item.begin(), item.end(), name.begin(), [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
        if (!same || codes.items[index].value == 0) {
            continue;
        }
        static const std::vector<int> table = build_key_table();
        for (std::size_t glfw = 0; glfw < table.size(); ++glfw) {
            if (table[glfw] == codes.items[index].value) {
                return static_cast<int>(glfw);
            }
        }
        return -1;
    }
    return -1;
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

void UserInputService::post_mouse_delta(float dx, float dy, bool processed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_ || (dx == 0.f && dy == 0.f)) {
        return;
    }
    InputRecord record;
    record.type = kMouseMovement;
    record.state = kChange;
    record.position = posted_mouse_;
    record.delta = Vec3{dx, dy, 0.f};
    record.processed = processed;
    const bool merges = !queue_.empty() && queue_.back().type == kMouseMovement && queue_.back().processed == processed;
    if (!merges && queue_.size() >= kMaxQueued) {
        return;
    }
    push_locked(record);
}

void UserInputService::set_mouse_behavior(int behavior) {
    const int previous = mouse_behavior_.exchange(behavior, std::memory_order_relaxed);
    if (previous != kMouseBehaviorDefault || behavior == kMouseBehaviorDefault) {
        return;
    }
    note_lock_started();
}

void UserInputService::note_lock_started() {
    // Motion so far is the pointer on its way to the press, so it must not
    // turn a camera: queued movement keeps its position but loses its Delta,
    // and mouse_delta() drops what was dispatched already.
    std::lock_guard<std::mutex> lock(mu_);
    for (InputRecord& record : queue_) {
        if (record.type == kMouseMovement) {
            record.delta = Vec3{};
        }
    }
    lock_starts_.fetch_add(1, std::memory_order_relaxed);
}

Vec3 UserInputService::mouse_delta() const {
    if (lock_starts_.load(std::memory_order_relaxed) != delta_lock_starts_) {
        return Vec3{};
    }
    return mouse_delta_;
}

bool UserInputService::set_mouse_delta_sensitivity(double value) {
    if (!std::isfinite(value)) {
        return false;
    }
    mouse_delta_sensitivity_ = std::max(0.0, value);
    return true;
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
    std::vector<InputRecord> records;
    {
        std::lock_guard<std::mutex> lock(mu_);
        records.swap(queue_);
        // Under the lock, so a lock that starts after this sees these records dispatched.
        delta_lock_starts_ = lock_starts_.load(std::memory_order_relaxed);
    }
    if (filter_) {
        filter_(records);
    }
    mouse_delta_ = Vec3{};
    for (const InputRecord& record : records) {
        mouse_ = record.position;
        if (record.type == kMouseMovement) {
            mouse_delta_.x += record.delta.x;
            mouse_delta_.y += record.delta.y;
        }
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
            LuaSlot input;
            input.kind = LuaSlot::Kind::InputObject;
            input.input = record;
            LuaSlot processed;
            processed.kind = LuaSlot::Kind::Bool;
            processed.flag = record.processed;
            events.emit_args(target->id(), 0, EventArgs{input, processed});
        }
    }
}

bool UserInputService::key_down(int key_code) const {
    return std::find(keys_down_.begin(), keys_down_.end(), key_code) != keys_down_.end();
}

bool UserInputService::button_down(int button) const { return valid_button(button) && buttons_down_[button]; }

void UserInputService::reset() {
    keys_down_.clear();
    std::fill(std::begin(buttons_down_), std::end(buttons_down_), false);
    mouse_ = Vec3{};
    mouse_delta_ = Vec3{};
    // What a session set of the pointer is its own, so the next one starts fresh.
    mouse_delta_sensitivity_ = 1.0;
    set_mouse_behavior(kMouseBehaviorDefault);
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

bool read_sensitivity(DataModel& world, DataModel&, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Number;
    out.number = world.input().mouse_delta_sensitivity();
    return true;
}

bool write_sensitivity(DataModel& world, DataModel&, LuaSlot& in) {
    if (in.kind != LuaSlot::Kind::Number || !world.input().set_mouse_delta_sensitivity(in.number)) {
        in.error = "MouseDeltaSensitivity must be a number";
        return false;
    }
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
        // No read: ScriptBindings pushes and checks the EnumItem itself.
        lua_property("MouseBehavior", "EnumItem", true, nullptr, nullptr),
        lua_property("MouseDeltaSensitivity", "number", true, read_sensitivity, write_sensitivity),
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
