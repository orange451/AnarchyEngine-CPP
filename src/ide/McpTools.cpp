#include "McpTools.hpp"

#include "profiler/ProfileJson.hpp"
#include "profiler/Profiler.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Enum.hpp"
#include "IdeResources.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "PropertySheet.hpp"
#include "ScopedRecording.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "SelectionService.hpp"
#include "Strings.hpp"
#include "TextSearch.hpp"
#include "UserInputService.hpp"

#include <filesystem>
#include <fstream>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
// How long a tool waits for script analysis, and how often it looks.
constexpr std::chrono::seconds kAnalysisWait(10);
constexpr std::chrono::milliseconds kAnalysisPoll(25);
// search_scripts lists this many matches unless asked for more, and cuts a
// longer line down to kMaxLineBytes.
constexpr std::size_t kDefaultMatches = 200;
constexpr std::size_t kMaxMatches = 2000;
constexpr std::size_t kMaxLineBytes = 300;
// The longest playtest run_for, in seconds, and how often it looks at the session.
constexpr double kMaxRunFor = 60;
constexpr std::chrono::milliseconds kPlayPoll(100);
constexpr int kMaxUndoSteps = 50;
constexpr int kDefaultCaptureSize = 1024;
// The most files one import_assets call takes.
constexpr std::size_t kMaxImports = 64;

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
        // Checked before the cast, which is undefined outside InstanceId's range.
        const bool whole = number >= 0 && number <= 4294967295.0 && std::floor(number) == number;
        if (!whole || !Exists(world, static_cast<InstanceId>(number))) {
            throw std::runtime_error("No instance has id " + engine_core::format_json_number(number) + ".");
        }
        return static_cast<InstanceId>(number);
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
        case PropertyKind::Vector2:
            return "Vector2";
        case PropertyKind::Color3:
            return "Color3";
        case PropertyKind::Ref:
            return "Instance";
        case PropertyKind::Transform:
            return "Matrix4";
        case PropertyKind::ReadOnlyText:
            return "string";
        case PropertyKind::Enum:
            return "EnumItem";
    }
    return "string";
}

JsonValue FloatTriple(engine_core::Vec3 value) {
    return JsonValue::array({JsonValue::number_from_float(value.x), JsonValue::number_from_float(value.y),
                             JsonValue::number_from_float(value.z)});
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
            return FloatTriple(row.value.vec);
        case PropertyKind::Vector2:
            return JsonValue::array({JsonValue::number_from_float(row.value.vec.x),
                                     JsonValue::number_from_float(row.value.vec.y)});
        case PropertyKind::Transform: {
            // As the Properties panel shows it: Position, and Orientation in degrees.
            JsonValue out = JsonValue::object();
            out.set("position", FloatTriple(row.value.vec));
            out.set("orientation", FloatTriple(row.value.orientation));
            return out;
        }
        case PropertyKind::Color3:
            return JsonValue::array({JsonValue::number_from_float(row.value.color.r),
                                     JsonValue::number_from_float(row.value.color.g),
                                     JsonValue::number_from_float(row.value.color.b)});
        case PropertyKind::Ref:
            if (row.value.nil_ref() || !Exists(world, row.value.ref)) {
                return JsonValue();
            }
            return Brief(world, row.value.ref);
        case PropertyKind::Enum: {
            // The item's name, as a project file holds it.
            const char* item = row.enum_type != nullptr
                                   ? engine_core::enum_item_name(*row.enum_type, static_cast<int>(row.value.number))
                                   : nullptr;
            return item != nullptr ? JsonValue::string(item) : JsonValue();
        }
    }
    return JsonValue();
}

// The instance's properties, as the Properties panel shows them.
JsonValue Properties(DataModel& world, InstanceId id) {
    JsonValue out = Brief(world, id);
    JsonValue properties = JsonValue::object();
    for (const PropertyRow& row : read_sheet(world, {id}, true).rows) {
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

// Checked before the cast, which is undefined past float's range.
float FloatArg(double number, const std::string& what) {
    if (!(std::fabs(number) <= static_cast<double>(std::numeric_limits<float>::max()))) {
        throw std::runtime_error(what + " is out of range.");
    }
    return static_cast<float>(number);
}

// [x, y, z], or {x, y, z}.
engine_core::Vec3 Vec3Arg(const JsonValue& value, const std::string& what) {
    double axes[3] = {0, 0, 0};
    if (value.is_array() && value.items().size() == 3) {
        for (int i = 0; i < 3; ++i) {
            axes[i] = NumberArg(value.items()[static_cast<std::size_t>(i)], what);
        }
    } else if (value.is_object() && value.find("x") && value.find("y") && value.find("z")) {
        axes[0] = NumberArg(*value.find("x"), what);
        axes[1] = NumberArg(*value.find("y"), what);
        axes[2] = NumberArg(*value.find("z"), what);
    } else {
        throw std::runtime_error(what + " takes [x, y, z].");
    }
    return {FloatArg(axes[0], what), FloatArg(axes[1], what), FloatArg(axes[2], what)};
}

engine_core::Vec3 Vec2Arg(const JsonValue& value, const std::string& what) {
    double axes[2] = {0, 0};
    if (value.is_array() && value.items().size() == 2) {
        for (int i = 0; i < 2; ++i) {
            axes[i] = NumberArg(value.items()[static_cast<std::size_t>(i)], what);
        }
    } else if (value.is_object() && value.find("x") && value.find("y")) {
        axes[0] = NumberArg(*value.find("x"), what);
        axes[1] = NumberArg(*value.find("y"), what);
    } else {
        throw std::runtime_error(what + " takes [x, y].");
    }
    return {FloatArg(axes[0], what), FloatArg(axes[1], what), 0.f};
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
        case PropertyKind::Vector3:
            edit.value.vec = Vec3Arg(value, row.name);
            break;
        case PropertyKind::Vector2:
            edit.value.vec = Vec2Arg(value, row.name);
            break;
        case PropertyKind::Transform: {
            // The whole matrix, built on the one held, so a part left out stays.
            engine_core::Matrix4 transform = row.value.transform;
            if (value.is_array() && value.items().size() == 16) {
                for (std::size_t i = 0; i < 16; ++i) {
                    transform.m[i] = FloatArg(NumberArg(value.items()[i], row.name), row.name);
                }
            } else if (value.is_object() && (value.find("position") || value.find("orientation"))) {
                if (const JsonValue* orientation = value.find("orientation")) {
                    transform = transform_with_orientation(transform, Vec3Arg(*orientation, row.name + ".orientation"));
                }
                if (const JsonValue* position = value.find("position")) {
                    const engine_core::Vec3 at = Vec3Arg(*position, row.name + ".position");
                    transform.m[12] = at.x;
                    transform.m[13] = at.y;
                    transform.m[14] = at.z;
                }
            } else {
                throw std::runtime_error(row.name + " takes {\"position\": [x, y, z], \"orientation\": [x, y, z]}, "
                                         "either part alone, or 16 numbers, column-major.");
            }
            edit.value.transform = transform;
            break;
        }
        case PropertyKind::Color3:
            if (value.is_string()) {
                if (!engine_core::color3_from_hex(value.as_string(), edit.value.color)) {
                    throw std::runtime_error(row.name + " takes [r, g, b] from 0 to 1, or a hex code.");
                }
            } else if (value.is_array() && value.items().size() == 3) {
                edit.value.color.r = FloatArg(NumberArg(value.items()[0], row.name), row.name);
                edit.value.color.g = FloatArg(NumberArg(value.items()[1], row.name), row.name);
                edit.value.color.b = FloatArg(NumberArg(value.items()[2], row.name), row.name);
            } else {
                throw std::runtime_error(row.name + " takes [r, g, b] from 0 to 1, or a hex code.");
            }
            break;
        case PropertyKind::Ref:
            edit.value.ref = value.is_null() ? DataModel::kNoParent : Resolve(world, &value, row.name.c_str());
            break;
        case PropertyKind::Enum: {
            // An item's name or its value, as a script may write it.
            int item = -1;
            if (row.enum_type != nullptr && value.is_string()) {
                item = engine_core::enum_item_value(*row.enum_type, value.as_string());
            } else if (row.enum_type != nullptr && value.is_number() &&
                       engine_core::enum_item_name(*row.enum_type, static_cast<int>(value.as_number())) != nullptr) {
                item = static_cast<int>(value.as_number());
            }
            if (item < 0) {
                std::string names;
                for (int i = 0; row.enum_type != nullptr && i < row.enum_type->count; ++i) {
                    names += (i == 0 ? "" : ", ");
                    names += row.enum_type->items[i].name;
                }
                throw std::runtime_error(row.name + " takes one of: " + names + ".");
            }
            edit.value.number = item;
            break;
        }
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

const char* SeverityName(engine_core::Severity severity) {
    switch (severity) {
        case engine_core::Severity::Error:
            return "error";
        case engine_core::Severity::Warning:
            return "warning";
        case engine_core::Severity::Information:
            return "information";
        case engine_core::Severity::Hint:
            return "hint";
    }
    return "error";
}

// In the order they appear. Lines and columns count from 1, as the editor shows them.
JsonValue ProblemList(std::vector<engine_core::Diagnostic> diagnostics) {
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const engine_core::Diagnostic& left, const engine_core::Diagnostic& right) {
                         if (left.range.start.line != right.range.start.line) {
                             return left.range.start.line < right.range.start.line;
                         }
                         return left.range.start.character < right.range.start.character;
                     });
    JsonValue list = JsonValue::array();
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        JsonValue entry = JsonValue::object();
        entry.set("line", JsonValue::number(diagnostic.range.start.line + 1.0));
        entry.set("column", JsonValue::number(diagnostic.range.start.character + 1.0));
        entry.set("severity", JsonValue::string(SeverityName(diagnostic.severity)));
        entry.set("code", JsonValue::string(diagnostic.code));
        entry.set("message", JsonValue::string(diagnostic.message));
        list.items().push_back(std::move(entry));
    }
    return list;
}

