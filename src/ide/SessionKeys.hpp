#pragma once

namespace ide {

// What a key press does to the play session.
enum class SessionAction { None, Test, Resume, Stop };

// Roblox Studio's keys. F5 starts a test, or resumes a paused one. Shift+F5
// stops it. F5 while a test is running, and any other modifier, does nothing.
// key is a JadeFX key code, which matches GLFW. testing: a play session is
// active. stepping: that session is executing.
SessionAction SessionKeyAction(int key, bool shift, bool control, bool alt, bool meta, bool testing, bool stepping);

}  // namespace ide
