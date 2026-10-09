#include "LuaApi.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine_core {
namespace {

struct ClassRecord {
    const char* name = nullptr;
    const char* base = nullptr;
    std::vector<LuaField> fields;
    std::vector<LuaOperator> operators;
};

std::vector<ClassRecord>& classes() {
    static std::vector<ClassRecord> records;
    return records;
}

ClassRecord* find_class(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (ClassRecord& record : classes()) {
        if (std::strcmp(record.name, name) == 0) {
            return &record;
        }
    }
    return nullptr;
}

const ClassRecord* find_class_const(const char* name) {
    return find_class(name);
}

struct ResultNote {
    std::string type_name;
    bool class_from_arg = false;
};

std::unordered_map<std::string, ResultNote>& results() {
    static std::unordered_map<std::string, ResultNote> notes;
    return notes;
}

// The analysis worker reads this map while a play VM's open_host_libraries writes it.
std::mutex& result_mu() {
    static std::mutex mu;
    return mu;
}

std::string result_key(std::string_view owner, std::string_view name) {
    std::string key;
    key.reserve(owner.size() + name.size() + 1);
    key.append(owner);
    key.push_back('\n');
    key.append(name);
    return key;
}

// True when `out` changed: the field is new, or replaces a different one.
bool append_unique(std::vector<LuaField>& out, const LuaField& field) {
    for (LuaField& existing : out) {
        if (existing.name != nullptr && field.name != nullptr && std::strcmp(existing.name, field.name) == 0) {
            const bool same = std::memcmp(&existing, &field, sizeof(LuaField)) == 0;
            existing = field;
            return !same;
        }
    }
    out.push_back(field);
    return true;
}

// Two operand names match when both are null or both spell the same type.
bool same_text(const char* a, const char* b) {
    return a == b || (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
}

void collect(const char* class_name, std::vector<LuaField>& out, int depth) {
    if (class_name == nullptr || depth > 32) {
        return;
    }
    const ClassRecord* record = find_class_const(class_name);
    if (record == nullptr) {
        return;
    }
    collect(record->base, out, depth + 1);
    for (const LuaField& field : record->fields) {
        append_unique(out, field);
    }
}

}  // namespace

namespace {
std::atomic<std::uint64_t> g_registry_revision{0};

// Called after a write that changed the registry, so a reader that sees the
// new revision also sees the write. A write that changes nothing, as a VM
// noting what every VM notes, leaves the revision alone.
void registry_changed() { g_registry_revision.fetch_add(1, std::memory_order_acq_rel); }
}  // namespace

std::uint64_t lua_registry_revision() { return g_registry_revision.load(std::memory_order_acquire); }

namespace {

// Interned property names. A deque keeps each string where it is, so a name
// handed out stays valid as more are added.
struct PropertyNames {
    std::mutex mu;
    std::deque<std::string> names{std::string()};
    std::unordered_map<std::string, std::uint32_t> ids;
};

PropertyNames& property_names() {
    static PropertyNames table;
    return table;
}

}  // namespace

std::uint32_t lua_property_id(std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    PropertyNames& table = property_names();
    std::lock_guard<std::mutex> lock(table.mu);
    const auto found = table.ids.find(std::string(name));
    if (found != table.ids.end()) {
        return found->second;
    }
    const auto id = static_cast<std::uint32_t>(table.names.size());
    table.names.emplace_back(name);
    table.ids.emplace(table.names.back(), id);
    return id;
}

const char* lua_property_name(std::uint32_t id) {
    PropertyNames& table = property_names();
    std::lock_guard<std::mutex> lock(table.mu);
    return id < table.names.size() ? table.names[id].c_str() : "";
}

void register_lua_class(const char* class_name, const char* base, const LuaField* fields, int count) {
    if (class_name == nullptr) {
        return;
    }
    bool changed = false;
    ClassRecord* record = find_class(class_name);
    if (record == nullptr) {
        classes().push_back(ClassRecord{});
        record = &classes().back();
        record->name = class_name;
        changed = true;
    }
    if (base != nullptr && record->base == nullptr) {
        record->base = base;
        changed = true;
    }
    for (int index = 0; fields != nullptr && index < count; ++index) {
        changed = append_unique(record->fields, fields[index]) || changed;
        if (!fields[index].method && fields[index].name != nullptr) {
            lua_property_id(fields[index].name);
        }
    }
    if (changed) {
        registry_changed();
    }
}

void register_lua_operators(const char* class_name, const LuaOperator* operators, int count) {
    if (class_name == nullptr) {
        return;
    }
    register_lua_class(class_name, nullptr, nullptr, 0);
    ClassRecord* record = find_class(class_name);
    if (record == nullptr || operators == nullptr) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        const LuaOperator& row = operators[index];
        if (row.metamethod == nullptr) {
            continue;
        }
        bool replaced = false;
        for (LuaOperator& existing : record->operators) {
            if (std::strcmp(existing.metamethod, row.metamethod) == 0 && same_text(existing.left, row.left) &&
                same_text(existing.right, row.right)) {
                if (std::memcmp(&existing, &row, sizeof(LuaOperator)) != 0) {
                    existing = row;
                    registry_changed();
                }
                replaced = true;
            }
        }
        if (!replaced) {
            record->operators.push_back(row);
            registry_changed();
        }
    }
}

void lua_class_operators(const char* class_name, std::vector<LuaOperator>& out) {
    out.clear();
    if (const ClassRecord* record = find_class_const(class_name)) {
        out = record->operators;
    }
}

void lua_class_members(const char* class_name, std::vector<LuaField>& out) {
    out.clear();
    collect(class_name, out, 0);
}

std::vector<LuaField> lua_saved_fields(const char* class_name) {
    struct Cache {
        std::mutex mu;
        std::uint64_t revision = ~std::uint64_t{0};
        std::unordered_map<std::string, std::vector<LuaField>> by_class;
    };
    static Cache cache;
    const std::string key = class_name != nullptr ? class_name : "";
    const std::uint64_t revision = lua_registry_revision();
    std::lock_guard<std::mutex> lock(cache.mu);
    if (cache.revision != revision) {
        cache.by_class.clear();
        cache.revision = revision;
    }
    const auto found = cache.by_class.find(key);
    if (found != cache.by_class.end()) {
        return found->second;
    }
    std::vector<LuaField> members;
    collect(class_name, members, 0);
    std::vector<LuaField> saved;
    for (const LuaField& field : members) {
        if (field.saved && !field.method && field.read != nullptr && field.write != nullptr) {
            saved.push_back(field);
        }
    }
    cache.by_class.emplace(key, saved);
    return saved;
}

const LuaField* lua_class_find(const char* class_name, std::string_view name) {
    const ClassRecord* record = find_class_const(class_name);
    int depth = 0;
    while (record != nullptr && depth < 32) {
        for (const LuaField& field : record->fields) {
            if (field.name != nullptr && name == field.name) {
                return &field;
            }
        }
        record = find_class_const(record->base);
        ++depth;
    }
    return nullptr;
}

bool lua_class_known(const char* class_name) { return find_class_const(class_name) != nullptr; }

bool lua_class_inherits(const char* class_name, const char* ancestor) {
    if (class_name == nullptr || ancestor == nullptr) {
        return false;
    }
    const ClassRecord* record = find_class_const(class_name);
    int depth = 0;
    while (record != nullptr && record->name != nullptr && depth < 32) {
        if (std::strcmp(record->name, ancestor) == 0) {
            return true;
        }
        record = find_class_const(record->base);
        ++depth;
    }
    return false;
}

void lua_class_names(std::vector<std::string>& out) {
    out.clear();
    for (const ClassRecord& record : classes()) {
        if (record.name != nullptr) {
            out.emplace_back(record.name);
        }
    }
}

bool lua_method_resolves_child(std::string_view name) {
    for (const ClassRecord& record : classes()) {
        for (const LuaField& field : record.fields) {
            if (field.method && field.resolves_child && field.name != nullptr && name == field.name) {
                return true;
            }
        }
    }
    return false;
}

void lua_class_own_members(const char* class_name, std::vector<LuaField>& out) {
    out.clear();
    const ClassRecord* record = find_class_const(class_name);
    if (record == nullptr) {
        return;
    }
    out = record->fields;
}

const char* lua_class_base(const char* class_name) {
    const ClassRecord* record = find_class_const(class_name);
    if (record == nullptr) {
        return nullptr;
    }
    return record->base;
}

namespace {

std::vector<const char*>& service_names() {
    static std::vector<const char*> names;
    return names;
}

}  // namespace

void register_lua_service(const char* name) {
    if (name == nullptr) {
        return;
    }
    for (const char* existing : service_names()) {
        if (std::strcmp(existing, name) == 0) {
            return;
        }
    }
    service_names().push_back(name);
    registry_changed();
}

bool lua_service_known(const char* name) {
    if (name == nullptr) {
        return false;
    }
    for (const char* existing : service_names()) {
        if (std::strcmp(existing, name) == 0) {
            return true;
        }
    }
    return false;
}

void lua_service_names(std::vector<std::string>& out) {
    out.clear();
    for (const char* name : service_names()) {
        if (name != nullptr) {
            out.emplace_back(name);
        }
    }
}

namespace {

struct Creatable {
    const char* name = nullptr;
    LuaCreate create = nullptr;
    bool from_scripts = true;
};

std::vector<Creatable>& creatables() {
    static std::vector<Creatable> records;
    return records;
}

const Creatable* find_creatable(const char* class_name) {
    if (class_name == nullptr) {
        return nullptr;
    }
    for (const Creatable& record : creatables()) {
        if (record.name != nullptr && std::strcmp(record.name, class_name) == 0) {
            return &record;
        }
    }
    return nullptr;
}

}  // namespace

void register_lua_creatable(const char* class_name, LuaCreate create, bool from_scripts) {
    if (class_name == nullptr || create == nullptr || find_creatable(class_name) != nullptr) {
        return;
    }
    creatables().push_back(Creatable{class_name, create, from_scripts});
    registry_changed();
}

bool lua_creatable_known(const char* class_name) { return find_creatable(class_name) != nullptr; }

bool lua_script_creatable(const char* class_name) {
    const Creatable* record = find_creatable(class_name);
    return record != nullptr && record->from_scripts;
}

void lua_creatable_names(std::vector<std::string>& out) {
    out.clear();
    for (const Creatable& record : creatables()) {
        if (record.name != nullptr && record.from_scripts) {
            out.emplace_back(record.name);
        }
    }
}

DataModel* lua_create_instance(DataModel& world, const char* class_name) {
    const Creatable* record = find_creatable(class_name);
    if (record == nullptr || record->create == nullptr) {
        return nullptr;
    }
    return &record->create(world);
}

void lua_note_result(const char* owner, const char* name, const char* type_name, bool class_from_arg) {
    if (owner == nullptr || name == nullptr) {
        return;
    }
    ResultNote note;
    note.type_name = type_name != nullptr ? type_name : "";
    note.class_from_arg = class_from_arg;
    {
        std::lock_guard<std::mutex> lock(result_mu());
        const std::string key = result_key(owner, name);
        const auto found = results().find(key);
        if (found != results().end() && found->second.type_name == note.type_name &&
            found->second.class_from_arg == note.class_from_arg) {
            return;
        }
        results()[key] = std::move(note);
    }
    registry_changed();
}

