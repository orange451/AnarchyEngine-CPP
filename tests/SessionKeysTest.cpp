#include "ide/SessionKeys.hpp"

#include <cstdio>

namespace {

int gFailures = 0;

constexpr int kF5 = 294;
constexpr int kF6 = 295;

void Expect(ide::SessionAction got, ide::SessionAction want, const char* label) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %d, want %d\n", label, static_cast<int>(got), static_cast<int>(want));
        ++gFailures;
    }
}

}  // namespace

int RunSessionKeysTests() {
    using ide::SessionAction;
    using ide::SessionKeyAction;
    // testing, stepping: edit mode is (false, false), running is (true, true), paused is (true, false).
    Expect(SessionKeyAction(kF5, false, false, false, false, false, false), SessionAction::Test, "F5 in edit mode tests");
    Expect(SessionKeyAction(kF5, false, false, false, false, true, true), SessionAction::None,
           "F5 while running does nothing");
    Expect(SessionKeyAction(kF5, false, false, false, false, true, false), SessionAction::Resume,
           "F5 while paused resumes");
    Expect(SessionKeyAction(kF5, true, false, false, false, true, true), SessionAction::Stop,
           "Shift+F5 while running stops");
    Expect(SessionKeyAction(kF5, true, false, false, false, true, false), SessionAction::Stop,
           "Shift+F5 while paused stops");
    Expect(SessionKeyAction(kF5, true, false, false, false, false, false), SessionAction::None,
           "Shift+F5 in edit mode does nothing");
    Expect(SessionKeyAction(kF5, false, true, false, false, false, false), SessionAction::None, "Ctrl+F5 is not F5");
    Expect(SessionKeyAction(kF5, false, false, true, false, false, false), SessionAction::None, "Alt+F5 is not F5");
    Expect(SessionKeyAction(kF5, false, false, false, true, false, false), SessionAction::None, "Cmd+F5 is not F5");
    Expect(SessionKeyAction(kF6, false, false, false, false, false, false), SessionAction::None, "F6 is not bound");
    return gFailures;
}
