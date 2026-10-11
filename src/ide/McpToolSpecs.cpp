#include "McpTools.hpp"

namespace ide {
namespace {

// A tool's metadata as written in the source. The schema is parsed when listed.
struct SpecText {
    const char* name;
    const char* description;
    const char* input_schema;
};

// In the order add_engine_tools adds them, which is the order tools/list shows.
constexpr SpecText kSpecs[] = {
    {"get_tree",
     "The instance tree under an instance: id, name, and class of each, nested. Rows below depth "
     "report child_count instead of children. Start here to find instances.",
     R"({"type":"object","properties":{
         "instance":{"type":["string","number"],"description":"Id or path. Default: the root, game."},
         "depth":{"type":"integer","minimum":0,"maximum":16,"description":"Levels to include. Default 2."}}})"},
    {"find_instances",
     "Instances whose Name contains the text, ignoring case, with their paths. Optionally only one "
     "class. At most 200.",
     R"({"type":"object","required":["name"],"properties":{
         "name":{"type":"string"},
         "class":{"type":"string","description":"Only instances of this class."}}})"},
    {"get_properties",
     "An instance's properties with their types and values, as the Properties panel shows them. "
     "Pass instances instead of instance to read several at once.",
     R"({"type":"object","properties":{
         "instance":{"type":["string","number"],"description":"Id or path."},
         "instances":{"type":"array","items":{"type":["string","number"]},"description":"Ids or paths. The result lists each one's properties under instances, in this order."}}})"},
    {"set_property",
     "Sets one property, as an edit in the Properties panel does: one undo step. Vector3 takes "
     "[x, y, z] and Vector2 [x, y]. Color3 takes [r, g, b], each 0 to 1, or a hex code such as \"#FF8000\". A "
     "Matrix4 (Transform) takes {\"position\": [x, y, z], \"orientation\": [x, y, z]}, either one "
     "alone keeping the other; orientation is in degrees, turned about Y, then X, then Z. It also takes its 16 numbers, "
     "column-major. An Instance property (such as Parent) takes an id, a path, or null.",
     R"({"type":"object","required":["instance","property","value"],"properties":{
         "instance":{"type":["string","number"],"description":"Id or path."},
         "property":{"type":"string"},
         "value":{"description":"string, number, boolean, [x,y,z], [r,g,b], hex code, {position, orientation}, 16 numbers, id, path, or null."}}})"},
    {"create_instance",
     "Creates an instance of a class Instance.new accepts (see list_classes) under a parent. One "
     "undo step. Returns its id and path.",
     R"({"type":"object","required":["class"],"properties":{
         "class":{"type":"string"},
         "parent":{"type":["string","number"],"description":"Id or path. Not game, which holds only the scene services Workspace, Lighting, Storage, Scripts, and Gui. Default: Workspace."},
         "name":{"type":"string","description":"Default: the class name."}}})"},
    {"delete_instance",
     "Deletes an instance and everything under it. One undo step. game and the scene services cannot be deleted.",
     R"({"type":"object","required":["instance"],"properties":{
         "instance":{"type":["string","number"],"description":"Id or path."}}})"},
    {"import_assets",
     "Imports image, sound, and model files from the computer the studio runs on, as dropping them on the "
     "studio does: one undo step. An image (PNG, JPEG, TGA, BMP, GIF, HDR, PSD) becomes a Texture in "
     "Assets.Textures. A sound (WAV, MP3, FLAC, OGG) becomes a Sound in Assets.Audio. A model (OBJ, "
     "FBX, glTF, GLB, DAE, 3DS, PLY, STL) becomes a Prefab in "
     "Assets.Prefabs with one Model per material, and its Meshes, Materials, and Textures go in a "
     "Folder named after it in each of those categories; the textures it names are found beside it. "
     "Files are copied into the project's resources folder; a place never saved keeps them in a "
     "scratch folder until its first save. A skinned model keeps its bones (GameObject:AddBone poses "
     "one), and its clips become Animations in a Folder in Assets.Animations; a file of clips alone "
     "makes only those. Returns what each file made, or why it failed, and what a "
     "model left out. To show a Prefab in the world, create a GameObject in Workspace and set its "
     "Prefab property to the Prefab. Not during a test.",
     R"({"type":"object","required":["files"],"properties":{
         "files":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"string"},"description":"Absolute paths of image, sound, and model files."}}})"},
    {"read_script",
     "The Source of a Script or ModuleScript, and how many lines it has. first_line and last_line "
     "return only those lines, counting from 1.",
     R"({"type":"object","required":["instance"],"properties":{
         "instance":{"type":["string","number"],"description":"Id or path."},
         "first_line":{"type":"integer","minimum":1,"description":"Default 1."},
         "last_line":{"type":"integer","minimum":1,"description":"Default: the last line."}}})"},
    {"write_script",
     "Replaces the Source of a Script or ModuleScript. One undo step. An open editor shows the new "
     "source; typing not yet written from it is written first, then replaced. Returns the problems "
     "the studio's analysis finds in the new source, lines from 1, or analysis \"pending\" when it "
     "has not finished, or \"not checked\" for a script analysis does not check: one outside the "
     "place, or one added during a playtest (see get_diagnostics). To change part of a long script, "
     "edit_script sends less.",
     R"({"type":"object","required":["instance","source"],"properties":{
         "instance":{"type":["string","number"],"description":"Id or path."},
         "source":{"type":"string"}}})"},
    {"edit_script",
     "Replaces text in a Script or ModuleScript's Source without sending all of it. Each edit's "
     "old_text must appear exactly once, spaces and line breaks included, unless replace_all is set. "
     "Edits apply in order, all or none, as one undo step. Returns how many places changed and the "
     "problems analysis finds, as write_script does.",
     R"({"type":"object","required":["instance","edits"],"properties":{
         "instance":{"type":["string","number"],"description":"Id or path."},
         "edits":{"type":"array","minItems":1,"items":{"type":"object","required":["old_text","new_text"],"properties":{
             "old_text":{"type":"string"},
             "new_text":{"type":"string"},
             "replace_all":{"type":"boolean","description":"Replace every place old_text appears. Default false."}}}}}})"},
    {"search_scripts",
     "Finds text in the Source of every Script and ModuleScript, or only those under instance, as "
     "the studio's Search pane does. Returns each script with a match and its matching lines, "
     "counting from 1, in the explorer's order. At most limit matches, 200 by default.",
     R"({"type":"object","required":["pattern"],"properties":{
         "pattern":{"type":"string"},
         "regex":{"type":"boolean","description":"pattern is an ECMAScript regular expression. A match stays within one line. Default false."},
         "match_case":{"type":"boolean","description":"Default false."},
         "whole_word":{"type":"boolean","description":"Default false."},
         "instance":{"type":["string","number"],"description":"Search only this and what is under it. Default: the root, game."},
         "limit":{"type":"integer","minimum":1,"maximum":2000}}})"},
    {"get_diagnostics",
     "The problems the studio's Luau analysis finds, as the script editor underlines them: syntax "
     "and type errors, and lint warnings. Checks one Script or ModuleScript, or every script under "
     "an instance, by default the whole place, waiting up to 10 seconds. Lists only scripts with "
     "problems, lines from 1. pending names scripts whose check had not finished, and unchecked "
     "scripts analysis does not check, such as one added during a playtest.",
     R"({"type":"object","properties":{
         "instance":{"type":["string","number"],"description":"A script, or an instance whose scripts to check. Default: the root, game."}}})"},
    {"run_lua",
     "Runs Luau against the live place, like the studio's command line, and returns what it printed "
     "and any error. `game` is the root. The chunk shows in the studio's console.",
     R"({"type":"object","required":["source"],"properties":{
         "source":{"type":"string"}}})"},
    {"get_output",
     "The studio console's recent lines: prints, errors, and commands, oldest first. Pass the "
     "returned next as since to get only newer lines.",
     R"({"type":"object","properties":{
         "since":{"type":"integer","minimum":0,"description":"A seq from an earlier call. Default: the oldest kept."},
         "limit":{"type":"integer","minimum":1,"maximum":1000,"description":"Default 200."}}})"},
    {"get_profile",
     "Frame timing from the studio's profiler: how long each frame took (avg, p95, max, and frames over "
     "16.6 ms), the scopes that cost the most per frame (engine phases, each Script with what resumed it, "
     "debug.profilebegin sections, and GPU passes), and the slowest frame as a tree per thread. When the "
     "profiler is not recording it records for seconds first; when it is paused it reads the paused frames "
     "at once. Numbers taken in edit mode are not the game's: start a playtest first to profile play. GPU "
     "times arrive a frame or two late. With path, also writes the profile as an HTML page that any browser "
     "shows, as the profiler's Save does.",
     R"({"type":"object","properties":{
         "seconds":{"type":"number","minimum":0.1,"maximum":10,"description":"How long to record when nothing is recording. Default 2."},
         "top":{"type":"integer","minimum":1,"maximum":200,"description":"Scope rows, slowest first. Default 25."},
         "include_timeline":{"type":"boolean","description":"Include the slowest frame's tree. Default true."},
         "path":{"type":"string","description":"Also save the profile as an HTML page (.html) at this absolute path."}}})"},
    {"get_selection",
     "The instances selected in the studio's explorers, in the order they were picked.",
     R"({"type":"object","properties":{}})"},
    {"set_selection",
     "Selects instances in the studio's explorers, which open the branches above them. An empty "
     "list clears the selection.",
     R"({"type":"object","required":["instances"],"properties":{
         "instances":{"type":"array","items":{"type":["string","number"]}}}})"},
    {"undo",
     "Undoes place edits as the studio's Undo does: the last count of them, 1 by default. With "
     "redo, redoes instead. Each edit these tools make is one step, write_script and edit_script "
     "included. Returns the steps taken and what Undo and Redo would do next.",
     R"({"type":"object","properties":{
         "redo":{"type":"boolean","description":"Default false."},
         "count":{"type":"integer","minimum":1,"maximum":50,"description":"Default 1."}}})"},
    {"list_classes",
     "The class names Luau knows, the ones Instance.new can create, and the services "
     "game:GetService returns.",
     R"({"type":"object","properties":{}})"},
    {"get_class",
     "One class's Luau API: its base class and its properties, methods, and events, including "
     "inherited ones.",
     R"({"type":"object","required":["class"],"properties":{
         "class":{"type":"string"}}})"},
    {"playtest",
     "Runs the place as the studio's Test button does. start begins a test (or resumes a "
     "paused one), pause and resume hold and continue it, stop ends it and restores the place "
     "as it was before start. status only reports. Every action returns the session state. "
     "With start or resume, run_for lets the test run that many seconds, ending early at the "
     "first error unless end_on_error is false, then pauses it so run_lua and get_properties "
     "can look at the play state (or does what then says), and returns what the place printed.",
     R"({"type":"object","required":["action"],"properties":{
         "action":{"type":"string","enum":["start","pause","resume","stop","status"]},
         "run_for":{"type":"number","exclusiveMinimum":0,"maximum":60,"description":"Seconds."},
         "then":{"type":"string","enum":["pause","stop","run"],"description":"After run_for: pause (the default), stop, or keep running."},
         "end_on_error":{"type":"boolean","description":"Default true."}}})"},
    {"screenshot",
     "A PNG of the studio's Scene View as it draws next, scaled to fit max_size pixels on its "
     "longer side. The view must be showing: a hidden tab or a minimized window does not draw.",
     R"({"type":"object","properties":{
         "max_size":{"type":"integer","minimum":64,"maximum":2048,"description":"Default 1024."}}})"},
    {"mouse_input",
     "Uses the mouse on the studio's Scene View as a person would, in edit mode or during a test. What "
     "is under the pointer hears it, a GUI first: a GuiButton is pressed, and a studio tool that is "
     "on selects and drags, in edit mode or during a test, keeping the left button from the game; "
     "then UserInputService fires InputBegan, InputChanged, and "
     "InputEnded, gameProcessedEvent true over a GUI. A press also gives the view the keyboard, as a "
     "click does. x and y are points from the view's top-left; screenshot returns view_width and "
     "view_height, so a pixel in its picture is at x = px * view_width / width. click (the default) "
     "moves there, presses, holds for hold seconds, and releases; count 2 double-clicks. down and up "
     "press or release only. move only moves. drag presses at x, y, moves to to over hold seconds in "
     "steps moves, and releases there. scroll turns the wheel amount lines. delta moves a pointer a "
     "script has locked (MouseBehavior LockCenter or LockCurrentPosition) by dx, dy, which "
     "GetMouseDelta reads, and needs no x or y; while the pointer is locked, as a person's it stays "
     "put, so the other actions press where it last was and ignore x and y. The view's tab must be "
     "in front. Returns the view's size in points.",
     R"({"type":"object","properties":{
         "action":{"type":"string","enum":["click","down","up","move","drag","scroll","delta"],"description":"Default click."},
         "x":{"type":"number","description":"Points from the view's left edge. Required except for delta."},
         "y":{"type":"number","description":"Points from the view's top edge. Required except for delta."},
         "button":{"type":"string","enum":["left","right","middle"],"description":"Default left. The right button turns the studio's camera in edit mode."},
         "count":{"type":"integer","minimum":1,"maximum":3,"description":"click: how many times. Default 1."},
         "hold":{"type":"number","minimum":0,"maximum":30,"description":"Seconds. click: between press and release, default 0.05. drag: the move's length, default 0.3."},
         "to":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2,"description":"drag: [x, y], where it ends."},
         "steps":{"type":"integer","minimum":1,"maximum":120,"description":"drag: moves along the way. Default 10."},
         "amount":{"type":"number","description":"scroll: lines, positive away from the user. Default 1."},
         "modifiers":{"type":"array","items":{"type":"string","enum":["shift","ctrl","alt","super"]},"description":"Keys held with it. click, drag, and scroll press and release them around it."},
         "dx":{"type":"number","description":"delta: points right."},
         "dy":{"type":"number","description":"delta: points down."}}})"},
    {"key_input",
     "Uses the keyboard on the studio's Scene View as a person would, in edit mode or during a test. "
     "The keys go to what has the keyboard in the view, such as a GUI's TextBox after a click on it, "
     "and to UserInputService (InputBegan and InputEnded, IsKeyDown); when the keyboard is elsewhere "
     "in the studio, the view takes it first. press (the default) presses each key in key in order, "
     "holds them hold seconds, and releases them in reverse, so [\"LeftControl\", \"Z\"] is a "
     "chord and W with hold 2 walks forward for 2 seconds. down and up press or release only, for "
     "keys held across other calls. type types text, each character with its key where it has one, "
     "\\n as Return. The view's tab must be in front.",
     R"({"type":"object","properties":{
         "action":{"type":"string","enum":["press","down","up","type"],"description":"Default press."},
         "key":{"type":["string","array"],"items":{"type":"string"},"description":"An Enum.KeyCode name, ignoring case, such as W, Space, LeftShift, Return, Escape, Up, or F1, or a character such as a or 1; or a list of them. Required except for type."},
         "hold":{"type":"number","minimum":0,"maximum":30,"description":"press: seconds held. Default 0.05."},
         "text":{"type":"string","description":"type: what to type."}}})"},
    {"tabs",
     "The studio's tabs, as its docks show them. list, the default, returns each dock's tabs in "
     "strip order, which one is in front, and the windows that are closed. select brings an open "
     "tab to the front, as clicking it does: screenshot needs the Scene View in front, and the "
     "Welcome page covers it when the studio starts. close closes a tab, as its x does; the Scene "
     "View cannot be closed. open docks a closed window from the closed list, as the Window menu "
     "does, or brings an open one to the front. A tab is named by its title or its window's name, "
     "ignoring case. Every action returns the tabs as they are after it.",
     R"({"type":"object","properties":{
         "action":{"type":"string","enum":["list","select","close","open"],"description":"Default list."},
         "tab":{"type":"string","description":"The tab's title or window's name, such as Scene View or Welcome. Required except for list."}}})"},
    {"save_place",
     "Saves the place to its project folder, as File > Save does: only files that changed are written, "
     "and open script editors' typing is saved with it. During a test it saves the place as it was at "
     "Test, so play edits stay out. When a file changed on disk since the last load or save, nothing "
     "is written: the error names the file, and the studio asks whether to overwrite. A place never "
     "saved needs folder. With folder, saves the whole project there, as Save As does, and the studio "
     "works in that folder from then on. Returns the project's name and folder, and what a Save wrote, "
     "moved, and removed.",
     R"({"type":"object","properties":{
         "folder":{"type":"string","description":"Save As: an absolute path to save the project in."}}})"},
    {"show_profiler",
     "Shows or hides the profiler over the Scene View, as View > Profiler (Ctrl+F6) does. screenshot "
     "leaves it out, as it does the view's other overlays. While it shows it records every frame; "
     "get_profile does not need it, since it records on its own. Hiding it ends a pause and closes an "
     "open capture. Without on it flips. Returns whether it shows now.",
     R"({"type":"object","properties":{
         "on":{"type":"boolean","description":"true shows it, false hides it. Default: the opposite of now."}}})"},
    {"gpu_detail",
     "Whether the profiler times each render pass on the GPU, rather than the whole 3D draw once a "
     "frame: the profiler's GPU button. Per-pass detail shows which pass costs the most in get_profile, "
     "but on macOS each timed pass stalls the CPU, so it inflates what it measures and slows the frame. "
     "Without on it flips. Returns whether it is on now.",
     R"({"type":"object","properties":{
         "on":{"type":"boolean","description":"true times each pass, false the whole frame. Default: the opposite of now."}}})"},
    {"get_studio_info",
     "Which studio this is: its project's name and folder, its process id, and its MCP port. "
     "Several studios may be open at once; check this before editing when it matters which.",
     R"({"type":"object","properties":{}})"},
};

}  // namespace

std::vector<McpToolSpec> engine_tool_specs() {
    std::vector<McpToolSpec> specs;
    for (const SpecText& spec : kSpecs) {
        specs.push_back({spec.name, spec.description, json_literal(spec.input_schema)});
    }
    return specs;
}

}  // namespace ide
