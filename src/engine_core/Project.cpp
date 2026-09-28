#include "Project.hpp"

#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "JsonMerge.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace engine_core {
namespace fs = std::filesystem;

namespace {

constexpr int kFormat = 1;
constexpr const char* kEngine = "engine_core";
constexpr std::size_t kMaxNameBytes = 120;
const char* const kResourceKinds[] = {"textures", "meshes", "audio"};

constexpr const char* kGitignore =
    ".studio/\n"
    "*.tmp\n"
    ".DS_Store\n";

constexpr const char* kGitattributes =
    "* text=auto eol=lf\n"
    "*.luau text eol=lf\n"
    "*.json text eol=lf\n"
    "resources/textures/** binary\n"
    "resources/meshes/** binary\n"
    "resources/audio/** binary\n";

constexpr const char* kResourcesReadme =
    "# Resources\n"
    "\n"
    "Textures, meshes, and audio live under `resources/`. The engine does not load\n"
    "them yet. These rules hold now so that projects saved today stay valid.\n"
    "\n"
    "1. `project.json` names the resources root: `\"resources\": { \"root\": \"resources\" }`.\n"
    "2. An instance refers to a resource with a string path relative to that root,\n"
    "   such as `\"path\": \"textures/brick.png\"`. It never stores a runtime id.\n"
    "3. Changing a resource file must not rewrite any instance file under `src/`.\n"
    "4. Resource bytes are never packed or embedded into JSON.\n"
    "5. Import, when it exists, copies a file into `resources/<kind>/` and creates\n"
    "   an instance that holds its path.\n";

struct ClassEntry {
    std::string name;
    ProjectFactory factory = nullptr;
};

std::vector<ClassEntry>& class_registry() {
    static std::vector<ClassEntry> entries = [] {
        std::vector<ClassEntry> out;
        out.push_back({"DataModel", [](DataModel& world) -> DataModel& { return world.create(); }});
        out.push_back({"GameObject", [](DataModel& world) -> DataModel& { return world.create_game_object(); }});
        out.push_back({"Script", [](DataModel& world) -> DataModel& { return world.create<Script>(); }});
        out.push_back({"ModuleScript", [](DataModel& world) -> DataModel& { return world.create<ModuleScript>(); }});
        out.push_back({"Folder", [](DataModel& world) -> DataModel& { return world.create<Folder>(); }});
        out.push_back({"TestTriangle", [](DataModel& world) -> DataModel& { return world.create<TestTriangle>(); }});
        return out;
    }();
    return entries;
}

ProjectFactory find_factory(std::string_view class_name) {
    for (const ClassEntry& entry : class_registry()) {
        if (entry.name == class_name) {
            return entry.factory;
        }
    }
    return nullptr;
}

// Paths inside a project are UTF-8 strings joined with '/'.
fs::path disk_path(const fs::path& root, const std::string& relative) { return root / fs::u8path(relative); }

std::string utf8(const fs::path& path) { return path.u8string(); }

std::string join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

bool ends_with(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[noreturn]] void fail(const std::string& message) { throw ProjectError(message); }

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("cannot read " + utf8(path));
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        fail("cannot read " + utf8(path));
    }
    return bytes;
}

// Writes beside the target, then renames over it, so a crash leaves the old file.
void write_file(const fs::path& path, const std::string& bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        fail("cannot create " + utf8(path.parent_path()) + ": " + error.message());
    }
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail("cannot write " + utf8(temp));
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            fail("cannot write " + utf8(temp));
        }
    }
    fs::rename(temp, path, error);
    if (error) {
        fs::remove(temp, error);
        fail("cannot replace " + utf8(path));
    }
}

void move_file(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    if (error) {
        fail("cannot create " + utf8(to.parent_path()) + ": " + error.message());
    }
    fs::rename(from, to, error);
    if (error) {
        fail("cannot move " + utf8(from) + " to " + utf8(to) + ": " + error.message());
    }
}

void remove_file(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
    if (error) {
        fail("cannot remove " + utf8(path) + ": " + error.message());
    }
}

bool missing_or_empty_dir(const fs::path& path) {
    std::error_code error;
    if (!fs::exists(path, error)) {
        return true;
    }
    return fs::is_directory(path, error) && fs::is_empty(path, error);
}

std::string project_name_for(const fs::path& root) {
    fs::path normal = root.lexically_normal();
    std::string name = utf8(normal.filename());
    if (name.empty() || name == ".") {
        name = utf8(normal.parent_path().filename());
    }
    return name.empty() ? std::string("Project") : name;
}

// A relative directory that stays inside the project root.
bool safe_relative(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const fs::path parsed = fs::u8path(path);
    if (parsed.is_absolute() || parsed.has_root_name()) {
        return false;
    }
    for (const fs::path& part : parsed) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

struct Layout {
    std::string src = "src";
    std::string resources = "resources";
};

JsonValue project_document(const std::string& name, const Layout& layout) {
    JsonValue doc = JsonValue::object();
    doc.set("format", JsonValue::number(kFormat));
    doc.set("name", JsonValue::string(name));
    doc.set("engine", JsonValue::string(kEngine));
    JsonValue tree = JsonValue::object();
    tree.set("src", JsonValue::string(layout.src));
    doc.set("tree", std::move(tree));
    JsonValue resources = JsonValue::object();
    resources.set("root", JsonValue::string(layout.resources));
    doc.set("resources", std::move(resources));
    return doc;
}

void read_project_json(const fs::path& root, std::string& name, Layout& layout) {
    const fs::path path = root / "project.json";
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        fail(utf8(path) + " is missing");
    }
    JsonValue doc;
    std::string message;
    if (!parse_json(read_file(path), doc, message)) {
        fail(utf8(path) + ": " + message);
    }
    if (!doc.is_object()) {
        fail(utf8(path) + ": expected an object");
    }
    const JsonValue* format = doc.find("format");
    if (format == nullptr || !format->is_number() || format->as_number() != kFormat) {
        fail(utf8(path) + ": format must be " + std::to_string(kFormat));
    }
    const JsonValue* label = doc.find("name");
    name = label != nullptr && label->is_string() ? label->as_string() : project_name_for(root);
    if (const JsonValue* tree = doc.find("tree")) {
        const JsonValue* src = tree->find("src");
        if (src == nullptr || !src->is_string() || !safe_relative(src->as_string())) {
            fail(utf8(path) + ": tree.src must be a relative directory");
        }
        layout.src = src->as_string();
    }
    if (const JsonValue* resources = doc.find("resources")) {
        const JsonValue* dir = resources->find("root");
        if (dir == nullptr || !dir->is_string() || !safe_relative(dir->as_string())) {
            fail(utf8(path) + ": resources.root must be a relative directory");
        }
        layout.resources = dir->as_string();
    }
}

bool reserved_key(const std::string& key) {
    return key == "class" || key == "id" || key == "Name" || key == "children";
}

}  // namespace

namespace detail {

// One instance read from disk, before anything is created.
struct PlanNode {
    std::string guid;
    std::string class_name;
    std::string name;
    PropertyBag properties;
    bool has_order = false;
    std::vector<std::string> order;
    bool has_source = false;
    std::string source;
    std::string props_path;
    std::string props_bytes;
    std::string source_path;
    std::string source_bytes;
    std::vector<std::size_t> children;
    // The properties file as parsed.
    JsonValue doc;
};

}  // namespace detail

namespace {

using detail::PlanNode;

// "Part.3f2a9c1e8b" -> guid "3f2a9c1e8b". The name part is display only.
bool split_stem(const std::string& stem, std::string& guid) {
    const std::size_t dot = stem.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return false;
    }
    guid = stem.substr(dot + 1);
    return valid_guid(guid);
}

