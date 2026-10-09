#pragma once

#include "DataModel.hpp"
#include "PropertyBag.hpp"
#include "types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace engine_core {

// One instance and everything under it, as copy and paste and instance files
// carry it: its class, name, saved properties (the ones its class does not
// know too), a script's source, and its children in order.
struct CopiedNode {
    std::string class_name;
    std::string name;
    PropertyBag properties;
    bool has_source = false;
    std::string source;
    std::vector<CopiedNode> children;
};

// The instance and everything under it. An empty node for a dead id.
CopiedNode copy_tree(const DataModel& game, InstanceId id);

// Builds each root, with its children, under parent, last among its
// children. A class that cannot be made, a full place, or a parent that
// refuses one leaves that root out, and refused, when given, gets the first
// reason. False when none could be built. Runs on the simulation thread.
bool paste_copies(DataModel& world, const std::vector<CopiedNode>& roots, InstanceId parent,
                  std::vector<InstanceId>* made = nullptr, std::string* refused = nullptr);

// An instance file (.aeinst, and .aeplugin, which is the same format):
// {"format": "aeinst", "version": 1, "roots": [node...]}, where a node is
// {"class", "name", "properties"?, "source"?, "children"?}.
JsonValue write_instance_file(const std::vector<CopiedNode>& roots);
// False, with error naming the JSON path that is wrong, for another format
// or version, a node missing its class or name, a class no one can make, or
// a value of the wrong kind. roots is left empty then.
bool read_instance_file(const JsonValue& json, std::vector<CopiedNode>& roots, std::string& error);
// Writes path.tmp, then renames it over path, so a reader never sees half a file.
bool save_instance_file(const std::filesystem::path& path, const std::vector<CopiedNode>& roots, std::string& error);
bool load_instance_file(const std::filesystem::path& path, std::vector<CopiedNode>& roots, std::string& error);

}  // namespace engine_core