// What analysis found in some scripts, each checked against the Source it has now.
struct Checked {
    std::unordered_map<InstanceId, std::vector<engine_core::Diagnostic>> problems;
    // Scripts analysis had not finished when the wait ran out. A deleted script is in neither.
    std::vector<InstanceId> pending;
    // Scripts analysis does not check: outside the place, or, while the place
    // plays, a script with no result from Edit mode for its current source.
    std::vector<InstanceId> unchecked;
    // Analysis is turned off, so nothing was checked.
    bool off = false;
};

// Waits up to kAnalysisWait for analysis to check these scripts.
Checked CheckScripts(engine_core::Engine& engine, const std::vector<InstanceId>& ids) {
    engine_core::ScriptAnalysis& analysis = engine.analysis();
    Checked out;
    if (!analysis.enabled()) {
        out.off = true;
        return out;
    }
    // Shared, since an edit that runs after a timed-out wait still writes it.
    struct Progress {
        std::vector<InstanceId> waiting;
        std::unordered_map<InstanceId, std::vector<engine_core::Diagnostic>> done;
        std::unordered_set<InstanceId> unchecked;
    };
    auto progress = std::make_shared<Progress>();
    progress->waiting = ids;
    const auto deadline = std::chrono::steady_clock::now() + kAnalysisWait;
    while (true) {
        try {
            // pump() publishes finished checks. It runs where edits run: the
            // simulation thread, or this thread under a paused edit, which is the
            // gameplay thread its contract allows. There it reads the tree it
            // compares against.
            RunEdit(engine, [&analysis, progress](DataModel& world) {
                analysis.pump();
                std::vector<InstanceId> still;
                for (InstanceId id : progress->waiting) {
                    const auto* script = dynamic_cast<const engine_core::LuaSource*>(world.instance(id));
                    if (script == nullptr) {
                        continue;
                    }
                    const std::optional<std::string> checked = analysis.analyzed_source(id);
                    // Ask once. The place thread can change the answer between two
                    // calls, and a second answer of settled would read an empty checked.
                    const bool settled = analysis.settled(id);
                    if (settled && !checked) {
                        // Settled with nothing checked: analysis does not check this script.
                        progress->unchecked.insert(id);
                    } else if (settled && *checked == script->source()) {
                        progress->done[id] = analysis.diagnostics(id);
                    } else if (settled && world.simulation_running()) {
                        // Nothing is checked during play, and its last result is
                        // for a source play has changed since.
                        progress->unchecked.insert(id);
                    } else {
                        still.push_back(id);
                    }
                }
                progress->waiting.swap(still);
                return JsonValue();
            });
        } catch (const std::exception&) {
            // The simulation is too busy to look. What is not done yet is pending.
            break;
        }
        if (progress->waiting.empty() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kAnalysisPoll);
    }
    for (InstanceId id : ids) {
        const auto found = progress->done.find(id);
        if (found != progress->done.end()) {
            out.problems.emplace(id, found->second);
        } else if (progress->unchecked.count(id) != 0) {
            out.unchecked.push_back(id);
        } else if (std::find(progress->waiting.begin(), progress->waiting.end(), id) != progress->waiting.end()) {
            out.pending.push_back(id);
        }
    }
    return out;
}

// Adds what analysis found in the script to out: its problems, or analysis
// "pending", "not checked", or "off" when there is no answer to give.
void AddProblems(engine_core::Engine& engine, InstanceId id, JsonValue& out) {
    const Checked checked = CheckScripts(engine, {id});
    const auto found = checked.problems.find(id);
    if (found != checked.problems.end()) {
        out.set("problems", ProblemList(found->second));
    } else {
        const bool unchecked = !checked.unchecked.empty();
        out.set("analysis", JsonValue::string(checked.off ? "off" : unchecked ? "not checked" : "pending"));
    }
}

