#pragma once

#include "McpServer.hpp"
#include "types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
}  // namespace engine_core

namespace ide {

// A picture the studio drew, encoded as PNG.
struct McpImage {
    std::string png;
    int width = 0;
    int height = 0;
};

// What import_assets made of one file.
struct McpImport {
    std::string file;
    // "texture" or "model".
    std::string kind;
    // Why nothing was made of it. Nothing below is set then.
    std::string error;
    // The Texture, or the model's Prefab.
    engine_core::InstanceId root = 0;
    // Every instance made, root included, in the order made.
    std::vector<engine_core::InstanceId> made;
    // What a model had that was left out, such as a texture not found.
    std::vector<std::string> notes;
};

// Makes the instances of the files an import_files hook read. Runs where edits
// run, inside the tool's undo step. Throws when the place can no longer take them.
using McpPlaceImports = std::function<std::vector<McpImport>(engine_core::DataModel& world)>;

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
    // Reads image and model files and writes what they need into the place's
    // resources folder, as dropping them on the studio does, then returns what
    // makes their instances. Throws when the place cannot take files now, as
    // during a test.
    std::function<McpPlaceImports(const std::vector<std::string>& files)> import_files;
    // The studio's tabs: docks, each with its tabs, and the closed windows.
    std::function<engine_core::JsonValue()> tabs;
    // Does action, "select", "close", or "open", to the tab or window named tab.
    std::function<void(const std::string& action, const std::string& tab)> change_tab;
    // Saves the place as File > Save does, or, when folder is set, as Save As
    // does to that folder. Returns the project's name and folder, and for a
    // Save the files it wrote, moved, and removed. Throws why nothing was saved.
    std::function<engine_core::JsonValue(const std::string& folder)> save_place;
};

// What tools/list shows of a tool, without the code that runs it.
struct McpToolSpec {
    std::string name;
    std::string description;
    // A JSON Schema object describing the arguments.
    engine_core::JsonValue input_schema;
};

// Every tool add_engine_tools can add, in its order, as a studio with every
// hook set offers them. The bridge lists these without an engine.
std::vector<McpToolSpec> engine_tool_specs();

// The tools over one engine: the tree, properties, instances, scripts and
// what analysis finds in them, selection, the class registry, Luau, output,
// undo, play testing, the Scene View, the studio's tabs, saving, the profiler's GPU detail, and which
// studio this is. Each is the
// one engine_tool_specs describes; a tool whose studio hooks are missing is
// left out.
//
// Reads take the DataModel read lock on the server thread. Edits run on the
// simulation thread, or under the write lock while paused, as the explorer's
// do, and each is one undo step.
void add_engine_tools(McpServer& server, engine_core::Engine& engine, McpStudio studio);

}  // namespace ide
