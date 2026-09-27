#include "SessionKeys.hpp"

namespace ide {

namespace {

// JadeFX key codes match GLFW. This is GLFW_KEY_F5.
constexpr int kKeyF5 = 294;

}  // namespace

SessionAction SessionKeyAction(int key, bool shift, bool control, bool alt, bool meta, bool testing, bool stepping) {
    if (key != kKeyF5 || control || alt || meta) {
        return SessionAction::None;
    }
    if (shift) {
        return testing ? SessionAction::Stop : SessionAction::None;
    }
    if (!testing) {
        return SessionAction::Test;
    }
    return stepping ? SessionAction::None : SessionAction::Resume;
}

}  // namespace ide
