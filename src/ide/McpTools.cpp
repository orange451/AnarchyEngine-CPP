#include "McpTools.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "PropertySheet.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "SelectionService.hpp"
#include "Strings.hpp"
#include "TextSearch.hpp"

#include <algorithm>
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
        case PropertyKind::Color3:
            return "Color3";
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
        case PropertyKind::Color3:
            return JsonValue::array({JsonValue::number_from_float(row.value.color.r),
                                     JsonValue::number_from_float(row.value.color.g),
                                     JsonValue::number_from_float(row.value.color.b)});
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

// Checked before the cast, which is undefined past float's range.
float FloatArg(double number, const std::string& what) {
    if (!(std::fabs(number) <= static_cast<double>(std::numeric_limits<float>::max()))) {
        throw std::runtime_error(what + " is out of range.");
    }
    return static_cast<float>(number);
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
            edit.value.vec.x = FloatArg(axes[0], row.name);
            edit.value.vec.y = FloatArg(axes[1], row.name);
            edit.value.vec.z = FloatArg(axes[2], row.name);
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

// Watches scripts for as long as it lives, as an open editor does.
class Watching {
public:
    Watching(engine_core::ScriptAnalysis& analysis, std::vector<InstanceId> ids)
        : analysis_(analysis), ids_(std::move(ids)) {
        for (InstanceId id : ids_) {
            analysis_.watch(id);
        }
    }
    ~Watching() {
        for (InstanceId id : ids_) {
            analysis_.unwatch(id);
        }
    }
    Watching(const Watching&) = delete;
    Watching& operator=(const Watching&) = delete;

private:
    engine_core::ScriptAnalysis& analysis_;
    std::vector<InstanceId> ids_;
};

// What analysis found in some scripts, each checked against the Source it has now.
struct Checked {
    std::unordered_map<InstanceId, std::vector<engine_core::Diagnostic>> problems;
    // Scripts analysis had not finished when the wait ran out. A deleted script is in neither.
    std::vector<InstanceId> pending;
    // Analysis is turned off, so nothing was checked.
    bool off = false;
};

// Waits up to kAnalysisWait for analysis to check these scripts. The studio
// analyzes only scripts open in an editor, so each is watched while this waits.
Checked CheckScripts(engine_core::Engine& engine, const std::vector<InstanceId>& ids) {
    engine_core::ScriptAnalysis& analysis = engine.analysis();
    Checked out;
    if (!analysis.enabled()) {
        out.off = true;
        return out;
    }
    const Watching watching(analysis, ids);
    // Shared, since an edit that runs after a timed-out wait still writes it.
    struct Progress {
        std::vector<InstanceId> waiting;
        std::unordered_map<InstanceId, std::vector<engine_core::Diagnostic>> done;
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
                    if (analysis.settled(id) && checked && *checked == script->source()) {
                        progress->done[id] = analysis.diagnostics(id);
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
        } else if (std::find(progress->waiting.begin(), progress->waiting.end(), id) != progress->waiting.end()) {
            out.pending.push_back(id);
        }
    }
    return out;
}

// Adds what analysis found in the script to out: its problems, or analysis
// "pending" or "off" when there is no answer to give.
void AddProblems(engine_core::Engine& engine, InstanceId id, JsonValue& out) {
    const Checked checked = CheckScripts(engine, {id});
    const auto found = checked.problems.find(id);
    if (found != checked.problems.end()) {
        out.set("problems", ProblemList(found->second));
    } else {
        out.set("analysis", JsonValue::string(checked.off ? "off" : "pending"));
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
        const PropertySheet sheet = read_sheet(world, {id});
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
        if (!engine_core::lua_creatable_known(class_name.c_str())) {
            throw std::runtime_error("Instance.new cannot make \"" + class_name +
                                     "\". list_classes names the ones it can.");
        }
        // Asked by class, before create, so a refused parent leaves nothing behind.
        if (std::optional<std::string> refused = world.placement_error_for_class(parent_id, class_name)) {
            throw std::runtime_error(*refused);
        }
        // Refused before the gesture opens, so a full place leaves nothing pending.
        if (world.room_left() == 0) {
            throw engine_core::InstanceCapacityError();
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
        world.history().set_pending_gesture("Delete");
        world.destroy_tree(id);
        CloseGesture(world);
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
    JsonValue out = JsonValue::object();
    out.set("checked", JsonValue::number(static_cast<double>(checked.problems.size())));
    out.set("errors", JsonValue::number(errors));
    out.set("warnings", JsonValue::number(warnings));
    out.set("scripts", std::move(scripts));
    if (!pending.items().empty()) {
        out.set("pending", std::move(pending));
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
    out.set(kImageMember, std::move(picture));
    return out;
}

JsonValue GetStudioInfo(const ToolContext& context, const JsonValue&) {
    JsonValue out = context.studio.info();
    if (context.studio.session) {
        out.set("session", JsonValue::string(context.studio.session()));
    }
    return out;
}

bool CanPlaytest(const McpStudio& studio) {
    return studio.session && studio.start_test && studio.pause_test && studio.resume_test && studio.stop_test;
}

bool CanCapture(const McpStudio& studio) { return studio.capture_view != nullptr; }

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
    {"read_script", ReadScript, nullptr},
    {"write_script", WriteScript, nullptr},
    {"edit_script", EditScript, nullptr},
    {"search_scripts", SearchScripts, nullptr},
    {"get_diagnostics", GetDiagnostics, nullptr},
    {"run_lua", RunLua, nullptr},
    {"get_output", GetOutput, nullptr},
    {"get_selection", GetSelection, nullptr},
    {"set_selection", SetSelection, nullptr},
    {"undo", Undo, nullptr},
    {"list_classes", ListClasses, nullptr},
    {"get_class", GetClass, nullptr},
    {"playtest", Playtest, CanPlaytest},
    {"screenshot", Screenshot, CanCapture},
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
