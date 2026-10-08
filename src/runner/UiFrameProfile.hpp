#pragma once

namespace jadefx {
class Stage;
}

namespace runner {

// The window's thread records on the profiler's "UI" row. Once per thread;
// safe to call again.
void register_ui_thread();

// Puts the window's frame phases on the UI row: the wait for the next frame,
// the system's event poll, input and tasks, styles and layout, the UI's paint,
// the frame tail, and the buffer swap. Without them the profiler shows the
// window's work between two Scene View paints as a gap, with the Render
// thread's Prepare alone at the start of the frame. Called on the window's
// thread, which it registers. Styles and layout is split into its styles,
// layout, and popups passes.
void profile_ui_frames(jadefx::Stage& stage);

}  // namespace runner
