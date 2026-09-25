#include "LuaApi.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine_core {
namespace {

struct ClassRecord {
    const char* name = nullptr;
    const char* base = nullptr;
    std::vector<LuaField> fields;
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

std::string result_key(std::string_view owner, std::string_view name) {
    std::string key;
    key.reserve(owner.size() + name.size() + 1);
    key.append(owner);
    key.push_back('\n');
    key.append(name);
    return key;
}

void append_unique(std::vector<LuaField>& out, const LuaField& field) {
    for (LuaField& existing : out) {
        if (existing.name != nullptr && field.name != nullptr && std::strcmp(existing.name, field.name) == 0) {
            existing = field;
            return;
        }
    }
    out.push_back(field);
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

void register_lua_class(const char* class_name, const char* base, const LuaField* fields, int count) {
    if (class_name == nullptr) {
        return;
    }
    ClassRecord* record = find_class(class_name);
    if (record == nullptr) {
        classes().push_back(ClassRecord{});
        record = &classes().back();
        record->name = class_name;
    }
    if (base != nullptr && record->base == nullptr) {
        record->base = base;
    }
    if (fields == nullptr || count <= 0) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        append_unique(record->fields, fields[index]);
    }
}

void lua_class_members(const char* class_name, std::vector<LuaField>& out) {
    out.clear();
    collect(class_name, out, 0);
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

void register_lua_creatable(const char* class_name, LuaCreate create) {
    if (class_name == nullptr || create == nullptr || find_creatable(class_name) != nullptr) {
        return;
    }
    creatables().push_back(Creatable{class_name, create});
}

bool lua_creatable_known(const char* class_name) { return find_creatable(class_name) != nullptr; }

void lua_creatable_names(std::vector<std::string>& out) {
    out.clear();
    for (const Creatable& record : creatables()) {
        if (record.name != nullptr) {
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
    results()[result_key(owner, name)] = std::move(note);
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

LuaField component(const char* name) { return lua_property(name, "number", true, nullptr, nullptr); }

ANARCHY_LUA_REGISTER(register_value_classes) {
    const LuaField color[] = {component("r"), component("g"), component("b"), component("a")};
    register_lua_class("Color", nullptr, color, 4);
    register_lua_class("Transform", nullptr, nullptr, 0);
    register_lua_class("Instance", "DataModel", nullptr, 0);
}

}  // namespace

LuaResult lua_function_result(std::string_view owner, std::string_view name) {
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

    add("", "Enum", "Named constants. NormalId and Axis are used by Vector3.FromNormalId and Vector3.FromAxis.", nullptr, false,
        {});
    add("EnumItem", "Name", "The item's name.", "string", false, {});
    add("EnumItem", "Value", "The item's numeric value.", "number", false, {});
    add("EnumItem", "EnumType", "The enum this item belongs to.", "table", false, {});

    add("", "Instance", "Builds instances. new takes a class name and an optional parent.", nullptr, false, {});
    add("Instance", "new", "Creates an instance of className and parents it when parent is given.", "Instance", false,
        {P("className", "string"), P("parent", "Instance?")});

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
    add("DataModel", "Parent", "The instance this one is parented to. nil means it has no parent.", "Instance?", false, {});
    add("DataModel", "Changed", "Fires with the name of the property that changed.", "Signal", false, {});
    add("DataModel", "Destroy", "Unparents this instance and its descendants.", nullptr, false, {});
    add("DataModel", "GetChildren", "The direct children of this instance.", "{Instance}", false, {});
    add("DataModel", "FindFirstChild", "The direct child with this name, or nil.", "Instance?", false, {P("name", "string")});
    add("DataModel", "IsA", "True when this instance's class is className or a subclass of it.", "boolean", false,
        {P("className", "string")});
    add("DataModel", "GetService", "The service with this name. RunService is the registered service.", "Instance", false,
        {P("className", "string")});

    add("Script", "Source", "The Luau source this script runs.", "string", false, {});
    add("Script", "Enabled", "When false, the script does not run.", "boolean", false, {});
    add("ModuleScript", "Source", "The Luau source require runs.", "string", false, {});
    add("ModuleScript", "Enabled", "Stored on the module. require does not check it.", "boolean", false, {});

    add("GameObject", "Color", "The color stored on this object.", "Color", false, {});
    add("GameObject", "Transform", "A table of 16 numbers.", "Transform", false, {});
    add("GameObject", "CFrame", "The same transform as Transform.", "Transform", false, {});
    add("TestTriangle", "Position", "Where the triangle is drawn. Positive z is toward the camera.", "Vector3", false, {});
    add("Color", "r", "The red component.", "number", false, {});
    add("Color", "g", "The green component.", "number", false, {});
    add("Color", "b", "The blue component.", "number", false, {});
    add("Color", "a", "The alpha component.", "number", false, {});

    add("Signal", "Connect", "Calls callback when the signal fires and returns the connection.", "Connection", false,
        {P("callback", "function")});
    add("Signal", "Wait", "Yields until the signal fires, then returns the signal's arguments.", "", false, {});
    add("Connection", "Disconnect", "Stops this connection from firing.", nullptr, false, {});
    add("Connection", "Connected", "True until Disconnect runs.", "boolean", false, {});

    add("RunService", "Heartbeat", "Fires on every simulation step. The argument dt is the step length in seconds.", "Signal",
        false, {});
    add("RunService", "PreSimulation", "Fires before the simulation step. The argument dt is the step length.", "Signal", false,
        {});
    add("RunService", "PostSimulation", "Fires after the simulation step. The argument dt is the step length.", "Signal", false,
        {});
    add("RunService", "PreAnimation", "Fires before animation in the simulation step. The argument dt is the step length.",
        "Signal", false, {});
    add("RunService", "PreRender", "A render step. Scripts cannot connect to it.", "Signal", false, {});
    add("RunService", "RenderStepped", "A render step. Scripts cannot connect to it.", "Signal", false, {});

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