class PlanReader {
public:
    PlanReader(const fs::path& root, const Layout& layout) : root_(root), layout_(layout) {}

    std::vector<PlanNode> read() {
        const std::string src = layout_.src;
        std::error_code error;
        if (!fs::is_directory(disk_path(root_, src), error)) {
            fail(utf8(disk_path(root_, src)) + " is missing");
        }
        const std::string init = join(src, "init.json");
        if (!fs::is_regular_file(disk_path(root_, init), error)) {
            fail(utf8(disk_path(root_, init)) + " is missing");
        }
        PlanNode root;
        read_props(root, init, std::string(), /*root*/ true);
        // Projects saved before the root was its own class call it DataModel.
        if (root.class_name != "Game" && root.class_name != "DataModel") {
            fail(init + ": the root class must be Game");
        }
        claim(root.guid, init);
        nodes_.push_back(std::move(root));
        scan(src, 0);
        for (PlanNode& node : nodes_) {
            order_children(node);
        }
        return std::move(nodes_);
    }

private:
    void claim(const std::string& guid, const std::string& path) {
        const auto found = seen_.find(guid);
        if (found != seen_.end()) {
            fail("GUID " + guid + " is used by both " + found->second + " and " + path);
        }
        seen_.emplace(guid, path);
    }

    // expected_guid empty: the root, whose GUID is not in a filename.
    // The root is game, which no factory makes. Its class is checked by the caller.
    void read_props(PlanNode& node, const std::string& path, const std::string& expected_guid, bool root = false) {
        node.props_path = path;
        node.props_bytes = read_file(disk_path(root_, path));
        JsonValue doc;
        std::string message;
        if (!parse_json(node.props_bytes, doc, message)) {
            fail(path + ": " + message);
        }
        if (!doc.is_object()) {
            fail(path + ": expected an object");
        }
        const JsonValue* klass = doc.find("class");
        const JsonValue* id = doc.find("id");
        const JsonValue* name = doc.find("Name");
        if (klass == nullptr || !klass->is_string() || klass->as_string().empty()) {
            fail(path + ": \"class\" must be a string");
        }
        if (id == nullptr || !id->is_string() || !valid_guid(id->as_string())) {
            fail(path + ": \"id\" must be a GUID of [0-9a-z-]");
        }
        if (!expected_guid.empty() && id->as_string() != expected_guid) {
            fail(path + ": id " + id->as_string() + " does not match the GUID " + expected_guid + " in the filename");
        }
        if (name == nullptr || !name->is_string()) {
            fail(path + ": \"Name\" must be a string");
        }
        node.class_name = klass->as_string();
        node.guid = id->as_string();
        node.name = name->as_string();
        node.doc = doc;
        if (!root && find_factory(node.class_name) == nullptr) {
            fail(path + ": unknown class " + node.class_name);
        }
        if (const JsonValue* children = doc.find("children")) {
            if (!children->is_array()) {
                fail(path + ": \"children\" must be an array of GUIDs");
            }
            node.has_order = true;
            for (const JsonValue& entry : children->items()) {
                if (!entry.is_string()) {
                    fail(path + ": \"children\" must be an array of GUIDs");
                }
                node.order.push_back(entry.as_string());
            }
        }
        for (const JsonValue::Member& member : doc.members()) {
            if (!reserved_key(member.first)) {
                bag_set(node.properties, member.first, member.second);
            }
        }
    }

    void read_source(PlanNode& node, const std::string& path) {
        node.has_source = true;
        node.source_path = path;
        node.source_bytes = read_file(disk_path(root_, path));
        node.source = node.source_bytes;
    }

    std::size_t add(PlanNode node, std::size_t parent) {
        const std::size_t index = nodes_.size();
        nodes_.push_back(std::move(node));
        nodes_[parent].children.push_back(index);
        return index;
    }

    void scan(const std::string& dir, std::size_t parent) {
        std::vector<std::pair<std::string, bool>> entries;
        std::error_code error;
        for (fs::directory_iterator it(disk_path(root_, dir), error), end; !error && it != end; it.increment(error)) {
            const std::string name = utf8(it->path().filename());
            std::error_code kind;
            entries.emplace_back(name, it->is_directory(kind));
        }
        if (error) {
            fail("cannot list " + dir + ": " + error.message());
        }
        std::sort(entries.begin(), entries.end());

        std::map<std::string, std::string> luau;
        std::map<std::string, std::string> meta;
        const bool instance_folder = parent != 0 || dir != layout_.src;
        for (const auto& [name, is_dir] : entries) {
            // Dotfiles (.gitkeep, .DS_Store) and interrupted writes are not instances.
            if (name.empty() || name[0] == '.' || ends_with(name, ".tmp")) {
                continue;
            }
            const std::string path = join(dir, name);
            if (!is_dir && (name == "init.json" || name == "init.meta.json" || name == "init.luau")) {
                // The folder's own files. The root's init.json was read already.
                if (!instance_folder && name != "init.json") {
                    fail(path + ": the root cannot be a script");
                }
                continue;
            }
            std::string guid;
            if (is_dir) {
                if (!split_stem(name, guid)) {
                    fail(path + ": a folder name must end in .<guid>");
                }
                read_folder(path, guid, parent);
                continue;
            }
            if (ends_with(name, ".meta.json")) {
                if (!split_stem(name.substr(0, name.size() - 10), guid)) {
                    fail(path + ": a filename must be <Name>.<guid>.meta.json");
                }
                if (!meta.emplace(guid, path).second) {
                    fail("GUID " + guid + " has two .meta.json files in " + dir);
                }
            } else if (ends_with(name, ".luau")) {
                if (!split_stem(name.substr(0, name.size() - 5), guid)) {
                    fail(path + ": a filename must be <Name>.<guid>.luau");
                }
                if (!luau.emplace(guid, path).second) {
                    fail("GUID " + guid + " has two .luau files in " + dir);
                }
            } else if (ends_with(name, ".json")) {
                if (!split_stem(name.substr(0, name.size() - 5), guid)) {
                    fail(path + ": a filename must be <Name>.<guid>.json");
                }
                PlanNode node;
                read_props(node, path, guid);
                claim(guid, path);
                add(std::move(node), parent);
            }
        }
        // A script is its .meta.json and its .luau, paired by GUID, never by Name.
        for (const auto& [guid, path] : meta) {
            const auto source = luau.find(guid);
            if (source == luau.end()) {
                fail(path + ": no .luau file with GUID " + guid);
            }
            PlanNode node;
            read_props(node, path, guid);
            read_source(node, source->second);
            claim(guid, path);
            add(std::move(node), parent);
            luau.erase(source);
        }
        for (const auto& [guid, path] : luau) {
            fail(path + ": no .meta.json file with GUID " + guid);
        }
    }

    void read_folder(const std::string& dir, const std::string& guid, std::size_t parent) {
        std::error_code error;
        const bool plain = fs::is_regular_file(disk_path(root_, join(dir, "init.json")), error);
        const bool meta = fs::is_regular_file(disk_path(root_, join(dir, "init.meta.json")), error);
        const bool source = fs::is_regular_file(disk_path(root_, join(dir, "init.luau")), error);
        if (plain && meta) {
            fail(dir + ": has both init.json and init.meta.json");
        }
        if (source && !meta) {
            fail(join(dir, "init.luau") + ": no init.meta.json");
        }
        if (meta && !source) {
            fail(join(dir, "init.meta.json") + ": no init.luau");
        }
        if (!plain && !meta) {
            fail(dir + ": a folder instance needs init.json");
        }
        PlanNode node;
        const std::string props = join(dir, plain ? "init.json" : "init.meta.json");
        read_props(node, props, guid);
        if (meta) {
            read_source(node, join(dir, "init.luau"));
        }
        claim(guid, props);
        const std::size_t index = add(std::move(node), parent);
        scan(dir, index);
    }