// Sets a script's Source to what change makes of it, as an editor's typing
// does: one undo step. Open editors write their typing first and show the
// new Source after. Returns the script, with what analysis finds in it.
JsonValue WriteSource(engine_core::Engine& engine, const McpStudio& studio, const JsonValue& instance,
                      std::function<std::string(const std::string&)> change) {
    if (studio.flush_scripts) {
        studio.flush_scripts();
    }
    JsonValue out = RunEdit(engine, [instance, change = std::move(change)](DataModel& world) {
        const InstanceId id = Resolve(world, &instance);
        engine_core::LuaSource& script = ScriptAt(world, id);
        const std::string source = change(script.source());
        if (script.source() != source) {
            // The same waypoint an editor's typing makes.
            std::optional<std::string> recording;
            if (!world.simulation_running()) {
                recording = world.history().try_begin_recording("Edit Script");
            }
            script.set_source(source);
            if (recording) {
                world.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
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
    AddProblems(engine, static_cast<InstanceId>(out.find("id")->as_number()), out);
    return out;
}

// The 1-based line that byte at of text is on.
int LineAt(const std::string& text, std::size_t at) {
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
}

// source with each of edits applied in turn. An old_text must be in the text
// exactly once, or with replace_all at least once. Throws naming the edit
// that could not apply. replaced counts the replacements.
std::string ApplyEdits(std::string source, const JsonValue* edits, int& replaced) {
    if (edits == nullptr || !edits->is_array() || edits->items().empty()) {
        throw std::runtime_error("edits is required: a list of {old_text, new_text}.");
    }
    int index = 0;
    for (const JsonValue& edit : edits->items()) {
        const std::string which = "Edit " + std::to_string(++index);
        const JsonValue* old_text = edit.find("old_text");
        const JsonValue* new_text = edit.find("new_text");
        if (old_text == nullptr || !old_text->is_string() || new_text == nullptr || !new_text->is_string()) {
            throw std::runtime_error(which + " needs old_text and new_text, both strings.");
        }
        const std::string& before = old_text->as_string();
        if (before.empty()) {
            throw std::runtime_error(which + ": old_text is empty.");
        }
        const JsonValue* every = edit.find("replace_all");
        const bool all = every != nullptr && every->is_bool() && every->as_bool();
        std::vector<std::size_t> found;
        for (std::size_t at = source.find(before); at != std::string::npos; at = source.find(before, at + before.size())) {
            found.push_back(at);
        }
        if (found.empty()) {
            throw std::runtime_error(which + ": old_text is not in the Source. It must match exactly, spaces "
                                             "and line breaks included.");
        }
        if (found.size() > 1 && !all) {
            std::string lines;
            for (std::size_t at : found) {
                lines += (lines.empty() ? "" : ", ") + std::to_string(LineAt(source, at));
            }
            throw std::runtime_error(which + ": old_text is in the Source " + std::to_string(found.size()) +
                                     " times, on lines " + lines +
                                     ". Include more of the lines around it, or set replace_all.");
        }
        std::string next;
        next.reserve(source.size());
        std::size_t kept = 0;
        for (std::size_t at : found) {
            next.append(source, kept, at - kept);
            next += new_text->as_string();
            kept = at + before.size();
        }
        next.append(source, kept, std::string::npos);
        source = std::move(next);
        replaced += static_cast<int>(found.size());
    }
    return source;
}

// A number argument, clamped to [low, high], or fallback when it is missing.
double NumberArg(const JsonValue& arguments, const char* key, double fallback, double low, double high) {
    const JsonValue* value = arguments.find(key);
    if (value == nullptr) {
        return fallback;
    }
    return std::max(low, std::min(high, NumberArg(*value, key)));
}

// A whole number argument, clamped to [low, high], or fallback when it is missing.
int IntArg(const JsonValue& arguments, const char* key, int fallback, int low, int high) {
    return static_cast<int>(NumberArg(arguments, key, fallback, low, high));
}

bool BoolArg(const JsonValue& arguments, const char* key, bool fallback) {
    const JsonValue* value = arguments.find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_bool()) {
        throw std::runtime_error(std::string(key) + " must be true or false.");
    }
    return value->as_bool();
}

// A string argument, or "" when it is missing or not a string.
std::string OptionalStringArg(const JsonValue& arguments, const char* key) {
    const JsonValue* value = arguments.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : "";
}

// A string argument that must be one of choices, or fallback when it is missing.
std::string ChoiceArg(const JsonValue& arguments, const char* key, const char* fallback,
                      const std::vector<std::string>& choices) {
    const JsonValue* value = arguments.find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->is_string() && std::find(choices.begin(), choices.end(), value->as_string()) != choices.end()) {
        return value->as_string();
    }
    std::string list;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        list += (i == 0 ? "" : i + 1 < choices.size() ? ", " : ", or ") + choices[i];
    }
    throw std::runtime_error(std::string(key) + " must be " + list + ".");
}

// An argument as given, or fallback when it is missing. A copy, for an edit
// that reads it on the simulation thread.
JsonValue ValueArg(const JsonValue& arguments, const char* key, JsonValue fallback = JsonValue()) {
    const JsonValue* value = arguments.find(key);
    return value != nullptr ? *value : std::move(fallback);
}

// The Scripts and ModuleScripts at and under id, in the explorer's order.
void CollectScripts(const DataModel& world, InstanceId id, std::vector<InstanceId>& out) {
    if (dynamic_cast<const engine_core::LuaSource*>(world.instance(id)) != nullptr) {
        out.push_back(id);
    }
    for (InstanceId child : world.get_children(id)) {
        CollectScripts(world, child, out);
    }
}

// text cut to about kMaxLineBytes, on a UTF-8 boundary, marked when cut.
std::string ClipLine(std::string text) {
    if (text.size() <= kMaxLineBytes) {
        return text;
    }
    text.resize(engine_core::fit_utf8(text, kMaxLineBytes));
    return text + "...";
}

// The lines of text the matches are on, each once, counting from 1.
JsonValue MatchedLines(const std::string& text, const std::vector<TextMatch>& matches) {
    JsonValue lines = JsonValue::array();
    int shown = 0;
    for (const TextMatch& match : matches) {
        if (match.line + 1 == shown) {
            continue;
        }
        shown = match.line + 1;
        std::size_t stop = text.find('\n', match.line_byte);
        if (stop == std::string::npos) {
            stop = text.size();
        }
        JsonValue line = JsonValue::object();
        line.set("line", JsonValue::number(shown));
        line.set("text", JsonValue::string(ClipLine(text.substr(match.line_byte, stop - match.line_byte))));
        lines.items().push_back(std::move(line));
    }
    return lines;
}

// What every tool's code shares: the engine it works on and the studio's hooks.
struct ToolContext {
    engine_core::Engine& engine;
    McpStudio studio;
};

// Each tool below runs one call. engine_tool_specs has its name, description, and schema.

JsonValue GetTree(const ToolContext& context, const JsonValue& arguments) {
    DataModel& world = context.engine.datamodel();
    const int depth = IntArg(arguments, "depth", kDefaultDepth, 0, kMaxDepth);
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
}

JsonValue FindInstances(const ToolContext& context, const JsonValue& arguments) {
    const std::string needle = AsciiLower(StringArg(arguments, "name"));
    const JsonValue* only = arguments.find("class");
    DataModel& world = context.engine.datamodel();
    ReadLock lock(world);
    JsonValue found = JsonValue::array();
    std::vector<InstanceId> pending = world.get_children(world.id());
    while (!pending.empty() && found.items().size() < 200) {
        const InstanceId id = pending.back();
        pending.pop_back();
        if (AsciiLower(world.name(id)).find(needle) != std::string::npos &&
            (only == nullptr || ClassOf(world, id) == only->as_string())) {
            found.items().push_back(Brief(world, id));
        }
        const std::vector<InstanceId> children = world.get_children(id);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    JsonValue out = JsonValue::object();
    out.set("instances", std::move(found));
    return out;
}

JsonValue GetProperties(const ToolContext& context, const JsonValue& arguments) {
    DataModel& world = context.engine.datamodel();
    ReadLock lock(world);
    const JsonValue* several = arguments.find("instances");
    if (several == nullptr) {
        return Properties(world, Resolve(world, arguments.find("instance")));
    }
    if (!several->is_array()) {
        throw std::runtime_error("instances must be a list of ids or paths.");
    }
    JsonValue list = JsonValue::array();
    for (const JsonValue& item : several->items()) {
        list.items().push_back(Properties(world, Resolve(world, &item)));
    }
    JsonValue out = JsonValue::object();
    out.set("instances", std::move(list));
    return out;
}

JsonValue SetProperty(const ToolContext& context, const JsonValue& arguments) {
    const JsonValue instance = ValueArg(arguments, "instance");
    const std::string property = StringArg(arguments, "property");
    const JsonValue* given = arguments.find("value");
    if (given == nullptr) {
        throw std::runtime_error("value is required.");
    }
    const JsonValue value = *given;
    return RunEdit(context.engine, [instance, property, value](DataModel& world) {
        const InstanceId id = Resolve(world, &instance);
        const PropertySheet sheet = read_sheet(world, {id}, true);
        const PropertyRow* row = sheet.find(property);
        if (row == nullptr) {
            throw std::runtime_error(PathOf(world, id) + " has no property " + property + " that can be set here.");
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
}

JsonValue CreateInstance(const ToolContext& context, const JsonValue& arguments) {
    const std::string class_name = StringArg(arguments, "class");
    const JsonValue parent = ValueArg(arguments, "parent", JsonValue::string("Workspace"));
    const std::string name = OptionalStringArg(arguments, "name");
    return RunEdit(context.engine, [class_name, parent, name](DataModel& world) {
        const InstanceId parent_id = Resolve(world, &parent, "parent");
        if (!engine_core::lua_script_creatable(class_name.c_str())) {
            throw std::runtime_error("Instance.new cannot make \"" + class_name +
                                     "\". list_classes names the ones it can.");
        }
        // Asked by class, before create, so a refused parent leaves nothing behind.
        if (std::optional<std::string> refused = world.placement_error_for_class(parent_id, class_name)) {
            throw std::runtime_error(*refused);
        }
        // Refused before the recording opens, so a full place leaves no empty step.
        if (world.room_left() == 0) {
            throw engine_core::InstanceCapacityError();
        }
        engine_core::InstanceId made_id = 0;
        {
            ScopedRecording step(world, "Insert " + class_name);
            DataModel* made = engine_core::lua_create_instance(world, class_name.c_str());
            if (made == nullptr) {
                throw std::runtime_error("Could not create " + class_name + ".");
            }
            if (!name.empty()) {
                world.set_name(made->id(), name);
            }
            world.set_parent(made->id(), parent_id);
            made_id = made->id();
        }
        // An edit while stopped is part of the place, as the explorer's insert is.
        if (!world.simulation_running()) {
            world.capture_place();
        }
        return Brief(world, made_id);
    });
}

JsonValue DeleteInstance(const ToolContext& context, const JsonValue& arguments) {
    const JsonValue instance = ValueArg(arguments, "instance");
    return RunEdit(context.engine, [instance](DataModel& world) {
        const InstanceId id = Resolve(world, &instance);
        if (std::optional<std::string> error = world.destroy_error(id)) {
            throw std::runtime_error(*error);
        }
        JsonValue out = JsonValue::object();
        out.set("deleted", Brief(world, id));
        {
            ScopedRecording step(world, "Delete");
            world.destroy_tree(id);
        }
        return out;
    });
}

// One file's row in import_assets' result.
JsonValue ImportRow(const DataModel& world, const McpImport& import) {
    JsonValue row = JsonValue::object();
    row.set("file", JsonValue::string(import.file));
    row.set("kind", JsonValue::string(import.kind));
    if (!import.error.empty() || !world.alive(import.root)) {
        row.set("error", JsonValue::string(import.error.empty() ? "Nothing was made." : import.error));
        return row;
    }
    if (import.kind == "texture") {
        row.set("texture", Brief(world, import.root));
        if (const auto* texture = dynamic_cast<const engine_core::Texture*>(world.instance(import.root))) {
            row.set("resource_path", JsonValue::string(texture->path()));
        }
        return row;
    }
    row.set("prefab", Brief(world, import.root));
    JsonValue folders = JsonValue::array();
    std::size_t meshes = 0;
    std::size_t materials = 0;
    std::size_t textures = 0;
    for (InstanceId id : import.made) {
        const std::string klass = ClassOf(world, id);
        if (klass == "Folder") {
            folders.items().push_back(Brief(world, id));
        }
        meshes += klass == "Mesh";
        materials += klass == "Material";
        textures += klass == "Texture";
    }
    row.set("folders", std::move(folders));
    row.set("meshes", JsonValue::number(static_cast<double>(meshes)));
    row.set("materials", JsonValue::number(static_cast<double>(materials)));
    row.set("textures", JsonValue::number(static_cast<double>(textures)));
    if (!import.notes.empty()) {
        JsonValue notes = JsonValue::array();
        for (const std::string& note : import.notes) {
            notes.items().push_back(JsonValue::string(note));
        }
        row.set("left_out", std::move(notes));
    }
    return row;
}

JsonValue ImportAssets(const ToolContext& context, const JsonValue& arguments) {
    const JsonValue* list = arguments.find("files");
    if (list == nullptr || !list->is_array() || list->items().empty()) {
        throw std::runtime_error("files is required: a list of absolute paths to image or model files.");
    }
    if (list->items().size() > kMaxImports) {
        throw std::runtime_error("files lists more than " + std::to_string(kMaxImports) + " files. Import them in parts.");
    }
    std::vector<std::string> files;
    for (const JsonValue& item : list->items()) {
        if (!item.is_string() || item.as_string().empty()) {
            throw std::runtime_error("Each of files must be a path, as a string.");
        }
        // The studio's own working folder means nothing to the caller.
        if (!path_from_utf8(item.as_string()).is_absolute()) {
            throw std::runtime_error(item.as_string() + " is not an absolute path.");
        }
        files.push_back(item.as_string());
    }
    // The files are read and written here, on this server thread, so a large
    // model does not hold the studio's window. Only the instances wait for the edit.
    const McpPlaceImports place = context.studio.import_files(files);
    return RunEdit(context.engine, [place](DataModel& world) {
        std::vector<McpImport> imports;
        {
            ScopedRecording step(world, "Import Assets");
            imports = place(world);
        }
        // An edit while stopped is part of the place, as the explorer's insert is.
        if (!world.simulation_running()) {
            world.capture_place();
        }
        JsonValue rows = JsonValue::array();
        std::size_t failed = 0;
        for (const McpImport& import : imports) {
            JsonValue row = ImportRow(world, import);
            failed += row.find("error") != nullptr;
            rows.items().push_back(std::move(row));
        }
        JsonValue out = JsonValue::object();
        out.set("imported", JsonValue::number(static_cast<double>(imports.size() - failed)));
        out.set("failed", JsonValue::number(static_cast<double>(failed)));
        out.set("files", std::move(rows));
        return out;
    });
}

JsonValue ReadScript(const ToolContext& context, const JsonValue& arguments) {
    DataModel& world = context.engine.datamodel();
    JsonValue out;
    std::string source;
    {
        ReadLock lock(world);
        const InstanceId id = Resolve(world, arguments.find("instance"));
        out = Brief(world, id);
        source = ScriptAt(world, id).source();
    }
    const int lines = LineAt(source, source.size());
    out.set("line_count", JsonValue::number(lines));
    if (arguments.find("first_line") == nullptr && arguments.find("last_line") == nullptr) {
        out.set("source", JsonValue::string(std::move(source)));
        return out;
    }
    const int first = IntArg(arguments, "first_line", 1, 1, lines + 1);
    const int last = IntArg(arguments, "last_line", lines, 1, lines);
    if (first > lines) {
        throw std::runtime_error("The Source has " + std::to_string(lines) + " lines.");
    }
    if (last < first) {
        throw std::runtime_error("last_line is before first_line.");
    }
    // Byte offsets of the first line's start and the last line's end.
    std::size_t begin = 0;
    for (int line = 1; line < first; ++line) {
        begin = source.find('\n', begin) + 1;
    }
    std::size_t end = begin;
    for (int line = first; line <= last; ++line) {
        end = source.find('\n', end);
        if (end == std::string::npos) {
            end = source.size();
            break;
        }
        if (line < last) {
            ++end;
        }
    }
    out.set("first_line", JsonValue::number(first));
    out.set("last_line", JsonValue::number(last));
    out.set("source", JsonValue::string(source.substr(begin, end - begin)));
    return out;
}

JsonValue WriteScript(const ToolContext& context, const JsonValue& arguments) {
    const JsonValue instance = ValueArg(arguments, "instance");
    const std::string source = StringArg(arguments, "source");
    return WriteSource(context.engine, context.studio, instance, [source](const std::string&) { return source; });
}

JsonValue EditScript(const ToolContext& context, const JsonValue& arguments) {
    const JsonValue instance = ValueArg(arguments, "instance");
    const JsonValue edits = ValueArg(arguments, "edits");
    auto replaced = std::make_shared<int>(0);
    JsonValue out = WriteSource(context.engine, context.studio, instance, [edits, replaced](const std::string& source) {
        *replaced = 0;
        return ApplyEdits(source, &edits, *replaced);
    });
    out.set("replaced", JsonValue::number(*replaced));
    return out;
}

JsonValue SearchScripts(const ToolContext& context, const JsonValue& arguments) {
    SearchQuery query;
    query.pattern = StringArg(arguments, "pattern");
    query.regex = BoolArg(arguments, "regex", false);
    query.match_case = BoolArg(arguments, "match_case", false);
    query.whole_word = BoolArg(arguments, "whole_word", false);
    const TextSearch search(query);
    if (!search.ready()) {
        throw std::runtime_error(search.error().empty() ? "pattern is empty." : "pattern is not a regex: " + search.error());
    }
    std::size_t left = static_cast<std::size_t>(
        IntArg(arguments, "limit", static_cast<int>(kDefaultMatches), 1, static_cast<int>(kMaxMatches)));
    struct Source {
        JsonValue brief;
        std::string text;
    };
    std::vector<Source> sources;
    {
        DataModel& world = context.engine.datamodel();
        ReadLock lock(world);
        const JsonValue* ref = arguments.find("instance");
        std::vector<InstanceId> ids;
        CollectScripts(world, ref != nullptr ? Resolve(world, ref) : world.id(), ids);
        for (InstanceId id : ids) {
            sources.push_back({Brief(world, id), ScriptAt(world, id).source()});
        }
    }
    JsonValue scripts = JsonValue::array();
    std::size_t total = 0;
    bool truncated = false;
    for (Source& source : sources) {
        if (left == 0) {
            truncated = true;
            break;
        }
        std::vector<TextMatch> matches = search.find_all(source.text, left + 1);
        if (matches.size() > left) {
            matches.resize(left);
            truncated = true;
        }
        if (matches.empty()) {
            continue;
        }
        left -= matches.size();
        total += matches.size();
        JsonValue entry = std::move(source.brief);
        entry.erase("name");
        entry.set("lines", MatchedLines(source.text, matches));
        scripts.items().push_back(std::move(entry));
    }
    JsonValue out = JsonValue::object();
    out.set("scripts", std::move(scripts));
    out.set("matches", JsonValue::number(static_cast<double>(total)));
    if (truncated) {
        out.set("truncated", JsonValue::boolean(true));
    }
    return out;
}

JsonValue GetDiagnostics(const ToolContext& context, const JsonValue& arguments) {
    DataModel& world = context.engine.datamodel();
    std::vector<InstanceId> ids;
    {
        ReadLock lock(world);
        const JsonValue* ref = arguments.find("instance");
        CollectScripts(world, ref != nullptr ? Resolve(world, ref) : world.id(), ids);
    }
    const Checked checked = CheckScripts(context.engine, ids);
    if (checked.off) {
        throw std::runtime_error("Script analysis is turned off in this studio.");
    }
    JsonValue scripts = JsonValue::array();
    JsonValue pending = JsonValue::array();
    int errors = 0;
    int warnings = 0;
    ReadLock lock(world);
    for (InstanceId id : ids) {
        const auto found = checked.problems.find(id);
        if (found == checked.problems.end() || found->second.empty() || !Exists(world, id)) {
            continue;
        }
        for (const engine_core::Diagnostic& diagnostic : found->second) {
            errors += diagnostic.severity == engine_core::Severity::Error ? 1 : 0;
            warnings += diagnostic.severity == engine_core::Severity::Warning ? 1 : 0;
        }
        JsonValue entry = Brief(world, id);
        entry.erase("name");
        entry.set("problems", ProblemList(found->second));
        scripts.items().push_back(std::move(entry));
    }
    for (InstanceId id : checked.pending) {
        if (Exists(world, id)) {
            pending.items().push_back(JsonValue::string(PathOf(world, id)));
        }
    }
    JsonValue unchecked = JsonValue::array();
    for (InstanceId id : checked.unchecked) {
        if (Exists(world, id)) {
            unchecked.items().push_back(JsonValue::string(PathOf(world, id)));
        }
    }
    JsonValue out = JsonValue::object();
    out.set("checked", JsonValue::number(static_cast<double>(checked.problems.size())));
    out.set("errors", JsonValue::number(errors));
    out.set("warnings", JsonValue::number(warnings));
    out.set("scripts", std::move(scripts));
    if (!pending.items().empty()) {
        out.set("pending", std::move(pending));
    }
    if (!unchecked.items().empty()) {
        out.set("unchecked", std::move(unchecked));
    }
    return out;
}

JsonValue RunLua(const ToolContext& context, const JsonValue& arguments) {
    const std::string source = StringArg(arguments, "source");
    engine_core::ScriptRuntime* scripts = &context.engine.scripts();
    return RunEdit(context.engine, [scripts, source](DataModel&) {
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
}

JsonValue GetOutput(const ToolContext& context, const JsonValue& arguments) {
    // 2^53: far past any line, and exact, so the cast is defined.
    const auto since = static_cast<std::uint64_t>(NumberArg(arguments, "since", 0, 0, 9007199254740992.0));
    const auto limit =
        static_cast<std::size_t>(IntArg(arguments, "limit", static_cast<int>(kDefaultOutputLines), 1, 1000));
    const engine_core::ScriptRuntime::OutputHistory history = context.engine.scripts().output_since(since, limit);
    JsonValue out = JsonValue::object();
    out.set("lines", OutputLines(history));
    out.set("next", JsonValue::number(static_cast<double>(history.first + history.lines.size())));
    return out;
}

JsonValue GetSelection(const ToolContext& context, const JsonValue&) {
    DataModel& world = context.engine.datamodel();
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
}

JsonValue SetSelection(const ToolContext& context, const JsonValue& arguments) {
    const JsonValue* given = arguments.find("instances");
    if (given == nullptr || !given->is_array()) {
        throw std::runtime_error("instances is required and must be a list.");
    }
    DataModel& world = context.engine.datamodel();
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
}

JsonValue Undo(const ToolContext& context, const JsonValue& arguments) {
    const bool redo = BoolArg(arguments, "redo", false);
    const int count = IntArg(arguments, "count", 1, 1, kMaxUndoSteps);
    if (context.studio.flush_scripts) {
        context.studio.flush_scripts();
    }
    JsonValue out = RunEdit(context.engine, [redo, count](DataModel& world) {
        engine_core::ChangeHistoryService& history = world.history();
        // History neither undoes nor redoes in the middle of an edit.
        if (history.is_recording_in_progress()) {
            throw std::runtime_error("An edit is still being made in the studio. Try again once it is done.");
        }
        JsonValue steps = JsonValue::array();
        for (int step = 0; step < count; ++step) {
            const std::pair<bool, std::string> next = redo ? history.can_redo() : history.can_undo();
            if (!next.first) {
                break;
            }
            steps.items().push_back(JsonValue::string(next.second));
            if (redo) {
                history.redo();
            } else {
                history.undo();
            }
        }
        const std::pair<bool, std::string> undo_next = history.can_undo();
        const std::pair<bool, std::string> redo_next = history.can_redo();
        JsonValue result = JsonValue::object();
        result.set(redo ? "redone" : "undone", std::move(steps));
        result.set("next_undo", undo_next.first ? JsonValue::string(undo_next.second) : JsonValue());
        result.set("next_redo", redo_next.first ? JsonValue::string(redo_next.second) : JsonValue());
        return result;
    });
    // Idle editors show the Source the history put back.
    if (context.studio.refresh_scripts) {
        context.studio.refresh_scripts();
    }
    return out;
}

JsonValue ListClasses(const ToolContext&, const JsonValue&) {
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
}

JsonValue GetClass(const ToolContext&, const JsonValue& arguments) {
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
                params.items().push_back(JsonValue::string(std::string(param.name != nullptr ? param.name : "") +
                                                           ": " + (param.type_name != nullptr ? param.type_name : "any")));
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
}

// Does what a playtest action asks of the session. One the session is already
// in, and status, do nothing.
void ChangeSession(const McpStudio& studio, const std::string& action) {
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
}

// Lets a test play until deadline, or until someone pauses or stops it in the
// studio. With end_on_error, stops waiting at the first error printed since
// from. True when an error ended the wait.
bool WaitWhilePlaying(const ToolContext& context, std::chrono::steady_clock::time_point deadline, bool end_on_error,
                      std::uint64_t from) {
    engine_core::ScriptRuntime& scripts = context.engine.scripts();
    std::uint64_t seen = from;
    bool errored = false;
    while (!errored) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(kPlayPoll, deadline - now));
        if (end_on_error) {
            const engine_core::ScriptRuntime::OutputHistory newer = scripts.output_since(seen, kDefaultOutputLines);
            for (const engine_core::ScriptRuntime::OutputLine& line : newer.lines) {
                errored = errored || line.kind == engine_core::ScriptRuntime::OutputKind::Error;
            }
            seen = newer.first + newer.lines.size();
        }
        // Someone pressed Pause or Stop in the studio.
        if (!errored && context.studio.session() != "running") {
            break;
        }
    }
    return errored;
}

JsonValue Playtest(const ToolContext& context, const JsonValue& arguments) {
    const McpStudio& studio = context.studio;
    const std::string& action = StringArg(arguments, "action");
    const double run_for = NumberArg(arguments, "run_for", 0, 0, kMaxRunFor);
    const std::string then = ChoiceArg(arguments, "then", "pause", {"pause", "stop", "run"});
    const bool end_on_error = BoolArg(arguments, "end_on_error", true);
    engine_core::ScriptRuntime& scripts = context.engine.scripts();
    const std::uint64_t from = scripts.output_next();
    ChangeSession(studio, action);
    JsonValue out = JsonValue::object();
    if (run_for <= 0 || (action != "start" && action != "resume")) {
        out.set("session", JsonValue::string(studio.session()));
        return out;
    }
    const auto began = std::chrono::steady_clock::now();
    const auto deadline =
        began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(run_for));
    const bool errored = WaitWhilePlaying(context, deadline, end_on_error, from);
    const double ran = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    const std::string after = studio.session();
    if (then == "pause" && after == "running") {
        studio.pause_test();
    } else if (then == "stop" && after != "stopped") {
        studio.stop_test();
    }
    const engine_core::ScriptRuntime::OutputHistory printed = scripts.output_since(from, kDefaultOutputLines);
    int errors = 0;
    for (const engine_core::ScriptRuntime::OutputLine& line : printed.lines) {
        errors += line.kind == engine_core::ScriptRuntime::OutputKind::Error ? 1 : 0;
    }
    out.set("session", JsonValue::string(studio.session()));
    out.set("ran_for", JsonValue::number(std::round(ran * 100.0) / 100.0));
    if (errored) {
        out.set("ended_on_error", JsonValue::boolean(true));
    }
    out.set("errors", JsonValue::number(errors));
    out.set("output", OutputLines(printed));
    const std::uint64_t next = printed.first + printed.lines.size();
    if (next < printed.next) {
        // More was printed than fits. get_output since next reads on.
        out.set("next", JsonValue::number(static_cast<double>(next)));
    }
    return out;
}

JsonValue Screenshot(const ToolContext& context, const JsonValue& arguments) {
    const McpImage image = context.studio.capture_view(IntArg(arguments, "max_size", kDefaultCaptureSize, 64, 2048));
    JsonValue picture = JsonValue::object();
    picture.set("data", JsonValue::string(base64_encode(image.png)));
    picture.set("mimeType", JsonValue::string("image/png"));
    JsonValue out = JsonValue::object();
    out.set("width", JsonValue::number(image.width));
    out.set("height", JsonValue::number(image.height));
    // What mouse_input measures in, to turn a point in the picture into one in the view.
    if (image.view_width > 0 && image.view_height > 0) {
        out.set("view_width", JsonValue::number(image.view_width));
        out.set("view_height", JsonValue::number(image.view_height));
    }
    out.set(kImageMember, std::move(picture));
    return out;
}

JsonValue GetProfile(const ToolContext&, const JsonValue& arguments) {
    const double seconds = NumberArg(arguments, "seconds", 2.0, 0.1, 10.0);
    profiler::ReportOptions options;
    options.top = IntArg(arguments, "top", 25, 1, 200);
    options.include_timeline = BoolArg(arguments, "include_timeline", true);
    const JsonValue* path = arguments.find("path");
    if (path != nullptr && !path->is_string()) {
        throw std::runtime_error("path must be a string.");
    }
    double recorded = 0;
    // Nothing recording, as with the profiler hidden or the window minimized:
    // record here, collecting on this thread, since no paint will.
    const bool own = !profiler::paused() && !profiler::enabled();
    if (own) {
        profiler::acquire();
        const auto began = std::chrono::steady_clock::now();
        while (recorded < seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            profiler::collect();
            recorded = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        }
    } else if (!profiler::paused()) {
        profiler::collect();
    }
    JsonValue out;
    std::string capture;
    profiler::with_view([&](const profiler::History& history) {
        out = profiler::build_report(history, options);
        if (path != nullptr) {
            capture = profiler::write_capture_html(history, "MCP", profiler::utc_stamp(std::time(nullptr)));
        }
    });
    if (own) {
        profiler::release();
    }
    out.set("recorded_for", JsonValue::number(std::round(recorded * 100.0) / 100.0));
    out.set("paused", JsonValue::boolean(profiler::paused()));
    if (path != nullptr) {
        const std::filesystem::path file = std::filesystem::u8path(path->as_string());
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        stream << capture;
        stream.close();
        if (!stream) {
            throw std::runtime_error("Could not write " + path->as_string() + ".");
        }
        out.set("saved", JsonValue::string(path->as_string()));
    }
    return out;
}

JsonValue Tabs(const ToolContext& context, const JsonValue& arguments) {
    const std::string action = ChoiceArg(arguments, "action", "list", {"list", "select", "close", "open"});
    if (action != "list") {
        context.studio.change_tab(action, StringArg(arguments, "tab"));
    }
    return context.studio.tabs();
}

JsonValue SavePlace(const ToolContext& context, const JsonValue& arguments) {
    std::string folder;
    if (const JsonValue* given = arguments.find("folder")) {
        if (!given->is_string() || !path_from_utf8(given->as_string()).is_absolute()) {
            throw std::runtime_error("folder must be an absolute path.");
        }
        folder = given->as_string();
    }
    return context.studio.save_place(folder);
}

JsonValue ShowProfiler(const ToolContext& context, const JsonValue& arguments) {
    std::optional<bool> shown;
    if (arguments.find("on") != nullptr) {
        shown = BoolArg(arguments, "on", false);
    }
    JsonValue out = JsonValue::object();
    out.set("shown", JsonValue::boolean(context.studio.show_profiler(shown)));
    return out;
}

JsonValue GpuDetail(const ToolContext&, const JsonValue& arguments) {
    profiler::set_gpu_detail(BoolArg(arguments, "on", !profiler::gpu_detail()));
    JsonValue out = JsonValue::object();
    out.set("on", JsonValue::boolean(profiler::gpu_detail()));
    return out;
}

JsonValue GetStudioInfo(const ToolContext& context, const JsonValue&) {
    JsonValue out = context.studio.info();
    if (context.studio.session) {
        out.set("session", JsonValue::string(context.studio.session()));
    }
    return out;
}

// A number that must be given.
double RequiredNumber(const JsonValue& arguments, const char* key) {
    const JsonValue* value = arguments.find(key);
    if (value == nullptr) {
        throw std::runtime_error(std::string(key) + " is required.");
    }
    return NumberArg(*value, key);
}

// The pause between the steps of one mouse_input or key_input, so each lands
// in a frame of its own: a script sees the pointer arrive before the press,
// and the press before the release.
constexpr double kInputStep = 0.05;
// The longest a button or key is held, and the most moves a drag takes.
constexpr double kMaxHold = 30;
constexpr int kMaxDragSteps = 120;
// How long a drag takes to move unless told.
constexpr double kDragSeconds = 0.3;

void Pause(double seconds) { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); }

// A modifier the mouse's modifiers name: its Key::Mod bit and the key that holds it.
struct Modifier {
    const char* name;
    int bit;
    const char* key;
};

constexpr Modifier kModifiers[] = {
    {"shift", 0x1, "LeftShift"}, {"ctrl", 0x2, "LeftControl"}, {"alt", 0x4, "LeftAlt"}, {"super", 0x8, "LeftSuper"}};

McpInput KeyChange(int key, bool down) {
    McpInput input;
    input.kind = McpInput::Kind::Key;
    input.code = key;
    input.down = down;
    return input;
}

McpInput PointerEvent(McpInput::Kind kind, double x, double y) {
    McpInput input;
    input.kind = kind;
    input.x = x;
    input.y = y;
    return input;
}

// The GLFW key a character is typed with, unshifted, or -1. GLFW numbers the
// printable keys by their ASCII characters, letters in upper case.
int KeyForCharacter(unsigned char c) {
    if (c == '\n') {
        return engine_core::UserInputService::glfw_key_named("Return");
    }
    if (c == '\t') {
        return engine_core::UserInputService::glfw_key_named("Tab");
    }
    const int key = std::toupper(c);
    return c < 0x80 && engine_core::UserInputService::key_code_from_glfw(key) != 0 ? key : -1;
}

// A key by its Enum.KeyCode name, ignoring case, or by a character on it.
int KeyArg(const JsonValue& value) {
    if (!value.is_string()) {
        throw std::runtime_error("key must be a key's name, or a list of them.");
    }
    const std::string& name = value.as_string();
    int key = engine_core::UserInputService::glfw_key_named(name);
    if (key < 0 && name.size() == 1) {
        key = KeyForCharacter(static_cast<unsigned char>(name[0]));
    }
    if (key < 0) {
        throw std::runtime_error("No key is named \"" + name + "\". Use an Enum.KeyCode name, such as W, Space, "
                                 "LeftShift, Return, Escape, or Up.");
    }
    return key;
}

JsonValue ViewSize(const McpViewSize& size) {
    JsonValue out = JsonValue::object();
    out.set("view_width", JsonValue::number(size.width));
    out.set("view_height", JsonValue::number(size.height));
    return out;
}

JsonValue MouseInput(const ToolContext& context, const JsonValue& arguments) {
    const std::string action =
        ChoiceArg(arguments, "action", "click", {"click", "down", "up", "move", "drag", "scroll", "delta"});
    if (action == "delta") {
        // What a locked pointer's motion becomes: GameView posts it so too.
        const double dx = RequiredNumber(arguments, "dx");
        const double dy = RequiredNumber(arguments, "dy");
        engine_core::UserInputService& input = context.engine.datamodel().input();
        input.post_mouse_delta(FloatArg(dx, "dx"), FloatArg(dy, "dy"));
        JsonValue out = JsonValue::object();
        const engine_core::EnumType& behaviors = engine_core::mouse_behavior_enum();
        for (int i = 0; i < behaviors.count; ++i) {
            if (behaviors.items[i].value == input.mouse_behavior()) {
                out.set("mouse_behavior", JsonValue::string(behaviors.items[i].name));
            }
        }
        return out;
    }
    const double x = RequiredNumber(arguments, "x");
    const double y = RequiredNumber(arguments, "y");
    const std::string button = ChoiceArg(arguments, "button", "left", {"left", "right", "middle"});
    // Keys held around a click, drag, or scroll; down and up only carry their bits.
    std::vector<McpInput> held;
    std::vector<McpInput> let_go;
    int mods = 0;
    if (const JsonValue* given = arguments.find("modifiers")) {
        if (!given->is_array()) {
            throw std::runtime_error("modifiers must be a list such as [\"shift\", \"ctrl\"].");
        }
        for (const JsonValue& item : given->items()) {
            const Modifier* modifier = std::find_if(std::begin(kModifiers), std::end(kModifiers), [&](const Modifier& m) {
                return item.is_string() && item.as_string() == m.name;
            });
            if (modifier == std::end(kModifiers)) {
                throw std::runtime_error("modifiers takes shift, ctrl, alt, and super.");
            }
            mods |= modifier->bit;
            const int key = engine_core::UserInputService::glfw_key_named(modifier->key);
            held.push_back(KeyChange(key, true));
            let_go.insert(let_go.begin(), KeyChange(key, false));
        }
    }
    auto press = [&](double at_x, double at_y, bool down) {
        McpInput input = PointerEvent(McpInput::Kind::Button, at_x, at_y);
        input.code = button == "left" ? 0 : button == "right" ? 1 : 2;
        input.down = down;
        input.mods = mods;
        return input;
    };
    const McpStudio& studio = context.studio;
    McpViewSize size;
    if (action == "move") {
        size = studio.send_input({PointerEvent(McpInput::Kind::Move, x, y)});
    } else if (action == "down" || action == "up") {
        size = studio.send_input({PointerEvent(McpInput::Kind::Move, x, y), press(x, y, action == "down")});
    } else if (action == "scroll") {
        McpInput scroll = PointerEvent(McpInput::Kind::Scroll, x, y);
        scroll.amount = NumberArg(arguments, "amount", 1, -100, 100);
        std::vector<McpInput> inputs = held;
        inputs.push_back(scroll);
        inputs.insert(inputs.end(), let_go.begin(), let_go.end());
        size = studio.send_input(inputs);
    } else {
        const double hold = NumberArg(arguments, "hold", action == "drag" ? kDragSeconds : kInputStep, 0, kMaxHold);
        const int count = action == "click" ? IntArg(arguments, "count", 1, 1, 3) : 1;
        double to_x = x;
        double to_y = y;
        if (action == "drag") {
            const JsonValue* to = arguments.find("to");
            if (to == nullptr || !to->is_array() || to->items().size() != 2) {
                throw std::runtime_error("drag needs to, the point it ends at, as [x, y].");
            }
            to_x = NumberArg(to->items()[0], "to");
            to_y = NumberArg(to->items()[1], "to");
        }
        std::vector<McpInput> first = held;
        first.push_back(PointerEvent(McpInput::Kind::Move, x, y));
        size = studio.send_input(first);
        if (!(to_x >= 0 && to_x < size.width && to_y >= 0 && to_y < size.height)) {
            studio.send_input(let_go);
            throw std::runtime_error("to is outside the Scene View, which is " +
                                     engine_core::format_json_number(size.width) + " by " +
                                     engine_core::format_json_number(size.height) + " points.");
        }
        Pause(kInputStep);
        try {
            for (int click = 0; click < count; ++click) {
                if (click > 0) {
                    Pause(kInputStep);
                }
                studio.send_input({press(x, y, true)});
                if (action == "drag") {
                    const int steps = IntArg(arguments, "steps", 10, 1, kMaxDragSteps);
                    for (int step = 1; step <= steps; ++step) {
                        Pause(hold / steps);
                        const double t = static_cast<double>(step) / steps;
                        studio.send_input({PointerEvent(McpInput::Kind::Move, x + (to_x - x) * t, y + (to_y - y) * t)});
                    }
                    Pause(kInputStep);
                } else {
                    Pause(hold);
                }
                studio.send_input({press(to_x, to_y, false)});
            }
        } catch (...) {
            // Nothing left held after a failure part way.
            try {
                std::vector<McpInput> release = {press(to_x, to_y, false)};
                release.insert(release.end(), let_go.begin(), let_go.end());
                studio.send_input(release);
            } catch (...) {
            }
            throw;
        }
        if (!let_go.empty()) {
            Pause(kInputStep);
            studio.send_input(let_go);
        }
    }
    return ViewSize(size);
}

JsonValue KeyInput(const ToolContext& context, const JsonValue& arguments) {
    const std::string action = ChoiceArg(arguments, "action", "press", {"press", "down", "up", "type"});
    std::vector<McpInput> inputs;
    if (action == "type") {
        const std::string& text = StringArg(arguments, "text");
        for (std::size_t at = 0; at < text.size();) {
            const unsigned char lead = static_cast<unsigned char>(text[at]);
            const std::size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            const int key = length == 1 ? KeyForCharacter(lead) : -1;
            if (key >= 0) {
                inputs.push_back(KeyChange(key, true));
            }
            // Return and Tab are keys a text field reads, not text.
            if (lead != '\n' && lead != '\t') {
                McpInput typed;
                typed.kind = McpInput::Kind::Text;
                typed.text = text.substr(at, length);
                inputs.push_back(std::move(typed));
            }
            if (key >= 0) {
                inputs.push_back(KeyChange(key, false));
            }
            at += length;
        }
        if (!inputs.empty()) {
            context.studio.send_input(inputs);
        }
        JsonValue out = JsonValue::object();
        out.set("events", JsonValue::number(static_cast<double>(inputs.size())));
        return out;
    }
    const JsonValue* given = arguments.find("key");
    if (given == nullptr) {
        throw std::runtime_error("key is required.");
    }
    std::vector<int> keys;
    if (given->is_array()) {
        for (const JsonValue& item : given->items()) {
            keys.push_back(KeyArg(item));
        }
    } else {
        keys.push_back(KeyArg(*given));
    }
    if (keys.empty()) {
        throw std::runtime_error("key names no keys.");
    }
    std::vector<McpInput> downs;
    std::vector<McpInput> ups;
    for (int key : keys) {
        downs.push_back(KeyChange(key, true));
        ups.insert(ups.begin(), KeyChange(key, false));
    }
    if (action == "down") {
        context.studio.send_input(downs);
    } else if (action == "up") {
        context.studio.send_input(ups);
    } else {
        const double hold = NumberArg(arguments, "hold", kInputStep, 0, kMaxHold);
        context.studio.send_input(downs);
        Pause(hold);
        try {
            context.studio.send_input(ups);
        } catch (...) {
            // Once more, so a key is not left held after a slow UI thread.
            context.studio.send_input(ups);
        }
    }
    JsonValue out = JsonValue::object();
    out.set("events", JsonValue::number(static_cast<double>(action == "press" ? 2 * keys.size() : keys.size())));
    return out;
}

bool CanPlaytest(const McpStudio& studio) {
    return studio.session && studio.start_test && studio.pause_test && studio.resume_test && studio.stop_test;
}

bool CanCapture(const McpStudio& studio) { return studio.capture_view != nullptr; }

bool CanImport(const McpStudio& studio) { return studio.import_files != nullptr; }

bool TakesInput(const McpStudio& studio) { return studio.send_input != nullptr; }

bool HasTabs(const McpStudio& studio) { return studio.tabs && studio.change_tab; }

bool CanSave(const McpStudio& studio) { return studio.save_place != nullptr; }

bool HasProfiler(const McpStudio& studio) { return studio.show_profiler != nullptr; }

bool KnowsItself(const McpStudio& studio) { return studio.info != nullptr; }

// The code that runs each tool, found by the name in its spec. offered, when
// set, says whether a studio has the hooks the tool needs.
struct ToolCode {
    const char* name;
    JsonValue (*run)(const ToolContext& context, const JsonValue& arguments);
    bool (*offered)(const McpStudio& studio);
};

constexpr ToolCode kToolCode[] = {
    {"get_tree", GetTree, nullptr},
    {"find_instances", FindInstances, nullptr},
    {"get_properties", GetProperties, nullptr},
    {"set_property", SetProperty, nullptr},
    {"create_instance", CreateInstance, nullptr},
    {"delete_instance", DeleteInstance, nullptr},
    {"import_assets", ImportAssets, CanImport},
    {"read_script", ReadScript, nullptr},
    {"write_script", WriteScript, nullptr},
    {"edit_script", EditScript, nullptr},
    {"search_scripts", SearchScripts, nullptr},
    {"get_diagnostics", GetDiagnostics, nullptr},
    {"run_lua", RunLua, nullptr},
    {"get_output", GetOutput, nullptr},
    {"get_profile", GetProfile, nullptr},
    {"get_selection", GetSelection, nullptr},
    {"set_selection", SetSelection, nullptr},
    {"undo", Undo, nullptr},
    {"list_classes", ListClasses, nullptr},
    {"get_class", GetClass, nullptr},
    {"playtest", Playtest, CanPlaytest},
    {"screenshot", Screenshot, CanCapture},
    {"mouse_input", MouseInput, TakesInput},
    {"key_input", KeyInput, TakesInput},
    {"tabs", Tabs, HasTabs},
    {"save_place", SavePlace, CanSave},
    {"show_profiler", ShowProfiler, HasProfiler},
    {"gpu_detail", GpuDetail, nullptr},
    {"get_studio_info", GetStudioInfo, KnowsItself},
};

}  // namespace

void add_engine_tools(McpServer& server, engine_core::Engine& engine, McpStudio studio) {
    // One context, shared by every tool this server runs.
    auto context = std::make_shared<const ToolContext>(ToolContext{engine, std::move(studio)});
    std::vector<McpToolSpec> specs = engine_tool_specs();
    // Every spec has code and all the code has a spec, so the bridge lists exactly what a studio runs.
    if (specs.size() != std::size(kToolCode)) {
        throw std::logic_error("The engine tools' specs and code do not match.");
    }
    // With the counts equal and no name twice, finding each spec's code below
    // pairs them one to one.
    for (std::size_t index = 0; index < specs.size(); ++index) {
        for (std::size_t other = index + 1; other < specs.size(); ++other) {
            if (specs[index].name == specs[other].name) {
                throw std::logic_error("Two engine tools are named " + specs[index].name + ".");
            }
        }
    }
    for (McpToolSpec& spec : specs) {
        const ToolCode* code = std::find_if(std::begin(kToolCode), std::end(kToolCode),
                                            [&spec](const ToolCode& each) { return spec.name == each.name; });
        if (code == std::end(kToolCode)) {
            throw std::logic_error("No code runs the tool " + spec.name + ".");
        }
        if (code->offered != nullptr && !code->offered(context->studio)) {
            continue;
        }
        const auto run = code->run;
        server.add_tool({std::move(spec.name), std::move(spec.description), std::move(spec.input_schema),
                         [context, run](const JsonValue& arguments) { return run(*context, arguments); }});
    }
}

}  // namespace ide
