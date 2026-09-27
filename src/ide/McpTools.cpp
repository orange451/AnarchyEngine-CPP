#include "McpTools.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "PropertySheet.hpp"
#include "ScriptRuntime.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ide {
namespace {

using engine_core::DataModel;
using engine_core::DataModelLock;
using engine_core::InstanceId;
using engine_core::JsonValue;

// A playing step holds the DataModel lock for its whole length. A read tries
// in short waits so it lands between steps, and gives up after about two seconds.
constexpr std::chrono::milliseconds kReadWait(20);
constexpr int kReadTries = 100;
// How long a tool waits for the simulation thread to run its edit.
constexpr std::chrono::seconds kEditWait(5);
// get_tree stops adding rows past this many, so one call cannot return the whole place.
constexpr std::size_t kMaxTreeRows = 500;
constexpr int kDefaultDepth = 2;
constexpr int kMaxDepth = 16;
constexpr std::size_t kDefaultOutputLines = 200;

// The read lock, taken between simulation steps.
class ReadLock {
public:
    explicit ReadLock(DataModel& world) {
        for (int attempt = 0; attempt < kReadTries; ++attempt) {
            lock_.emplace(world, DataModelLock::Read, kReadWait);
            if (lock_->owns()) {
                return;
            }
            lock_.reset();
        }
        throw std::runtime_error("The place is busy. Try again.");
    }

private:
    std::optional<DataModelLock> lock_;
};

// Runs fn where edits run, waits for it, and returns what it returned. What
// fn throws comes back here, so it never reaches the simulation thread.
JsonValue RunEdit(engine_core::Engine& engine, std::function<JsonValue(DataModel&)> fn) {
    struct Wait {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        JsonValue value;
        std::string error;
    };
    auto wait = std::make_shared<Wait>();
    engine.on_simulation([wait, fn = std::move(fn)](DataModel& world) {
        JsonValue value;
        std::string error;
        try {
            value = fn(world);
        } catch (const std::exception& ex) {
            error = ex.what();
            if (error.empty()) {
                error = "The edit failed.";
            }
        }
        {
            std::lock_guard<std::mutex> guard(wait->mu);
            wait->value = std::move(value);
            wait->error = std::move(error);
            wait->done = true;
        }
        wait->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(wait->mu);
    if (!wait->cv.wait_for(lock, kEditWait, [&] { return wait->done; })) {
        throw std::runtime_error("The simulation did not run the edit within 5 seconds. It may still apply.");
    }
    if (!wait->error.empty()) {
        throw std::runtime_error(wait->error);
    }
    return std::move(wait->value);
}

std::string ClassOf(const DataModel& world, InstanceId id) {
    const DataModel* object = id == world.id() ? &world : world.instance(id);
    const char* name = object != nullptr ? object->class_name() : nullptr;
    return name != nullptr ? name : "";
}

bool Exists(const DataModel& world, InstanceId id) { return id == world.id() || world.alive(id); }

// Names from under the root down, joined with dots. The root is "game". An
// instance out of the tree, such as a cut one, starts from its own top.
std::string PathOf(const DataModel& world, InstanceId id) {
    if (id == world.id()) {
        return "game";
    }
    std::vector<std::string> names;
    for (InstanceId at = id; at != world.id() && at != DataModel::kNoParent && world.alive(at);
         at = world.parent(at)) {
        names.push_back(world.name(at));
        if (names.size() > 4096) {
            break;
        }
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!path.empty()) {
            path.push_back('.');
        }
        path += *it;
    }
    return path;
}

// An instance named by id or by path. The root is "game", "", or its id.
// A path may start with "game.". Throws when nothing is there.
InstanceId Resolve(const DataModel& world, const JsonValue* ref, const char* what = "instance") {
    if (ref == nullptr || ref->is_null()) {
        throw std::runtime_error(std::string(what) + " is required: an id or a path such as \"Folder.Part\".");
    }
    if (ref->is_number()) {
        const double number = ref->as_number();
        const auto id = static_cast<InstanceId>(number);
        if (number < 0 || static_cast<double>(id) != number || !Exists(world, id)) {
            throw std::runtime_error("No instance has id " + engine_core::format_json_number(number) + ".");
        }
        return id;
    }
    if (!ref->is_string()) {
        throw std::runtime_error(std::string(what) + " must be an id or a path.");
    }
    std::string path = ref->as_string();
    if (path.empty() || path == "game") {
        return world.id();
    }
    if (path.compare(0, 5, "game.") == 0) {
        path.erase(0, 5);
    }
    InstanceId at = world.id();
    std::size_t begin = 0;
    while (begin <= path.size()) {
        std::size_t end = path.find('.', begin);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string name = path.substr(begin, end - begin);
        const InstanceId child = world.find_first_child(at, name);
        if (child == 0 || child == DataModel::kNoParent) {
            throw std::runtime_error("No instance at \"" + ref->as_string() + "\": " + PathOf(world, at) +
                                     " has no child named \"" + name + "\".");
        }
        at = child;
        begin = end + 1;
    }
    return at;
}

JsonValue Brief(const DataModel& world, InstanceId id) {
    JsonValue out = JsonValue::object();
    out.set("id", JsonValue::number(id));
    out.set("name", JsonValue::string(id == world.id() ? "game" : world.name(id)));
    out.set("class", JsonValue::string(ClassOf(world, id)));
    out.set("path", JsonValue::string(PathOf(world, id)));
    return out;
}

JsonValue TreeNode(const DataModel& world, InstanceId id, int depth, std::size_t& rows, bool& truncated) {
    JsonValue out = JsonValue::object();
    out.set("id", JsonValue::number(id));
    out.set("name", JsonValue::string(id == world.id() ? "game" : world.name(id)));
    out.set("class", JsonValue::string(ClassOf(world, id)));
    ++rows;
    const std::vector<InstanceId> children = world.get_children(id);
    if (children.empty()) {
        return out;
    }
    if (depth <= 0 || rows >= kMaxTreeRows) {
        out.set("child_count", JsonValue::number(static_cast<double>(children.size())));
        truncated = truncated || depth > 0;
        return out;
    }
    JsonValue list = JsonValue::array();
    for (InstanceId child : children) {
        if (rows >= kMaxTreeRows) {
            truncated = true;
            out.set("child_count", JsonValue::number(static_cast<double>(children.size())));
            break;
        }
        list.items().push_back(TreeNode(world, child, depth - 1, rows, truncated));
    }
    out.set("children", std::move(list));
    return out;
}

const char* KindName(PropertyKind kind) {
    switch (kind) {
        case PropertyKind::String:
            return "string";
        case PropertyKind::Bool:
            return "boolean";
        case PropertyKind::Number:
            return "number";
        case PropertyKind::Vector3:
            return "Vector3";
        case PropertyKind::Ref:
            return "Instance";
        case PropertyKind::ReadOnlyText:
            return "string";
    }
    return "string";
}

JsonValue RowValue(const DataModel& world, const PropertyRow& row) {
    switch (row.kind) {
        case PropertyKind::String:
        case PropertyKind::ReadOnlyText:
            return JsonValue::string(row.value.text);
        case PropertyKind::Bool:
            return JsonValue::boolean(row.value.flag);
        case PropertyKind::Number:
            return JsonValue::number(row.value.number);
        case PropertyKind::Vector3:
            // Floats, written as the shortest decimal that reads back to the same float.
            return JsonValue::array({JsonValue::number_from_float(row.value.vec.x),
                                     JsonValue::number_from_float(row.value.vec.y),
                                     JsonValue::number_from_float(row.value.vec.z)});
        case PropertyKind::Ref:
            if (row.value.nil_ref() || !Exists(world, row.value.ref)) {
                return JsonValue();
            }
            return Brief(world, row.value.ref);
    }
    return JsonValue();
}

// The instance's properties, as the Properties panel shows them.
JsonValue Properties(DataModel& world, InstanceId id) {
    JsonValue out = Brief(world, id);
    JsonValue properties = JsonValue::object();
    for (const PropertyRow& row : read_sheet(world, {id}).rows) {
        JsonValue entry = JsonValue::object();
        entry.set("type", JsonValue::string(row.type_name.empty() ? KindName(row.kind) : row.type_name));
        entry.set("value", RowValue(world, row));
        if (!row.writable || row.kind == PropertyKind::ReadOnlyText) {
            entry.set("readonly", JsonValue::boolean(true));
        }
        properties.set(row.name, std::move(entry));
    }
    if (dynamic_cast<const engine_core::LuaSource*>(world.instance(id)) != nullptr) {
        JsonValue entry = JsonValue::object();
        entry.set("type", JsonValue::string("string"));
        entry.set("value", JsonValue::string("(use read_script)"));
        properties.set("Source", std::move(entry));
    }
    out.set("properties", std::move(properties));
    return out;
}

double NumberArg(const JsonValue& value, const std::string& what) {
    if (!value.is_number()) {
        throw std::runtime_error(what + " must be a number.");
    }
    return value.as_number();
}

// The typed value for one row, from the JSON a client sent.
PropertyEdit EditFor(const DataModel& world, const PropertyRow& row, const JsonValue& value) {
    PropertyEdit edit;
    edit.property = row.name;
    edit.kind = row.kind;
    switch (row.kind) {
        case PropertyKind::String:
        case PropertyKind::ReadOnlyText:
            if (!value.is_string()) {
                throw std::runtime_error(row.name + " takes a string.");
            }
            edit.value.text = value.as_string();
            break;
        case PropertyKind::Bool:
            if (!value.is_bool()) {
                throw std::runtime_error(row.name + " takes true or false.");
            }
            edit.value.flag = value.as_bool();
            break;
        case PropertyKind::Number:
            edit.value.number = NumberArg(value, row.name);
            break;
        case PropertyKind::Vector3: {
            double axes[3] = {0, 0, 0};
            if (value.is_array() && value.items().size() == 3) {
                for (int i = 0; i < 3; ++i) {
                    axes[i] = NumberArg(value.items()[static_cast<std::size_t>(i)], row.name);
                }
            } else if (value.is_object() && value.find("x") && value.find("y") && value.find("z")) {
                axes[0] = NumberArg(*value.find("x"), row.name);
                axes[1] = NumberArg(*value.find("y"), row.name);
                axes[2] = NumberArg(*value.find("z"), row.name);
            } else {
                throw std::runtime_error(row.name + " takes [x, y, z].");
            }
            edit.value.vec.x = static_cast<float>(axes[0]);
            edit.value.vec.y = static_cast<float>(axes[1]);
            edit.value.vec.z = static_cast<float>(axes[2]);
            break;
        }
        case PropertyKind::Ref:
            edit.value.ref = value.is_null() ? DataModel::kNoParent : Resolve(world, &value, row.name.c_str());
            break;
    }
    return edit;
}

const std::string& StringArg(const JsonValue& arguments, const char* key) {
    const JsonValue* value = arguments.find(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error(std::string(key) + " is required and must be a string.");
    }
    return value->as_string();
}

engine_core::LuaSource& ScriptAt(DataModel& world, InstanceId id) {
    auto* source = dynamic_cast<engine_core::LuaSource*>(world.instance(id));
    if (source == nullptr) {
        throw std::runtime_error(PathOf(world, id) + " is a " + ClassOf(world, id) + ", not a Script or ModuleScript.");
    }
    return *source;
}

const char* OutputKindName(engine_core::ScriptRuntime::OutputKind kind) {
    switch (kind) {
        case engine_core::ScriptRuntime::OutputKind::Print:
            return "print";
        case engine_core::ScriptRuntime::OutputKind::Error:
            return "error";
        case engine_core::ScriptRuntime::OutputKind::Command:
            return "command";
    }
    return "print";
}

JsonValue OutputLines(const engine_core::ScriptRuntime::OutputHistory& history) {
    JsonValue lines = JsonValue::array();
    std::uint64_t seq = history.first;
    for (const engine_core::ScriptRuntime::OutputLine& line : history.lines) {
        JsonValue entry = JsonValue::object();
        entry.set("seq", JsonValue::number(static_cast<double>(seq++)));
        entry.set("kind", JsonValue::string(OutputKindName(line.kind)));
        std::string text = line.text;
        if (!text.empty() && text.back() == '\n') {
            text.pop_back();
        }
        entry.set("text", JsonValue::string(std::move(text)));
        lines.items().push_back(std::move(entry));
    }
    return lines;
}

// Ends the edit's undo step, as the explorer's edits do.
void CloseGesture(DataModel& world) { world.history().end_gesture(); }

}  // namespace

void add_engine_tools(McpServer& server, engine_core::Engine& engine, McpStudio studio) {
    engine_core::Engine* live = &engine;

    server.add_tool({"get_tree",
                     "The instance tree under an instance: id, name, and class of each, nested. Rows below depth "
                     "report child_count instead of children. Start here to find instances.",
                     json_literal(R"({"type":"object","properties":{
                         "instance":{"type":["string","number"],"description":"Id or path. Default: the root, game."},
                         "depth":{"type":"integer","minimum":0,"maximum":16,"description":"Levels to include. Default 2."}}})"),
                     [live](const JsonValue& arguments) {
                         DataModel& world = live->datamodel();
                         int depth = kDefaultDepth;
                         if (const JsonValue* asked = arguments.find("depth")) {
                             depth = static_cast<int>(NumberArg(*asked, "depth"));
                         }
                         depth = std::max(0, std::min(depth, kMaxDepth));
                         ReadLock lock(world);
                         const JsonValue* ref = arguments.find("instance");
                         const InstanceId id = ref != nullptr ? Resolve(world, ref) : world.id();
                         std::size_t rows = 0;
                         bool truncated = false;
                         JsonValue out = JsonValue::object();
                         out.set("path", JsonValue::string(PathOf(world, id)));
                         out.set("tree", TreeNode(world, id, depth, rows, truncated));
                         if (truncated) {
                             out.set("truncated", JsonValue::boolean(true));
                         }
                         return out;
                     }});

    server.add_tool({"find_instances",
                     "Instances whose Name contains the text, ignoring case, with their paths. Optionally only one "
                     "class. At most 200.",
                     json_literal(R"({"type":"object","required":["name"],"properties":{
                         "name":{"type":"string"},
                         "class":{"type":"string","description":"Only instances of this class."}}})"),
                     [live](const JsonValue& arguments) {
                         auto lower = [](std::string text) {
                             for (char& unit : text) {
                                 if (unit >= 'A' && unit <= 'Z') {
                                     unit = static_cast<char>(unit - 'A' + 'a');
                                 }
                             }
                             return text;
                         };
                         const std::string needle = lower(StringArg(arguments, "name"));
                         const JsonValue* only = arguments.find("class");
                         DataModel& world = live->datamodel();
                         ReadLock lock(world);
                         JsonValue found = JsonValue::array();
                         std::vector<InstanceId> pending = world.get_children(world.id());
                         while (!pending.empty() && found.items().size() < 200) {
                             const InstanceId id = pending.back();
                             pending.pop_back();
                             if (lower(world.name(id)).find(needle) != std::string::npos &&
                                 (only == nullptr || ClassOf(world, id) == only->as_string())) {
                                 found.items().push_back(Brief(world, id));
                             }
                             const std::vector<InstanceId> children = world.get_children(id);
                             pending.insert(pending.end(), children.rbegin(), children.rend());
                         }
                         JsonValue out = JsonValue::object();
                         out.set("instances", std::move(found));
                         return out;
                     }});

    server.add_tool({"get_properties",
                     "An instance's properties with their types and values, as the Properties panel shows them.",
                     json_literal(R"({"type":"object","required":["instance"],"properties":{
                         "instance":{"type":["string","number"],"description":"Id or path."}}})"),
                     [live](const JsonValue& arguments) {
                         DataModel& world = live->datamodel();
                         ReadLock lock(world);
                         return Properties(world, Resolve(world, arguments.find("instance")));
                     }});

    server.add_tool({"set_property",
                     "Sets one property, as an edit in the Properties panel does: one undo step. Vector3 takes "
                     "[x, y, z]. An Instance property (such as Parent) takes an id, a path, or null.",
                     json_literal(R"({"type":"object","required":["instance","property","value"],"properties":{
                         "instance":{"type":["string","number"],"description":"Id or path."},
                         "property":{"type":"string"},
                         "value":{"description":"string, number, boolean, [x,y,z], id, path, or null."}}})"),
                     [live](const JsonValue& arguments) {
                         const JsonValue instance = arguments.find("instance") ? *arguments.find("instance") : JsonValue();
                         const std::string property = StringArg(arguments, "property");
                         const JsonValue* given = arguments.find("value");
                         if (given == nullptr) {
                             throw std::runtime_error("value is required.");
                         }
                         const JsonValue value = *given;
                         return RunEdit(*live, [instance, property, value](DataModel& world) {
                             const InstanceId id = Resolve(world, &instance);
                             const PropertySheet sheet = read_sheet(world, {id});
                             const PropertyRow* row = sheet.find(property);
                             if (row == nullptr) {
                                 throw std::runtime_error(PathOf(world, id) + " has no property " + property +
                                                          " that can be set here.");
                             }
                             if (!row->writable || row->kind == PropertyKind::ReadOnlyText) {
                                 throw std::runtime_error(property + " is read-only.");
                             }
                             const EditResult result = apply_edit(world, {id}, EditFor(world, *row, value));
                             if (result.rejected) {
                                 throw std::runtime_error(result.error.empty() ? "The edit was refused." : result.error);
                             }
                             return Properties(world, id);
                         });
                     }});

    server.add_tool({"create_instance",
                     "Creates an instance of a class Instance.new accepts (see list_classes) under a parent. One "
                     "undo step. Returns its id and path.",
                     json_literal(R"({"type":"object","required":["class"],"properties":{
                         "class":{"type":"string"},
                         "parent":{"type":["string","number"],"description":"Id or path. Default: the root, game."},
                         "name":{"type":"string","description":"Default: the class name."}}})"),
                     [live](const JsonValue& arguments) {
                         const std::string class_name = StringArg(arguments, "class");
                         const JsonValue parent = arguments.find("parent") ? *arguments.find("parent") : JsonValue::string("game");
                         const JsonValue* named = arguments.find("name");
                         const std::string name = named != nullptr && named->is_string() ? named->as_string() : "";
                         return RunEdit(*live, [class_name, parent, name](DataModel& world) {
                             const InstanceId parent_id = Resolve(world, &parent, "parent");
                             if (!engine_core::lua_creatable_known(class_name.c_str())) {
                                 throw std::runtime_error("Instance.new cannot make \"" + class_name +
                                                          "\". list_classes names the ones it can.");
                             }
                             world.history().set_pending_gesture("Insert " + class_name);
                             DataModel* made = engine_core::lua_create_instance(world, class_name.c_str());
                             if (made == nullptr) {
                                 throw std::runtime_error("Could not create " + class_name + ".");
                             }
                             if (!name.empty()) {
                                 world.set_name(made->id(), name);
                             }
                             world.set_parent(made->id(), parent_id);
                             CloseGesture(world);
                             // An edit while stopped is part of the place, as the explorer's insert is.
                             if (!world.simulation_running()) {
                                 world.capture_place();
                             }
                             return Brief(world, made->id());
                         });
                     }});

    server.add_tool({"delete_instance",
                     "Deletes an instance and everything under it. One undo step.",
                     json_literal(R"({"type":"object","required":["instance"],"properties":{
                         "instance":{"type":["string","number"],"description":"Id or path."}}})"),
                     [live](const JsonValue& arguments) {
                         const JsonValue instance = arguments.find("instance") ? *arguments.find("instance") : JsonValue();
                         return RunEdit(*live, [instance](DataModel& world) {
                             const InstanceId id = Resolve(world, &instance);
                             if (id == world.id()) {
                                 throw std::runtime_error("The root cannot be deleted.");
                             }
                             JsonValue out = JsonValue::object();
                             out.set("deleted", Brief(world, id));
                             world.history().set_pending_gesture("Delete");
                             world.destroy_tree(id);
                             CloseGesture(world);
                             return out;
                         });
                     }});

    server.add_tool({"read_script",
                     "The Source of a Script or ModuleScript.",
                     json_literal(R"({"type":"object","required":["instance"],"properties":{
                         "instance":{"type":["string","number"],"description":"Id or path."}}})"),
                     [live](const JsonValue& arguments) {
                         DataModel& world = live->datamodel();
                         ReadLock lock(world);
                         const InstanceId id = Resolve(world, arguments.find("instance"));
                         JsonValue out = Brief(world, id);
                         out.set("source", JsonValue::string(ScriptAt(world, id).source()));
                         return out;
                     }});

    server.add_tool({"write_script",
                     "Replaces the Source of a Script or ModuleScript. One undo step. An open editor shows the new "
                     "source; typing not yet written from it is written first, then replaced.",
                     json_literal(R"({"type":"object","required":["instance","source"],"properties":{
                         "instance":{"type":["string","number"],"description":"Id or path."},
                         "source":{"type":"string"}}})"),
                     [live, studio](const JsonValue& arguments) {
                         const JsonValue instance = arguments.find("instance") ? *arguments.find("instance") : JsonValue();
                         const std::string source = StringArg(arguments, "source");
                         if (studio.flush_scripts) {
                             studio.flush_scripts();
                         }
                         JsonValue out = RunEdit(*live, [instance, source](DataModel& world) {
                             const InstanceId id = Resolve(world, &instance);
                             engine_core::LuaSource& script = ScriptAt(world, id);
                             if (script.source() != source) {
                                 // The same waypoint an editor's typing makes.
                                 std::optional<std::string> recording;
                                 if (!world.simulation_running()) {
                                     recording = world.history().try_begin_recording("Edit Script");
                                 }
                                 script.set_source(source);
                                 if (recording) {
                                     world.history().finish_recording(*recording,
                                                                      engine_core::FinishRecordingOperation::Commit);
                                 }
                                 if (!world.simulation_running()) {
                                     world.capture_place();
                                 }
                             }
                             return Brief(world, id);
                         });
                         if (studio.refresh_scripts) {
                             studio.refresh_scripts();
                         }
                         return out;
                     }});

    server.add_tool({"run_lua",
                     "Runs Luau against the live place, like the studio's command line, and returns what it printed "
                     "and any error. `game` is the root. The chunk shows in the studio's console.",
                     json_literal(R"({"type":"object","required":["source"],"properties":{
                         "source":{"type":"string"}}})"),
                     [live](const JsonValue& arguments) {
                         const std::string source = StringArg(arguments, "source");
                         engine_core::ScriptRuntime* scripts = &live->scripts();
                         return RunEdit(*live, [scripts, source](DataModel&) {
                             scripts->append_output(engine_core::ScriptRuntime::OutputKind::Command, source);
                             const std::uint64_t from = scripts->output_next();
                             scripts->run_chunk(source);
                             const std::uint64_t to = scripts->output_next();
                             const engine_core::ScriptRuntime::OutputHistory history =
                                 scripts->output_since(from, static_cast<std::size_t>(to - from));
                             bool failed = false;
                             for (const engine_core::ScriptRuntime::OutputLine& line : history.lines) {
                                 failed = failed || line.kind == engine_core::ScriptRuntime::OutputKind::Error;
                             }
                             JsonValue out = JsonValue::object();
                             out.set("output", OutputLines(history));
                             out.set("error", JsonValue::boolean(failed));
                             return out;
                         });
                     }});

    server.add_tool({"get_output",
                     "The studio console's recent lines: prints, errors, and commands, oldest first. Pass the "
                     "returned next as since to get only newer lines.",
                     json_literal(R"({"type":"object","properties":{
                         "since":{"type":"integer","minimum":0,"description":"A seq from an earlier call. Default: the oldest kept."},
                         "limit":{"type":"integer","minimum":1,"maximum":1000,"description":"Default 200."}}})"),
                     [live](const JsonValue& arguments) {
                         std::uint64_t since = 0;
                         if (const JsonValue* value = arguments.find("since")) {
                             since = static_cast<std::uint64_t>(std::max(0.0, NumberArg(*value, "since")));
                         }
                         std::size_t limit = kDefaultOutputLines;
                         if (const JsonValue* value = arguments.find("limit")) {
                             limit = static_cast<std::size_t>(std::max(1.0, std::min(1000.0, NumberArg(*value, "limit"))));
                         }
                         const engine_core::ScriptRuntime::OutputHistory history = live->scripts().output_since(since, limit);
                         JsonValue out = JsonValue::object();
                         out.set("lines", OutputLines(history));
                         out.set("next", JsonValue::number(static_cast<double>(history.first + history.lines.size())));
                         return out;
                     }});

    server.add_tool({"get_selection",
                     "The instances selected in the studio's explorers, in the order they were picked.",
                     json_literal(R"({"type":"object","properties":{}})"),
                     [live](const JsonValue&) {
                         DataModel& world = live->datamodel();
                         const std::vector<InstanceId> ids = world.selection().get();
                         ReadLock lock(world);
                         JsonValue list = JsonValue::array();
                         for (InstanceId id : ids) {
                             if (Exists(world, id)) {
                                 list.items().push_back(Brief(world, id));
                             }
                         }
                         JsonValue out = JsonValue::object();
                         out.set("instances", std::move(list));
                         return out;
                     }});

    server.add_tool({"set_selection",
                     "Selects instances in the studio's explorers, which open the branches above them. An empty "
                     "list clears the selection.",
                     json_literal(R"({"type":"object","required":["instances"],"properties":{
                         "instances":{"type":"array","items":{"type":["string","number"]}}}})"),
                     [live](const JsonValue& arguments) {
                         const JsonValue* given = arguments.find("instances");
                         if (given == nullptr || !given->is_array()) {
                             throw std::runtime_error("instances is required and must be a list.");
                         }
                         DataModel& world = live->datamodel();
                         std::vector<InstanceId> ids;
                         JsonValue list = JsonValue::array();
                         {
                             ReadLock lock(world);
                             for (const JsonValue& item : given->items()) {
                                 const InstanceId id = Resolve(world, &item);
                                 ids.push_back(id);
                                 list.items().push_back(Brief(world, id));
                             }
                         }
                         world.selection().set(std::move(ids));
                         JsonValue out = JsonValue::object();
                         out.set("instances", std::move(list));
                         return out;
                     }});

    server.add_tool({"list_classes",
                     "The class names Luau knows, the ones Instance.new can create, and the services "
                     "game:GetService returns.",
                     json_literal(R"({"type":"object","properties":{}})"),
                     [](const JsonValue&) {
                         auto to_json = [](const std::vector<std::string>& names) {
                             JsonValue list = JsonValue::array();
                             for (const std::string& name : names) {
                                 list.items().push_back(JsonValue::string(name));
                             }
                             return list;
                         };
                         std::vector<std::string> names;
                         JsonValue out = JsonValue::object();
                         engine_core::lua_class_names(names);
                         out.set("classes", to_json(names));
                         names.clear();
                         engine_core::lua_creatable_names(names);
                         out.set("creatable", to_json(names));
                         names.clear();
                         engine_core::lua_service_names(names);
                         out.set("services", to_json(names));
                         return out;
                     }});

    server.add_tool({"get_class",
                     "One class's Luau API: its base class and its properties, methods, and events, including "
                     "inherited ones.",
                     json_literal(R"({"type":"object","required":["class"],"properties":{
                         "class":{"type":"string"}}})"),
                     [](const JsonValue& arguments) {
                         const std::string& class_name = StringArg(arguments, "class");
                         if (!engine_core::lua_class_known(class_name.c_str())) {
                             throw std::runtime_error("No class is named \"" + class_name + "\". See list_classes.");
                         }
                         std::vector<engine_core::LuaField> fields;
                         engine_core::lua_class_members(class_name.c_str(), fields);
                         JsonValue properties = JsonValue::array();
                         JsonValue methods = JsonValue::array();
                         JsonValue events = JsonValue::array();
                         for (const engine_core::LuaField& field : fields) {
                             if (field.name == nullptr) {
                                 continue;
                             }
                             JsonValue entry = JsonValue::object();
                             entry.set("name", JsonValue::string(field.name));
                             entry.set("type", JsonValue::string(field.type_name != nullptr ? field.type_name : "any"));
                             const bool event = field.type_name != nullptr && std::string(field.type_name) == "Signal";
                             if (field.method) {
                                 methods.items().push_back(std::move(entry));
                             } else if (event) {
                                 JsonValue params = JsonValue::array();
                                 for (int i = 0; i < field.param_count; ++i) {
                                     const engine_core::LuaParam& param = field.params[i];
                                     params.items().push_back(JsonValue::string(
                                         std::string(param.name != nullptr ? param.name : "") + ": " +
                                         (param.type_name != nullptr ? param.type_name : "any")));
                                 }
                                 entry.erase("type");
                                 entry.set("params", std::move(params));
                                 events.items().push_back(std::move(entry));
                             } else {
                                 if (!field.writable) {
                                     entry.set("readonly", JsonValue::boolean(true));
                                 }
                                 properties.items().push_back(std::move(entry));
                             }
                         }
                         JsonValue out = JsonValue::object();
                         out.set("class", JsonValue::string(class_name));
                         const char* base = engine_core::lua_class_base(class_name.c_str());
                         out.set("base", base != nullptr ? JsonValue::string(base) : JsonValue());
                         out.set("properties", std::move(properties));
                         out.set("methods", std::move(methods));
                         out.set("events", std::move(events));
                         return out;
                     }});

    if (studio.session && studio.start_test && studio.pause_test && studio.resume_test && studio.stop_test) {
        server.add_tool({"playtest",
                         "Runs the place as the studio's Test button does. start begins a test (or resumes a "
                         "paused one), pause and resume hold and continue it, stop ends it and restores the place "
                         "as it was before start. status only reports. Every action returns the session state.",
                         json_literal(R"({"type":"object","required":["action"],"properties":{
                             "action":{"type":"string","enum":["start","pause","resume","stop","status"]}}})"),
                         [studio](const JsonValue& arguments) {
                             const std::string& action = StringArg(arguments, "action");
                             const std::string before = studio.session();
                             if (action == "start") {
                                 if (before == "stopped") {
                                     studio.start_test();
                                 } else if (before == "paused") {
                                     studio.resume_test();
                                 }
                             } else if (action == "pause") {
                                 if (before == "running") {
                                     studio.pause_test();
                                 }
                             } else if (action == "resume") {
                                 if (before == "paused") {
                                     studio.resume_test();
                                 }
                             } else if (action == "stop") {
                                 if (before != "stopped") {
                                     studio.stop_test();
                                 }
                             } else if (action != "status") {
                                 throw std::runtime_error("action must be start, pause, resume, stop, or status.");
                             }
                             JsonValue out = JsonValue::object();
                             out.set("session", JsonValue::string(studio.session()));
                             return out;
                         }});
    }
}

}  // namespace ide