    // Listed GUIDs first, in the listed order. The rest sort by GUID, not by Name.
    void order_children(PlanNode& node) {
        std::vector<std::size_t> sorted = node.children;
        std::sort(sorted.begin(), sorted.end(),
                  [this](std::size_t a, std::size_t b) { return nodes_[a].guid < nodes_[b].guid; });
        if (!node.has_order) {
            node.children = std::move(sorted);
            return;
        }
        std::vector<std::size_t> ordered;
        std::unordered_set<std::size_t> used;
        for (const std::string& guid : node.order) {
            for (std::size_t child : sorted) {
                if (nodes_[child].guid == guid && used.insert(child).second) {
                    ordered.push_back(child);
                    break;
                }
            }
        }
        for (std::size_t child : sorted) {
            if (used.count(child) == 0) {
                ordered.push_back(child);
            }
        }
        node.children = std::move(ordered);
    }

    const fs::path& root_;
    const Layout& layout_;
    std::vector<PlanNode> nodes_;
    std::unordered_map<std::string, std::string> seen_;
};

// Every file under src that carries a GUID in its name, by GUID: <Name>.<guid>.json,
// .meta.json, and .luau, and a folder's init files under <Name>.<guid>/. Names
// that fit none of these, dotfiles, dot folders, and .tmp files are skipped, as
// a load skips or reports them.
std::map<std::string, std::vector<std::string>> guid_claims(const fs::path& root, const std::string& src) {
    std::map<std::string, std::vector<std::string>> claims;
    const fs::path top = disk_path(root, src);
    std::error_code error;
    for (fs::recursive_directory_iterator it(top, error), end; !error && it != end; it.increment(error)) {
        const std::string name = utf8(it->path().filename());
        std::error_code kind;
        std::string guid;
        if (it->is_directory(kind)) {
            // Only an instance's folder, <Name>.<guid>: not a dot folder, and not
            // a copy such as "Box.3f2a copy", whose files are the user's.
            if (name.empty() || name[0] == '.' || !split_stem(name, guid)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (name.empty() || name[0] == '.' || ends_with(name, ".tmp") || !it->is_regular_file(kind)) {
            continue;
        }
        if (name == "init.json" || name == "init.meta.json" || name == "init.luau") {
            // The root's init.json names no GUID; its GUID is inside.
            if (it->path().parent_path() == top || !split_stem(utf8(it->path().parent_path().filename()), guid)) {
                continue;
            }
        } else {
            std::string stem;
            if (ends_with(name, ".meta.json")) {
                stem = name.substr(0, name.size() - 10);
            } else if (ends_with(name, ".luau")) {
                stem = name.substr(0, name.size() - 5);
            } else if (ends_with(name, ".json")) {
                stem = name.substr(0, name.size() - 5);
            } else {
                continue;
            }
            if (!split_stem(stem, guid)) {
                continue;
            }
        }
        claims[guid].push_back(it->path().lexically_relative(root).generic_u8string());
    }
    return claims;
}

// "src/Box.3f2a/init.json" -> "src/Box.3f2a": the folder an instance with
// children owns. Empty for a leaf's file.
std::string own_folder(const std::string& props_path) {
    const std::size_t slash = props_path.rfind('/');
    if (slash == std::string::npos) {
        return std::string();
    }
    const std::string name = props_path.substr(slash + 1);
    return name == "init.json" || name == "init.meta.json" ? props_path.substr(0, slash) : std::string();
}

// Each node's parent, by index. Parents come before children.
template <typename Node>
std::vector<std::size_t> parents_of(const std::vector<Node>& nodes) {
    std::vector<std::size_t> out(nodes.size(), 0);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        for (std::size_t child : nodes[index].children) {
            out[child] = index;
        }
    }
    return out;
}

// "game.Box.Part": the Names from the root down to index. The root is "game".
template <typename Node>
std::string path_of(const std::vector<Node>& nodes, const std::vector<std::size_t>& parents, std::size_t index) {
    std::vector<const std::string*> names;
    for (std::size_t at = index; at != 0; at = parents[at]) {
        names.push_back(&nodes[at].name);
    }
    std::string out = "game";
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        out += "." + **it;
    }
    return out;
}

// The first line where a and b differ, from each, as "2: print(1)".
std::pair<std::string, std::string> differing_lines(const std::string& a, const std::string& b) {
    std::istringstream left(a);
    std::istringstream right(b);
    std::string one;
    std::string two;
    for (int line = 1;; ++line) {
        const bool more_left = static_cast<bool>(std::getline(left, one));
        const bool more_right = static_cast<bool>(std::getline(right, two));
        if (!more_left && !more_right) {
            // Only line endings differ.
            return {"(line endings)", "(line endings)"};
        }
        if (more_left != more_right || one != two) {
            const std::string at = std::to_string(line) + ": ";
            return {more_left ? at + one : "(no line " + std::to_string(line) + ")",
                    more_right ? at + two : "(no line " + std::to_string(line) + ")"};
        }
    }
}

// An instance's file as merge_keys compares it: with its parent's GUID as
// "Parent" and a script's text as "Source". The root has no parent.
JsonValue compared_json(JsonValue props, const std::string& parent, bool root, bool has_source,
                        const std::string& source) {
    if (!root) {
        props.set("Parent", JsonValue::string(parent));
    }
    if (has_source) {
        props.set("Source", JsonValue::string(source));
    }
    return props;
}

// b differs from a in some key.
bool differs(const JsonValue& a, const JsonValue& b) {
    for (const KeyMerge& merged : merge_keys(a, a, b)) {
        if (merged.change == KeyChange::StudioOnly) {
            return true;
        }
    }
    return false;
}

void apply_properties(DataModel& object, const PlanNode& node) {
    DataModel& world = object;
    for (const JsonValue::Member& member : node.properties) {
        std::string error;
        const bool owned = object.load_property(member.first, member.second, error);
        if (!error.empty()) {
            fail(node.props_path + ": " + error);
        }
        if (!owned) {
            world.set_extra_property(object.id(), member.first, member.second);
        }
    }
}

void clear_extras(DataModel& world, InstanceId id) {
    std::vector<std::string> keys;
    for (const JsonValue::Member& member : world.extra_properties(id)) {
        keys.push_back(member.first);
    }
    for (const std::string& key : keys) {
        world.erase_extra_property(id, key);
    }
}

// Destroys every live instance, parented or not, and clears the root's extras.
void clear_world(DataModel& world) {
    std::vector<InstanceId> ids;
    world.for_each_instance([&ids](DataModel& object) { ids.push_back(object.id()); });
    for (InstanceId id : ids) {
        if (world.alive(id)) {
            world.destroy(id);
        }
    }
    clear_extras(world, 0);
}

