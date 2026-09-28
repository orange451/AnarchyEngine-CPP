#pragma once

#include "McpServer.hpp"

#include <functional>
#include <string>

namespace engine_core {
class Engine;
}  // namespace engine_core

namespace ide {

// A picture the studio drew, encoded as PNG.
struct McpImage {
    std::string png;
    int width = 0;
    int height = 0;
};

// What the studio does for the tools that need its UI thread. Each is called
// on a server thread, returns once the studio has done it, and throws when it
// could not. A missing one leaves that part of a tool out.
struct McpStudio {
    // Test, Pause, Resume, and Stop, as the ribbon runs them.
    std::function<void()> start_test;
    std::function<void()> pause_test;
    std::function<void()> resume_test;
    std::function<void()> stop_test;
    // "stopped", "running", or "paused".
    std::function<std::string()> session;
    // Writes open script editors' typing to their scripts, before a tool replaces a Source.
    std::function<void()> flush_scripts;
    // Shows a Source a tool wrote in an idle open editor.
    std::function<void()> refresh_scripts;
    // An object naming this studio: project, root, pid, and port.
    std::function<engine_core::JsonValue()> info;
    // The Scene View as its next paint draws it, scaled down to fit max_size
    // pixels on its longer side.
    std::function<McpImage(int max_size)> capture_view;
};

// The tools over one engine: the tree, properties, instances, scripts and
// what analysis finds in them, selection, the class registry, Luau, output,
// undo, play testing, the Scene View, and which studio this is.
//
// Reads take the DataModel read lock on the server thread. Edits run on the
// simulation thread, or under the write lock while paused, as the explorer's
// do, and each is one undo step.
void add_engine_tools(McpServer& server, engine_core::Engine& engine, McpStudio studio);

}  // namespace ide
