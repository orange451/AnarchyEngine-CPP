#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace engine_core {

struct TableSnapshot;

// One field of a printed table, copied when print ran. Both strings are display text:
// key is `name`, `[1]`, or `["two words"]`, and value is `"text"`, `12`, or what tostring gives.
// table is set when the value is a table that was copied too; one with a __tostring is not,
// and value holds that name. A table already open above this field, or past the depth limit,
// is not copied either: note says which, "cycle" or "too deep".
struct TableField {
    std::string key;
    std::string value;
    std::shared_ptr<const TableSnapshot> table;
    std::string note;
};

// The fields of a table, numbers first in order, then names, then the rest.
// The copy is immutable, so the console can read it on another thread.
// A print copies at most a fixed number of fields in all; omitted counts what was left out.
struct TableSnapshot {
    std::vector<TableField> fields;
    std::size_t omitted = 0;
};

// True for a table print copies: one without a __tostring. A table that names itself
// prints that name, the same as any other value, and does not open.
bool snapshots_as_table(lua_State* state, int index);

// Copies the table at index. Metamethods are not used to walk it; __tostring still names values.
// An error from a __tostring propagates the same way it does from print.
std::shared_ptr<const TableSnapshot> snapshot_table(lua_State* state, int index);

}  // namespace engine_core