// Creates the plan in world. Returns the runtime id of each plan node.
std::vector<InstanceId> build(DataModel& world, const std::vector<PlanNode>& plan) {
    std::vector<InstanceId> ids(plan.size(), 0);
    const PlanNode& top = plan[0];
    world.set_guid(0, top.guid);
    world.set_name(0, top.name);
    clear_extras(world, 0);
    for (const JsonValue::Member& member : top.properties) {
        std::string error;
        const bool owned = world.load_property(member.first, member.second, error);
        if (!error.empty()) {
            fail(top.props_path + ": " + error);
        }
        if (!owned) {
            world.set_extra_property(0, member.first, member.second);
        }
    }
    for (std::size_t index = 1; index < plan.size(); ++index) {
        const PlanNode& node = plan[index];
        const ProjectFactory factory = find_factory(node.class_name);
        if (factory == nullptr) {
            fail(node.props_path + ": unknown class " + node.class_name);
        }
        DataModel& object = factory(world);
        const InstanceId id = object.id();
        ids[index] = id;
        world.set_guid(id, node.guid);
        world.set_name(id, node.name);
        apply_properties(object, node);
        auto* lua = dynamic_cast<LuaSource*>(&object);
        if (lua != nullptr && !node.has_source) {
            fail(node.props_path + ": class " + node.class_name + " keeps Source in a .luau file; use .meta.json");
        }
        if (lua == nullptr && node.has_source) {
            fail(node.props_path + ": class " + node.class_name + " has no Source; use .json");
        }
        if (lua != nullptr) {
            lua->set_source(node.source);
        }
    }
    // set_parent puts a child last, so children go in in their saved order.
    for (std::size_t index = 0; index < plan.size(); ++index) {
        for (std::size_t child : plan[index].children) {
            world.set_parent(ids[child], ids[index]);
        }
    }
    return ids;
}

// History off while the tree is built. Afterwards the loaded tree is the
// place, and nothing from before is undoable.
class Rebuild {
public:
    explicit Rebuild(DataModel& world) : world_(world), was_(world.history().enabled()) {
        if (world_.simulation_running()) {
            world_.stop_simulation();
        }
        world_.history().set_enabled(false);
    }

    ~Rebuild() { world_.history().set_enabled(was_); }

    void finish() {
        world_.capture_place();
        world_.history().reset_waypoints();
        world_.clear_authored_dirty();
    }

private:
    DataModel& world_;
    bool was_;
};

JsonValue instance_json(const AuthoredNode& node, const std::vector<AuthoredNode>& tree) {
    JsonValue doc = JsonValue::object();
    doc.set("class", JsonValue::string(node.class_name));
    doc.set("id", JsonValue::string(node.guid));
    doc.set("Name", JsonValue::string(node.name));
    for (const JsonValue::Member& member : node.properties) {
        if (reserved_key(member.first)) {
            fail("instance " + node.guid + ": property \"" + member.first + "\" is reserved");
        }
        doc.set(member.first, member.second);
    }
    // Written only when the order is not the default GUID sort.
    std::vector<std::string> order;
    order.reserve(node.children.size());
    for (std::size_t child : node.children) {
        order.push_back(tree[child].guid);
    }
    if (!std::is_sorted(order.begin(), order.end())) {
        std::vector<JsonValue> items;
        for (std::string& guid : order) {
            items.push_back(JsonValue::string(std::move(guid)));
        }
        doc.set("children", JsonValue::array(std::move(items)));
    }
    return doc;
}

std::string instance_bytes(const JsonValue& doc, const AuthoredNode& node) {
    try {
        return write_json(doc);
    } catch (const std::invalid_argument&) {
        fail("instance " + node.guid + " (" + node.name + ") has a non-finite number");
    }
}

}  // namespace

void register_project_class(const char* class_name, ProjectFactory factory) {
    if (class_name == nullptr || factory == nullptr || find_factory(class_name) != nullptr) {
        return;
    }
    class_registry().push_back({class_name, factory});
}

bool project_class_known(std::string_view class_name) { return find_factory(class_name) != nullptr; }