namespace {

std::vector<const char*>& host_libraries() {
    static std::vector<const char*> names;
    return names;
}

}  // namespace

void lua_note_host_library(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    for (const char* existing : host_libraries()) {
        if (std::strcmp(existing, name) == 0) {
            return;
        }
    }
    host_libraries().push_back(name);
    registry_changed();
}

void lua_host_library_names(std::vector<std::string>& out) {
    out.clear();
    for (const char* name : host_libraries()) {
        if (name != nullptr) {
            out.emplace_back(name);
        }
    }
}

namespace {

ANARCHY_LUA_REGISTER(register_value_classes) {
    // DataModel is everything in the tree. Instance is what Instance.new makes;
    // Game, the root that scripts see as game, is not one.
    register_lua_class("Instance", "DataModel", nullptr, 0);
}

}  // namespace

LuaResult lua_function_result(std::string_view owner, std::string_view name) {
    std::lock_guard<std::mutex> lock(result_mu());
    const auto found = results().find(result_key(owner, name));
    LuaResult result;
    if (found == results().end()) {
        return result;
    }
    result.known = true;
    result.type_name = found->second.type_name;
    result.class_from_arg = found->second.class_from_arg;
    return result;
}

namespace {

// `returns` nullptr means the function returns no value. An empty string means
// the return is not one named type. Any other string is the return type.
void add_doc(std::unordered_map<std::string, LuaDoc>& docs, const char* owner, const char* name, const char* summary,
             const char* returns, bool variadic, std::initializer_list<LuaDocParam> params) {
    if (name == nullptr) {
        return;
    }
    LuaDoc doc;
    doc.found = true;
    if (summary != nullptr) {
        doc.summary = summary;
    }
    if (returns == nullptr) {
        doc.returns_nothing = true;
    } else if (returns[0] == '\0') {
        doc.return_unknown = true;
    } else {
        doc.return_type = returns;
    }
    doc.variadic = variadic;
    doc.params.assign(params.begin(), params.end());
    docs[result_key(owner != nullptr ? owner : "", name)] = std::move(doc);
}

LuaDocParam P(const char* name, const char* type_name) {
    LuaDocParam param;
    param.name = name != nullptr ? name : "";
    param.type_name = type_name != nullptr ? type_name : "";
    return param;
}

std::unordered_map<std::string, LuaDoc> build_docs() {
    std::unordered_map<std::string, LuaDoc> docs;
    auto add = [&](const char* owner, const char* name, const char* summary, const char* returns, bool variadic,
                   std::initializer_list<LuaDocParam> params) {
        add_doc(docs, owner, name, summary, returns, variadic, params);
    };

    add("", "task", "Schedules threads on the simulation clock. Its functions use Heartbeat time, not wall time.", nullptr,
        false, {});
    add("task", "wait", "Yields the running script for seconds of simulation time. Omit seconds to yield once.", nullptr,
        false, {P("seconds", "number?")});
    add("task", "spawn", "Runs callback now on a new thread. Extra arguments are passed to callback.", "thread", true,
        {P("callback", "function")});
    add("task", "defer", "Runs callback after the current thread yields, before the next simulation step.", "thread", true,
        {P("callback", "function")});
    add("task", "delay", "Runs callback after seconds of simulation time. Extra arguments are passed to callback.", "thread",
        true, {P("seconds", "number"), P("callback", "function")});
    add("task", "cancel", "Stops a thread started by spawn, defer, or delay.", nullptr, false, {P("thread", "thread")});

    add("", "debug",
        "Marks sections of a script for the profiler (Cmd+F6 over the Scene View). Only these two functions are here.",
        nullptr, false, {});
    add("debug", "profilebegin",
        "Starts a profiler scope named name inside the running script's own. End it with profileend before the "
        "script yields.",
        nullptr, false, {P("name", "string")});
    add("debug", "profileend", "Ends the innermost scope profilebegin started.", nullptr, false, {});

    add("", "math", "Numeric functions and constants.", nullptr, false, {});
    add("math", "abs", "The absolute value of n.", "number", false, {P("n", "number")});
    add("math", "acos", "Arc cosine of n, in radians.", "number", false, {P("n", "number")});
    add("math", "asin", "Arc sine of n, in radians.", "number", false, {P("n", "number")});
    add("math", "atan", "Arc tangent of n, in radians.", "number", false, {P("n", "number")});
    add("math", "atan2", "Arc tangent of y / x, in radians, using the signs of both.", "number", false,
        {P("y", "number"), P("x", "number")});
    add("math", "ceil", "Smallest integer greater than or equal to n.", "number", false, {P("n", "number")});
    add("math", "floor", "Largest integer less than or equal to n.", "number", false, {P("n", "number")});
    add("math", "cos", "Cosine of n radians.", "number", false, {P("n", "number")});
    add("math", "cosh", "Hyperbolic cosine of n.", "number", false, {P("n", "number")});
    add("math", "sin", "Sine of n radians.", "number", false, {P("n", "number")});
    add("math", "sinh", "Hyperbolic sine of n.", "number", false, {P("n", "number")});
    add("math", "tan", "Tangent of n radians.", "number", false, {P("n", "number")});
    add("math", "tanh", "Hyperbolic tangent of n.", "number", false, {P("n", "number")});
    add("math", "deg", "Converts radians to degrees.", "number", false, {P("n", "number")});
    add("math", "rad", "Converts degrees to radians.", "number", false, {P("n", "number")});
    add("math", "exp", "e raised to n.", "number", false, {P("n", "number")});
    add("math", "sqrt", "Square root of n.", "number", false, {P("n", "number")});
    add("math", "pow", "x raised to y.", "number", false, {P("x", "number"), P("y", "number")});
    add("math", "log", "Logarithm of n. base defaults to e.", "number", false, {P("n", "number"), P("base", "number?")});
    add("math", "log10", "Base-10 logarithm of n.", "number", false, {P("n", "number")});
    add("math", "fmod", "Remainder of x divided by y.", "number", false, {P("x", "number"), P("y", "number")});
    add("math", "modf", "Returns the integral part and the fractional part of n.", "number, number", false, {P("n", "number")});
    add("math", "frexp", "Returns the mantissa and the exponent of n.", "number, number", false, {P("n", "number")});
    add("math", "ldexp", "Returns m * 2^e.", "number", false, {P("m", "number"), P("e", "number")});
    add("math", "min", "The smallest of the arguments.", "number", true, {P("n", "number")});
    add("math", "max", "The largest of the arguments.", "number", true, {P("n", "number")});
    add("math", "clamp", "n limited to the range min..max.", "number", false,
        {P("n", "number"), P("min", "number"), P("max", "number")});
    add("math", "sign", "1 when n is positive, -1 when n is negative, and 0 when n is 0.", "number", false, {P("n", "number")});
    add("math", "round", "n rounded to the nearest integer.", "number", false, {P("n", "number")});
    add("math", "noise", "Perlin noise at x, y, z. y and z default to 0.", "number", false,
        {P("x", "number"), P("y", "number?"), P("z", "number?")});
    add("math", "map", "Scales n from the input range to the output range.", "number", false,
        {P("n", "number"), P("inMin", "number"), P("inMax", "number"), P("outMin", "number"), P("outMax", "number")});
    add("math", "lerp", "Linear blend from a to b. t = 0 returns a and t = 1 returns b.", "number", false,
        {P("a", "number"), P("b", "number"), P("t", "number")});
    add("math", "random", "A random number. No arguments returns a value in [0, 1). One integer n returns 1..n. Two integers return a value in that range.",
        "number", false, {P("m", "number?"), P("n", "number?")});
    add("math", "randomseed", "Seeds the random generator.", nullptr, false, {P("seed", "number")});
    add("math", "isnan", "True when n is NaN.", "boolean", false, {P("n", "number")});
    add("math", "isinf", "True when n is infinite.", "boolean", false, {P("n", "number")});
    add("math", "isfinite", "True when n is finite.", "boolean", false, {P("n", "number")});
    add("math", "pi", "The constant π.", "number", false, {});
    add("math", "huge", "A number larger than any finite value.", "number", false, {});
    add("math", "nan", "The quiet NaN value.", "number", false, {});
    add("math", "e", "The constant e.", "number", false, {});
    add("math", "phi", "The golden ratio.", "number", false, {});
    add("math", "sqrt2", "The square root of 2.", "number", false, {});
    add("math", "tau", "The constant 2π.", "number", false, {});

    add("", "string", "Functions that search and build strings. A string value has the same functions as methods.", nullptr,
        false, {});
    add("string", "byte", "The numeric codes of the characters from i to j. i defaults to 1.", "number", false,
        {P("s", "string"), P("i", "number?"), P("j", "number?")});
    add("string", "char", "The string made from the given character codes.", "string", true, {});
    add("string", "find", "The start and end of pattern in s, or nil. init defaults to 1. plain disables pattern matching.",
        "", false, {P("s", "string"), P("pattern", "string"), P("init", "number?"), P("plain", "boolean?")});
    add("string", "format", "Formats args with a printf-style format string.", "string", true, {P("format", "string")});
    add("string", "gmatch", "An iterator over the matches of pattern in s.", "function", false,
        {P("s", "string"), P("pattern", "string")});
    add("string", "gsub", "Replaces matches of pattern. Returns the new string and the number of replacements.", "string, number",
        false, {P("s", "string"), P("pattern", "string"), P("repl", "string"), P("n", "number?")});
    add("string", "len", "The length of s in bytes.", "number", false, {P("s", "string")});
    add("string", "lower", "s converted to lowercase.", "string", false, {P("s", "string")});
    add("string", "upper", "s converted to uppercase.", "string", false, {P("s", "string")});
    add("string", "match", "The captures of the first match of pattern, or nil.", "", false,
        {P("s", "string"), P("pattern", "string"), P("init", "number?")});
    add("string", "rep", "s repeated n times, with sep between copies.", "string", false,
        {P("s", "string"), P("n", "number"), P("sep", "string?")});
    add("string", "reverse", "s with its bytes reversed.", "string", false, {P("s", "string")});
    add("string", "sub", "The substring from i through j. j defaults to the end.", "string", false,
        {P("s", "string"), P("i", "number"), P("j", "number?")});
    add("string", "split", "The pieces of s divided by separator. separator defaults to a comma.", "table", false,
        {P("s", "string"), P("separator", "string?")});
    add("string", "pack", "Packs the values into a binary string described by format.", "string", true, {P("format", "string")});
    add("string", "packsize", "The size of a packed string with this format.", "number", false, {P("format", "string")});
    add("string", "unpack", "Reads values from a packed string. pos defaults to 1.", "", false,
        {P("format", "string"), P("s", "string"), P("pos", "number?")});

    add("", "table", "Functions that read and write tables.", nullptr, false, {});
    add("table", "concat", "Joins the array part of t from i to j, with sep between items.", "string", false,
        {P("t", "table"), P("sep", "string?"), P("i", "number?"), P("j", "number?")});
    add("table", "insert", "Inserts value at the end of list. Pass a position before value to insert there.", nullptr, false,
        {P("list", "table"), P("value", "any")});
    add("table", "remove", "Removes the item at pos and returns it. pos defaults to the end.", "", false,
        {P("list", "table"), P("pos", "number?")});
    add("table", "sort", "Sorts the array part of list. comp(a, b) returns whether a belongs before b.", nullptr, false,
        {P("list", "table"), P("comp", "function?")});
    add("table", "find", "The first index of value in the array part, or nil. init defaults to 1.", "number", false,
        {P("list", "table"), P("value", "any"), P("init", "number?")});
    add("table", "clear", "Removes every entry from t.", nullptr, false, {P("t", "table")});
    add("table", "clone", "A shallow copy of t.", "table", false, {P("t", "table")});
    add("table", "create", "A table with count slots. value, when given, fills 1..count.", "table", false,
        {P("count", "number"), P("value", "any")});
    add("table", "freeze", "Makes t read-only and returns it.", "table", false, {P("t", "table")});
    add("table", "isfrozen", "True when t is frozen.", "boolean", false, {P("t", "table")});
    add("table", "pack", "A table of the arguments, with field n set to the count.", "table", true, {});
    add("table", "unpack", "The array values from i through j. i defaults to 1 and j to the length.", "", false,
        {P("list", "table"), P("i", "number?"), P("j", "number?")});
    add("table", "move", "Copies a[f] through a[e] into b starting at index t. b defaults to a.", "table", false,
        {P("a", "table"), P("f", "number"), P("e", "number"), P("t", "number"), P("b", "table?")});
    add("table", "getn", "The length of the array part of list.", "number", false, {P("list", "table")});
    add("table", "maxn", "The largest positive numeric key in t.", "number", false, {P("t", "table")});
    add("table", "foreach", "Calls f(key, value) for each entry in t.", nullptr, false, {P("t", "table"), P("f", "function")});
    add("table", "foreachi", "Calls f(index, value) for each item in the array part of t.", nullptr, false,
        {P("t", "table"), P("f", "function")});

    add("", "coroutine", "Functions that create, resume, and yield threads.", nullptr, false, {});
    add("coroutine", "create", "A new thread that will run fn.", "thread", false, {P("fn", "function")});
    add("coroutine", "running", "The running thread, or nil on the main thread.", "thread", false, {});
    add("coroutine", "status", "running, suspended, normal, or dead.", "string", false, {P("co", "thread")});
    add("coroutine", "resume", "Starts or resumes co. Returns true and the yielded values, or false and the error.", "", true,
        {P("co", "thread")});
    add("coroutine", "yield", "Yields the running thread and passes the arguments to resume.", "", true, {});
    add("coroutine", "wrap", "A function that resumes a thread created for fn.", "function", false, {P("fn", "function")});
    add("coroutine", "isyieldable", "True when the running thread can yield.", "boolean", false, {});
    add("coroutine", "close", "Closes a suspended thread.", nullptr, false, {P("co", "thread")});

    add("", "bit32", "Bitwise operations on 32-bit integers.", nullptr, false, {});
    add("bit32", "band", "Bitwise AND of the arguments.", "number", true, {P("x", "number")});
    add("bit32", "bor", "Bitwise OR of the arguments.", "number", true, {P("x", "number")});
    add("bit32", "bxor", "Bitwise exclusive OR of the arguments.", "number", true, {P("x", "number")});
    add("bit32", "bnot", "Bitwise NOT of x.", "number", false, {P("x", "number")});
    add("bit32", "btest", "True when the bitwise AND of the arguments is not zero.", "boolean", true, {P("x", "number")});
    add("bit32", "lshift", "x shifted left by disp bits.", "number", false, {P("x", "number"), P("disp", "number")});
    add("bit32", "rshift", "x shifted right by disp bits.", "number", false, {P("x", "number"), P("disp", "number")});
    add("bit32", "arshift", "x shifted right by disp bits, keeping the sign bit.", "number", false,
        {P("x", "number"), P("disp", "number")});
    add("bit32", "lrotate", "x rotated left by disp bits.", "number", false, {P("x", "number"), P("disp", "number")});
    add("bit32", "rrotate", "x rotated right by disp bits.", "number", false, {P("x", "number"), P("disp", "number")});
    add("bit32", "extract", "The bits of n starting at field, width bits wide. width defaults to 1.", "number", false,
        {P("n", "number"), P("field", "number"), P("width", "number?")});
    add("bit32", "replace", "Replaces width bits of n at field with the low bits of v.", "number", false,
        {P("n", "number"), P("v", "number"), P("field", "number"), P("width", "number?")});
    add("bit32", "countlz", "The number of zero bits above the highest set bit.", "number", false, {P("n", "number")});
    add("bit32", "countrz", "The number of zero bits below the lowest set bit.", "number", false, {P("n", "number")});
    add("bit32", "byteswap", "n with its bytes reversed.", "number", false, {P("n", "number")});

    add("", "utf8", "Functions that read UTF-8 strings.", nullptr, false, {});
    add("utf8", "char", "The UTF-8 string for the given code points.", "string", true, {});
    add("utf8", "codes", "An iterator of position and code point for each character in s.", "function", false, {P("s", "string")});
    add("utf8", "codepoint", "The code points of s from i through j.", "number", false,
        {P("s", "string"), P("i", "number?"), P("j", "number?")});
    add("utf8", "len", "The number of characters from i through j, or nil and the bad position.", "number", false,
        {P("s", "string"), P("i", "number?"), P("j", "number?")});
    add("utf8", "offset", "The byte position of the n-th character starting at i.", "number", false,
        {P("s", "string"), P("n", "number"), P("i", "number?")});
    add("utf8", "charpattern", "A pattern that matches one UTF-8 character.", "string", false, {});

    add("", "buffer", "A block of bytes and the functions that read and write it. Offsets start at 0.", nullptr, false, {});
    add("buffer", "create", "A zero-filled buffer of size bytes.", "buffer", false, {P("size", "number")});
    add("buffer", "fromstring", "A buffer holding the bytes of str.", "buffer", false, {P("str", "string")});
    add("buffer", "tostring", "The bytes of b as a string.", "string", false, {P("b", "buffer")});
    add("buffer", "len", "The size of b in bytes.", "number", false, {P("b", "buffer")});
    add("buffer", "readstring", "count bytes from b at offset, as a string.", "string", false,
        {P("b", "buffer"), P("offset", "number"), P("count", "number")});
    add("buffer", "writestring", "Writes value into b at offset.", nullptr, false,
        {P("b", "buffer"), P("offset", "number"), P("value", "string")});
    add("buffer", "copy", "Copies count bytes from source at sourceOffset into target at targetOffset.", nullptr, false,
        {P("target", "buffer"), P("targetOffset", "number"), P("source", "buffer"), P("sourceOffset", "number?"),
         P("count", "number?")});
    add("buffer", "fill", "Writes value into count bytes of b starting at offset.", nullptr, false,
        {P("b", "buffer"), P("offset", "number"), P("value", "number"), P("count", "number?")});
    add("buffer", "readbits", "Reads bitCount bits from b starting at bitOffset.", "number", false,
        {P("b", "buffer"), P("bitOffset", "number"), P("bitCount", "number")});
    add("buffer", "writebits", "Writes the low bitCount bits of value into b at bitOffset.", nullptr, false,
        {P("b", "buffer"), P("bitOffset", "number"), P("bitCount", "number"), P("value", "number")});
    const char* widths[] = {"i8", "u8", "i16", "u16", "i32", "u32"};
    for (const char* width : widths) {
        const std::string read = std::string("read") + width;
        const std::string write = std::string("write") + width;
        add("buffer", read.c_str(), "Reads an integer at offset.", "number", false, {P("b", "buffer"), P("offset", "number")});
        add("buffer", write.c_str(), "Writes an integer at offset.", nullptr, false,
            {P("b", "buffer"), P("offset", "number"), P("value", "number")});
    }
    add("buffer", "readf32", "Reads a 32-bit float at offset.", "number", false, {P("b", "buffer"), P("offset", "number")});
    add("buffer", "readf64", "Reads a 64-bit float at offset.", "number", false, {P("b", "buffer"), P("offset", "number")});
    add("buffer", "writef32", "Writes a 32-bit float at offset.", nullptr, false,
        {P("b", "buffer"), P("offset", "number"), P("value", "number")});
    add("buffer", "writef64", "Writes a 64-bit float at offset.", nullptr, false,
        {P("b", "buffer"), P("offset", "number"), P("value", "number")});

    add("", "vector", "Creates the built-in vector value. Vector3 is the vector scripts usually use.", nullptr, false, {});
    add("vector", "create", "A vector. z defaults to 0.", "Vector3", false,
        {P("x", "number"), P("y", "number"), P("z", "number?")});
    add("vector", "magnitude", "The length of v.", "number", false, {P("v", "Vector3")});
    add("vector", "normalize", "v scaled to length 1.", "Vector3", false, {P("v", "Vector3")});
    add("vector", "cross", "The cross product of a and b.", "Vector3", false, {P("a", "Vector3"), P("b", "Vector3")});
    add("vector", "dot", "The dot product of a and b.", "number", false, {P("a", "Vector3"), P("b", "Vector3")});
    add("vector", "angle", "The angle in radians between a and b.", "number", false, {P("a", "Vector3"), P("b", "Vector3")});
    add("vector", "floor", "Each component rounded down.", "Vector3", false, {P("v", "Vector3")});
    add("vector", "ceil", "Each component rounded up.", "Vector3", false, {P("v", "Vector3")});
    add("vector", "abs", "Each component made non-negative.", "Vector3", false, {P("v", "Vector3")});
    add("vector", "sign", "The sign of each component.", "Vector3", false, {P("v", "Vector3")});
    add("vector", "clamp", "Each component of v limited to the matching component of min and max.", "Vector3", false,
        {P("v", "Vector3"), P("min", "Vector3"), P("max", "Vector3")});
    add("vector", "max", "The component-wise maximum of a and b.", "Vector3", false, {P("a", "Vector3"), P("b", "Vector3")});
    add("vector", "min", "The component-wise minimum of a and b.", "Vector3", false, {P("a", "Vector3"), P("b", "Vector3")});
    add("vector", "lerp", "A linear blend from a to b.", "Vector3", false,
        {P("a", "Vector3"), P("b", "Vector3"), P("t", "number")});

    add("", "Vector3", "A 3D vector. new builds one. Omitted components are 0.", nullptr, false, {});
    add("Vector3", "new", "A vector. Omitted components are 0.", "Vector3", false,
        {P("x", "number?"), P("y", "number?"), P("z", "number?")});
    add("Vector3", "FromNormalId", "The unit vector for a NormalId face.", "Vector3", false, {P("normal", "Enum.NormalId")});
    add("Vector3", "FromAxis", "The unit vector for an Axis.", "Vector3", false, {P("axis", "Enum.Axis")});
    add("Vector3", "zero", "The vector (0, 0, 0).", "Vector3", false, {});
    add("Vector3", "one", "The vector (1, 1, 1).", "Vector3", false, {});
    add("Vector3", "xAxis", "The vector (1, 0, 0).", "Vector3", false, {});
    add("Vector3", "yAxis", "The vector (0, 1, 0).", "Vector3", false, {});
    add("Vector3", "zAxis", "The vector (0, 0, 1).", "Vector3", false, {});
    add("Vector3", "X", "The x component.", "number", false, {});
    add("Vector3", "Y", "The y component.", "number", false, {});
    add("Vector3", "Z", "The z component.", "number", false, {});
    add("Vector3", "Magnitude", "The length of the vector.", "number", false, {});
    add("Vector3", "Unit", "The vector scaled to length 1.", "Vector3", false, {});
    add("Vector3", "Abs", "Each component made non-negative.", "Vector3", false, {});
    add("Vector3", "Ceil", "Each component rounded up.", "Vector3", false, {});
    add("Vector3", "Floor", "Each component rounded down.", "Vector3", false, {});
    add("Vector3", "Sign", "The sign of each component.", "Vector3", false, {});
    add("Vector3", "Cross", "The cross product with other.", "Vector3", false, {P("other", "Vector3")});
    add("Vector3", "Dot", "The dot product with other.", "number", false, {P("other", "Vector3")});
    add("Vector3", "Angle", "The angle in radians from this vector to other. axis picks the sign.", "number", false,
        {P("other", "Vector3"), P("axis", "Vector3?")});
    add("Vector3", "FuzzyEq", "True when each component is within epsilon. epsilon defaults to 1e-5.", "boolean", false,
        {P("other", "Vector3"), P("epsilon", "number?")});
    add("Vector3", "Lerp", "A linear blend toward goal. alpha 0 returns this vector and alpha 1 returns goal.", "Vector3",
        false, {P("goal", "Vector3"), P("alpha", "number")});
    add("Vector3", "Max", "The component-wise maximum with other.", "Vector3", false, {P("other", "Vector3")});
    add("Vector3", "Min", "The component-wise minimum with other.", "Vector3", false, {P("other", "Vector3")});

    add("", "Color3", "A color from red, green, and blue, each 0 to 1. new, fromRGB, fromHSV, and fromHex build one.",
        nullptr, false, {});
    add("Color3", "new", "A color from red, green, and blue, each 0 to 1. Omitted channels are 0.", "Color3", false,
        {P("r", "number?"), P("g", "number?"), P("b", "number?")});
    add("Color3", "fromRGB", "A color from red, green, and blue, each 0 to 255. Omitted channels are 0.", "Color3", false,
        {P("r", "number?"), P("g", "number?"), P("b", "number?")});
    add("Color3", "fromHSV", "A color from hue, saturation, and value, each 0 to 1.", "Color3", false,
        {P("h", "number"), P("s", "number"), P("v", "number")});
    add("Color3", "fromHex", "A color from a hex code: RGB or RRGGBB, with or without #.", "Color3", false,
        {P("hex", "string")});
    add("Color3", "toHSV", "The hue, saturation, and value of a color, each 0 to 1.", "number", false,
        {P("color", "Color3")});
    add("Color3", "R", "The red channel, 0 to 1.", "number", false, {});
    add("Color3", "G", "The green channel, 0 to 1.", "number", false, {});
    add("Color3", "B", "The blue channel, 0 to 1.", "number", false, {});
    add("Color3", "Lerp", "A linear blend toward goal. alpha 0 returns this color and alpha 1 returns goal.", "Color3",
        false, {P("goal", "Color3"), P("alpha", "number")});
    add("Color3", "ToHSV", "The hue, saturation, and value, each 0 to 1.", "number", false, {});
    add("Color3", "ToHex", "The hex code, RRGGBB in capitals without #.", "string", false, {});

    add("", "Vector2", "A 2D vector, such as a point on the screen. new builds one. Omitted components are 0.", nullptr, false,
        {});
    add("Vector2", "new", "A vector. Omitted components are 0.", "Vector2", false, {P("x", "number?"), P("y", "number?")});
    add("Vector2", "zero", "The vector (0, 0).", "Vector2", false, {});
    add("Vector2", "one", "The vector (1, 1).", "Vector2", false, {});
    add("Vector2", "xAxis", "The vector (1, 0).", "Vector2", false, {});
    add("Vector2", "yAxis", "The vector (0, 1).", "Vector2", false, {});
    add("Vector2", "X", "The x component.", "number", false, {});
    add("Vector2", "Y", "The y component.", "number", false, {});
    add("Vector2", "Magnitude", "The length of the vector.", "number", false, {});
    add("Vector2", "Unit", "The vector scaled to length 1.", "Vector2", false, {});
    add("Vector2", "Abs", "Each component made non-negative.", "Vector2", false, {});
    add("Vector2", "Ceil", "Each component rounded up.", "Vector2", false, {});
    add("Vector2", "Floor", "Each component rounded down.", "Vector2", false, {});
    add("Vector2", "Sign", "The sign of each component.", "Vector2", false, {});
    add("Vector2", "Cross", "The z of the cross product with other.", "number", false, {P("other", "Vector2")});
    add("Vector2", "Dot", "The dot product with other.", "number", false, {P("other", "Vector2")});
    add("Vector2", "Angle", "The angle in radians to other. Signed, counterclockwise positive, when isSigned is true.",
        "number", false, {P("other", "Vector2"), P("isSigned", "boolean?")});
    add("Vector2", "FuzzyEq", "True when each component is within epsilon. epsilon defaults to 1e-5.", "boolean", false,
        {P("other", "Vector2"), P("epsilon", "number?")});
    add("Vector2", "Lerp", "A linear blend toward goal. alpha 0 returns this vector and alpha 1 returns goal.", "Vector2",
        false, {P("goal", "Vector2"), P("alpha", "number")});
    add("Vector2", "Max", "The component-wise maximum with the others.", "Vector2", true, {P("other", "Vector2")});
    add("Vector2", "Min", "The component-wise minimum with the others.", "Vector2", true, {P("other", "Vector2")});

    add("", "Matrix4",
        "A position and rotation: a 4x4 matrix. new, lookAt, Angles, and fromAxisAngle build one. "
        "* composes two or moves a Vector3.",
        nullptr, false, {});
    add("Matrix4", "new",
        "No arguments: the identity. (pos), (pos, lookAt), (x, y, z), (x, y, z, qX, qY, qZ, qW) from a quaternion, or "
        "(x, y, z, R00, R01, R02, R10, R11, R12, R20, R21, R22) from the rotation's rows.",
        "Matrix4", true, {});
    add("Matrix4", "identity", "No rotation, at the origin.", "Matrix4", false, {});
    add("Matrix4", "lookAt", "At at, looking toward target. up defaults to Vector3.yAxis.", "Matrix4", false,
        {P("at", "Vector3"), P("target", "Vector3"), P("up", "Vector3?")});
    add("Matrix4", "lookAlong", "At at, looking along direction. up defaults to Vector3.yAxis.", "Matrix4", false,
        {P("at", "Vector3"), P("direction", "Vector3"), P("up", "Vector3?")});
    add("Matrix4", "fromRotationBetweenVectors", "The shortest rotation that turns from to face along to.", "Matrix4",
        false, {P("from", "Vector3"), P("to", "Vector3")});
    add("Matrix4", "fromEulerAngles", "Rotations about X, Y, and Z in radians, composed in order. order defaults to XYZ.",
        "Matrix4", false, {P("rx", "number"), P("ry", "number"), P("rz", "number"), P("order", "Enum.RotationOrder?")});
    add("Matrix4", "fromEulerAnglesXYZ", "Rotations about X, Y, and Z in radians, applied Z first, then Y, then X.",
        "Matrix4", false, {P("rx", "number"), P("ry", "number"), P("rz", "number")});
    add("Matrix4", "Angles", "The same as fromEulerAnglesXYZ.", "Matrix4", false,
        {P("rx", "number"), P("ry", "number"), P("rz", "number")});
    add("Matrix4", "fromEulerAnglesYXZ", "Rotations about X, Y, and Z in radians, applied Z first, then X, then Y.",
        "Matrix4", false, {P("rx", "number"), P("ry", "number"), P("rz", "number")});
    add("Matrix4", "fromOrientation", "The same as fromEulerAnglesYXZ.", "Matrix4", false,
        {P("rx", "number"), P("ry", "number"), P("rz", "number")});
    add("Matrix4", "fromAxisAngle", "A rotation of angle radians about axis.", "Matrix4", false,
        {P("axis", "Vector3"), P("angle", "number")});
    add("Matrix4", "fromMatrix", "At pos, with these right, up, and back axes. vZ defaults to vX:Cross(vY).Unit.",
        "Matrix4", false, {P("pos", "Vector3"), P("vX", "Vector3"), P("vY", "Vector3"), P("vZ", "Vector3?")});
    add("Matrix4", "X", "The x of the position.", "number", false, {});
    add("Matrix4", "Y", "The y of the position.", "number", false, {});
    add("Matrix4", "Z", "The z of the position.", "number", false, {});
    add("Matrix4", "Position", "The translation.", "Vector3", false, {});
    add("Matrix4", "Rotation", "The same rotation at the origin.", "Matrix4", false, {});
    add("Matrix4", "LookVector", "The forward direction: the third column, negated.", "Vector3", false, {});
    add("Matrix4", "RightVector", "The right direction: the first column.", "Vector3", false, {});
    add("Matrix4", "UpVector", "The up direction: the second column.", "Vector3", false, {});
    add("Matrix4", "XVector", "The rotation's first row: R00, R01, R02.", "Vector3", false, {});
    add("Matrix4", "YVector", "The rotation's second row: R10, R11, R12.", "Vector3", false, {});
    add("Matrix4", "ZVector", "The rotation's third row: R20, R21, R22.", "Vector3", false, {});
    add("Matrix4", "Inverse", "The matrix that undoes this one.", "Matrix4", false, {});
    add("Matrix4", "Lerp",
        "A blend toward goal: position in a line, rotation along the shortest arc. alpha 0 returns this and alpha 1 "
        "returns goal.",
        "Matrix4", false, {P("goal", "Matrix4"), P("alpha", "number")});
    add("Matrix4", "Orthonormalize", "The same position, with the rotation made orthonormal and any scale removed.",
        "Matrix4", false, {});
    add("Matrix4", "ToWorldSpace", "Each argument, taken as relative to this, in world space: self * other.", "Matrix4",
        true, {P("other", "Matrix4")});
    add("Matrix4", "ToObjectSpace", "Each argument relative to this: self:Inverse() * other.", "Matrix4", true,
        {P("other", "Matrix4")});
    add("Matrix4", "PointToWorldSpace", "Each point, taken as relative to this, in world space.", "Vector3", true,
        {P("point", "Vector3")});
    add("Matrix4", "PointToObjectSpace", "Each world point, relative to this.", "Vector3", true, {P("point", "Vector3")});
    add("Matrix4", "VectorToWorldSpace", "Each direction, taken as relative to this, in world space. Position is ignored.",
        "Vector3", true, {P("direction", "Vector3")});
    add("Matrix4", "VectorToObjectSpace", "Each world direction, relative to this. Position is ignored.", "Vector3",
        true, {P("direction", "Vector3")});
    add("Matrix4", "GetComponents", "x, y, z, then the rotation's rows: R00, R01, R02, R10, R11, R12, R20, R21, R22.",
        "number,number,number,number,number,number,number,number,number,number,number,number", false, {});
    add("Matrix4", "ToEulerAngles", "rx, ry, and rz that fromEulerAngles takes to build this rotation.",
        "number,number,number", false, {P("order", "Enum.RotationOrder?")});
    add("Matrix4", "ToEulerAnglesXYZ", "rx, ry, and rz that fromEulerAnglesXYZ takes to build this rotation.",
        "number,number,number", false, {});
    add("Matrix4", "ToEulerAnglesYXZ", "rx, ry, and rz that fromEulerAnglesYXZ takes to build this rotation.",
        "number,number,number", false, {});
    add("Matrix4", "ToOrientation", "The same as ToEulerAnglesYXZ.", "number,number,number", false, {});
    add("Matrix4", "ToAxisAngle", "The rotation as a unit axis and an angle in radians, from 0 to pi.", "Vector3,number",
        false, {});
    add("Matrix4", "FuzzyEq", "True when each component is within epsilon. epsilon defaults to 1e-5.", "boolean", false,
        {P("other", "Matrix4"), P("epsilon", "number?")});

    add("", "Enum",
        "Named constants. NormalId and Axis are used by Vector3.FromNormalId and Vector3.FromAxis, and RotationOrder by "
        "Matrix4.fromEulerAngles. KeyCode, UserInputType, and UserInputState describe an InputObject.",
        nullptr, false, {});
    add("EnumItem", "Name", "The item's name.", "string", false, {});
    add("EnumItem", "Value", "The item's numeric value.", "number", false, {});
    add("EnumItem", "EnumType", "The enum this item belongs to.", "table", false, {});

    add("", "Instance", "Builds instances. new takes a class name and an optional parent.", nullptr, false, {});
    add("Instance", "new", "Creates an instance of className and parents it when parent is given.", "Instance", false,
        {P("className", "string"), P("parent", "DataModel?")});

    add("", "print", "Writes each argument to the console, separated by tabs.", nullptr, true, {});
    add("", "require", "Runs a ModuleScript and returns what that module returns.", "", false, {P("module", "ModuleScript")});
    add("", "typeof", "The value's type name, such as Vector3 for a vector.", "string", false, {P("value", "any")});
    add("", "type", "The Luau type name, such as vector for a Vector3.", "string", false, {P("value", "any")});
    add("", "tostring", "The string for value.", "string", false, {P("value", "any")});
    add("", "tonumber", "value parsed as a number, or nil. radix selects the base.", "number", false,
        {P("value", "any"), P("radix", "number?")});
    add("", "assert", "Returns value when it is truthy. Errors with message otherwise.", "", false,
        {P("value", "any"), P("message", "string?")});
    add("", "error", "Stops the script with message.", nullptr, false, {P("message", "string"), P("level", "number?")});
    add("", "pcall", "Calls fn. Returns true and its results, or false and the error.", "", true, {P("fn", "function")});
    add("", "xpcall", "Calls fn. On error, calls err with the error object.", "", true,
        {P("fn", "function"), P("err", "function")});
    add("", "pairs", "An iterator over every key and value in t.", "function", false, {P("t", "table")});
    add("", "ipairs", "An iterator over the array part of t.", "function", false, {P("t", "table")});
    add("", "next", "The next key and value after key. Omit key to start at the first entry.", "", false,
        {P("t", "table"), P("key", "any")});
    add("", "select", "With index \"#\", the count of the extra arguments. Otherwise the arguments from index onward.", "", true,
        {P("index", "any")});
    add("", "unpack", "The array values from i through j.", "", false,
        {P("list", "table"), P("i", "number?"), P("j", "number?")});
    add("", "rawget", "t[key] without invoking a metatable.", "", false, {P("t", "table"), P("key", "any")});
    add("", "rawset", "Sets t[key] without invoking a metatable and returns t.", "table", false,
        {P("t", "table"), P("key", "any"), P("value", "any")});
    add("", "rawequal", "True when a and b are the same value, without invoking a metatable.", "boolean", false,
        {P("a", "any"), P("b", "any")});
    add("", "rawlen", "The length of value without invoking a metatable.", "number", false, {P("value", "any")});
    add("", "getmetatable", "The metatable of value, or nil.", "table", false, {P("value", "any")});
    add("", "setmetatable", "Sets the metatable of t and returns t.", "table", false, {P("t", "table"), P("meta", "table")});
    add("", "gcinfo", "The Lua heap size in kilobytes.", "number", false, {});
    add("", "_G", "A table shared by every script in the play session.", nullptr, false, {});
    add("", "shared", "A table shared by every script in the play session.", nullptr, false, {});
    add("", "_VERSION", "The Luau version name.", "string", false, {});

    add("DataModel", "Name", "The instance's name.", "string", false, {});
    add("DataModel", "ClassName", "The instance's class. This cannot be changed.", "string", false, {});
    add("DataModel", "Parent", "The instance this one is parented to, or game. nil means it has no parent.", "DataModel?",
        false, {});
    add("DataModel", "Changed", "Fires with the name of the property that changed.", "Signal", false, {});
    add("DataModel", "Destroy", "Unparents this instance and its descendants.", nullptr, false, {});
    add("DataModel", "GetChildren", "The direct children of this instance.", "{Instance}", false, {});
    add("DataModel", "FindFirstChild", "The direct child with this name, or nil.", "Instance?", false, {P("name", "string")});
    add("DataModel", "WaitForChild",
        "The direct child with this name. Yields the running script until it exists. With timeout, gives nil once "
        "that many seconds pass.",
        "Instance", false, {P("name", "string"), P("timeout", "number?")});
    add("DataModel", "IsA", "True when this instance's class is className or a subclass of it.", "boolean", false,
        {P("className", "string")});
    add("Game", "GetService",
        "The service with this name: Workspace, Lighting, Storage, Scripts, RunService, Selection, or "
        "UserInputService.",
        "Instance", false,
        {P("className", "string")});

    add("LuaSource", "Source", "The Luau source this instance holds.", "string", false, {});
    add("Script", "Source", "The Luau source this script runs.", "string", false, {});
    add("Script", "Enabled", "When false, the script does not run.", "boolean", false, {});
    add("ModuleScript", "Source", "The Luau source require runs.", "string", false, {});

    add("GameObject", "Transform", "Where this object is and how it is turned.", "Matrix4", false, {});
    add("GameObject", "Color", "Tints what this object draws: it multiplies each Material's Color. White leaves it as is.",
        "Color3", false, {});
    add("GameObject", "Transparency",
        "How much this object lets through what is behind it, from 0 (opaque) to 1. It stacks on each Material's "
        "Transparency.",
        "number", false, {});
    add("Attachment", "Offset", "Where this is relative to its parent PVInstance's Transform.", "Matrix4", false, {});
    add("Attachment", "OffsetSpace",
        "Local measures Offset from the parent PVInstance's whole Transform. World measures it from only the "
        "parent's position, along the world's axes.",
        "EnumItem", false, {});
    add("Attachment", "Transform",
        "Where this is in the world: Offset, measured from the parent PVInstance as OffsetSpace says. Writing it "
        "sets Offset.",
        "Matrix4", false, {});
    add("Camera", "FieldOfView",
        "How many degrees this camera sees from bottom to top, from 1 to 120. A Scene View linked to it draws with it.",
        "number", false, {});
    add("Camera", "ViewportPointToRay",
        "The ray from this camera through a point in its view: its origin (the camera's position) and unit "
        "direction. The point is in ViewportSize's units from the view's top-left corner. Errors until a view shows "
        "the camera.",
        "Vector3,Vector3", false, {P("x", "number"), P("y", "number")});
    add("Camera", "WorldToViewportPoint",
        "Where a world point falls in this camera's view: X and Y in ViewportSize's units from the top-left corner, "
        "Z its depth along the look (negative behind the camera), then whether it is in front and inside the view. "
        "Errors until a view shows the camera.",
        "Vector3,boolean", false, {P("point", "Vector3")});
    add("WireframeAdornment", "Adornee",
        "The PVInstance whose space the lines are in, so they follow it. nil draws them in world space.",
        "PVInstance?", false, {});
    add("WireframeAdornment", "Color3", "The color of every line added without its own.", "Color3", false, {});
    add("WireframeAdornment", "Transparency", "From 0 (solid) to 1 (unseen), for every line.", "number", false, {});
    add("WireframeAdornment", "Visible", "Whether the lines are drawn. They draw only in Workspace or Core.",
        "boolean", false, {});
    add("WireframeAdornment", "AddLine",
        "Adds a line between two points in the Adornee's space, drawn over the scene and faint where something "
        "nearer hides it. With color, the line keeps that color; without, it follows Color3. Lines are not saved.",
        "nil", false, {P("from", "Vector3"), P("to", "Vector3"), P("color", "Color3?")});
    add("WireframeAdornment", "AddLines", "Adds a line for each pair of points: 1 to 2, 3 to 4, and so on.", "nil",
        false, {P("points", "{Vector3}"), P("color", "Color3?")});
    add("WireframeAdornment", "AddPath", "Adds lines joining the points in order, and the last back to the first "
        "when closed is true.", "nil", false, {P("points", "{Vector3}"), P("closed", "boolean?"), P("color", "Color3?")});
    add("WireframeAdornment", "Clear", "Removes every line.", "nil", false, {});
    add("WireframeAdornment", "GetLineCount", "How many lines it holds, up to 65536.", "number", false, {});
    add("Light", "Color", "The color of the light this gives.", "Color3", false, {});
    add("Light", "Intensity", "How bright this light is. 0 gives none.", "number", false, {});
    add("Light", "Radius", "How many studs this light reaches. It fades to nothing there.", "number", false, {});
    add("DirectionalLight", "Direction",
        "Which way the light is, as the sun is in the sky: (0, 1, 0) shines straight down. It shines on everything "
        "alike, with no position or reach.",
        "Vector3", false, {});
    add("DirectionalLight", "Color", "The color of the light this gives.", "Color3", false, {});
    add("DirectionalLight", "Intensity", "How bright this light is. 0 gives none.", "number", false, {});
    add("DirectionalLight", "Enabled", "When false, this light gives none.", "boolean", false, {});
    add("Light", "Enabled", "When false, this light gives none.", "boolean", false, {});
    add("Light", "Shadows", "When true, this light casts shadows.", "boolean", false, {});
    add("DirectionalLight", "Shadows", "When true, this light casts shadows.", "boolean", false, {});
    add("DirectionalLight", "ShadowDistance", "How many studs from the camera get this light's shadows.", "number",
        false, {});
    add("Skybox", "Image",
        "The sky, an equirectangular image drawn behind everything and lighting every surface. An .hdr gives light "
        "brighter than white. Nil draws no sky.",
        "Texture?", false, {});
    add("Skybox", "Exposure", "How bright the sky and its light are, from 0 to 10. 1 is the image as it is.", "number",
        false, {});
    add("Skybox", "LightScale",
        "Multiplies the light the sky gives surfaces, from 0 to 10, without changing how the sky looks behind them. "
        "Lower it to let shadows show against a bright sky.",
        "number", false, {});
    add("Skybox", "Rotation", "How many degrees the sky is turned about the world's Y axis, from 0 up to 360.",
        "number", false, {});
    add("Skybox", "Tint", "A color the sky and its light are multiplied by. White leaves them as they are.", "Color3",
        false, {});
    add("DynamicSky", "TimeOfDay",
        "The hour, from 0 up to 24: sunrise in the east at 6, noon at 12, sunset in the west at 18. It moves "
        "only when set.",
        "number", false, {});
    add("DynamicSky", "Latitude",
        "Degrees from -90 to 90: at noon the sun stands 90 minus this many degrees up, to the south for a "
        "northern latitude.",
        "number", false, {});
    add("DynamicSky", "Brightness",
        "The sun light's intensity, from 0 to 20. The moon's light is a tenth of it.", "number", false, {});
    add("DynamicSky", "Shadows", "When true, the sun or moon light casts shadows.", "boolean", false, {});
    add("DynamicSky", "CloudCover", "How much of the sky has cloud, from 0 (clear) to 1 (overcast).", "number",
        false, {});
    add("DynamicSky", "CloudDensity",
        "How thick and opaque the clouds are, from 0 to 1. Thick cover dims the sun light.", "number", false, {});
    add("DynamicSky", "WindDirection",
        "Which way the clouds drift, across X and Z; its length is their speed in studs per second.", "Vector3",
        false, {});
    add("DynamicSky", "SunTexture", "An image drawn in place of the sun's disc. Nil draws the disc.", "Texture?",
        false, {});
    add("DynamicSky", "MoonTexture", "An image drawn in place of the moon's disc. Nil draws the disc.",
        "Texture?", false, {});
    add("DynamicSky", "SunSize", "How many degrees across the sun looks, from 0.1 to 20.", "number", false, {});
    add("DynamicSky", "MoonSize", "How many degrees across the moon looks, from 0.1 to 20.", "number", false, {});
    add("DynamicSky", "ReflectionQuality",
        "Enum.EffectQuality: how sharp the sky's light and reflections are (Low, Medium, High). The sky "
        "itself is always drawn at full resolution.",
        "Enum.EffectQuality", false, {});
    add("Lighting", "Antialiasing",
        "How the 3D scene's edges are smoothed: Enum.AntialiasingMode.FXAA, or None for hard pixel edges.",
        "Enum.AntialiasingMode", false, {});
    add("Lighting", "TerrainQuality",
        "Enum.EffectQuality: how much terrain texturing spends for how good it looks (Low, Medium, High).",
        "Enum.EffectQuality", false, {});
    add("BloomEffect", "Enabled", "When false, this BloomEffect draws no bloom.", "boolean", false, {});
    add("BloomEffect", "Intensity",
        "How much of the image moves into its blurred copy, from 0 to 1. 0 draws no bloom.", "number", false, {});
    add("BloomEffect", "Size",
        "How far bright light spreads, in pixels at a 1080-pixel-tall view, from 0 to 56. Smaller and larger views "
        "spread it in proportion.",
        "number", false, {});
    add("BloomEffect", "Threshold",
        "The brightness where bloom starts, from 0 to 10, fading in just below it. 0 blooms everything a little; "
        "above 1, only light brighter than white, such as a glowing Material or the sun in an .hdr sky.",
        "number", false, {});
    add("Terrain", "Transform",
        "Where this island is and how it is turned. It may be moved and rotated, but not scaled: VoxelSize sets "
        "the size of its cells.",
        "Matrix4", false, {});
    add("Terrain", "VoxelSize", "The size of one cell, in studs. Read-only; always 1.", "number", false, {});
    add("Terrain", "CanCollide", "When false, nothing collides with this island.", "boolean", false, {});
    add("Terrain", "TextureSize",
        "Enum.TextureSize: how large this Terrain's packed textures are (Small 256, Medium 512, Large 1024, Max "
        "2048 pixels).",
        "Enum.TextureSize", false, {});
    // The voxel methods take positions and frames in world space unless space
    // is Enum.TransformSpace.Local, and a TerrainMaterial of this Terrain or nil
    // for the default material.
    add("Terrain", "FillBall",
        "Fills a ball of radius around center with material. Meshes and colliders follow a frame or two later.", "nil",
        false,
        {P("center", "Vector3"), P("radius", "number"), P("material", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "FillBlock",
        "Fills a box of size placed by transform with material. Meshes and colliders follow a frame or two later.",
        "nil", false,
        {P("transform", "Matrix4"), P("size", "Vector3"), P("material", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "FillCylinder",
        "Fills a cylinder along transform's Y with material. Meshes and colliders follow a frame or two later.", "nil",
        false,
        {P("transform", "Matrix4"), P("height", "number"), P("radius", "number"), P("material", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "FillWedge",
        "Fills a wedge of size placed by transform with material. Meshes and colliders follow a frame or two later.",
        "nil", false,
        {P("transform", "Matrix4"), P("size", "Vector3"), P("material", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "SubtractBall",
        "Carves a ball of radius around center out of the voxels. Meshes and colliders follow a frame or two later.",
        "nil", false, {P("center", "Vector3"), P("radius", "number"), P("space", "Enum.TransformSpace?")});
    add("Terrain", "SubtractBlock",
        "Carves a box of size placed by transform out of the voxels. Meshes and colliders follow a frame or two later.",
        "nil", false, {P("transform", "Matrix4"), P("size", "Vector3"), P("space", "Enum.TransformSpace?")});
    add("Terrain", "SubtractCylinder",
        "Carves a cylinder along transform's Y out of the voxels. Meshes and colliders follow a frame or two later.",
        "nil", false,
        {P("transform", "Matrix4"), P("height", "number"), P("radius", "number"), P("space", "Enum.TransformSpace?")});
    add("Terrain", "SubtractWedge",
        "Carves a wedge of size placed by transform out of the voxels. Meshes and colliders follow a frame or two "
        "later.",
        "nil", false, {P("transform", "Matrix4"), P("size", "Vector3"), P("space", "Enum.TransformSpace?")});
    add("Terrain", "PaintBall",
        "Sets the material of the solid cells in a ball, keeping their shape. Meshes and colliders follow a frame or "
        "two later.",
        "nil", false,
        {P("center", "Vector3"), P("radius", "number"), P("material", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "PaintBlock",
        "Sets the material of the solid cells in a box, keeping their shape. Meshes and colliders follow a frame or "
        "two later.",
        "nil", false,
        {P("transform", "Matrix4"), P("size", "Vector3"), P("material", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "ReplaceMaterial",
        "Swaps from for to in the solid cells of the box between min and max, both included. Meshes and colliders "
        "follow a frame or two later.",
        "nil", false,
        {P("min", "Vector3"), P("max", "Vector3"), P("from", "TerrainMaterial?"), P("to", "TerrainMaterial?"),
         P("space", "Enum.TransformSpace?")});
    add("Terrain", "AddMaterial",
        "Adds a TerrainMaterial on the lowest free Id, named after material. Raises past 255.", "TerrainMaterial",
        false, {P("material", "Material?")});
    add("Terrain", "ReadVoxels",
        "The cells from min to max, both included, in whole cell coordinates: Distances in studs, negative inside, "
        "and Materials as Ids, 0 for air and the default. Distances[i][j][k] is cell min + (i - 1, j - 1, k - 1).",
        "{Distances: {{{number}}}, Materials: {{{number}}}}", false, {P("min", "Vector3"), P("max", "Vector3")});
    add("Terrain", "WorldToCell", "The cell nearest a world position, as whole numbers.", "Vector3", false,
        {P("position", "Vector3")});
    add("Terrain", "GetMaterials", "Every TerrainMaterial of this Terrain, ordered by Id.", "{TerrainMaterial}", false,
        {});
    add("Terrain", "GetMaterialById", "The TerrainMaterial with that Id, or nil.", "TerrainMaterial?", false,
        {P("id", "number")});
    add("Terrain", "GetMaterialsFor", "Every TerrainMaterial backed by material, ordered by Id.", "{TerrainMaterial}",
        false, {P("material", "Material")});
    add("Terrain", "RemoveMaterial",
        "The same as entry:Destroy(). Cells on its Id keep it and draw as the default. Raises if entry belongs to "
        "another Terrain.",
        "nil", false, {P("entry", "TerrainMaterial")});
    add("Terrain", "WriteVoxels",
        "Writes cells as ReadVoxels gives them: distances[i][j][k] and materials[i][j][k] become cell min + (i - 1, "
        "j - 1, k - 1). Both arrays must have the same shape, and Ids must be whole numbers from 0 to 255; an Id with "
        "no TerrainMaterial draws as the default. Meshes and colliders follow a frame or two later.",
        "nil", false, {P("min", "Vector3"), P("distances", "{{{number}}}"), P("materials", "{{{number}}}")});
    add("Terrain", "CellToWorld", "The world position a cell samples; WorldToCell goes the other way.", "Vector3",
        false, {P("cell", "Vector3")});
    add("Terrain", "Clear",
        "Removes every voxel. The TerrainMaterials stay. Meshes and colliders follow a frame or two later.", "nil",
        false, {});
    add("TerrainMaterial", "Id",
        "The voxel Id this entry names, from 1 to 255. Read-only: its Terrain picks the lowest one free when it is "
        "added.",
        "number", false, {});
    add("TerrainMaterial", "Material",
        "The Material that voxels with this Id draw and collide as. Nil draws them as the default material.",
        "Material?", false, {});
    add("ScreenSpaceReflections", "Enabled", "When false, this ScreenSpaceReflections traces nothing.", "boolean",
        false, {});
    add("ScreenSpaceReflections", "Intensity",
        "How much of a smooth surface's sky reflection the reflection of what is on screen replaces, from 0 to 1.",
        "number", false, {});
    add("ScreenSpaceReflections", "MaxDistance",
        "How far a reflected ray may travel, in studs, from 0 to 1000. It fades out over the last quarter.", "number",
        false, {});
    add("ScreenSpaceReflections", "MaxRoughness",
        "Surfaces rougher than this, from 0 to 1, keep the sky's reflection; it fades in just below.", "number",
        false, {});
    add("AmbientOcclusionEffect", "Enabled", "When false, this AmbientOcclusionEffect shades nothing.", "boolean",
        false, {});
    add("AmbientOcclusionEffect", "Intensity",
        "How strongly creases and contact are shaded, from 0 to 4. 1 is physical; above 1 is darker.", "number",
        false, {});
    add("AmbientOcclusionEffect", "Radius",
        "How far, in studs, from 0 to 10, nearby geometry still hides the sky from a surface.", "number", false, {});
    add("AmbientOcclusionEffect", "Quality",
        "Enum.EffectQuality: Low and Medium shade at half resolution, High at full.", "Enum.EffectQuality", false,
        {});
    add("SpotLight", "OuterFOV",
        "The whole angle of this light's cone, in degrees, from 1 to 179. The cone points down the Transform's -Z.",
        "number", false, {});
    add("SpotLight", "InnerFOVScale",
        "How much of the cone, from 0 to 1, is at full brightness. The light fades from there to the cone's edge.",
        "number", false, {});
    add("Material", "Color", "The color that tints this material's surface. White leaves it as it is.", "Color3",
        false, {});
    add("Material", "Metalness", "How metallic this material is, from 0 to 1. Scales its MetalnessTexture.", "number",
        false, {});
    add("Material", "Roughness", "How rough this material is, from 0 (shiny) to 1. Scales its RoughnessTexture.",
        "number", false, {});
    add("Material", "Emissive", "The light this material gives off itself. Black gives none. Scales its EmissiveTexture.",
        "Color3", false, {});
    add("Material", "Reflectivity", "How much this material reflects its surroundings, from 0 to 1.", "number", false,
        {});
    add("Material", "Transparency", "How much this material lets through what is behind it, from 0 (opaque) to 1.",
        "number", false, {});
    add("Material", "HeightTexture",
        "A height map for terrain texturing's blend. Nil falls back to NormalTexture, then DiffuseTexture's "
        "brightness, then flat.",
        "Texture?", false, {});
    add("Material", "TextureScale",
        "For terrain texturing: units per texture repeat when this material is applied in world space. Must be "
        "greater than 0.",
        "number", false, {});
    add("Material", "BlendSharpness",
        "For terrain texturing: how sharp the height blend against other materials is, from 0 (soft) to 1 (sharp).",
        "number", false, {});
    add("Material", "HeightStrength",
        "For terrain texturing: how much HeightTexture's height, over the plain blend weight, decides which "
        "material wins. Must not be negative.",
        "number", false, {});
    // Every Add writes the Mesh's AMESH file under the project's resources folder, giving
    // the Mesh a Path first if it has none. During play they change a copy for the session
    // instead, which the Scene View draws and Stop drops.
    add("Mesh", "AddBox", "Adds a box of size, centered on position, to this mesh's file.", "nil", false,
        {P("size", "Vector3"), P("position", "Vector3?")});
    add("Mesh", "AddSphere", "Adds a sphere, segments around (24 by default), centered on position.", "nil", false,
        {P("radius", "number"), P("segments", "number?"), P("position", "Vector3?")});
    add("Mesh", "AddCylinder", "Adds a cylinder along Y, segments around (16 by default), capped unless capped is false.",
        "nil", false,
        {P("radius", "number"), P("height", "number"), P("segments", "number?"), P("capped", "boolean?"),
         P("position", "Vector3?")});
    add("Mesh", "AddCone", "Adds a cone along Y, its point up, its base capped unless capped is false.", "nil", false,
        {P("radius", "number"), P("height", "number"), P("segments", "number?"), P("capped", "boolean?"),
         P("position", "Vector3?")});
    add("Mesh", "AddPlane", "Adds a flat rectangle facing up, centered on position.", "nil", false,
        {P("width", "number"), P("depth", "number"), P("position", "Vector3?")});
    add("Mesh", "AddTeapot", "Adds a teapot size tall, standing on position, its spout toward +X.", "nil", false,
        {P("size", "number"), P("position", "Vector3?")});
    add("Mesh", "Clear", "Empties this mesh's file.", "nil", false, {});

    add("SoundEmitter", "Play",
        "Plays Sound from TimePosition, or from the start when it is already playing. Parented to a PVInstance, it is "
        "heard from there and follows it; anywhere else it is heard the same in both ears.",
        "nil", false, {});
    add("SoundEmitter", "Stop", "Stops the sound and puts TimePosition back to 0.", "nil", false, {});
    add("SoundEmitter", "Sound", "The Sound this plays.", "Sound?", false, {});
    add("SoundEmitter", "Volume", "How loud this plays, from 0 to 5. 1 is the file as it is.", "number", false, {});
    add("SoundEmitter", "Pitch", "How fast this plays, from 0 to 5, which moves its pitch with it. 1 is as recorded.",
        "number", false, {});
    add("SoundEmitter", "Looped", "When true, the sound plays again from the start each time it ends.", "boolean",
        false, {});
    add("SoundEmitter", "RollOffMode", "How the sound gets quieter with distance from its PVInstance.", "EnumItem", false,
        {});
    add("SoundEmitter", "RollOffMinDistance", "Within this many studs, from 0 to 512, the sound is at full volume.",
        "number", false, {});
    add("SoundEmitter", "RollOffMaxDistance", "Past this many studs, from 0 to 512, the sound gets no quieter.",
        "number", false, {});
    add("SoundEmitter", "TimePosition", "How many seconds into the sound it is. Writing it while playing seeks.",
        "number", false, {});
    add("SoundEmitter", "IsPlaying", "True while the sound plays. Read-only.", "boolean", false, {});
    add("Sound", "TimeLength",
        "How long this Sound's file plays, in seconds. 0 when Path names no file that plays. Read-only.", "number",
        false, {});

    add("GuiBase", "ClassList", "CSS classes, separated by spaces, that a stylesheet's .class selectors match.",
        "string", false, {});
    add("GuiBase", "Style", "Inline CSS declarations, as an HTML style attribute. They win over stylesheets.",
        "string", false, {});
    add("GuiBase", "Size", "The preferred size in points. 0 on an axis leaves it to the content and CSS.", "Vector2",
        false, {});
    add("GuiBase", "Alignment", "Where the children sit, as an Enum.GuiAlignment.", "EnumItem", false, {});
    add("GuiBase", "Visible", "When false, it and everything in it is hidden.", "boolean", false, {});
    add("GuiBase", "MouseTransparent", "When true the mouse passes through it to what is behind.", "boolean", false,
        {});
    add("GuiBase", "MouseClicked", "Fires when the left button is pressed and released on it, or on what is in it.",
        "Signal", false, {});
    add("GuiBase", "MousePressed", "Fires when the left button goes down on it, or on what is in it.", "Signal", false,
        {});
    add("GuiBase", "MouseReleased", "Fires when the left button comes up after a press on it.", "Signal", false, {});
    add("GuiBase", "MouseEntered", "Fires when the pointer moves onto it.", "Signal", false, {});
    add("GuiBase", "MouseExited", "Fires when the pointer leaves it.", "Signal", false, {});
    add("GuiBasePane", "BackgroundColor", "The color behind its children. A stylesheet's background-color wins.",
        "Color3", false, {});
    add("GuiBasePane", "BackgroundTransparency", "0 is the BackgroundColor as it is, 1 is no background.", "number",
        false, {});
    add("ImagePane", "Image",
        "The Texture drawn over its background and under its children, stretched to fill it. Nil draws none.",
        "Texture?", false, {});
    add("ImagePane", "ImageTransparency", "0 is the Image as it is, 1 is no image. Its background and children stay.",
        "number", false, {});
    add("HBox", "Spacing", "Points between its children.", "number", false, {});
    add("VBox", "Spacing", "Points between its children.", "number", false, {});
    add("Label", "Text", "The text it shows.", "string", false, {});
    add("Label", "TextColor", "The text's color. A stylesheet's color wins.", "Color3", false, {});
    add("Label", "FontSize", "The text's size in points, from 1 to 512.", "number", false, {});
    add("Label", "TextScaled", "When true, the text grows or shrinks to the largest size that fits the label.",
        "boolean", false, {});
    add("Button", "Text", "The text on the button.", "string", false, {});
    add("Button", "TextScaled", "When true, the text grows or shrinks to the largest size that fits the button.",
        "boolean", false, {});
    add("Button", "Action", "Fires when the button is clicked, or Enter is pressed while it has focus.", "Signal",
        false, {});
    add("TextField", "Text", "The text in the field. Typing changes it.", "string", false, {});
    add("TextField", "Prompt", "Shown while the field is empty.", "string", false, {});
    add("TextField", "Action", "Fires when Enter is pressed in the field.", "Signal", false, {});
    add("CSS", "Source", "The stylesheet for its parent GuiBase and everything inside it.", "string", false, {});

    add("Signal", "Connect", "Calls callback when the signal fires and returns the connection.", "Connection", false,
        {P("callback", "function")});
    add("Signal", "Wait", "Yields until the signal fires, then returns the signal's arguments.", "", false, {});
    add("Connection", "Disconnect", "Stops this connection from firing.", nullptr, false, {});
    add("Connection", "Connected", "True until Disconnect runs.", "boolean", false, {});

    add("Selection", "Get", "The selected instances, in the order they were selected.", "{Instance}", false, {});
    add("Selection", "Set", "Selects these instances and nothing else. The explorer shows the same selection.", nullptr,
        false, {P("selection", "{Instance}")});

    add("ChangeHistoryService", "TryBeginRecording",
        "Opens a recording: every change to the place until FinishRecording is one undo step with this name. Returns its "
        "id, or nil when a recording is already open, history is off, or an undo is being applied. A change no "
        "recording covers is not an undo step.",
        "string?", false, {P("name", "string"), P("displayName", "string?")});
    add("ChangeHistoryService", "FinishRecording",
        "Closes the recording with this id. Commit keeps its changes as one undo step; Cancel puts them back. An id "
        "that names no open recording does nothing.",
        nullptr, false, {P("id", "string"), P("operation", "Enum.FinishRecordingOperation")});
    add("ChangeHistoryService", "IsRecordingInProgress",
        "True while the recording with this id is open, or while any is when id is omitted.", "boolean", false,
        {P("id", "string?")});
    add("ChangeHistoryService", "SetWaypoint", "Commits the open recording under this name. Does nothing when none is open.",
        nullptr, false, {P("name", "string")});
    add("ChangeHistoryService", "Undo", "Undoes the newest step. Does nothing while a recording is open.", nullptr, false, {});
    add("ChangeHistoryService", "Redo", "Redoes the step last undone. Does nothing while a recording is open.", nullptr,
        false, {});
    add("ChangeHistoryService", "GetCanUndo", "Whether there is a step to undo, and its name.", "boolean, string", false, {});
    add("ChangeHistoryService", "GetCanRedo", "Whether there is a step to redo, and its name.", "boolean, string", false, {});
    add("ChangeHistoryService", "ResetWaypoints",
        "Forgets every undo and redo step, and drops an open recording without putting its changes back. During play "
        "it forgets only the steps made since Play; the edit steps from before stay.",
        nullptr, false, {});
    add("ChangeHistoryService", "OnUndo", "Fires after an undo. The argument is the step's name.", "Signal", false, {});
    add("ChangeHistoryService", "OnRedo", "Fires after a redo. The argument is the step's name.", "Signal", false, {});
    add("ChangeHistoryService", "OnRecordingStarted", "Fires when a recording opens. The arguments are its name and displayName.",
        "Signal", false, {});
    add("ChangeHistoryService", "OnRecordingFinished",
        "Fires when a recording closes. The arguments are its name, displayName, id, and the Enum.FinishRecordingOperation.",
        "Signal", false, {});

    add("UserInputService", "InputBegan",
        "Fires when a key or mouse button goes down in the scene view. The arguments are the InputObject and "
        "gameProcessedEvent.",
        "Signal", false, {});
    add("UserInputService", "InputChanged",
        "Fires when the mouse moves or the wheel turns over the scene view. The arguments are the InputObject and "
        "gameProcessedEvent.",
        "Signal", false, {});
    add("UserInputService", "InputEnded",
        "Fires when a key or mouse button comes back up, or the scene view loses focus while it is down. The arguments "
        "are the InputObject and gameProcessedEvent.",
        "Signal", false, {});
    add("UserInputService", "IsKeyDown", "True while this key is held.", "boolean", false, {P("keyCode", "Enum.KeyCode")});
    add("UserInputService", "IsMouseButtonPressed", "True while this mouse button is held.", "boolean", false,
        {P("mouseButton", "Enum.UserInputType")});
    add("UserInputService", "GetKeysPressed", "An InputObject for each key held, in the order they went down.", "{InputObject}",
        false, {});
    add("UserInputService", "GetMouseButtonsPressed", "An InputObject for each mouse button held.", "{InputObject}", false, {});
    add("UserInputService", "GetMouseLocation", "The pointer in the scene view, in points from its top-left corner.",
        "Vector2", false, {});
    add("UserInputService", "KeyboardEnabled", "True when there is a keyboard.", "boolean", false, {});
    add("UserInputService", "MouseEnabled", "True when there is a mouse.", "boolean", false, {});
    add("UserInputService", "TouchEnabled", "True when there is a touch screen.", "boolean", false, {});
    add("UserInputService", "MouseBehavior",
        "What the pointer does. LockCurrentPosition or LockCenter hides it and holds it in the focused scene view, and "
        "GetMouseDelta reports its motion. Default frees it. Losing the view's focus sets Default.",
        "EnumItem", false, {});
    add("UserInputService", "MouseDeltaSensitivity", "Scales GetMouseDelta. 1 by default, never below 0.", "number",
        false, {});
    add("UserInputService", "GetMouseDelta",
        "How far the mouse moved in the latest step, in points, times MouseDeltaSensitivity. It keeps reporting while "
        "the pointer is locked.",
        "Vector2", false, {});
    add("InputObject", "KeyCode", "The key, or Enum.KeyCode.Unknown for mouse input.", "EnumItem", false, {});
    add("InputObject", "UserInputType", "What made the input: Keyboard, MouseButton1, MouseMovement, and so on.",
        "EnumItem", false, {});
    add("InputObject", "UserInputState", "Begin, Change, or End.", "EnumItem", false, {});
    add("InputObject", "Position", "The pointer in the scene view when the input happened. For MouseWheel, z is how far the wheel turned.", "Vector3", false,
        {});
    add("InputObject", "Delta", "How far the mouse moved. z is 0.", "Vector3", false,
        {});

    add("RunService", "Heartbeat", "Fires on every simulation step. The argument dt is the step length in seconds.", "Signal",
        false, {});
    add("RunService", "PreSimulation", "Fires before the simulation step. The argument dt is the step length.", "Signal", false,
        {});
    add("RunService", "PostSimulation", "Fires after the simulation step. The argument dt is the step length.", "Signal", false,
        {});
    add("RunService", "PreAnimation", "Fires before animation in the simulation step. The argument dt is the step length.",
        "Signal", false, {});
    add("RunService", "PreRender", "A render step. Scripts cannot connect to it.", "Signal", false, {});
    add("RunService", "RenderStepped",
        "Fires every displayed frame, before the frame is drawn. The handler runs in the render step: what it "
        "reads is what the frame draws, and a visual write lands in that frame. The argument dt is the frame's "
        "time in seconds.",
        "Signal", false, {});
    add("RunService", "IsRunning", "True while a play session is open, paused or not. False in edit mode.", "boolean",
        false, {});

    add("Workspace", "CurrentCamera",
        "The Camera the studio's scene view last used, set when you click in a view or pick its camera. Nil when that "
        "Camera is gone. Not saved.",
        "Camera", false, {});
    add("Workspace", "Gravity",
        "How fast every PhysicsObject and PlayerController speeds downward, in studs per second per second. "
        "Below 0 pulls up; 0 is none. Defaults to 9.81.",
        "number", false, {});
    add("Workspace", "Raycast",
        "Casts a ray from origin along direction (its length is how far) and returns the first thing it hits, as a "
        "RaycastResult, or nil. params (a RaycastParams) chooses what it sees. Works stopped and playing. A ray "
        "that starts inside something does not hit it.",
        "RaycastResult?", false, {P("origin", "Vector3"), P("direction", "Vector3"), P("params", "RaycastParams?")});

    add("RaycastParams", "new", "A new RaycastParams, excluding nothing.", "RaycastParams", false, {});
    add("RaycastParams", "FilterType",
        "Whether FilterDescendantsInstances is what the ray sees (Include) or what it does not (Exclude, the "
        "default).",
        "Enum.RaycastFilterType", false, {});
    add("RaycastParams", "FilterDescendantsInstances",
        "The instances (and everything under them) FilterType either limits the ray to or hides from it. Empty by "
        "default.",
        "{Instance}", false, {});

    add("RaycastResult", "Instance", "What the ray hit, or nil if it is gone since.", "Instance", false, {});
    add("RaycastResult", "Position", "Where the ray hit, in world space.", "Vector3", false, {});
    add("RaycastResult", "Normal", "The hit surface's outward normal.", "Vector3", false, {});
    add("RaycastResult", "Distance", "How far along the ray the hit is, in studs.", "number", false, {});
    add("RaycastResult", "Material",
        "The Material where the ray hit: a Terrain's material there, or the hit Brush face's. Nil for anything else, or "
        "a face with none.",
        "Material?", false, {});
    add("RaycastResult", "Face", "The index into the hit Brush's GetFaces(). Nil for anything but a Brush.", "number?",
        false, {});

    add("", "BrushFace",
        "One face of a Brush: three points on its plane, counter-clockwise seen from outside, its Material, and its "
        "texture alignment. A value, like Vector3: change one with With.",
        nullptr, false, {});
    add("BrushFace", "new", "A face through three points, counter-clockwise seen from outside. Collinear points raise.",
        "BrushFace", false, {P("p1", "Vector3"), P("p2", "Vector3"), P("p3", "Vector3")});
    add("BrushFace", "fromPlane", "A face from its outward normal and a point on it.", "BrushFace", false,
        {P("normal", "Vector3"), P("point", "Vector3")});
    add("BrushFace", "P1", "The first point, in the Brush's space.", "Vector3", false, {});
    add("BrushFace", "P2", "The second point.", "Vector3", false, {});
    add("BrushFace", "P3", "The third point.", "Vector3", false, {});
    add("BrushFace", "Material", "The face's Material; nil draws the default.", "Material?", false, {});
    add("BrushFace", "UAxis", "The texture's U axis, in the Brush's space.", "Vector3", false, {});
    add("BrushFace", "VAxis", "The texture's V axis, in the Brush's space.", "Vector3", false, {});
    add("BrushFace", "Offset", "The texture's shift, in repeats.", "Vector2", false, {});
    add("BrushFace", "Scale", "Multiplies the Material's TextureScale.", "Vector2", false, {});
    add("BrushFace", "Rotation", "Turns the texture about the face's normal, in degrees.", "number", false, {});
    add("BrushFace", "Normal", "The outward unit normal, from the points.", "Vector3", false, {});
    add("BrushFace", "With", "A copy with the named fields changed, such as {Material = m, Rotation = 90}.",
        "BrushFace", false, {P("changes", "table")});

    add("Brush", "CanCollide", "When false, nothing collides with this Brush and rays pass through it.", "boolean",
        false, {});
    add("Brush", "Color", "Tints every face, over its Material.", "Color3", false, {});
    add("Brush", "Transparency", "Fades every face, over its Material.", "number", false, {});
    add("Brush", "GetFaces", "Every face, in order.", "{BrushFace}", false, {});
    add("Brush", "SetFaces",
        "Replaces every face. Faces that cut nothing are dropped. Raises, changing nothing, when the faces do not "
        "close a solid.",
        "nil", false, {P("faces", "{BrushFace}")});
    add("Brush", "GetFace", "One face.", "BrushFace", false, {P("index", "number")});
    add("Brush", "SetFace", "Replaces one face.", "nil", false, {P("index", "number"), P("face", "BrushFace")});
    add("Brush", "SetFaceMaterial", "Sets one face's Material.", "nil", false,
        {P("index", "number"), P("material", "Material?")});
    add("Brush", "SetMaterial", "Sets every face's Material at once; nil is the default.", "nil", false,
        {P("material", "Material?")});
    add("Brush", "GetFaceVertices", "One face's corners, counter-clockwise seen from outside.", "{Vector3}", false,
        {P("index", "number")});
    add("Brush", "GetVertices", "Every corner, in the Brush's space.", "{Vector3}", false, {});
    add("Brush", "GetBounds", "The smallest and largest corner, in the Brush's space.", "Vector3,Vector3", false, {});
    add("Brush", "ContainsPoint", "Whether a point in the Brush's space is inside or on it.", "boolean", false,
        {P("point", "Vector3")});
    add("Brush", "Clip", "Adds a face, cutting away everything in front of it. A face with no Material takes the Material, scale, and "
        "rotation of the face it most nearly faces.", "nil", false,
        {P("face", "BrushFace")});
    add("Brush", "MoveFace", "Moves a face along its normal; negative moves it in.", "nil", false,
        {P("index", "number"), P("distance", "number")});
    add("Brush", "Expand", "Moves every face along its normal; negative shrinks.", "nil", false,
        {P("distance", "number")});
    add("Brush", "MakeBox", "Becomes a box of this size around the origin.", "nil", false, {P("size", "Vector3")});
    add("Brush", "MakeCylinder", "Becomes a cylinder along Y filling size.", "nil", false,
        {P("size", "Vector3"), P("sides", "number")});
    add("Brush", "MakeCone", "Becomes a cone along Y filling size, its tip at the top.", "nil", false,
        {P("size", "Vector3"), P("sides", "number")});
    add("Brush", "MakeSphere", "Becomes an icosphere filling size; detail 0 to 3.", "nil", false,
        {P("size", "Vector3"), P("detail", "number?")});
    add("Brush", "MakeHull", "Becomes the convex hull of points in the Brush's space. No Materials.", "nil", false,
        {P("points", "{Vector3}")});
    add("Brush", "GetEdges", "Every edge once, as two indices into GetVertices().", "{{number}}", false, {});
    add("Brush", "GetFaceVertexIndices", "One face's corners as indices into GetVertices(), counter-clockwise seen "
        "from outside.", "{number}", false, {P("index", "number")});
    add("Brush", "GetFaceAt", "The face whose plane holds a point in the Brush's space, with the normal nearest "
        "the one given; nil when none does.", "number?", false, {P("point", "Vector3"), P("normal", "Vector3")});
    add("Brush", "Split", "Cuts the Brush in two along a plane in its space. It keeps the half behind the normal "
        "and returns the faces of the half in front, for SetFaces on another Brush. Errors when the plane misses.",
        "{BrushFace}", false, {P("normal", "Vector3"), P("point", "Vector3")});
    add("Brush", "GetLoopCut", "A loop cut across one face's edge nearest point: the plane square to that edge, at "
        "a whole number of grid steps from its start (no snapping when grid is 0 or nil), or at its middle. Returns "
        "the plane's normal and where it crosses the edge, then the edge's two ends; nil when the cut lands on an "
        "end. Pass the first two to Split.", "Vector3?,Vector3?,Vector3?,Vector3?", false,
        {P("index", "number"), P("point", "Vector3"), P("grid", "number?"), P("middle", "boolean?")});
    add("Brush", "MoveVertex", "Moves the corner at from to to, in the Brush's space. Faces keep their texture "
        "where their plane survives. Errors when no corner is at from or the Brush would not stay convex.", "nil",
        false, {P("from", "Vector3"), P("to", "Vector3")});
    add("Brush", "TransformShape", "Moves every face by a Matrix4 in the Brush's space, turning texture axes with "
        "them. Mirroring keeps faces facing out. Transform is untouched.", "nil", false,
        {P("transform", "Matrix4")});

    return docs;
}

const std::unordered_map<std::string, LuaDoc>& docs() {
    static const std::unordered_map<std::string, LuaDoc> table = build_docs();
    return table;
}

}  // namespace

LuaDoc lua_symbol_doc(std::string_view owner, std::string_view name) {
    std::string current(owner);
    for (int depth = 0; depth < 32; ++depth) {
        const auto found = docs().find(result_key(current, name));
        if (found != docs().end()) {
            return found->second;
        }
        if (current.empty()) {
            break;
        }
        const ClassRecord* record = find_class_const(current.c_str());
        if (record == nullptr || record->base == nullptr || record->base[0] == '\0') {
            break;
        }
        current = record->base;
    }
    return {};
}

void lua_doc_names(std::string_view owner, std::vector<std::string>& out) {
    out.clear();
    const std::string prefix = std::string(owner) + "\n";
    for (const auto& entry : docs()) {
        if (entry.first.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        out.emplace_back(entry.first.substr(prefix.size()));
    }
    std::sort(out.begin(), out.end());
}

}  // namespace engine_core
