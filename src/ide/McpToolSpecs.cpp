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
         "parent":{"type":["string","number"],"description":"Id or path. Not game, which holds only the scene services Workspace, Lighting, Storage, and Scripts. Default: Workspace."},
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
     "scratch folder until its first save. Skinned meshes come in static: bones and animations are "
     "not imported. Returns what each file made, or why it failed, and what a model left out. To "
     "show a Prefab in the world, create a GameObject in Workspace and set its Prefab property to "
     "the Prefab. Not during a test.",
     R"({"type":"object","required":["files"],"properties":{
         "files":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"string"},"description":"Absolute paths of image and model files."}}})"},
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
     "has not finished (see get_diagnostics). To change part of a long script, edit_script sends less.",
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
     "problems, lines from 1. pending names scripts whose check had not finished.",
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