std::string sanitize_file_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        const bool illegal = byte < 0x20 || byte == 0x7f || std::strchr("<>:\"/\\|?*", c) != nullptr;
        out.push_back(illegal ? '_' : c);
    }
    if (out.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
    if (out.empty() || out == "." || out == "..") {
        return "_";
    }
    if (out[0] == '.') {
        out[0] = '_';
    }
    // Windows reserves these before the first dot, whatever the extension.
    std::string head = out.substr(0, out.find('.'));
    while (!head.empty() && head.back() == ' ') {
        head.pop_back();
    }
    std::transform(head.begin(), head.end(), head.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const std::set<std::string> kDevices = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                                   "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                                   "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (kDevices.count(head) != 0) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string describe_conflict(const SaveConflict& conflict) {
    switch (conflict.kind) {
    case SaveConflict::Kind::EditedOutside:
        return conflict.key.empty() ? conflict.path + " changed on disk"
                                    : conflict.path + ": " + conflict.key + " changed on disk";
    case SaveConflict::Kind::DeletedOutside:
        return conflict.path + " was deleted on disk";
    case SaveConflict::Kind::MovedOutside:
        return conflict.path + " was moved or renamed on disk";
    case SaveConflict::Kind::AddedOutside:
        return conflict.path + " was added on disk";
    }
    return conflict.path;
}

namespace {

std::string conflict_message(const std::vector<SaveConflict>& conflicts) {
    if (conflicts.empty()) {
        return "files changed on disk";
    }
    std::string text = describe_conflict(conflicts.front()) + " since it was loaded or saved";
    if (conflicts.size() > 1) {
        text += " (and " + std::to_string(conflicts.size() - 1) + " more)";
    }
    return text;
}

}  // namespace

ProjectConflict::ProjectConflict(std::vector<SaveConflict> conflicts)
    : ProjectError(conflict_message(conflicts)), conflicts_(std::move(conflicts)) {}

Project::Project() = default;
Project::Project(Project&&) noexcept = default;
Project& Project::operator=(Project&&) noexcept = default;
Project::~Project() = default;

void Project::bind(DataModel* game, std::unique_ptr<DataModel> owned) {
    owned_ = std::move(owned);
    game_ = owned_ ? owned_.get() : game;
}

std::optional<InstanceId> Project::instance_for(std::string_view guid) const {
    const auto found = guid_id_.find(std::string(guid));
    if (found == guid_id_.end()) {
        return std::nullopt;
    }
    return found->second;
}

Project::Files Project::from_disk(const detail::PlanNode& node, std::string parent) {
    Files files;
    files.props_path = node.props_path;
    files.props_bytes = node.props_bytes;
    files.has_source = node.has_source;
    files.source_path = node.source_path;
    files.source_bytes = node.source_bytes;
    files.props = node.doc;
    files.parent = std::move(parent);
    return files;
}

void Project::write_skeleton(const fs::path& root) const {
    const Layout layout;
    write_file(root / "project.json", write_json(project_document(name_, layout)));
    for (const char* kind : kResourceKinds) {
        write_file(disk_path(root, join(join(layout.resources, kind), ".gitkeep")), std::string());
    }
    write_file(root / ".gitignore", kGitignore);
    write_file(root / ".gitattributes", kGitattributes);
    write_file(root / "RESOURCES.md", kResourcesReadme);
    std::error_code error;
    fs::create_directories(disk_path(root, layout.src), error);
    if (error) {
        fail("cannot create " + utf8(disk_path(root, layout.src)) + ": " + error.message());
    }
}

Project Project::create(const fs::path& root) {
    auto owned = std::make_unique<Game>();
    DataModel& game = *owned;
    Project project = create(root, game);
    project.bind(nullptr, std::move(owned));
    return project;
}

Project Project::create(const fs::path& root, DataModel& into) {
    if (!missing_or_empty_dir(root)) {
        fail(utf8(root) + " is not empty");
    }
    Project project;
    project.bind(&into, nullptr);
    project.root_ = root;
    project.name_ = project_name_for(root);
    {
        Rebuild rebuild(into);
        clear_world(into);
        into.set_name(0, project.name_);
        project.write_skeleton(root);
        project.save_tree(true);
        rebuild.finish();
    }
    return project;
}

Project Project::adopt(const fs::path& root, DataModel& game) {
    if (!missing_or_empty_dir(root)) {
        fail(utf8(root) + " is not empty");
    }
    Project project;
    project.bind(&game, nullptr);
    project.root_ = root;
    project.name_ = project_name_for(root);
    project.write_skeleton(root);
    project.save_tree(true);
    return project;
}

Project Project::load(const fs::path& root) {
    auto owned = std::make_unique<Game>();
    Project project;
    project.bind(nullptr, std::move(owned));
    project.root_ = root;
    Layout layout;
    read_project_json(root, project.name_, layout);
    const std::vector<PlanNode> plan = PlanReader(root, layout).read();
    std::vector<InstanceId> ids;
    {
        Rebuild rebuild(*project.game_);
        ids = build(*project.game_, plan);
        rebuild.finish();
    }
    const std::vector<std::size_t> parents = parents_of(plan);
    for (std::size_t index = 0; index < plan.size(); ++index) {
        const PlanNode& node = plan[index];
        project.files_[node.guid] = from_disk(node, index == 0 ? std::string() : plan[parents[index]].guid);
        project.id_guid_[ids[index]] = node.guid;
        project.guid_id_[node.guid] = ids[index];
    }
    return project;
}

Project Project::load(const fs::path& root, DataModel& into) {
    Project project;
    project.bind(&into, nullptr);
    project.root_ = root;
    Layout layout;
    read_project_json(root, project.name_, layout);
    const std::vector<PlanNode> plan = PlanReader(root, layout).read();
    {
        // A class-level error (a bad Color) must not leave `into` half rebuilt.
        Game scratch;
        scratch.history().set_enabled(false);
        build(scratch, plan);
    }
    std::vector<InstanceId> ids;
    {
        Rebuild rebuild(into);
        clear_world(into);
        ids = build(into, plan);
        rebuild.finish();
    }
    const std::vector<std::size_t> parents = parents_of(plan);
    for (std::size_t index = 0; index < plan.size(); ++index) {
        const PlanNode& node = plan[index];
        project.files_[node.guid] = from_disk(node, index == 0 ? std::string() : plan[parents[index]].guid);
        project.id_guid_[ids[index]] = node.guid;
        project.guid_id_[node.guid] = ids[index];
    }
    return project;
}

void Project::save(const std::vector<SaveConflict>& overwrite) { save_tree(false, overwrite); }

void Project::save_as(const fs::path& root) {
    if (!missing_or_empty_dir(root)) {
        fail(utf8(root) + " is not empty");
    }
    Layout old_layout;
    std::string ignored;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        read_project_json(root_, ignored, old_layout);
    }
    write_skeleton(root);
    // Instances name resources by relative path. Copy them so the paths still resolve.
    const fs::path from = disk_path(root_, old_layout.resources);
    if (fs::is_directory(from, error)) {
        fs::copy(from, disk_path(root, Layout{}.resources),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
        if (error) {
            fail("cannot copy " + utf8(from) + ": " + error.message());
        }
    }
    root_ = root;
    files_.clear();
    save_tree(true);
}

std::map<std::string, Project::Files> Project::plan_files(const std::vector<AuthoredNode>& tree,
                                                          const std::string& src,
                                                          const std::unordered_map<std::string, Files>& cache) {
    // Paths. Every name carries the GUID, so two instances never want one path.
    std::vector<std::string> dirs(tree.size());
    std::map<std::string, Files> next;
    if (tree.empty()) {
        return next;
    }
    const std::vector<std::size_t> parents = parents_of(tree);
    dirs[0] = src;
    for (std::size_t index = 0; index < tree.size(); ++index) {
        const AuthoredNode& node = tree[index];
        Files files;
        if (index == 0) {
            files.props_path = join(src, "init.json");
            for (std::size_t child : node.children) {
                dirs[child] = src;
            }
        } else {
            // Parents come before children in the tree, so dirs[parent] is set.
            const std::string& dir = dirs[index];
            const std::string stem = sanitize_file_name(node.name) + "." + node.guid;
            const bool folder = !node.children.empty();
            files.has_source = node.has_source;
            if (folder) {
                const std::string own = join(dir, stem);
                files.props_path = join(own, node.has_source ? "init.meta.json" : "init.json");
                files.source_path = node.has_source ? join(own, "init.luau") : std::string();
                for (std::size_t child : node.children) {
                    dirs[child] = own;
                }
            } else {
                files.props_path = join(dir, stem + (node.has_source ? ".meta.json" : ".json"));
                files.source_path = node.has_source ? join(dir, stem + ".luau") : std::string();
            }
        }
        files.parent = index == 0 ? std::string() : tree[parents[index]].guid;
        if (node.has_properties) {
            files.props = instance_json(node, tree);
            files.props_bytes = instance_bytes(files.props, node);
            files.source_bytes = node.source;
        } else {
            const Files& known = cache.at(node.guid);
            files.props = known.props;
            files.props_bytes = known.props_bytes;
            files.source_bytes = known.source_bytes;
        }
        next.emplace(node.guid, std::move(files));
    }
    return next;
}

std::uint64_t Project::place_fingerprint(const DataModel& game) {
    // FNV-1a over each path and its bytes, in GUID order.
    std::uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](const std::string& text) {
        for (const char c : text) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ull;
        }
        // A separator, so "ab"+"c" and "a"+"bc" differ.
        hash ^= 0xffu;
        hash *= 1099511628211ull;
    };
    const std::vector<AuthoredNode> tree = game.authored_tree(nullptr);
    try {
        const std::map<std::string, Files> files = plan_files(tree, Layout{}.src, {});
        for (const auto& [guid, entry] : files) {
            mix(guid);
            mix(entry.props_path);
            mix(entry.props_bytes);
            mix(entry.source_path);
            mix(entry.source_bytes);
        }
    } catch (const ProjectError& error) {
        // A place that cannot be written is never "saved".
        mix(error.what());
    }
    return hash;
}

void Project::reset_place(DataModel& game) {
    Rebuild rebuild(game);
    clear_world(game);
    game.set_name(0, game.class_name());
    game.set_guid(0, make_guid());
    rebuild.finish();
}

std::vector<SaveConflict> Project::outside_changes(const std::vector<AuthoredNode>& tree,
                                                   const std::map<std::string, Files>& next,
                                                   const std::map<std::string, std::vector<std::string>>& claims,
                                                   std::set<std::string>& left_gone) const {
    const std::vector<std::size_t> parent = parents_of(tree);
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t index = 0; index < tree.size(); ++index) {
        index_of.emplace(tree[index].guid, index);
    }
    // A row names its instance as the explorers do: its Name, and where it sits.
    auto row = [&](const std::string& guid, const std::string& path, SaveConflict::Kind kind) {
        SaveConflict out;
        out.guid = guid;
        out.path = path;
        out.kind = kind;
        if (const auto live = index_of.find(guid); live != index_of.end()) {
            out.name = tree[live->second].name;
            out.where = live->second == 0 ? std::string() : path_of(tree, parent, parent[live->second]);
        } else if (const auto base = files_.find(guid); base != files_.end()) {
            if (const JsonValue* name = base->second.props.find("Name"); name != nullptr && name->is_string()) {
                out.name = name->as_string();
            }
        }
        return out;
    };
    // Every folder above an instance the save writes or moves counts as changed
    // too: without its own file, that folder would not load. So one gone from
    // disk is a conflict rather than left gone.
    std::set<std::string> holds;
    for (std::size_t index = 1; index < tree.size(); ++index) {
        const auto base = files_.find(tree[index].guid);
        const Files& planned = next.at(tree[index].guid);
        const bool places = base == files_.end() || planned.props_path != base->second.props_path ||
                            planned.source_path != base->second.source_path ||
                            planned.has_source != base->second.has_source ||
                            planned.props_bytes != base->second.props_bytes ||
                            planned.source_bytes != base->second.source_bytes;
        for (std::size_t up = parent[index]; places; up = parent[up]) {
            if (!holds.insert(tree[up].guid).second || up == 0) {
                break;
            }
        }
    }
    // Gone from its own paths, and claimed by a file somewhere else.
    auto moved = [&claims](const std::string& guid, const Files& base) {
        const auto claimed = claims.find(guid);
        if (claimed == claims.end()) {
            return false;
        }
        return std::any_of(claimed->second.begin(), claimed->second.end(), [&base](const std::string& path) {
            return path != base.props_path && path != base.source_path;
        });
    };

    std::vector<SaveConflict> conflicts;
    // Left gone: GUID -> its first missing file.
    std::map<std::string, std::string> gone_paths;
    std::error_code error;
    for (const auto& [guid, base] : files_) {
        const auto planned = next.find(guid);
        const bool removed = planned == next.end();
        // A file's bytes matter only where the save rewrites or deletes it. A
        // move alone carries whatever is on disk to the new path.
        struct Own {
            const std::string* path;
            const std::string* bytes;
            bool rewritten;
        };
        std::vector<Own> own{{&base.props_path, &base.props_bytes,
                              removed || planned->second.props_bytes != base.props_bytes}};
        if (base.has_source) {
            own.push_back({&base.source_path, &base.source_bytes,
                           removed || !planned->second.has_source ||
                               planned->second.source_bytes != base.source_bytes});
        }
        const bool touched = removed || holds.count(guid) != 0 || planned->second.props_path != base.props_path ||
                             planned->second.source_path != base.source_path ||
                             planned->second.has_source != base.has_source ||
                             std::any_of(own.begin(), own.end(), [](const Own& file) { return file.rewritten; });

        const std::string* gone = nullptr;
        bool some_left = false;
        for (const Own& file : own) {
            if (fs::exists(disk_path(root_, *file.path), error)) {
                some_left = true;
            } else if (gone == nullptr) {
                gone = file.path;
            }
        }
        if (gone != nullptr) {
            // A folder's own directory, still there with its children, is part of it.
            const std::string folder = own_folder(base.props_path);
            some_left = some_left || (!folder.empty() && fs::is_directory(disk_path(root_, folder), error));
            if (!touched && some_left) {
                // A lone .luau or .meta.json, or a folder without its init file,
                // does not load. The save writes the missing part back.
                continue;
            }
            if (!touched) {
                // The studio left it alone, so the save leaves it gone.
                left_gone.insert(guid);
                gone_paths.emplace(guid, *gone);
                continue;
            }
            const bool elsewhere = moved(guid, base);
            if (removed && !elsewhere) {
                continue;  // Deleted on both sides.
            }
            conflicts.push_back(
                row(guid, *gone, elsewhere ? SaveConflict::Kind::MovedOutside : SaveConflict::Kind::DeletedOutside));
            continue;
        }
        // What the save rewrites or deletes, against the disk: the properties
        // key by key, so formatting alone is no change, and a script's source
        // whole. An instance the studio deleted is one row for all of it.
        std::vector<SaveConflict> edited;
        auto whole = [&, id = guid](const std::string& path) {
            SaveConflict out = row(id, path, SaveConflict::Kind::EditedOutside);
            out.studio = "deleted";
            out.disk = "changed on disk";
            return out;
        };
        if (own[0].rewritten) {
            const std::string bytes = read_file(disk_path(root_, base.props_path));
            JsonValue disk;
            std::string error;
            if (bytes == base.props_bytes) {
                // Unchanged on disk.
            } else if (!parse_json(bytes, disk, error) || !disk.is_object()) {
                SaveConflict unreadable = row(guid, base.props_path, SaveConflict::Kind::EditedOutside);
                unreadable.disk = "can't be read";
                edited.push_back(std::move(unreadable));
            } else {
                const JsonValue none = JsonValue::object();
                const JsonValue& mine = removed ? none : planned->second.props;
                for (const KeyMerge& merged : merge_keys(base.props, disk, mine)) {
                    // The save would write over a value that changed on disk.
                    if (merged.change != KeyChange::DiskOnly && merged.change != KeyChange::Conflict) {
                        continue;
                    }
                    if (removed) {
                        edited.push_back(whole(base.props_path));
                        break;
                    }
                    SaveConflict key = row(guid, base.props_path, SaveConflict::Kind::EditedOutside);
                    key.key = merged.key;
                    key.studio = display_value(mine.find(merged.key));
                    key.disk = display_value(disk.find(merged.key));
                    edited.push_back(std::move(key));
                }
            }
        }
        if (own.size() > 1 && own[1].rewritten) {
            const std::string text = read_file(disk_path(root_, base.source_path));
            if (text != base.source_bytes && (removed || text != planned->second.source_bytes)) {
                if (removed) {
                    if (edited.empty()) {
                        edited.push_back(whole(base.props_path));
                    }
                } else {
                    SaveConflict source = row(guid, base.source_path, SaveConflict::Kind::EditedOutside);
                    source.key = "Source";
                    std::tie(source.studio, source.disk) = differing_lines(planned->second.source_bytes, text);
                    edited.push_back(std::move(source));
                }
            }
        }
        conflicts.insert(conflicts.end(), edited.begin(), edited.end());
    }

    // An instance left gone inside a folder that is itself deleted or moved on
    // disk is listed with that folder. Writing the folder back without it would
    // strand it where the folder moved, or drop it.
    std::set<std::string> gone_folders;
    for (const SaveConflict& conflict : conflicts) {
        if (conflict.kind == SaveConflict::Kind::DeletedOutside || conflict.kind == SaveConflict::Kind::MovedOutside) {
            gone_folders.insert(conflict.guid);
        }
    }
    for (std::size_t index = 1; index < tree.size(); ++index) {
        const std::string& guid = tree[index].guid;
        const auto gone = gone_paths.find(guid);
        if (gone == gone_paths.end() || gone_folders.count(tree[parent[index]].guid) == 0) {
            continue;
        }
        conflicts.push_back(row(guid, gone->second,
                                moved(guid, files_.at(guid)) ? SaveConflict::Kind::MovedOutside
                                                             : SaveConflict::Kind::DeletedOutside));
        gone_folders.insert(guid);
        left_gone.erase(guid);
    }

    // A folder the save moves or deletes must not strand a file it does not
    // know: an instance added in it on disk would be left without the folder's
    // init file.
    std::set<std::pair<std::string, std::string>> added;
    for (const auto& [guid, base] : files_) {
        const std::string folder = own_folder(base.props_path);
        const auto planned = next.find(guid);
        if (folder.empty() || (planned != next.end() && planned->second.props_path == base.props_path)) {
            continue;
        }
        const std::string prefix = folder + "/";
        for (const auto& [claimed, paths] : claims) {
            if (files_.count(claimed) != 0) {
                continue;
            }
            for (const std::string& path : paths) {
                if (path.compare(0, prefix.size(), prefix) == 0 && added.emplace(claimed, path).second) {
                    conflicts.push_back(row(claimed, path, SaveConflict::Kind::AddedOutside));
                }
            }
        }
    }
    std::sort(conflicts.begin(), conflicts.end(), [](const SaveConflict& a, const SaveConflict& b) {
        return std::tie(a.path, a.key) < std::tie(b.path, b.key);
    });
    return conflicts;
}

struct Project::Comparison {
    struct Action {
        enum class Type { Create, Restore, Recreate, Set, Destroy };
        Type type = Type::Set;
        std::string guid;
        // Set: the key, "Parent", "Source", or "children".
        std::string key;
    };
    std::vector<detail::PlanNode> plan;
    std::vector<std::size_t> plan_parents;
    std::unordered_map<std::string, std::size_t> on_disk;
    std::vector<AuthoredNode> tree;
    std::vector<std::size_t> tree_parents;
    std::unordered_map<std::string, std::size_t> in_studio;
    // What only the disk changed, in the order to apply it.
    std::vector<Action> actions;
    std::vector<SaveConflict> rows;

    std::string disk_parent(std::size_t index) const {
        return index == 0 ? std::string() : plan[plan_parents[index]].guid;
    }
    std::string studio_parent(std::size_t index) const {
        return index == 0 ? std::string() : tree[tree_parents[index]].guid;
    }
};

Project::Comparison Project::compare_disk() const {
    using Type = Comparison::Action::Type;
    Layout layout;
    std::string ignored;
    read_project_json(root_, ignored, layout);
    Comparison out;
    out.plan = PlanReader(root_, layout).read();
    {
        // A value a class rejects, such as a Color that is not numbers, stops
        // the scan here rather than halfway through an apply.
        Game scratch;
        scratch.history().set_enabled(false);
        build(scratch, out.plan);
    }
    out.plan_parents = parents_of(out.plan);
    for (std::size_t index = 0; index < out.plan.size(); ++index) {
        out.on_disk.emplace(out.plan[index].guid, index);
    }
    out.tree = game_->authored_tree(nullptr);
    out.tree_parents = parents_of(out.tree);
    for (std::size_t index = 0; index < out.tree.size(); ++index) {
        out.in_studio.emplace(out.tree[index].guid, index);
    }

    auto base_json = [&](const Files& base) {
        return compared_json(base.props, base.parent, base.parent.empty(), base.has_source, base.source_bytes);
    };
    auto disk_json = [&](std::size_t index) {
        const PlanNode& node = out.plan[index];
        return compared_json(node.doc, out.disk_parent(index), index == 0, node.has_source, node.source);
    };
    auto studio_json = [&](std::size_t index) {
        const AuthoredNode& node = out.tree[index];
        return compared_json(instance_json(node, out.tree), out.studio_parent(index), index == 0, node.has_source,
                             node.source);
    };
    // The studio changed index, or anything under it, since the base.
    std::function<bool(std::size_t)> studio_below = [&](std::size_t index) {
        const AuthoredNode& node = out.tree[index];
        const auto base = files_.find(node.guid);
        if (base == files_.end() || differs(base_json(base->second), studio_json(index))) {
            return true;
        }
        return std::any_of(node.children.begin(), node.children.end(), studio_below);
    };
    // The disk changed index, or added or changed anything under it.
    std::function<bool(std::size_t)> disk_below = [&](std::size_t index) {
        const PlanNode& node = out.plan[index];
        const auto base = files_.find(node.guid);
        if (base == files_.end() || differs(base_json(base->second), disk_json(index))) {
            return true;
        }
        return std::any_of(node.children.begin(), node.children.end(), disk_below);
    };
    auto add_row = [&](const std::string& guid, const std::string& path, SaveConflict::Kind kind, std::string key,
                       std::string studio, std::string disk) {
        SaveConflict row;
        row.guid = guid;
        row.path = path;
        row.kind = kind;
        row.key = std::move(key);
        row.studio = std::move(studio);
        row.disk = std::move(disk);
        if (const auto live = out.in_studio.find(guid); live != out.in_studio.end()) {
            row.name = out.tree[live->second].name;
            row.where = live->second == 0 ? std::string() : path_of(out.tree, out.tree_parents, out.tree_parents[live->second]);
        } else if (const auto file = out.on_disk.find(guid); file != out.on_disk.end()) {
            row.name = out.plan[file->second].name;
            row.where = file->second == 0 ? std::string() : path_of(out.plan, out.plan_parents, out.plan_parents[file->second]);
        }
        out.rows.push_back(std::move(row));
    };
    // A parent's GUID as the path of that parent on one side.
    auto parent_text = [&](bool studio_side, const std::string& guid) {
        if (studio_side) {
            const auto found = out.in_studio.find(guid);
            return found == out.in_studio.end() ? guid : path_of(out.tree, out.tree_parents, found->second);
        }
        const auto found = out.on_disk.find(guid);
        return found == out.on_disk.end() ? guid : path_of(out.plan, out.plan_parents, found->second);
    };
    auto key_row = [&](const std::string& guid, const std::string& path, const std::string& key,
                       const JsonValue& mine, const JsonValue& disk) {
        const JsonValue* studio = mine.find(key);
        const JsonValue* theirs = disk.find(key);
        std::string studio_text;
        std::string disk_text;
        if (key == "Parent") {
            studio_text = parent_text(true, studio != nullptr ? studio->as_string() : std::string());
            disk_text = parent_text(false, theirs != nullptr ? theirs->as_string() : std::string());
        } else if (key == "Source") {
            std::tie(studio_text, disk_text) = differing_lines(studio != nullptr ? studio->as_string() : std::string(),
                                                               theirs != nullptr ? theirs->as_string() : std::string());
        } else {
            studio_text = display_value(studio);
            disk_text = display_value(theirs);
        }
        add_row(guid, path, SaveConflict::Kind::EditedOutside, key, studio_text, disk_text);
    };
    auto act = [&](Type type, const std::string& guid, std::string key = {}) {
        out.actions.push_back({type, guid, std::move(key)});
    };

    // Everything on disk, parents before children.
    std::vector<bool> creatable(out.plan.size(), false);
    for (std::size_t index = 0; index < out.plan.size(); ++index) {
        const PlanNode& node = out.plan[index];
        const auto base = files_.find(node.guid);
        const auto studio = out.in_studio.find(node.guid);
        const bool in_base = base != files_.end();
        const bool live = studio != out.in_studio.end();
        if (!in_base && !live) {
            // Added on disk: made under its disk parent when that parent is in
            // the studio, or is made too. Under one the studio deleted, that
            // parent's row carries it.
            const std::string parent = out.disk_parent(index);
            creatable[index] = out.in_studio.count(parent) != 0 ||
                               (files_.count(parent) == 0 && creatable[out.plan_parents[index]]);
            if (creatable[index]) {
                act(Type::Create, node.guid);
            }
            continue;
        }
        if (!live) {
            // The studio deleted it. A save removes it, unless the disk changed
            // it or added under it.
            if (disk_below(index)) {
                add_row(node.guid, base->second.props_path, SaveConflict::Kind::EditedOutside, "", "deleted",
                        "changed on disk");
            }
            continue;
        }
        const JsonValue disk = disk_json(index);
        const JsonValue mine = studio_json(studio->second);
        const JsonValue was = in_base ? base_json(base->second) : JsonValue::object();
        const std::string path = in_base ? base->second.props_path : node.props_path;
        // A new class is a new instance: taken whole, or kept whole.
        if (index != 0 && in_base && !same_value(was.find("class"), disk.find("class"))) {
            if (differs(was, mine)) {
                add_row(node.guid, path, SaveConflict::Kind::EditedOutside, "class", display_value(mine.find("class")),
                        display_value(disk.find("class")));
            } else {
                act(Type::Recreate, node.guid);
            }
            continue;
        }
        for (const KeyMerge& merged : merge_keys(was, disk, mine)) {
            if (merged.key == "class") {
                continue;
            }
            if (merged.change == KeyChange::DiskOnly) {
                act(Type::Set, node.guid, merged.key);
            } else if (merged.change == KeyChange::Conflict) {
                key_row(node.guid, path, merged.key, mine, disk);
            }
        }
    }
    // Gone from disk.
    for (const auto& [guid, files] : files_) {
        if (out.on_disk.count(guid) != 0) {
            continue;
        }
        const auto studio = out.in_studio.find(guid);
        if (studio == out.in_studio.end()) {
            continue;  // Deleted on both sides.
        }
        if (studio_below(studio->second)) {
            add_row(guid, files.props_path, SaveConflict::Kind::DeletedOutside, "", "changed in the studio", "deleted");
        } else {
            act(Type::Destroy, guid);
        }
    }
    std::sort(out.rows.begin(), out.rows.end(), [](const SaveConflict& a, const SaveConflict& b) {
        return std::tie(a.where, a.name, a.guid, a.key) < std::tie(b.where, b.name, b.guid, b.key);
    });
    return out;
}

DiskScan Project::scan_disk() const {
    Comparison compared = compare_disk();
    DiskScan out;
    out.conflicts = std::move(compared.rows);
    out.has_disk_changes = !compared.actions.empty();
    return out;
}

void Project::save_tree(bool full, const std::vector<SaveConflict>& overwrite) {
    DataModel& world = *game_;
    const bool playing = world.simulation_running();
    const AuthoredDirty dirty = world.authored_dirty();
    const bool everything = full || playing || dirty.all;
    const std::unordered_set<InstanceId> dirty_ids(dirty.ids.begin(), dirty.ids.end());
    auto want = [&](InstanceId id) {
        if (everything || dirty_ids.count(id) != 0) {
            return true;
        }
        const auto known = id_guid_.find(id);
        return known == id_guid_.end() || known->second != world.guid(id) || files_.count(known->second) == 0;
    };
    std::vector<AuthoredNode> tree = world.authored_tree(want);

    Layout layout;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        std::string ignored;
        read_project_json(root_, ignored, layout);
    }

    // Validate before touching disk.
    std::unordered_map<std::string, std::size_t> by_guid;
    for (std::size_t index = 0; index < tree.size(); ++index) {
        const AuthoredNode& node = tree[index];
        if (!valid_guid(node.guid)) {
            fail("instance \"" + node.name + "\" has no valid GUID");
        }
        if (!by_guid.emplace(node.guid, index).second) {
            fail("GUID " + node.guid + " is used by two instances");
        }
        const bool known = node.id == 0 ? node.class_name == "Game" : project_class_known(node.class_name);
        if (!known) {
            fail("instance " + node.guid + " (" + node.name + ") has unknown class " + node.class_name);
        }
        if (!node.has_properties && files_.count(node.guid) == 0) {
            fail("instance " + node.guid + " has no saved bytes");
        }
    }

    std::map<std::string, Files> next = plan_files(tree, layout.src, files_);

    // A file changed on disk since the last load or save stops a guarded save
    // here, before anything is written.
    const std::map<std::string, std::vector<std::string>> claims = guid_claims(root_, layout.src);
    std::set<std::string> left_gone;
    std::vector<SaveConflict> conflicts = outside_changes(tree, next, claims, left_gone);
    // Only the conflicts overwrite lists are written over. Any other, as a file
    // that changed after that list was made, stops the save.
    const bool listed = std::all_of(conflicts.begin(), conflicts.end(), [&overwrite](const SaveConflict& conflict) {
        return std::find(overwrite.begin(), overwrite.end(), conflict) != overwrite.end();
    });
    if (!listed) {
        throw ProjectConflict(std::move(conflicts));
    }

    SaveReport report;
    std::set<std::string> vacated;
    auto note_vacated = [&vacated](const std::string& path) {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            vacated.insert(path.substr(0, slash));
        }
    };
    auto place = [&](const std::string& old_path, const std::string& old_bytes, bool had, const std::string& path,
                     const std::string& bytes) {
        const fs::path target = disk_path(root_, path);
        bool present = false;
        if (had && old_path != path && fs::exists(disk_path(root_, old_path), error)) {
            move_file(disk_path(root_, old_path), target);
            report.moved.push_back(old_path + " -> " + path);
            note_vacated(old_path);
            present = true;
        } else {
            present = had && fs::exists(target, error);
        }
        if (!present || old_bytes != bytes) {
            write_file(target, bytes);
            report.written.push_back(path);
        }
    };
    for (const auto& [guid, files] : next) {
        if (left_gone.count(guid) != 0) {
            // Gone from disk while the studio left it alone: the save leaves it gone.
            continue;
        }
        const auto old = files_.find(guid);
        const bool had = old != files_.end();
        place(had ? old->second.props_path : std::string(), had ? old->second.props_bytes : std::string(), had,
              files.props_path, files.props_bytes);
        if (files.has_source) {
            const bool had_source = had && old->second.has_source;
            place(had_source ? old->second.source_path : std::string(),
                  had_source ? old->second.source_bytes : std::string(), had_source, files.source_path,
                  files.source_bytes);
        }
    }
    // Overwrite: a GUID the save wrote over a conflict keeps only the files it
    // wrote. Any other file claiming it, as one moved outside, would load as a
    // second instance with the same GUID.
    for (const SaveConflict& conflict : conflicts) {
        const auto claimed = claims.find(conflict.guid);
        if (claimed == claims.end()) {
            continue;
        }
        const auto planned = next.find(conflict.guid);
        // The same file, by name or by the file system: a name that differs
        // only in case is one file on macOS and Windows.
        auto wrote = [&](const fs::path& target, const std::string& path) {
            return !path.empty() && fs::equivalent(target, disk_path(root_, path), error);
        };
        for (const std::string& path : claimed->second) {
            const fs::path target = disk_path(root_, path);
            if (planned != next.end() && (path == planned->second.props_path || path == planned->second.source_path ||
                                          wrote(target, planned->second.props_path) ||
                                          wrote(target, planned->second.source_path))) {
                continue;
            }
            if (fs::exists(target, error)) {
                remove_file(target);
                report.removed.push_back(path);
                note_vacated(path);
            }
        }
    }
    for (const auto& [guid, files] : files_) {
        if (next.count(guid) != 0) {
            continue;
        }
        for (const std::string* path : {&files.props_path, &files.source_path}) {
            if (path->empty()) {
                continue;
            }
            const fs::path target = disk_path(root_, *path);
            if (fs::exists(target, error)) {
                remove_file(target);
                report.removed.push_back(*path);
            }
            note_vacated(*path);
        }
    }
    // Folders emptied by a move or a delete go too, up to src/.
    for (auto it = vacated.rbegin(); it != vacated.rend(); ++it) {
        std::string dir = *it;
        while (!dir.empty() && dir != layout.src && dir.size() > layout.src.size()) {
            const fs::path path = disk_path(root_, dir);
            if (!fs::is_directory(path, error) || !fs::is_empty(path, error)) {
                break;
            }
            fs::remove(path, error);
            const std::size_t slash = dir.rfind('/');
            if (slash == std::string::npos) {
                break;
            }
            dir = dir.substr(0, slash);
        }
    }

    files_ = std::unordered_map<std::string, Files>(std::make_move_iterator(next.begin()),
                                                   std::make_move_iterator(next.end()));
    id_guid_.clear();
    guid_id_.clear();
    for (const AuthoredNode& node : tree) {
        id_guid_[node.id] = node.guid;
        guid_id_[node.guid] = node.id;
    }
    // Play leaves the edit dirty set alone. Stop marks everything anyway.
    if (!playing) {
        world.clear_authored_dirty();
    }
    std::sort(report.written.begin(), report.written.end());
    std::sort(report.moved.begin(), report.moved.end());
    std::sort(report.removed.begin(), report.removed.end());
    last_save_ = std::move(report);
}

}  // namespace engine_core
