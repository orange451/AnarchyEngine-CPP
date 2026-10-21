#include "Project.hpp"
#include "ProjectPack.hpp"

#include "Environment.hpp"

#include "AmbientOcclusionEffect.hpp"
#include "AssetInstances.hpp"
#include "Assemblies.hpp"
#include "Attachment.hpp"
#include "Constraint.hpp"
#include "JointConstraint.hpp"
#include "Animator.hpp"
#include "Bone.hpp"
#include "Atmosphere.hpp"
#include "BloomEffect.hpp"
#include "ShaderEffect.hpp"
#include "Camera.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "DynamicSky.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "MeshEditors.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "GameService.hpp"
#include "FileBytes.hpp"
#include "JsonMerge.hpp"
#include "Light.hpp"
#include "ModuleScript.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "SceneService.hpp"
#include "ScreenSpaceReflections.hpp"
#include "SelectionService.hpp"
#include "Skybox.hpp"
#include "ParticleEmitter.hpp"
#include "SoundEmitter.hpp"
#include "Script.hpp"
#include "Shader.hpp"
#include "Brush.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "ValueBase.hpp"
#include "RemoteEvent.hpp"
#include "terrain/TerrainStash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <exception>
#include <mutex>
#include <thread>
#include <tuple>
#include <string_view>
#include <unordered_map>
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
    // Baked textures: made again from resources/ whenever they are missing.
    ".cache/\n"
    "*.tmp\n"
    ".DS_Store\n";

constexpr const char* kGitattributes =
    "* text=auto eol=lf\n"
    "*.luau text eol=lf\n"
    "*.glsl text eol=lf\n"
    "*.json text eol=lf\n"
    "resources/textures/** binary\n"
    "resources/meshes/** binary\n"
    "resources/audio/** binary\n";

// The extensions a SourceText keeps its text in, without the dot: a Script's
// .luau and a Shader's .glsl (SourceText::source_extension).
constexpr const char* kSourceExtensions[] = {"luau", "glsl"};
// The extensions of stages past the first (SourceText::stage_count): a
// Shader's vertex text. Matched before kSourceExtensions, as ".vert.glsl"
// ends in ".glsl" too.
constexpr const char* kExtraExtensions[] = {"vert.glsl"};

// The extra stage's extension name ends in, as ".vert.glsl", or null.
const char* extra_suffix_of(std::string_view name) {
    static const std::string suffixes[] = {".vert.glsl"};
    for (const std::string& suffix : suffixes) {
        if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
            return suffix.c_str();
        }
    }
    return nullptr;
}

// The source extension name ends in, as ".luau", or null; an extra stage's
// file is not a source file.
const char* source_suffix_of(std::string_view name) {
    static const std::string suffixes[] = {".luau", ".glsl"};
    if (extra_suffix_of(name) != nullptr) {
        return nullptr;
    }
    for (const std::string& suffix : suffixes) {
        if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
            return suffix.c_str();
        }
    }
    return nullptr;
}

// Whether name is a folder instance's source file: init.luau or init.glsl.
bool init_source(std::string_view name) {
    for (const char* extension : kSourceExtensions) {
        if (name == std::string("init.") + extension) {
            return true;
        }
    }
    return false;
}

// Whether name is a folder instance's extra stage file: init.vert.glsl.
bool init_extra(std::string_view name) {
    for (const char* extension : kExtraExtensions) {
        if (name == std::string("init.") + extension) {
            return true;
        }
    }
    return false;
}

// The extra stage extension of path's file, without the dot: "vert.glsl", or "".
std::string extra_extension_of(const std::string& path) {
    const char* suffix = extra_suffix_of(path);
    return suffix != nullptr ? std::string(suffix + 1) : std::string();
}

// Whether two lists of extra stage files are the same: a stage with no file
// and no text is the same as no entry, as a load from disk lists none.
template <typename Extra>
bool same_extras(const std::vector<Extra>& a, const std::vector<Extra>& b) {
    const Extra none{};
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        if ((i < a.size() ? a[i] : none) != (i < b.size() ? b[i] : none)) {
            return false;
        }
    }
    return true;
}

// The extension of path's file, without the dot: "glsl" for ".../X.guid.glsl".
std::string extension_of(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.rfind('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string() : path.substr(dot + 1);
}

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

// A service back at its class defaults, with no extras. Its children stay.
void reset_service(DataModel& service) {
    PropertyBag defaults;
    service.default_properties(defaults);
    for (const JsonValue::Member& member : defaults) {
        std::string error;
        service.load_property(member.first, member.second, error);
    }
    std::vector<std::string> keys;
    for (const JsonValue::Member& member : service.extra_properties(service.id())) {
        keys.push_back(member.first);
    }
    for (const std::string& key : keys) {
        service.erase_extra_property(service.id(), key);
    }
}

// A world has one of each service, made with it, so the factory hands back
// that one, at its defaults, as if it were new.
template <std::size_t Index>
DataModel& existing_service(DataModel& world) {
    DataModel* service = world.instance(world.service(kServices[Index].class_name));
    if (service == nullptr) {
        contract_fail("a Game holds every service");
    }
    reset_service(*service);
    return *service;
}

static_assert(std::size(kServices) == 15, "one registry entry per service");

std::vector<ClassEntry>& class_registry() {
    static std::vector<ClassEntry> entries = [] {
        std::vector<ClassEntry> out;
        out.push_back({kServices[0].class_name, existing_service<0>});
        out.push_back({kServices[1].class_name, existing_service<1>});
        out.push_back({kServices[2].class_name, existing_service<2>});
        out.push_back({kServices[3].class_name, existing_service<3>});
        out.push_back({kServices[4].class_name, existing_service<4>});
        out.push_back({kServices[5].class_name, existing_service<5>});
        out.push_back({kServices[6].class_name, existing_service<6>});
        out.push_back({kServices[7].class_name, existing_service<7>});
        out.push_back({kServices[8].class_name, existing_service<8>});
        out.push_back({kServices[9].class_name, existing_service<9>});
        out.push_back({kServices[10].class_name, existing_service<10>});
        out.push_back({kServices[11].class_name, existing_service<11>});
        out.push_back({kServices[12].class_name, existing_service<12>});
        out.push_back({kServices[13].class_name, existing_service<13>});
        out.push_back({kServices[14].class_name, existing_service<14>});
        out.push_back({"DataModel", [](DataModel& world) -> DataModel& { return world.create(); }});
        out.push_back({"GameObject", [](DataModel& world) -> DataModel& { return world.create_game_object(); }});
        out.push_back({"Camera", [](DataModel& world) -> DataModel& { return world.create<Camera>(); }});
        out.push_back({"PointLight", [](DataModel& world) -> DataModel& { return world.create<PointLight>(); }});
        out.push_back({"SpotLight", [](DataModel& world) -> DataModel& { return world.create<SpotLight>(); }});
        out.push_back(
            {"DirectionalLight", [](DataModel& world) -> DataModel& { return world.create<DirectionalLight>(); }});
        out.push_back({"Script", [](DataModel& world) -> DataModel& { return world.create<Script>(); }});
        out.push_back({"ModuleScript", [](DataModel& world) -> DataModel& { return world.create<ModuleScript>(); }});
        out.push_back({"Folder", [](DataModel& world) -> DataModel& { return world.create<Folder>(); }});
        out.push_back(
            {"PhysicsObject", [](DataModel& world) -> DataModel& { return world.create<PhysicsObject>(); }});
        out.push_back(
            {"PlayerController", [](DataModel& world) -> DataModel& { return world.create<PlayerController>(); }});
        out.push_back(
            {"SoundEmitter", [](DataModel& world) -> DataModel& { return world.create<SoundEmitter>(); }});
        out.push_back(
            {"ParticleEmitter", [](DataModel& world) -> DataModel& { return world.create<ParticleEmitter>(); }});
        out.push_back({"Attachment", [](DataModel& world) -> DataModel& { return world.create<Attachment>(); }});
        out.push_back(
            {"WeldConstraint", [](DataModel& world) -> DataModel& { return world.create<WeldConstraint>(); }});
        out.push_back(
            {"RigidConstraint", [](DataModel& world) -> DataModel& { return world.create<RigidConstraint>(); }});
        out.push_back({"NoCollisionConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<NoCollisionConstraint>(); }});
        out.push_back({"RevoluteConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<RevoluteConstraint>(); }});
        out.push_back({"PrismaticConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<PrismaticConstraint>(); }});
        out.push_back({"DistanceConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<DistanceConstraint>(); }});
        out.push_back({"SphericalConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<SphericalConstraint>(); }});
        out.push_back({"WheelConstraint",
                       [](DataModel& world) -> DataModel& { return world.create<WheelConstraint>(); }});
        out.push_back({"Bone", [](DataModel& world) -> DataModel& { return world.create<Bone>(); }});
        out.push_back({"Animator", [](DataModel& world) -> DataModel& { return world.create<Animator>(); }});
        out.push_back({"Skybox", [](DataModel& world) -> DataModel& { return world.create<Skybox>(); }});
        out.push_back({"DynamicSky", [](DataModel& world) -> DataModel& { return world.create<DynamicSky>(); }});
        out.push_back({"BloomEffect", [](DataModel& world) -> DataModel& { return world.create<BloomEffect>(); }});
        out.push_back({"ShaderEffect", [](DataModel& world) -> DataModel& { return world.create<ShaderEffect>(); }});
        out.push_back({"Atmosphere", [](DataModel& world) -> DataModel& { return world.create<Atmosphere>(); }});
        out.push_back({"ScreenSpaceReflections",
                       [](DataModel& world) -> DataModel& { return world.create<ScreenSpaceReflections>(); }});
        out.push_back({"AmbientOcclusionEffect",
                       [](DataModel& world) -> DataModel& { return world.create<AmbientOcclusionEffect>(); }});
        out.push_back({"ScreenGui", [](DataModel& world) -> DataModel& { return world.create<ScreenGui>(); }});
        out.push_back({"BillboardGui", [](DataModel& world) -> DataModel& { return world.create<BillboardGui>(); }});
        out.push_back({"Pane", [](DataModel& world) -> DataModel& { return world.create<Pane>(); }});
        out.push_back({"ImagePane", [](DataModel& world) -> DataModel& { return world.create<ImagePane>(); }});
        out.push_back({"ViewportPane", [](DataModel& world) -> DataModel& { return world.create<ViewportPane>(); }});
        out.push_back({"HBox", [](DataModel& world) -> DataModel& { return world.create<HBox>(); }});
        out.push_back({"VBox", [](DataModel& world) -> DataModel& { return world.create<VBox>(); }});
        out.push_back({"Label", [](DataModel& world) -> DataModel& { return world.create<Label>(); }});
        out.push_back({"Button", [](DataModel& world) -> DataModel& { return world.create<Button>(); }});
        out.push_back({"TextField", [](DataModel& world) -> DataModel& { return world.create<TextField>(); }});
        out.push_back({"AssetPicker", [](DataModel& world) -> DataModel& { return world.create<AssetPicker>(); }});
        out.push_back({"Slider", [](DataModel& world) -> DataModel& { return world.create<Slider>(); }});
        out.push_back({"CSS", [](DataModel& world) -> DataModel& { return world.create<Css>(); }});
        out.push_back({"Texture", [](DataModel& world) -> DataModel& { return world.create<Texture>(); }});
        out.push_back({"Mesh", [](DataModel& world) -> DataModel& { return world.create<Mesh>(); }});
        out.push_back({"Sound", [](DataModel& world) -> DataModel& { return world.create<Sound>(); }});
        out.push_back({"Animation", [](DataModel& world) -> DataModel& { return world.create<Animation>(); }});
        out.push_back({"Material", [](DataModel& world) -> DataModel& { return world.create<Material>(); }});
        out.push_back({"Shader", [](DataModel& world) -> DataModel& { return world.create<Shader>(); }});
        out.push_back({"Model", [](DataModel& world) -> DataModel& { return world.create<Model>(); }});
        out.push_back({"LodLayer", [](DataModel& world) -> DataModel& { return world.create<LodLayer>(); }});
        out.push_back({"Prefab", [](DataModel& world) -> DataModel& { return world.create<Prefab>(); }});
        out.push_back({"Terrain", [](DataModel& world) -> DataModel& { return world.create<Terrain>(); }});
        out.push_back({"Brush", [](DataModel& world) -> DataModel& { return world.create<Brush>(); }});
        out.push_back(
            {"TerrainMaterial", [](DataModel& world) -> DataModel& { return world.create<TerrainMaterial>(); }});
        out.push_back({"IntValue", [](DataModel& world) -> DataModel& { return world.create<IntValue>(); }});
        out.push_back({"NumberValue", [](DataModel& world) -> DataModel& { return world.create<NumberValue>(); }});
        out.push_back({"StringValue", [](DataModel& world) -> DataModel& { return world.create<StringValue>(); }});
        out.push_back({"BoolValue", [](DataModel& world) -> DataModel& { return world.create<BoolValue>(); }});
        out.push_back({"ObjectValue", [](DataModel& world) -> DataModel& { return world.create<ObjectValue>(); }});
        out.push_back({"RemoteEvent", [](DataModel& world) -> DataModel& { return world.create<RemoteEvent>(); }});
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

std::string join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

bool ends_with(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[noreturn]] void fail(const std::string& message) { throw ProjectError(message); }

std::string read_file(const fs::path& path) {
    std::string bytes;
    std::string error;
    if (!engine_core::read_file(path, bytes, error)) {
        fail(error);
    }
    return bytes;
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::string error;
    if (!engine_core::write_file(path, bytes, error)) {
        fail(error);
    }
}

// A project made before the texture cache has a .gitignore without .cache/:
// adds the line, once. A project with no .gitignore is left without one, and
// a file that cannot be read or written is left as it is.
void ignore_texture_cache(const fs::path& root) {
    const fs::path path = root / ".gitignore";
    std::string text;
    std::string error;
    if (!fs::is_regular_file(path) || !engine_core::read_file(path, text, error)) {
        return;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == ".cache/" || line == ".cache" || line == "/.cache/" || line == "/.cache") {
            return;
        }
        start = end + 1;
    }
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += ".cache/\n";
    engine_core::write_file(path, text, error);
}

void move_file(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    if (error) {
        fail("cannot create " + utf8_path(to.parent_path()) + ": " + error.message());
    }
    fs::rename(from, to, error);
    if (error) {
        fail("cannot move " + utf8_path(from) + " to " + utf8_path(to) + ": " + error.message());
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
    std::string name = utf8_path(normal.filename());
    if (name.empty() || name == ".") {
        name = utf8_path(normal.parent_path().filename());
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

// Whether text is a gameId: 32 lowercase hex characters.
bool well_formed_game_id(const std::string& text) {
    if (text.size() != 32) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

JsonValue project_document(const std::string& name, const Layout& layout, const std::string& game_id,
                           const std::string& version) {
    JsonValue doc = JsonValue::object();
    doc.set("format", JsonValue::number(kFormat));
    doc.set("name", JsonValue::string(name));
    doc.set("engine", JsonValue::string(kEngine));
    doc.set("gameId", JsonValue::string(game_id));
    doc.set("version", JsonValue::string(version));
    JsonValue tree = JsonValue::object();
    tree.set("src", JsonValue::string(layout.src));
    doc.set("tree", std::move(tree));
    JsonValue resources = JsonValue::object();
    resources.set("root", JsonValue::string(layout.resources));
    doc.set("resources", std::move(resources));
    return doc;
}

// What project.json says. game_id and version are empty when the file
// lacks them or they are malformed; the load fills them in and writes back.
struct ProjectJson {
    std::string name;
    Layout layout;
    std::string game_id;
    std::string version;
    JsonValue doc;   // the parsed file, so a migration keeps keys it does not know
};

ProjectJson read_project_json(const fs::path& root) {
    const fs::path path = root / "project.json";
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        fail(utf8_path(path) + " is missing");
    }
    JsonValue doc;
    std::string message;
    if (!parse_json(read_file(path), doc, message)) {
        fail(utf8_path(path) + ": " + message);
    }
    if (!doc.is_object()) {
        fail(utf8_path(path) + ": expected an object");
    }
    const JsonValue* format = doc.find("format");
    if (format == nullptr || !format->is_number() || format->as_number() != kFormat) {
        fail(utf8_path(path) + ": format must be " + std::to_string(kFormat));
    }
    ProjectJson json;
    const JsonValue* label = doc.find("name");
    json.name = label != nullptr && label->is_string() ? label->as_string() : project_name_for(root);
    if (const JsonValue* tree = doc.find("tree")) {
        const JsonValue* src = tree->find("src");
        if (src == nullptr || !src->is_string() || !safe_relative(src->as_string())) {
            fail(utf8_path(path) + ": tree.src must be a relative directory");
        }
        json.layout.src = src->as_string();
    }
    if (const JsonValue* resources = doc.find("resources")) {
        const JsonValue* dir = resources->find("root");
        if (dir == nullptr || !dir->is_string() || !safe_relative(dir->as_string())) {
            fail(utf8_path(path) + ": resources.root must be a relative directory");
        }
        json.layout.resources = dir->as_string();
    }
    if (const JsonValue* id = doc.find("gameId"); id != nullptr && id->is_string() && well_formed_game_id(id->as_string())) {
        json.game_id = id->as_string();
    }
    if (const JsonValue* version = doc.find("version"); version != nullptr && version->is_string() && !version->as_string().empty()) {
        json.version = version->as_string();
    }
    json.doc = std::move(doc);
    return json;
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
    // Extra stage files found beside the source, each its path and bytes.
    struct Extra {
        std::string path;
        std::string bytes;
    };
    std::vector<Extra> extras;
    std::vector<std::size_t> children;
    // The properties file as parsed.
    JsonValue doc;
    // A scene service the files lack, made by the read at its defaults. It has
    // no file until the next save writes one.
    bool made = false;
};

// One file as a load's read listed it: the stamp a disk check compares.
struct PlanReaderNote {
    std::uintmax_t size = 0;
    fs::file_time_type written;
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

// Calls work(i) for every i in [0, count): on this thread and up to
// hardware_concurrency - 1 others, each taking the next i. work must not throw.
template <class Work>
void parallel_for(std::size_t count, const Work& work) {
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
            work(i);
        }
    };
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t helpers = std::min<std::size_t>(hardware, count) - (count > 0 ? 1 : 0);
    std::vector<std::thread> threads;
    threads.reserve(helpers);
    for (std::size_t t = 0; t < helpers; ++t) {
        try {
            threads.emplace_back(run);
        } catch (const std::system_error&) {
            break;  // fewer threads: the ones started, and this one, take every i
        }
    }
    run();
    for (std::thread& thread : threads) {
        thread.join();
    }
}

// expected_guid empty: the root, whose GUID is not in a filename.
// The root is game, which no factory makes. Its class is checked by the caller.
// cached, when given, is the file's bytes from the load pack.
void read_plan_props(const fs::path& tree_root, PlanNode& node, const std::string& path, const std::string& expected_guid,
                 bool root, const std::string_view* cached = nullptr) {
    node.props_path = path;
    node.props_bytes = cached != nullptr ? std::string(*cached) : read_file(disk_path(tree_root, path));
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

void read_plan_source(const fs::path& tree_root, PlanNode& node, const std::string& path,
                      const std::string_view* cached = nullptr) {
    node.has_source = true;
    node.source_path = path;
    node.source_bytes = cached != nullptr ? std::string(*cached) : read_file(disk_path(tree_root, path));
    node.source = node.source_bytes;
}

void read_plan_extra(const fs::path& tree_root, PlanNode& node, const std::string& path,
                     const std::string_view* cached = nullptr) {
    PlanNode::Extra extra;
    extra.path = path;
    extra.bytes = cached != nullptr ? std::string(*cached) : read_file(disk_path(tree_root, path));
    node.extras.push_back(std::move(extra));
}

// Each of node's extra files into the stage whose extension it carries; a
// stage with no file reads as empty. A file no stage takes fails the load.
void apply_extras(SourceText& text, const PlanNode& node) {
    std::vector<bool> given(static_cast<std::size_t>(std::max(text.stage_count(), 1)), false);
    for (const PlanNode::Extra& extra : node.extras) {
        const std::string extension = extra_extension_of(extra.path);
        int stage = -1;
        for (int candidate = 1; candidate < text.stage_count(); ++candidate) {
            if (extension == text.source_extension(candidate)) {
                stage = candidate;
            }
        }
        if (stage < 0) {
            throw ProjectError(extra.path + ": a " + text.class_name() + " has no ." + extension + " stage");
        }
        text.set_source(stage, extra.bytes);
        given[static_cast<std::size_t>(stage)] = true;
    }
    for (int stage = 1; stage < text.stage_count(); ++stage) {
        if (!given[static_cast<std::size_t>(stage)]) {
            text.set_source(stage, std::string());
        }
    }
}

// node's extra stage texts, stage 1 first, as compared_json takes them.
std::vector<std::string> extra_bytes_of(const PlanNode& node) {
    std::vector<std::string> out;
    for (const PlanNode::Extra& extra : node.extras) {
        out.push_back(extra.bytes);
    }
    return out;
}



class PlanReader {
public:
    PlanReader(const fs::path& root, const Layout& layout) : root_(root), layout_(layout) {}

    // As read() lists each folder, it notes each file's size and write time,
    // the stamp a disk check compares against, so a load walks the tree once.
    // A file whose note matches pack's comes from pack, not from disk.
    // racy_from: a file written since may still change with the same time.
    PlanReader(const fs::path& root, const Layout& layout, const project_pack::Pack* pack,
               fs::file_time_type racy_from)
        : root_(root), layout_(layout), pack_(pack), racy_from_(racy_from), noting_(true) {
        // The stamp names files as a walk from root does. A tree.src
        // written another way ("./src") would name them differently.
        std::string relative = utf8_path(disk_path(root_, layout_.src).lexically_relative(root_));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        noted_ok_ = relative == layout_.src;
    }

    using Noted = detail::PlanReaderNote;
    // Every file under the tree, by path relative to root with '/', as the
    // read found it; or false when a file could not be noted.
    bool noted_ok() const { return noting_ && noted_ok_; }
    const std::unordered_map<std::string, Noted>& noted() const { return noted_; }
    // How many files the read took from disk rather than the pack.
    std::size_t disk_reads() const { return disk_reads_.load(); }

    std::vector<PlanNode> read() {
        const std::string src = layout_.src;
        std::error_code error;
        if (!fs::is_directory(disk_path(root_, src), error)) {
            fail(utf8_path(disk_path(root_, src)) + " is missing");
        }
        const std::string init = join(src, "init.json");
        if (!fs::is_regular_file(disk_path(root_, init), error)) {
            fail(utf8_path(disk_path(root_, init)) + " is missing");
        }
        PlanNode root;
        read_props(root, init, std::string(), /*root*/ true);
        if (root.class_name != "Game") {
            fail(init + ": the root class must be Game");
        }
        claim(root.guid, init);
        nodes_.push_back(std::move(root));
        // The tree's shape first, reading no file: each instance's place, in
        // the order the walk finds them, and where the walk had to stop.
        // Then every file read and parsed at once, on all cores. Then each
        // instance taken in walk order, so a project with something wrong
        // fails on the first thing, as when each file was read in turn.
        jobs_.resize(1);
        try {
            scan(src, 0);
        } catch (const WalkStopped&) {
            // The instance limit: settle fails at the node that passed it.
        } catch (const ProjectError& stop) {
            stop_ = std::string(stop.what());
        }
        read_queued();
        settle();
        for (PlanNode& node : nodes_) {
            order_children(node);
        }
        adopt_services();
        check_placement();
        return std::move(nodes_);
    }

private:
    // A file's bytes from the pack, when its note matches and it is not racy.
    const std::string_view* cached(const std::string& path) {
        if (pack_ != nullptr) {
            const auto note = noted_.find(path);
            const auto packed = pack_->files.find(path);
            if (note != noted_.end() && packed != pack_->files.end() && note->second.written < racy_from_ &&
                packed->second.size == note->second.size &&
                packed->second.written == static_cast<std::int64_t>(note->second.written.time_since_epoch().count())) {
                return &packed->second.bytes;
            }
        }
        disk_reads_.fetch_add(1);
        return nullptr;
    }

    void note(const fs::directory_entry& entry, const std::string& path) {
        std::error_code error;
        Noted noted;
        noted.size = entry.file_size(error);
        if (!error) {
            noted.written = entry.last_write_time(error);
        }
        if (error) {
            noted_ok_ = false;
            return;
        }
        noted_.emplace(path, noted);
    }

    // A folder the read skips, as .git: its files are still in the stamp.
    void note_tree(const std::string& dir) {
        std::error_code error;
        for (fs::recursive_directory_iterator it(disk_path(root_, dir), error), end; it != end; it.increment(error)) {
            if (error) {
                break;
            }
            std::error_code kind;
            if (it->is_regular_file(kind)) {
                std::string name = utf8_path(it->path().lexically_relative(root_));
                std::replace(name.begin(), name.end(), '\\', '/');
                note(*it, name);
            }
        }
        if (error) {
            noted_ok_ = false;
        }
    }

    // What the walk queued for one node: its files, read by read_queued.
    struct Job {
        std::string props;
        std::string guid;
        // Its .luau, or empty for none.
        std::string source;
        // Its extra stage files (.vert.glsl), if any.
        std::vector<std::string> extras;
        // Past the instance limit: the walk stopped here.
        bool over_limit = false;
        // Why its files did not read, or empty.
        std::string error;
    };
    struct WalkStopped {};

    // A node in walk order, its files read later by read_queued.
    std::size_t queue(const std::string& props, const std::string& guid, const std::string& source,
                      std::size_t parent, std::vector<std::string> extras = {}) {
        const std::size_t index = nodes_.size();
        nodes_.emplace_back();
        nodes_[parent].children.push_back(index);
        Job& job = jobs_.emplace_back();
        job.props = props;
        job.guid = guid;
        job.source = source;
        job.extras = std::move(extras);
        // Building more would stop the studio at the world's capacity. The
        // root, nodes_[0], takes no instance slot.
        if (index > Project::instance_limit()) {
            job.over_limit = true;
            throw WalkStopped{};
        }
        return index;
    }

    void read_queued() {
        parallel_for(nodes_.size() - 1, [this](std::size_t i) {
            const std::size_t index = i + 1;
            Job& job = jobs_[index];
            try {
                read_plan_props(root_, nodes_[index], job.props, job.guid, false, cached(job.props));
                if (!job.source.empty()) {
                    read_plan_source(root_, nodes_[index], job.source, cached(job.source));
                }
                for (const std::string& extra : job.extras) {
                    read_plan_extra(root_, nodes_[index], extra, cached(extra));
                }
            } catch (const std::exception& error) {
                job.error = error.what();
            }
        });
    }

    // Each node in walk order as one read would have taken it: its files,
    // its GUID, the limit; then why the walk stopped, if it did.
    void settle() {
        for (std::size_t index = 1; index < nodes_.size(); ++index) {
            const Job& job = jobs_[index];
            if (!job.error.empty()) {
                fail(job.error);
            }
            claim(nodes_[index].guid, job.props);
            if (job.over_limit) {
                fail(layout_.src + " holds more than " + std::to_string(Project::instance_limit()) +
                     " instances, the most a place can hold");
            }
        }
        if (stop_) {
            fail(*stop_);
        }
        jobs_.clear();
    }

    void claim(const std::string& guid, const std::string& path) {
        const auto found = seen_.find(guid);
        if (found != seen_.end()) {
            fail("GUID " + guid + " is used by both " + found->second + " and " + path);
        }
        seen_.emplace(guid, path);
    }

    // expected_guid empty: the root, whose GUID is not in a filename.
    void read_props(PlanNode& node, const std::string& path, const std::string& expected_guid, bool root = false) {
        read_plan_props(root_, node, path, expected_guid, root);
    }

    void read_source(PlanNode& node, const std::string& path) { read_plan_source(root_, node, path); }

    void scan(const std::string& dir, std::size_t parent) {
        std::vector<std::pair<std::string, bool>> entries;
        std::error_code error;
        for (fs::directory_iterator it(disk_path(root_, dir), error), end; !error && it != end; it.increment(error)) {
            const std::string name = utf8_path(it->path().filename());
            std::error_code kind;
            const bool is_dir = it->is_directory(kind);
            entries.emplace_back(name, is_dir);
            if (noting_) {
                std::error_code regular;
                if (is_dir) {
                    if (!name.empty() && (name[0] == '.' || ends_with(name, ".tmp"))) {
                        note_tree(join(dir, name));
                    }
                } else if (it->is_regular_file(regular)) {
                    note(*it, join(dir, name));
                }
            }
        }
        if (error) {
            fail("cannot list " + dir + ": " + error.message());
        }
        std::sort(entries.begin(), entries.end());

        // Source files of every extension, by GUID, and the extra stage files.
        std::map<std::string, std::string> sources;
        std::map<std::string, std::vector<std::string>> extras;
        std::map<std::string, std::string> meta;
        const bool instance_folder = parent != 0 || dir != layout_.src;
        for (const auto& [name, is_dir] : entries) {
            // Dotfiles (.gitkeep, .DS_Store) and interrupted writes are not instances.
            if (name.empty() || name[0] == '.' || ends_with(name, ".tmp")) {
                continue;
            }
            const std::string path = join(dir, name);
            if (!is_dir && (name == "init.json" || name == "init.meta.json" || init_source(name) || init_extra(name))) {
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
            } else if (const char* extra = extra_suffix_of(name)) {
                if (!split_stem(name.substr(0, name.size() - std::strlen(extra)), guid)) {
                    fail(path + ": a filename must be <Name>.<guid>" + extra);
                }
                extras[guid].push_back(path);
            } else if (const char* suffix = source_suffix_of(name)) {
                if (!split_stem(name.substr(0, name.size() - std::strlen(suffix)), guid)) {
                    fail(path + ": a filename must be <Name>.<guid>" + suffix);
                }
                if (!sources.emplace(guid, path).second) {
                    fail("GUID " + guid + " has two source files in " + dir);
                }
            } else if (ends_with(name, ".json")) {
                if (!split_stem(name.substr(0, name.size() - 5), guid)) {
                    fail(path + ": a filename must be <Name>.<guid>.json");
                }
                queue(path, guid, std::string(), parent);
            }
        }
        // A script or shader is its .meta.json and its source file, paired by
        // GUID, never by Name.
        for (const auto& [guid, path] : meta) {
            const auto source = sources.find(guid);
            if (source == sources.end()) {
                fail(path + ": no .luau or .glsl file with GUID " + guid);
            }
            const auto extra = extras.find(guid);
            queue(path, guid, source->second, parent, extra != extras.end() ? extra->second : std::vector<std::string>{});
            sources.erase(source);
            if (extra != extras.end()) {
                extras.erase(extra);
            }
        }
        for (const auto& [guid, path] : sources) {
            fail(path + ": no .meta.json file with GUID " + guid);
        }
        for (const auto& [guid, paths] : extras) {
            fail(paths.front() + ": no .meta.json file with GUID " + guid);
        }
    }

    void read_folder(const std::string& dir, const std::string& guid, std::size_t parent) {
        std::error_code error;
        const bool plain = fs::is_regular_file(disk_path(root_, join(dir, "init.json")), error);
        const bool meta = fs::is_regular_file(disk_path(root_, join(dir, "init.meta.json")), error);
        std::string init;
        for (const char* extension : kSourceExtensions) {
            const std::string candidate = join(dir, std::string("init.") + extension);
            if (fs::is_regular_file(disk_path(root_, candidate), error)) {
                if (!init.empty()) {
                    fail(dir + ": has two init source files");
                }
                init = candidate;
            }
        }
        const bool source = !init.empty();
        if (plain && meta) {
            fail(dir + ": has both init.json and init.meta.json");
        }
        if (source && !meta) {
            fail(init + ": no init.meta.json");
        }
        if (meta && !source) {
            fail(join(dir, "init.meta.json") + ": no init.luau or init.glsl");
        }
        if (!plain && !meta) {
            fail(dir + ": a folder instance needs init.json");
        }
        const std::string props = join(dir, plain ? "init.json" : "init.meta.json");
        std::vector<std::string> extras;
        for (const char* extension : kExtraExtensions) {
            const std::string candidate = join(dir, std::string("init.") + extension);
            if (meta && fs::is_regular_file(disk_path(root_, candidate), error)) {
                extras.push_back(candidate);
            }
        }
        const std::size_t index = queue(props, guid, meta ? init : std::string(), parent, std::move(extras));
        scan(dir, index);
    }

    // Each node's parent index. The root's is 0.
    std::vector<std::size_t> parents() const {
        std::vector<std::size_t> parent(nodes_.size(), 0);
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            for (std::size_t child : nodes_[index].children) {
                parent[child] = index;
            }
        }
        return parent;
    }

    // game and Assets hold their services, as kServices lists them. A service
    // the files lack is made at its defaults. Each service's GUID is fixed, so
    // every read of the same files gives the same tree, and nothing is written
    // until the next save.
    void adopt_services() {
        const std::vector<std::size_t> parent = parents();
        for (std::size_t index = 1; index < nodes_.size(); ++index) {
            PlanNode& node = nodes_[index];
            if (const ServiceSpec* spec = find_service(node.class_name)) {
                const std::string guid = service_guid(node.class_name);
                const bool home = spec->parent_class == nullptr
                                      ? parent[index] == 0
                                      : parent[index] != 0 && nodes_[parent[index]].class_name == spec->parent_class;
                // ServerStorage was a child of game before it went inside
                // Storage: a nested service a file left under game is moved home.
                const bool moved_home = spec->parent_class != nullptr && parent[index] == 0;
                if ((!home && !moved_home) || node.guid != guid) {
                    fail(node.props_path + ": " + node.class_name + " must be a child of " +
                         (spec->parent_class != nullptr ? spec->parent_class : "game") + " with GUID " + guid);
                }
                // The name is fixed too. A file that says otherwise is read as the service.
                node.name = node.class_name;
                continue;
            }
            if (parent[index] == 0) {
                fail(node.props_path + ": only a scene service can be a child of game");
            }
            for (const ServiceSpec& service : kServices) {
                if (node.guid == service_guid(service.class_name)) {
                    fail(node.props_path + ": GUID " + node.guid + " is reserved for " + service.class_name);
                }
            }
        }
        // Each service, found or made, in table order under its parent, before
        // any other child that parent has.
        std::unordered_map<std::string, std::size_t> at;
        std::unordered_map<std::size_t, std::vector<std::size_t>> services_of;
        for (const ServiceSpec& spec : kServices) {
            const std::size_t holder = spec.parent_class == nullptr ? 0 : at.at(spec.parent_class);
            std::size_t found = 0;
            for (std::size_t child : nodes_[holder].children) {
                if (nodes_[child].class_name == spec.class_name) {
                    found = child;
                }
            }
            if (found == 0 && holder != 0) {
                // Left under game by an older file (see above): game's list
                // drops it below, as it keeps only game's own services.
                for (std::size_t child : nodes_[0].children) {
                    if (nodes_[child].class_name == spec.class_name) {
                        found = child;
                    }
                }
            }
            if (found == 0) {
                PlanNode made;
                made.guid = service_guid(spec.class_name);
                made.class_name = spec.class_name;
                made.name = spec.class_name;
                made.doc = JsonValue::object();
                made.doc.set("class", JsonValue::string(spec.class_name));
                made.doc.set("id", JsonValue::string(made.guid));
                made.doc.set("Name", JsonValue::string(spec.class_name));
                made.made = true;
                found = nodes_.size();
                nodes_.push_back(std::move(made));
            }
            at[spec.class_name] = found;
            services_of[holder].push_back(found);
        }
        for (auto& [holder, services] : services_of) {
            std::vector<std::size_t> children = services;
            for (std::size_t child : nodes_[holder].children) {
                if (find_service(nodes_[child].class_name) == nullptr) {
                    children.push_back(child);
                }
            }
            nodes_[holder].children = std::move(children);
        }
    }

    // Every instance where the placement rules allow it, checked before any is made.
    void check_placement() {
        const std::vector<std::size_t> parent = parents();
        for (std::size_t index = 1; index < nodes_.size(); ++index) {
            const PlanNode& node = nodes_[index];
            if (find_service(node.class_name) != nullptr) {
                continue;
            }
            std::size_t holder = parent[index];
            while (holder != 0 && passes_rule_up(nodes_[holder].class_name)) {
                holder = parent[holder];
            }
            const std::string holder_class = holder == 0 ? std::string("Game") : nodes_[holder].class_name;
            if (std::optional<std::string> error = placement_error(holder_class, node.class_name, node.name)) {
                fail(node.props_path + ": " + *error);
            }
        }
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
        // Each listed GUID takes the first child with that GUID not taken yet,
        // in sorted order. Found through a map, not a scan per listed GUID: a
        // folder of 100,000 children would cost billions of compares.
        std::unordered_map<std::string_view, std::vector<std::size_t>> by_guid;
        by_guid.reserve(sorted.size());
        for (std::size_t child : sorted) {
            by_guid[nodes_[child].guid].push_back(child);
        }
        std::unordered_map<std::string_view, std::size_t> taken;
        std::vector<std::size_t> ordered;
        ordered.reserve(sorted.size());
        std::unordered_set<std::size_t> used;
        used.reserve(sorted.size());
        for (const std::string& guid : node.order) {
            const auto found = by_guid.find(guid);
            if (found == by_guid.end()) {
                continue;
            }
            std::size_t& next = taken[found->first];
            if (next < found->second.size()) {
                const std::size_t child = found->second[next++];
                used.insert(child);
                ordered.push_back(child);
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
    const project_pack::Pack* pack_ = nullptr;
    fs::file_time_type racy_from_ = fs::file_time_type::min();
    bool noting_ = false;
    bool noted_ok_ = true;
    std::unordered_map<std::string, Noted> noted_;
    std::atomic<std::size_t> disk_reads_{0};
    std::vector<PlanNode> nodes_;
    // One per node while the walk's files are read; index 0, the root, is read first.
    std::vector<Job> jobs_;
    // What stopped the walk early, reported once the nodes before it are settled.
    std::optional<std::string> stop_;
    std::unordered_map<std::string, std::string> seen_;
};

// Every file under src that carries a GUID in its name, by GUID: <Name>.<guid>.json,
// .meta.json, and its source file (.luau, .glsl), and a folder's init files under <Name>.<guid>/. Names
// that fit none of these, dotfiles, dot folders, and .tmp files are skipped, as
// a load skips or reports them.
std::map<std::string, std::vector<std::string>> guid_claims(const fs::path& root, const std::string& src) {
    std::map<std::string, std::vector<std::string>> claims;
    const fs::path top = disk_path(root, src);
    std::error_code error;
    for (fs::recursive_directory_iterator it(top, error), end; !error && it != end; it.increment(error)) {
        const std::string name = utf8_path(it->path().filename());
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
        if (name == "init.json" || name == "init.meta.json" || init_source(name) || init_extra(name)) {
            // The root's init.json names no GUID; its GUID is inside.
            if (it->path().parent_path() == top || !split_stem(utf8_path(it->path().parent_path().filename()), guid)) {
                continue;
            }
        } else {
            std::string stem;
            if (ends_with(name, ".meta.json")) {
                stem = name.substr(0, name.size() - 10);
            } else if (const char* extra = extra_suffix_of(name)) {
                stem = name.substr(0, name.size() - std::strlen(extra));
            } else if (const char* suffix = source_suffix_of(name)) {
                stem = name.substr(0, name.size() - std::strlen(suffix));
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
                        const std::string& source, const std::vector<std::string>& extras = {}) {
    if (!root) {
        props.set("Parent", JsonValue::string(parent));
    }
    if (has_source) {
        props.set("Source", JsonValue::string(source));
        // An empty stage has no file, so a disk read has no key either.
        if (!extras.empty() && !extras.front().empty()) {
            props.set("VertexSource", JsonValue::string(extras.front()));
        }
    }
    return props;
}

// A node's extra stage texts, stage 1 first, as compared_json takes them.
std::vector<std::string> extra_texts(const std::vector<AuthoredNode>& tree, std::size_t index) {
    std::vector<std::string> out;
    for (const ExtraSource& extra : tree[index].extra_sources) {
        out.push_back(extra.text);
    }
    return out;
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

// Destroys every live instance, parented or not, but the services, which go
// back to their defaults, and Core with all it holds, which stays as it is.
// Clears the root's extras.
void clear_world(DataModel& world) {
    std::vector<InstanceId> ids;
    world.for_each_instance([&world, &ids](DataModel& object) {
        if (!object.is_service() && !world.outside_place(object.id())) {
            ids.push_back(object.id());
        }
    });
    for (InstanceId id : ids) {
        if (world.alive(id)) {
            world.destroy(id);
        }
    }
    Project::reset_services(world);
    // The camera a view last used belongs to the place being cleared.
    if (auto* workspace = dynamic_cast<Workspace*>(world.instance(world.scene_service("Workspace")))) {
        workspace->set_current_camera(0);
    }
    clear_extras(world, 0);
}

// Creates the plan in world. Returns the runtime id of each plan node.
std::vector<InstanceId> build(DataModel& world, const std::vector<PlanNode>& plan) {
    // Each instance as its file has it: a joint made here snaps nothing. In a
    // scratch place a Bone has no mesh to pose it, so what a joint holds to it
    // would move, and read as changed on disk.
    AssemblySuspend suspend(world);
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
        auto* lua = dynamic_cast<SourceText*>(&object);
        if (lua != nullptr && !node.has_source) {
            fail(node.props_path + ": class " + node.class_name + " keeps Source in a ." + lua->source_extension() +
                 " file; use .meta.json");
        }
        if (lua != nullptr && extension_of(node.source_path) != lua->source_extension()) {
            fail(node.source_path + ": a " + node.class_name + " keeps Source in a ." + lua->source_extension() +
                 " file");
        }
        if (lua == nullptr && node.has_source) {
            fail(node.props_path + ": class " + node.class_name + " has no Source; use .json");
        }
        if (lua != nullptr) {
            lua->set_source(node.source);
            apply_extras(*lua, node);
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

// A new instance from its disk file, with its GUID, out of the tree.
InstanceId create_from(DataModel& world, const PlanNode& node) {
    DataModel& object = find_factory(node.class_name)(world);
    const InstanceId id = object.id();
    world.set_guid(id, node.guid);
    world.set_name(id, node.name);
    apply_properties(object, node);
    if (auto* lua = dynamic_cast<SourceText*>(&object)) {
        lua->set_source(node.source);
        apply_extras(*lua, node);
    }
    return id;
}

// One property from the disk's file, or the class default when the file no
// longer has it. A key the class does not own is an extra.
void set_key(DataModel& world, InstanceId id, DataModel& object, const std::string& key, const JsonValue* value) {
    std::string error;
    if (value != nullptr) {
        if (!object.load_property(key, *value, error)) {
            world.set_extra_property(id, key, *value);
        }
        return;
    }
    PropertyBag defaults;
    object.default_properties(defaults);
    if (const JsonValue* fallback = bag_find(defaults, key)) {
        object.load_property(key, *fallback, error);
    } else {
        world.erase_extra_property(id, key);
    }
}

// id's children in the order a load gives them: those doc's "children" lists,
// in that order, then the rest by GUID. A child moves by leaving and rejoining
// its parent, since setting the same parent does nothing.
void order_children_as(DataModel& world, InstanceId id, const JsonValue& doc) {
    // game's children are the scene services, in the order Game made them.
    if (id == 0) {
        return;
    }
    // Assets holds only its categories, made with the world in table order. A
    // category's own children are ordinary assets, so they still reorder.
    if (id == world.service("Assets")) {
        return;
    }
    // A listed child's place in the file. The first listing counts.
    std::unordered_map<std::string, std::size_t> listed;
    if (const JsonValue* children = doc.find("children"); children != nullptr && children->is_array()) {
        for (const JsonValue& item : children->items()) {
            if (item.is_string()) {
                listed.emplace(item.as_string(), listed.size());
            }
        }
    }
    // Listed children in file order, then unlisted ones by GUID.
    struct Ranked {
        std::size_t at;
        std::string guid;
        InstanceId id;
    };
    const std::vector<InstanceId> now = world.get_children(id);
    std::vector<Ranked> ranked;
    ranked.reserve(now.size());
    for (InstanceId child : now) {
        std::string guid = world.guid(child);
        const auto found = listed.find(guid);
        if (found != listed.end()) {
            ranked.push_back(Ranked{found->second, std::string(), child});
        } else {
            ranked.push_back(Ranked{listed.size(), std::move(guid), child});
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.at != b.at ? a.at < b.at : a.guid < b.guid;
    });
    // Children already in place at the front stay. Moving the rest to the end
    // in order gives the whole order, with no events for the ones that stayed.
    std::size_t first = 0;
    while (first < now.size() && ranked[first].id == now[first]) {
        ++first;
    }
    for (std::size_t at = first; at < ranked.size(); ++at) {
        world.set_parent(ranked[at].id, DataModel::kNoParent);
        world.set_parent(ranked[at].id, id);
    }
}

// An instance's file as the studio would write it back: each property as its
// class stores it (a float, three channels for an opaque color, a default left
// out), extras as they are, and the file's own class, id, Name, and children.
JsonValue written_doc(const DataModel& world, InstanceId id, const JsonValue& doc) {
    JsonValue out = JsonValue::object();
    for (const char* key : {"class", "id", "Name", "children"}) {
        if (const JsonValue* value = doc.find(key)) {
            out.set(key, *value);
        }
    }
    const DataModel* object = id == 0 ? &world : world.instance(id);
    PropertyBag bag;
    if (object != nullptr) {
        object->save_properties(bag);
    }
    for (const JsonValue::Member& member : world.extra_properties(id)) {
        if (bag_find(bag, member.first) == nullptr) {  // a key the class writes wins over a stale extra
            bag_set(bag, member.first, member.second);
        }
    }
    for (const JsonValue::Member& member : bag) {
        out.set(member.first, member.second);
    }
    return out;
}

// doc as its class stores it, made in scratch. doc itself when no class takes it.
JsonValue as_written(DataModel& scratch, const JsonValue& doc) {
    const JsonValue* klass = doc.find("class");
    const ProjectFactory factory =
        klass != nullptr && klass->is_string() ? find_factory(klass->as_string()) : nullptr;
    if (factory == nullptr) {
        return doc;
    }
    DataModel& object = factory(scratch);
    for (const JsonValue::Member& member : doc.members()) {
        if (reserved_key(member.first)) {
            continue;
        }
        std::string error;
        if (!object.load_property(member.first, member.second, error)) {
            scratch.set_extra_property(object.id(), member.first, member.second);
        }
        if (!error.empty()) {
            return doc;
        }
    }
    return written_doc(scratch, object.id(), doc);
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
        world_.assemblies().suspend();
    }

    ~Rebuild() {
        world_.assemblies().resume(world_);
        world_.history().set_enabled(was_);
    }

    void finish() {
        // The old place's Terrain voxels are no longer named by any place bytes.
        terrain::TerrainStash::clear();
        // The old place's snapshot is no use to Stop any more. Play captures
        // the place as it starts, so the read need not copy all of it now.
        world_.forget_place();
        world_.history().reset_waypoints();
        world_.clear_authored_dirty();
        world_.history().mark_saved();
        // The old place's instances are gone. A new one may reuse their GUIDs,
        // so what named them, as the Move tool's Adornee does, must not follow.
        world_.selection().set({});
    }

private:
    DataModel& world_;
    bool was_;
};

JsonValue instance_doc(const AuthoredNode& node, const std::vector<std::string>& children);

JsonValue instance_json(const AuthoredNode& node, const std::vector<AuthoredNode>& tree) {
    std::vector<std::string> order;
    order.reserve(node.children.size());
    for (std::size_t child : node.children) {
        order.push_back(tree[child].guid);
    }
    return instance_doc(node, order);
}

// An instance's file, given the GUIDs of its saved children in their order.
JsonValue instance_doc(const AuthoredNode& node, const std::vector<std::string>& children) {
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
    // Written only when the order is not the default GUID sort. game's children
    // are the scene services, in the order Game makes them, so it has none.
    std::vector<std::string> order = children;
    if (node.id != 0 && !std::is_sorted(order.begin(), order.end())) {
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

namespace {
std::atomic<std::size_t> g_instance_limit{DataModel::kMaxInstances};
}  // namespace

std::size_t Project::instance_limit() { return g_instance_limit.load(std::memory_order_relaxed); }

namespace {

// Off unless asked for: by the sandbox, or by ANARCHY_VERIFY_DISK_COMPARE.
std::atomic<int> g_compare_check{-1};

bool compare_check() {
    int on = g_compare_check.load(std::memory_order_relaxed);
    if (on < 0) {
        const std::optional<std::string> asked = environment_variable("ANARCHY_VERIFY_DISK_COMPARE");
        on = asked && !asked->empty() && *asked != "0" ? 1 : 0;
        g_compare_check.store(on, std::memory_order_relaxed);
    }
    return on == 1;
}

// More changed files than this, and a disk check reads the whole tree: it is
// about as quick, and it is the path every rule was written against.
constexpr std::size_t kMaxChangedFiles = 512;
// More changed instances than this, and a save plans the whole tree.
constexpr std::size_t kMaxChangedSave = 4096;

}  // namespace

void Project::set_compare_check(bool on) { g_compare_check.store(on ? 1 : 0, std::memory_order_relaxed); }

namespace {
std::atomic<std::size_t> g_partial_reads{0};
}  // namespace

std::size_t Project::partial_reads() { return g_partial_reads.load(std::memory_order_relaxed); }

namespace {
std::atomic<std::size_t> g_quick_saves{0};
}  // namespace

std::size_t Project::quick_saves() { return g_quick_saves.load(std::memory_order_relaxed); }

void Project::set_instance_limit(std::size_t limit) {
    g_instance_limit.store(std::clamp<std::size_t>(limit, 1, DataModel::kMaxInstances), std::memory_order_relaxed);
}

Project::Project() = default;

std::filesystem::path Project::resources_root() const { return disk_path(root_, resources_); }

std::filesystem::path Project::tree_root() const { return disk_path(root_, src_); }

void Project::publish_resources_root() const {
    if (game_ != nullptr) {
        game_->set_resources_root(resources_root());
    }
}

Project::Project(Project&&) = default;
Project& Project::operator=(Project&&) = default;
Project::~Project() = default;

void Project::bind(DataModel* game, std::unique_ptr<DataModel> owned) {
    owned_ = std::move(owned);
    game_ = owned_ ? owned_.get() : game;
}

std::vector<std::pair<std::string, std::string>> Project::base_paths_for_bench() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(files_.size());
    for (const auto& [guid, files] : files_) {
        out.emplace_back(guid, files.props_path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<InstanceId> Project::instance_for(std::string_view guid) const {
    const auto found = guid_id_.find(std::string(guid));
    if (found == guid_id_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void Project::take_from_disk(const detail::PlanNode& node, std::string parent, JsonValue props) {
    if (node.made) {
        files_.erase(node.guid);
        return;
    }
    files_[node.guid] = from_disk(node, std::move(parent), std::move(props));
}

Project::Files Project::from_disk(const detail::PlanNode& node, std::string parent, JsonValue props) {
    Files files;
    files.props_path = node.props_path;
    files.props_bytes = node.props_bytes;
    files.has_source = node.has_source;
    files.source_path = node.source_path;
    files.source_bytes = node.source_bytes;
    for (const PlanNode::Extra& extra : node.extras) {
        Files::Extra own;
        own.path = extra.path;
        own.bytes = extra.bytes;
        files.extras.push_back(std::move(own));
    }
    files.props = std::move(props);
    files.parent = std::move(parent);
    return files;
}

void Project::write_skeleton(const fs::path& root) {
    const Layout layout;
    if (game_id_.empty()) game_id_ = make_guid() + make_guid();
    write_file(root / "project.json", write_json(project_document(name_, layout, game_id_, version_)));
    for (const char* kind : kResourceKinds) {
        write_file(disk_path(root, join(join(layout.resources, kind), ".gitkeep")), std::string());
    }
    write_file(root / ".gitignore", kGitignore);
    write_file(root / ".gitattributes", kGitattributes);
    write_file(root / "RESOURCES.md", kResourcesReadme);
    std::error_code error;
    fs::create_directories(disk_path(root, layout.src), error);
    if (error) {
        fail("cannot create " + utf8_path(disk_path(root, layout.src)) + ": " + error.message());
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
        fail(utf8_path(root) + " is not empty");
    }
    Project project;
    project.bind(&into, nullptr);
    project.root_ = root;
    project.name_ = project_name_for(root);
    // The skeleton goes first: a folder that cannot be made fails before the
    // world is cleared.
    project.write_skeleton(root);
    {
        Rebuild rebuild(into);
        clear_world(into);
        into.set_name(0, project.name_);
        add_default_camera(into);
        add_default_light(into);
        project.save_tree(true);
        rebuild.finish();
    }
    project.publish_resources_root();
    return project;
}

Project Project::adopt(const fs::path& root, DataModel& game) {
    if (!missing_or_empty_dir(root)) {
        fail(utf8_path(root) + " is not empty");
    }
    Project project;
    project.bind(&game, nullptr);
    project.root_ = root;
    project.name_ = project_name_for(root);
    project.write_skeleton(root);
    project.save_tree(true);
    project.publish_resources_root();
    return project;
}

Project Project::load(const fs::path& root) {
    Project project;
    project.bind(nullptr, std::make_unique<Game>());
    project.read_into_game(root, false);
    return project;
}

Project Project::load(const fs::path& root, DataModel& into) {
    Project project;
    project.bind(&into, nullptr);
    project.read_into_game(root, true);
    return project;
}

void Project::read_into_game(const fs::path& root, bool replace) {
    root_ = root;
    ProjectJson json = read_project_json(root);
    name_ = json.name;
    const Layout layout = json.layout;
    game_id_ = json.game_id;
    version_ = json.version;
    // A project from before gameId and version gets them now, once, the way
    // ignore_texture_cache adds .cache/ to .gitignore.
    if (game_id_.empty() || version_.empty()) {
        if (game_id_.empty()) game_id_ = make_guid() + make_guid();
        if (version_.empty()) version_ = "1.0.0";
        // Best effort, like ignore_texture_cache: a read-only folder still opens, with the values kept in memory.
        json.doc.set("gameId", JsonValue::string(game_id_));
        json.doc.set("version", JsonValue::string(version_));
        std::string write_error;
        engine_core::write_file(root / "project.json", write_json(json.doc), write_error);
    }
    ignore_texture_cache(root);
    src_ = layout.src;
    resources_ = layout.resources;
    // Each file's stamp is taken as the read lists its folder, before it reads
    // the file: one that changes during the read then differs from its stamp,
    // so the next check compares it rather than missing it.
    const fs::file_time_type racy_from = fs::file_time_type::clock::now() - std::chrono::seconds(kRacySeconds);
    const std::unique_ptr<project_pack::Pack> pack = project_pack::read(project_pack::path_for(root));
    PlanReader reader(root, layout, pack.get(), racy_from);
    const std::vector<PlanNode> plan = reader.read();
    std::optional<DiskStamp> read_stamp;
    if (reader.noted_ok()) {
        read_stamp = stamp_from(reader.noted(), racy_from);
    } else {
        read_stamp = disk_stamp();
    }
    // Replace builds a scratch place first. Its instances are each file as its
    // class stores it: the base a disk scan and a save compare against.
    std::unique_ptr<Game> scratch;
    std::vector<InstanceId> scratch_ids;
    if (replace) {
        // A class-level error (a bad Color) must not leave the world half rebuilt.
        scratch = std::make_unique<Game>();
        scratch->history().set_enabled(false);
        scratch_ids = build(*scratch, plan);
    }
    // Each file as its class stores it, from the scratch place: worked out on
    // other cores while the world builds, since nothing changes scratch now.
    std::vector<JsonValue> disk_docs;
    std::thread disk_reader;
    // The first failure on a worker (out of memory), thrown here once they end.
    std::mutex failed_lock;
    std::exception_ptr failed;
    const auto guarded = [&](const auto& work) {
        try {
            work();
        } catch (...) {
            const std::lock_guard<std::mutex> hold(failed_lock);
            if (!failed) {
                failed = std::current_exception();
            }
        }
    };
    if (scratch != nullptr) {
        disk_docs.resize(plan.size());
        const auto read_disk = [&] {
            parallel_for(plan.size(), [&](std::size_t index) {
                if (!plan[index].made) {
                    guarded([&] { disk_docs[index] = written_doc(*scratch, scratch_ids[index], plan[index].doc); });
                }
            });
        };
        try {
            disk_reader = std::thread(read_disk);
        } catch (const std::system_error&) {
            read_disk();
        }
    }
    std::vector<InstanceId> ids;
    try {
        // Before the build, so a Terrain reads its voxel file as it loads.
        publish_resources_root();
        Rebuild rebuild(*game_);
        if (replace) {
            clear_world(*game_);
        }
        ids = build(*game_, plan);
        rebuild.finish();
    } catch (...) {
        if (disk_reader.joinable()) {
            disk_reader.join();
        }
        throw;
    }
    if (disk_reader.joinable()) {
        disk_reader.join();
    }
    // Reads alone, on every core: the world is built, and nothing else runs on it.
    std::vector<JsonValue> live_docs(plan.size());
    // An instance that changed itself as it loaded, as a Terrain whose voxel
    // file is damaged takes a file of its own, differs from its file: that is
    // the studio's change, which the next save writes.
    std::vector<char> changed_itself(plan.size(), 0);
    parallel_for(plan.size(), [&](std::size_t index) {
        if (!plan[index].made) {
            guarded([&] {
                live_docs[index] = written_doc(*game_, ids[index], plan[index].doc);
                if (scratch != nullptr) {
                    changed_itself[index] = !(disk_docs[index] == live_docs[index]);
                }
            });
        }
    });
    if (failed) {
        std::rethrow_exception(failed);
    }
    const std::vector<std::size_t> parents = parents_of(plan);
    bool made = false;
    id_guid_.reserve(id_guid_.size() + plan.size());
    guid_id_.reserve(guid_id_.size() + plan.size());
    files_.reserve(files_.size() + plan.size());
    for (std::size_t index = 0; index < plan.size(); ++index) {
        const PlanNode& node = plan[index];
        id_guid_[ids[index]] = node.guid;
        guid_id_[node.guid] = ids[index];
        // A service the read made has no file: to a save, it is new.
        if (node.made) {
            made = true;
            continue;
        }
        JsonValue live = std::move(live_docs[index]);
        if (scratch != nullptr) {
            if (changed_itself[index] != 0) {
                game_->mark_authored_dirty(ids[index]);
            }
            live = std::move(disk_docs[index]);
        }
        files_[node.guid] =
            from_disk(node, index == 0 ? std::string() : plan[parents[index]].guid, std::move(live));
    }
    // A service the read made is not among game's children on disk, so the next save writes game.
    if (made) {
        game_->mark_authored_dirty(0);
    }
    synced_stamp_ = read_stamp;
    publish_resources_root();
    if (reader.disk_reads() >= kPackRewrite) {
        write_pack();
    }
}

std::optional<Project::DiskStamp> Project::stamp_from(
    const std::unordered_map<std::string, detail::PlanReaderNote>& noted, fs::file_time_type racy_from) const {
    DiskStamp stamp;
    std::error_code error;
    const fs::path project_json = root_ / "project.json";
    if (fs::is_regular_file(project_json, error)) {
        const fs::directory_entry entry(project_json, error);
        const std::uintmax_t size = entry.file_size(error);
        if (error) {
            return std::nullopt;
        }
        const fs::file_time_type written = entry.last_write_time(error);
        if (error) {
            return std::nullopt;
        }
        if (written >= racy_from) {
            stamp.racy.push_back("project.json");
        }
        stamp.files.emplace("project.json",
                            DiskStamp::Entry{size, static_cast<std::int64_t>(written.time_since_epoch().count())});
    }
    stamp.files.reserve(noted.size() + 1);
    for (const auto& [name, note] : noted) {
        if (note.written >= racy_from) {
            stamp.racy.push_back(name);
        }
        stamp.files.emplace(name,
                            DiskStamp::Entry{note.size, static_cast<std::int64_t>(note.written.time_since_epoch().count())});
    }
    std::sort(stamp.racy.begin(), stamp.racy.end());
    return stamp;
}

void Project::write_pack() const {
    if (!synced_stamp_ || files_.size() < kPackMinFiles) {
        return;
    }
    std::vector<project_pack::Source> sources;
    sources.reserve(files_.size() + 8);
    const std::set<std::string> racy(synced_stamp_->racy.begin(), synced_stamp_->racy.end());
    const auto add = [&](const std::string& path, const SharedText& bytes) {
        const auto stamped = synced_stamp_->files.find(path);
        if (stamped == synced_stamp_->files.end() || racy.count(path) != 0 || !bytes.shared()) {
            return;
        }
        sources.push_back(project_pack::Source{path, stamped->second.size, stamped->second.written, bytes.shared()});
    };
    for (const auto& [guid, files] : files_) {
        add(files.props_path, files.props_bytes);
        if (files.has_source) {
            add(files.source_path, files.source_bytes);
        }
        for (const Files::Extra& extra : files.extras) {
            if (!extra.path.empty()) {
                add(extra.path, extra.bytes);
            }
        }
    }
    project_pack::write_in_background(project_pack::path_for(root_), std::move(sources));
}

namespace {

// A file the save was about to touch no longer has the stamp the save trusted:
// something wrote it since the walk. The save undoes what it wrote and runs
// again with a walk of its own, which finds it as a conflict.
struct StaleStamp {};

}  // namespace

void Project::save(const std::vector<SaveConflict>& overwrite, bool just_checked) {
    try {
        save_tree(false, overwrite, just_checked);
    } catch (const StaleStamp&) {
        synced_stamp_.reset();
        save_tree(false, overwrite, false);
    }
}

void Project::save_as(const fs::path& root) {
    if (!missing_or_empty_dir(root)) {
        fail(utf8_path(root) + " is not empty");
    }
    Layout old_layout;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        old_layout = read_project_json(root_).layout;
    }
    write_skeleton(root);
    // Instances name resources by relative path. Copy them so the paths still resolve.
    const fs::path from = disk_path(root_, old_layout.resources);
    if (fs::is_directory(from, error)) {
        fs::copy(from, disk_path(root, Layout{}.resources),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
        if (error) {
            fail("cannot copy " + utf8_path(from) + ": " + error.message());
        }
    }
    root_ = root;
    src_ = Layout{}.src;
    resources_ = Layout{}.resources;
    publish_resources_root();
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
        // Where this node's extra stage file of an extension goes; the root has none.
        std::function<std::string(const std::string&)> extra_path;
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
            const char* source_extension = node.source_extension != nullptr ? node.source_extension : "luau";
            if (folder) {
                const std::string own = join(dir, stem);
                files.props_path = join(own, node.has_source ? "init.meta.json" : "init.json");
                files.source_path = node.has_source ? join(own, std::string("init.") + source_extension) : std::string();
                for (std::size_t child : node.children) {
                    dirs[child] = own;
                }
            } else {
                files.props_path = join(dir, stem + (node.has_source ? ".meta.json" : ".json"));
                files.source_path = node.has_source ? join(dir, stem + "." + source_extension) : std::string();
            }
            extra_path = [dir, stem, folder](const std::string& extension) {
                return folder ? join(join(dir, stem), "init." + extension) : join(dir, stem + "." + extension);
            };
            // An extra stage with text gets its file beside the source; one without has none.
            if (node.has_properties) {
                for (const ExtraSource& extra : node.extra_sources) {
                    Files::Extra own;
                    if (!extra.text.empty()) {
                        own.path = extra_path(extra.extension);
                    }
                    own.bytes = extra.text;
                    files.extras.push_back(std::move(own));
                }
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
            // Its bytes as they were, at the path its parent's folder gives it now.
            files.extras = known.extras;
            for (Files::Extra& extra : files.extras) {
                if (!extra.path.empty()) {
                    extra.path = extra_path(extra_extension_of(extra.path));
                }
            }
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
            for (const Files::Extra& extra : entry.extras) {
                mix(extra.path);
                mix(extra.bytes);
            }
        }
    } catch (const ProjectError& error) {
        // A place that cannot be written is never "saved".
        mix(error.what());
    }
    return hash;
}

bool Project::unsaved() const { return game_->history().dirty(); }

void Project::reset_place(DataModel& game) {
    Rebuild rebuild(game);
    clear_world(game);
    game.set_name(0, game.class_name());
    game.set_guid(0, make_guid());
    game.set_resources_root({});
    add_default_camera(game);
    add_default_light(game);
    rebuild.finish();
}

void Project::reset_services(DataModel& game) {
    for (const ServiceSpec& spec : kServices) {
        if (DataModel* service = game.instance(game.service(spec.class_name))) {
            reset_service(*service);
        }
    }
}

InstanceId Project::add_default_camera(DataModel& game) {
    Camera& camera = game.create<Camera>();
    camera.set_transform(matrix4_look_at(Vec3{0.f, 9.f, 21.f}, Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 1.f, 0.f}));
    camera.set_field_of_view(Camera::kNewPlaceFieldOfView);
    game.set_parent(camera.id(), game.scene_service("Workspace"));
    return camera.id();
}

InstanceId Project::add_default_light(DataModel& game) {
    DynamicSky& sky = game.create<DynamicSky>();
    game.set_parent(sky.id(), game.scene_service("Lighting"));
    return sky.id();
}

std::vector<SaveConflict> Project::outside_changes(const std::vector<AuthoredNode>& tree,
                                                   const std::map<std::string, Files>& next,
                                                   const std::map<std::string, std::vector<std::string>>& claims,
                                                   std::set<std::string>& left_gone, bool in_sync) const {
    // In sync, every base file is on disk with its base bytes, and with no
    // claims read there is no file the base does not know: no rule below can
    // find anything.
    if (in_sync && claims.empty()) {
        return {};
    }
    const std::vector<std::size_t> parent = parents_of(tree);
    // A place to read a changed file back into, as its class stores it; made
    // the first time a file needs it.
    std::unique_ptr<Game> scratch_game;
    auto scratch = [&scratch_game]() -> DataModel& {
        if (!scratch_game) {
            scratch_game = std::make_unique<Game>();
            scratch_game->history().set_enabled(false);
        }
        return *scratch_game;
    };
    // Made when the first row needs it: a save with no conflict never does.
    std::unordered_map<std::string, std::size_t> index_of;
    // A row names its instance as the explorers do: its Name, and where it sits.
    auto row = [&](const std::string& guid, const std::string& path, SaveConflict::Kind kind) {
        if (index_of.empty()) {
            for (std::size_t index = 0; index < tree.size(); ++index) {
                index_of.emplace(tree[index].guid, index);
            }
        }
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
                            planned.source_bytes != base->second.source_bytes ||
                            !same_extras(planned.extras, base->second.extras);
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
            if (path == base.props_path || path == base.source_path) {
                return false;
            }
            return std::none_of(base.extras.begin(), base.extras.end(),
                                [&path](const Files::Extra& extra) { return path == extra.path; });
        });
    };

    std::vector<SaveConflict> conflicts;
    // Left gone: GUID -> its first missing file.
    std::map<std::string, std::string> gone_paths;
    std::error_code error;
    // In sync, every base file is on disk with its base bytes: none is gone,
    // none was edited, so this loop could find nothing. It stats and reads
    // every file, which is most of a save's time in a large place.
    static const std::unordered_map<std::string, Files> kNoFiles;
    const std::unordered_map<std::string, Files>& checked = in_sync ? kNoFiles : files_;
    for (const auto& [guid, base] : checked) {
        const auto planned = next.find(guid);
        const bool removed = planned == next.end();
        // A file's bytes matter only where the save rewrites or deletes it. A
        // move alone carries whatever is on disk to the new path.
        struct Own {
            const std::string* path;
            const std::string* bytes;
            bool rewritten;
        };
        std::vector<Own> own{{&base.props_path, &base.props_bytes.str(),
                              removed || planned->second.props_bytes != base.props_bytes}};
        if (base.has_source) {
            own.push_back({&base.source_path, &base.source_bytes.str(),
                           removed || !planned->second.has_source ||
                               planned->second.source_bytes != base.source_bytes});
        }
        for (std::size_t i = 0; i < base.extras.size(); ++i) {
            const Files::Extra& extra = base.extras[i];
            if (extra.path.empty()) {
                continue;
            }
            const bool kept = !removed && i < planned->second.extras.size() && planned->second.extras[i] == extra;
            own.push_back({&extra.path, &extra.bytes.str(), !kept});
        }
        const bool touched = removed || holds.count(guid) != 0 || planned->second.props_path != base.props_path ||
                             planned->second.source_path != base.source_path ||
                             planned->second.has_source != base.has_source ||
                             !same_extras(planned->second.extras, base.extras) ||
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
            std::string parse_error;
            if (bytes == base.props_bytes) {
                // Unchanged on disk.
            } else if (!parse_json(bytes, disk, parse_error) || !disk.is_object() ||
                       !(disk = as_written(scratch(), disk)).is_object()) {
                SaveConflict unreadable = row(guid, base.props_path, SaveConflict::Kind::EditedOutside);
                unreadable.disk = "can't be read";
                edited.push_back(std::move(unreadable));
            } else {
                const JsonValue none = JsonValue::object();
                const JsonValue& mine = removed ? none : planned->second.props.get();
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
        for (std::size_t i = 1; i < own.size(); ++i) {
            if (!own[i].rewritten) {
                continue;
            }
            const bool primary = i == 1 && base.has_source;
            const std::string text = read_file(disk_path(root_, *own[i].path));
            const std::string* planned_bytes = nullptr;
            if (!removed) {
                if (primary) {
                    planned_bytes = &planned->second.source_bytes.str();
                } else {
                    // The planned extra at this path, or none when the stage was emptied.
                    for (const Files::Extra& extra : planned->second.extras) {
                        if (extra.path == *own[i].path) {
                            planned_bytes = &extra.bytes.str();
                        }
                    }
                }
            }
            if (text != *own[i].bytes && (removed || planned_bytes == nullptr || text != *planned_bytes)) {
                if (removed || planned_bytes == nullptr) {
                    if (edited.empty()) {
                        edited.push_back(whole(base.props_path));
                    }
                } else {
                    SaveConflict source = row(guid, *own[i].path, SaveConflict::Kind::EditedOutside);
                    source.key = primary ? "Source" : "VertexSource";
                    std::tie(source.studio, source.disk) = differing_lines(*planned_bytes, text);
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
    // Each plan node's file as its class stores it.
    std::vector<JsonValue> docs;
    std::vector<std::size_t> plan_parents;
    std::unordered_map<std::string, std::size_t> on_disk;
    std::vector<AuthoredNode> tree;
    std::vector<std::size_t> tree_parents;
    std::unordered_map<std::string, std::size_t> in_studio;
    // What only the disk changed, in the order to apply it.
    std::vector<Action> actions;
    std::vector<SaveConflict> rows;
    // Parents whose sibling order the studio changed: a child from the disk
    // does not re-sort them.
    std::set<std::string> keep_order;
    // Read from only the changed files and the instances they are under: an
    // instance the plan lacks is on disk as the base has it.
    bool partial = false;

    std::string disk_parent(std::size_t index) const {
        return index == 0 ? std::string() : plan[plan_parents[index]].guid;
    }
    std::string studio_parent(std::size_t index) const {
        return index == 0 ? std::string() : tree[tree_parents[index]].guid;
    }
    // top and every plan node under it. The plan lists a parent before its children.
    std::vector<std::size_t> subtree(std::size_t top) const {
        std::vector<std::size_t> out{top};
        std::set<std::size_t> within{top};
        for (std::size_t index = top + 1; index < plan.size(); ++index) {
            if (within.count(plan_parents[index]) != 0) {
                within.insert(index);
                out.push_back(index);
            }
        }
        return out;
    }
    // An instance as it stands once the actions run: its class, Name, and
    // parent's GUID, and the actions that made, moved, or remade it.
    struct Placed {
        std::string class_name;
        std::string name;
        std::string parent;
        bool alive = true;
        std::vector<std::size_t> by;
    };
    struct Outcome {
        // Every instance, by GUID; the root's parent is empty.
        std::map<std::string, Placed> nodes;
        // Creates whose parent does not end up in the place.
        std::set<std::size_t> unplaced;
    };
    // The place after actions, leaving out those in skip. apply_changes makes
    // and moves instances to match it.
    Outcome outcome(const std::set<std::size_t>& skip) const;
    // Leaves out every action whose result a placement rule refuses, and every
    // Create whose parent that leaves out, so an apply never passes a refused
    // move to set_parent. Only the studio's own edits can make a disk plan
    // that read cleanly break a rule. Returns the refused ones.
    std::vector<std::pair<Action, std::string>> drop_refused();
    // At most how many instances applying actions makes. Every one is made
    // before anything is destroyed.
    std::size_t instances_to_make() const {
        using Type = Action::Type;
        std::size_t count = 0;
        for (const Action& action : actions) {
            if (action.type == Type::Create || action.type == Type::Recreate) {
                ++count;
            } else if (action.type == Type::Restore) {
                for (std::size_t index : subtree(on_disk.at(action.guid))) {
                    count += in_studio.count(plan[index].guid) == 0 ? 1 : 0;
                }
            }
        }
        return count;
    }
};

Project::Comparison::Outcome Project::Comparison::outcome(const std::set<std::size_t>& skip) const {
    using Type = Action::Type;
    Outcome out;
    std::map<std::string, Placed>& nodes = out.nodes;
    if (tree.empty() || plan.empty()) {
        return out;
    }
    const std::string& root = tree[0].guid;
    for (std::size_t index = 0; index < tree.size(); ++index) {
        Placed& node = nodes[tree[index].guid];
        node.class_name = tree[index].class_name;
        node.name = tree[index].name;
        node.parent = studio_parent(index);
    }
    // The disk's root is the studio's root, whatever GUID either gives it.
    auto parent_on_disk = [&](std::size_t index) {
        const std::string parent = disk_parent(index);
        return parent == plan[0].guid ? root : parent;
    };
    auto alive = [&](const std::string& guid) {
        const auto found = nodes.find(guid);
        return found != nodes.end() && found->second.alive;
    };
    // guid is top or under it.
    auto within = [&](std::string guid, const std::string& top) {
        for (std::size_t guard = 0; guard <= nodes.size() && !guid.empty(); ++guard) {
            if (guid == top) {
                return true;
            }
            const auto found = nodes.find(guid);
            guid = found == nodes.end() ? std::string() : found->second.parent;
        }
        return false;
    };
    auto make = [&](std::size_t index, const std::string& parent, std::size_t action) {
        Placed& node = nodes[plan[index].guid];
        node = Placed();
        node.class_name = plan[index].class_name;
        node.name = plan[index].name;
        node.parent = parent;
        node.by.push_back(action);
    };
    // In the order apply_changes runs them: made first, then moved, then destroyed.
    for (std::size_t at = 0; at < actions.size(); ++at) {
        const Action& action = actions[at];
        if (skip.count(at) != 0) {
            continue;
        }
        if (action.type == Type::Create) {
            const std::size_t index = on_disk.at(action.guid);
            const std::string parent = parent_on_disk(index);
            if (alive(parent)) {
                make(index, parent, at);
            } else {
                out.unplaced.insert(at);
            }
        } else if (action.type == Type::Restore) {
            for (std::size_t index : subtree(on_disk.at(action.guid))) {
                const std::string parent = parent_on_disk(index);
                if (!alive(plan[index].guid) && alive(parent)) {
                    make(index, parent, at);
                }
            }
        } else if (action.type == Type::Recreate) {
            if (action.guid == root || !alive(action.guid)) {
                continue;
            }
            const std::size_t index = on_disk.at(action.guid);
            Placed& node = nodes[action.guid];
            node.class_name = plan[index].class_name;
            node.by.push_back(at);
            // Under its disk parent, unless that parent is it or under it.
            const std::string parent = parent_on_disk(index);
            if (alive(parent) && !within(parent, action.guid)) {
                node.parent = parent;
            }
        }
    }
    // Keys, shallowest instance first, so a parent moves before a child moves under it.
    std::vector<std::size_t> sets;
    for (std::size_t at = 0; at < actions.size(); ++at) {
        if (actions[at].type == Type::Set && skip.count(at) == 0) {
            sets.push_back(at);
        }
    }
    auto depth = [&](std::size_t at) {
        std::size_t count = 0;
        for (std::size_t index = on_disk.at(actions[at].guid); index != 0; index = plan_parents[index]) {
            ++count;
        }
        return count;
    };
    std::stable_sort(sets.begin(), sets.end(), [&](std::size_t a, std::size_t b) { return depth(a) < depth(b); });
    for (std::size_t at : sets) {
        const Action& action = actions[at];
        if (!alive(action.guid)) {
            continue;
        }
        const std::size_t index = on_disk.at(action.guid);
        Placed& node = nodes[action.guid];
        if (action.key == "Name") {
            node.name = plan[index].name;
        } else if (action.key == "Parent") {
            const std::string parent = parent_on_disk(index);
            if (alive(parent) && !within(parent, action.guid) && node.parent != parent) {
                node.parent = parent;
                node.by.push_back(at);
            }
        }
    }
    // Destroyed, with whatever is still under them.
    std::set<std::string> gone;
    for (std::size_t at = 0; at < actions.size(); ++at) {
        if (actions[at].type == Type::Destroy && skip.count(at) == 0 && actions[at].guid != root) {
            gone.insert(actions[at].guid);
        }
    }
    if (!gone.empty()) {
        for (auto& [guid, node] : nodes) {
            for (std::string at = guid; !at.empty() && node.alive; at = nodes.at(at).parent) {
                node.alive = gone.count(at) == 0;
            }
        }
    }
    return out;
}

std::vector<std::pair<Project::Comparison::Action, std::string>> Project::Comparison::drop_refused() {
    // Nothing to apply leaves the place as the studio has it, which the rules already passed.
    if (actions.empty()) {
        return {};
    }
    std::set<std::size_t> skip;
    std::vector<std::pair<std::size_t, std::string>> refused;
    Outcome result = outcome(skip);
    for (bool again = true; again;) {
        again = false;
        for (const auto& [guid, node] : result.nodes) {
            if (!node.alive || node.parent.empty() || find_service(node.class_name) != nullptr) {
                continue;
            }
            // The instance whose rule decides: the first ancestor that is not a Folder.
            std::string holder = node.parent;
            for (std::size_t guard = 0; guard <= result.nodes.size(); ++guard) {
                const Placed& up = result.nodes.at(holder);
                if (!passes_rule_up(up.class_name) || up.parent.empty()) {
                    break;
                }
                holder = up.parent;
            }
            const std::optional<std::string> error =
                placement_error(result.nodes.at(holder).class_name, node.class_name, node.name);
            if (!error) {
                continue;
            }
            // The nearest instance on the way up to the holder that an action
            // made, moved, or remade. Its actions are what the rule refuses.
            const std::vector<std::size_t>* blame = nullptr;
            for (std::string at = guid; blame == nullptr;) {
                const Placed& step = result.nodes.at(at);
                if (!step.by.empty()) {
                    blame = &step.by;
                }
                if (at == holder || step.parent.empty()) {
                    break;
                }
                at = step.parent;
            }
            if (blame == nullptr) {
                continue;
            }
            for (std::size_t at : *blame) {
                skip.insert(at);
                refused.emplace_back(at, *error);
            }
            again = true;
            break;
        }
        if (again) {
            result = outcome(skip);
        }
    }
    // A Create under one left out goes with it: that one's row carries it.
    skip.insert(result.unplaced.begin(), result.unplaced.end());
    std::vector<std::pair<Action, std::string>> out;
    for (const auto& [at, reason] : refused) {
        out.emplace_back(actions[at], reason);
    }
    std::vector<Action> kept;
    for (std::size_t at = 0; at < actions.size(); ++at) {
        if (skip.count(at) == 0) {
            kept.push_back(std::move(actions[at]));
        }
    }
    actions = std::move(kept);
    return out;
}

Project::Comparison Project::compare_disk(const std::optional<DiskStamp>* walked) const {
    Comparison out;
    std::vector<std::size_t> compare;
    if (!read_changed(out, compare, walked)) {
        out = Comparison();
        read_whole(out);
        find_rows(out, nullptr);
        return out;
    }
    out.partial = true;
    g_partial_reads.fetch_add(1, std::memory_order_relaxed);
    find_rows(out, &compare);
    if (compare_check()) {
        Comparison whole;
        read_whole(whole);
        find_rows(whole, nullptr);
        const auto same_actions = [](const std::vector<Comparison::Action>& a, const std::vector<Comparison::Action>& b) {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
                return x.type == y.type && x.guid == y.guid && x.key == y.key;
            });
        };
        if (out.rows != whole.rows || !same_actions(out.actions, whole.actions)) {
            contract_fail("a disk check that read only the changed files found other rows or actions than a whole read");
        }
    }
    return out;
}

bool Project::read_changed(Comparison& out, std::vector<std::size_t>& compare,
                           const std::optional<DiskStamp>* walked) const {
    if (!synced_stamp_ || files_.empty() || game_ == nullptr) {
        return false;
    }
    const std::optional<DiskStamp> now = walked != nullptr ? *walked : disk_stamp();
    if (!now || now->files.size() != synced_stamp_->files.size()) {
        return false;
    }
    // Every file whose stamp moved, and every file the synced stamp could not
    // vouch for. No file may be new or gone.
    std::unordered_set<std::string> paths;
    for (const auto& [path, entry] : now->files) {
        const auto was = synced_stamp_->files.find(path);
        if (was == synced_stamp_->files.end()) {
            return false;
        }
        if (was->second != entry) {
            paths.insert(path);
        }
    }
    paths.insert(synced_stamp_->racy.begin(), synced_stamp_->racy.end());
    // project.json says where the tree is: the only thing a disk check reads from it.
    if (paths.erase("project.json") != 0) {
        Layout layout;
        try {
            layout = read_project_json(root_).layout;
        } catch (const std::exception&) {
            return false;
        }
        if (layout.src != src_) {
            return false;
        }
    }
    if (paths.size() > kMaxChangedFiles) {
        return false;
    }
    // The instance each file belongs to. The bytes decide what changed: a
    // stamp that moved over the same bytes changes nothing.
    std::set<std::string> changed;
    const std::string* root = nullptr;
    std::size_t owned = 0;
    try {
        for (const auto& [guid, files] : files_) {
            if (files.parent.empty()) {
                root = &guid;
            }
            const bool props = paths.count(files.props_path) != 0;
            const bool source = files.has_source && paths.count(files.source_path) != 0;
            std::size_t extras = 0;
            bool extra_changed = false;
            for (const Files::Extra& extra : files.extras) {
                if (extra.path.empty() || paths.count(extra.path) == 0) {
                    continue;
                }
                ++extras;
                extra_changed = extra_changed || read_file(disk_path(root_, extra.path)) != extra.bytes;
            }
            if (!props && !source && extras == 0) {
                continue;
            }
            owned += (props ? 1 : 0) + (source ? 1 : 0) + extras;
            if ((props && read_file(disk_path(root_, files.props_path)) != files.props_bytes) ||
                (source && read_file(disk_path(root_, files.source_path)) != files.source_bytes) || extra_changed) {
                changed.insert(guid);
            }
        }
    } catch (const std::exception&) {
        return false;
    }
    if (root == nullptr || owned != paths.size()) {
        return false;
    }
    // Each changed instance and every instance it is under, shallowest first.
    std::unordered_map<std::string, std::size_t> depth;
    std::vector<std::string> chain{*root};
    depth.emplace(*root, 0);
    for (const std::string& guid : changed) {
        std::vector<std::string> up;
        for (std::string at = guid; depth.count(at) == 0;) {
            const auto found = files_.find(at);
            if (found == files_.end() || found->second.parent.empty()) {
                return false;
            }
            up.push_back(at);
            at = found->second.parent;
        }
        for (auto it = up.rbegin(); it != up.rend(); ++it) {
            depth.emplace(*it, depth.at(files_.at(*it).parent) + 1);
            chain.push_back(*it);
        }
    }
    std::stable_sort(chain.begin(), chain.end(),
                     [&depth](const std::string& a, const std::string& b) { return depth.at(a) < depth.at(b); });
    auto base_text = [](const Files& base, const char* key) {
        const JsonValue* value = base.props.find(key);
        return value != nullptr && value->is_string() ? value->as_string() : std::string();
    };
    std::vector<PlanNode> plan;
    plan.reserve(chain.size());
    std::unordered_map<std::string, std::size_t> index_of;
    try {
        for (const std::string& guid : chain) {
            const Files& base = files_.at(guid);
            const bool top = base.parent.empty();
            PlanNode node;
            if (changed.count(guid) != 0) {
                // As the whole read takes it: the same reader, the same checks.
                read_plan_props(root_, node, base.props_path, top ? std::string() : guid, top);
                if (base.has_source) {
                    read_plan_source(root_, node, base.source_path);
                }
                for (const Files::Extra& extra : base.extras) {
                    if (!extra.path.empty()) {
                        read_plan_extra(root_, node, extra.path);
                    }
                }
                const std::string was_class = top ? std::string("Game") : base_text(base, "class");
                if (node.class_name != was_class) {
                    return false;
                }
                std::vector<std::string> was_order;
                const JsonValue* was_children = base.props.find("children");
                if (was_children != nullptr && was_children->is_array()) {
                    for (const JsonValue& entry : was_children->items()) {
                        was_order.push_back(entry.is_string() ? entry.as_string() : std::string());
                    }
                }
                if (node.has_order != (was_children != nullptr) || node.order != was_order) {
                    return false;
                }
                if (!top && find_service(node.class_name) != nullptr) {
                    node.name = node.class_name;
                }
            } else {
                node.guid = guid;
                node.class_name = top ? std::string("Game") : base_text(base, "class");
                node.name = base_text(base, "Name");
                node.props_path = base.props_path;
                node.props_bytes = base.props_bytes;
                node.has_source = base.has_source;
                node.source_path = base.source_path;
                node.source = base.source_bytes;
                node.source_bytes = base.source_bytes;
                for (const Files::Extra& extra : base.extras) {
                    if (!extra.path.empty()) {
                        node.extras.push_back(PlanNode::Extra{extra.path, extra.bytes.str()});
                    }
                }
                node.doc = base.props;
                for (const JsonValue::Member& member : base.props.members()) {
                    if (!reserved_key(member.first)) {
                        bag_set(node.properties, member.first, member.second);
                    }
                }
            }
            index_of.emplace(guid, plan.size());
            plan.push_back(std::move(node));
        }
        for (std::size_t index = 1; index < plan.size(); ++index) {
            plan[index_of.at(files_.at(plan[index].guid).parent)].children.push_back(index);
        }
        // A renamed instance must still be allowed where it is.
        const std::vector<std::size_t> parents = parents_of(plan);
        for (std::size_t index = 1; index < plan.size(); ++index) {
            const PlanNode& node = plan[index];
            if (changed.count(node.guid) == 0 || find_service(node.class_name) != nullptr) {
                continue;
            }
            std::size_t holder = parents[index];
            while (holder != 0 && passes_rule_up(plan[holder].class_name)) {
                holder = parents[holder];
            }
            const std::string holder_class = holder == 0 ? std::string("Game") : plan[holder].class_name;
            if (placement_error(holder_class, node.class_name, node.name)) {
                return false;
            }
        }
        // Each changed file as its class stores it, made in a scratch place
        // under the instances it is under.
        std::vector<JsonValue> docs;
        docs.reserve(plan.size());
        Game scratch;
        scratch.history().set_enabled(false);
        const std::vector<InstanceId> ids = build(scratch, plan);
        for (std::size_t index = 0; index < plan.size(); ++index) {
            docs.push_back(changed.count(plan[index].guid) != 0
                               ? written_doc(scratch, ids[index], plan[index].doc)
                               : files_.at(plan[index].guid).props.get());
        }
        out.plan = std::move(plan);
        out.docs = std::move(docs);
        out.plan_parents = parents;
    } catch (const std::exception&) {
        return false;
    }
    for (std::size_t index = 0; index < out.plan.size(); ++index) {
        out.on_disk.emplace(out.plan[index].guid, index);
    }
    // The studio's side: every instance, with properties only where compared.
    const DataModel& world = *game_;
    const std::unordered_set<std::string> wanted(chain.begin(), chain.end());
    out.tree = world.authored_tree([&](InstanceId id) { return wanted.count(world.guid(id)) != 0; });
    out.tree_parents = parents_of(out.tree);
    for (std::size_t index = 0; index < out.tree.size(); ++index) {
        out.in_studio.emplace(out.tree[index].guid, index);
    }
    compare.resize(out.plan.size());
    std::iota(compare.begin(), compare.end(), std::size_t(0));
    return true;
}

void Project::read_whole(Comparison& out) const {
    const Layout layout = read_project_json(root_).layout;
    out.plan = PlanReader(root_, layout).read();
    {
        // A value a class rejects, such as a Color that is not numbers, stops
        // the scan here rather than halfway through an apply.
        Game scratch;
        scratch.history().set_enabled(false);
        const std::vector<InstanceId> ids = build(scratch, out.plan);
        // Each file as its class stores it, so a value the class writes back
        // differently compares as the studio would write it.
        out.docs.reserve(out.plan.size());
        for (std::size_t index = 0; index < out.plan.size(); ++index) {
            out.docs.push_back(written_doc(scratch, ids[index], out.plan[index].doc));
        }
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

}

void Project::find_rows(Comparison& out, const std::vector<std::size_t>* only) const {
    using Type = Comparison::Action::Type;
    auto base_json = [&](const Files& base) {
        std::vector<std::string> extras;
        for (const Files::Extra& extra : base.extras) {
            extras.push_back(extra.bytes.str());
        }
        return compared_json(base.props, base.parent, base.parent.empty(), base.has_source, base.source_bytes, extras);
    };
    auto disk_json = [&](std::size_t index) {
        const PlanNode& node = out.plan[index];
        return compared_json(out.docs[index], out.disk_parent(index), index == 0, node.has_source, node.source,
                             extra_bytes_of(node));
    };
    auto studio_json = [&](std::size_t index) {
        const AuthoredNode& node = out.tree[index];
        return compared_json(instance_json(node, out.tree), out.studio_parent(index), index == 0, node.has_source,
                             node.source, extra_texts(out.tree, index));
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
    std::vector<std::size_t> every;
    if (only == nullptr) {
        every.resize(out.plan.size());
        std::iota(every.begin(), every.end(), std::size_t(0));
    }
    for (const std::size_t index : only != nullptr ? *only : every) {
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
        // A new class is a new instance: taken whole, or kept whole. When the
        // studio already has the disk's class, the rest merges key by key.
        if (index != 0 && in_base && !same_value(was.find("class"), disk.find("class")) &&
            !same_value(mine.find("class"), disk.find("class"))) {
            if (differs(was, mine)) {
                add_row(node.guid, path, SaveConflict::Kind::EditedOutside, "class", display_value(mine.find("class")),
                        display_value(disk.find("class")));
            } else {
                act(Type::Recreate, node.guid);
            }
            continue;
        }
        for (const KeyMerge& merged : merge_keys(was, disk, mine)) {
            // game's children are the scene services in a fixed order; a list in
            // the root file is nothing to load.
            if (merged.key == "class" || (index == 0 && merged.key == "children")) {
                continue;
            }
            if (merged.key == "children" &&
                (merged.change == KeyChange::StudioOnly || merged.change == KeyChange::Conflict)) {
                out.keep_order.insert(node.guid);
            }
            if (merged.change == KeyChange::DiskOnly) {
                act(Type::Set, node.guid, merged.key);
            } else if (merged.change == KeyChange::Conflict) {
                key_row(node.guid, path, merged.key, mine, disk);
            }
        }
    }
    // Gone from disk. A partial read is only taken when nothing is.
    std::set<std::string> kept;
    std::vector<std::string> gone;
    static const std::unordered_map<std::string, Files> kNone;
    for (const auto& [guid, files] : only != nullptr ? kNone : files_) {
        if (out.on_disk.count(guid) != 0) {
            continue;
        }
        const auto studio = out.in_studio.find(guid);
        if (studio == out.in_studio.end()) {
            continue;  // Deleted on both sides.
        }
        if (studio_below(studio->second)) {
            add_row(guid, files.props_path, SaveConflict::Kind::DeletedOutside, "", "changed in the studio", "deleted");
            kept.insert(guid);
        } else {
            gone.push_back(guid);
        }
    }
    // One inside a folder that has a row stays with that folder: the folder's
    // row decides for both.
    for (const std::string& guid : gone) {
        bool held = false;
        for (std::size_t at = out.tree_parents[out.in_studio.at(guid)]; at != 0 && !held; at = out.tree_parents[at]) {
            held = kept.count(out.tree[at].guid) != 0;
        }
        if (!held) {
            act(Type::Destroy, guid);
        }
    }
    // A change the studio's own edits leave nowhere the rules allow is a row.
    for (const auto& [action, reason] : out.drop_refused()) {
        const std::size_t index = out.on_disk.at(action.guid);
        const auto base = files_.find(action.guid);
        const std::string path = base != files_.end() ? base->second.props_path : out.plan[index].props_path;
        const std::size_t first = out.rows.size();
        if (action.type == Type::Create) {
            add_row(action.guid, path, SaveConflict::Kind::AddedOutside, "", "missing", "added on disk");
        } else if (action.type == Type::Recreate) {
            const JsonValue mine = studio_json(out.in_studio.at(action.guid));
            add_row(action.guid, path, SaveConflict::Kind::EditedOutside, "class", display_value(mine.find("class")),
                    display_value(disk_json(index).find("class")));
        } else {
            key_row(action.guid, path, action.key, studio_json(out.in_studio.at(action.guid)), disk_json(index));
        }
        for (std::size_t at = first; at < out.rows.size(); ++at) {
            out.rows[at].disk += " (" + reason + ")";
        }
    }
    std::sort(out.rows.begin(), out.rows.end(), [](const SaveConflict& a, const SaveConflict& b) {
        return std::tie(a.where, a.name, a.guid, a.key) < std::tie(b.where, b.name, b.guid, b.key);
    });
}

std::optional<Project::DiskStamp> Project::disk_stamp() const {
    DiskStamp stamp;
    // A write after this is racy: the file's clock may still read the same.
    const fs::file_time_type racy_from =
        fs::file_time_type::clock::now() - std::chrono::seconds(kRacySeconds);
    const auto add = [&](const fs::directory_entry& entry, std::string name) {
        std::error_code error;
        const std::uintmax_t size = entry.file_size(error);
        if (error) {
            return false;
        }
        const fs::file_time_type written = entry.last_write_time(error);
        if (error) {
            return false;
        }
        std::replace(name.begin(), name.end(), '\\', '/');
        if (written >= racy_from) {
            stamp.racy.push_back(name);
        }
        stamp.files.emplace(std::move(name),
                            DiskStamp::Entry{size, static_cast<std::int64_t>(written.time_since_epoch().count())});
        return true;
    };
    std::error_code error;
    const fs::path project_json = root_ / "project.json";
    if (fs::is_regular_file(project_json, error) && !add(fs::directory_entry(project_json, error), "project.json")) {
        return std::nullopt;
    }
    const fs::path top = disk_path(root_, src_);
    if (fs::is_directory(top, error)) {
        for (fs::recursive_directory_iterator it(top, error), end; it != end; it.increment(error)) {
            if (error) {
                return std::nullopt;
            }
            std::error_code kind;
            if (it->is_regular_file(kind) && !add(*it, utf8_path(it->path().lexically_relative(root_)))) {
                return std::nullopt;
            }
        }
        if (error) {
            return std::nullopt;
        }
    }
    std::sort(stamp.racy.begin(), stamp.racy.end());
    return stamp;
}

bool Project::disk_matches_base(std::optional<DiskStamp>* walked) const {
    const std::optional<DiskStamp> now = disk_stamp();
    if (walked != nullptr) {
        *walked = now;
    }
    return disk_matches(now);
}

bool Project::disk_matches(const std::optional<DiskStamp>& now) const {
    if (!synced_stamp_) {
        return false;
    }
    if (!now || now->files != synced_stamp_->files) {
        return false;
    }
    // A racy file's stamp proves nothing: it must hold the base's bytes.
    // project.json and any file the base does not know take the full compare.
    if (!synced_stamp_->racy.empty()) {
        std::unordered_map<std::string_view, const std::string*> base_bytes;
        for (const auto& [guid, files] : files_) {
            base_bytes.emplace(files.props_path, &files.props_bytes.str());
            if (files.has_source) {
                base_bytes.emplace(files.source_path, &files.source_bytes.str());
            }
            for (const Files::Extra& extra : files.extras) {
                if (!extra.path.empty()) {
                    base_bytes.emplace(extra.path, &extra.bytes.str());
                }
            }
        }
        for (const std::string& path : synced_stamp_->racy) {
            // project.json is not an instance's file: what a disk check takes
            // from it is where the tree is.
            if (path == "project.json") {
                Layout layout;
                try {
                    layout = read_project_json(root_).layout;
                } catch (const std::exception&) {
                    return false;
                }
                if (layout.src != src_) {
                    return false;
                }
                continue;
            }
            const auto known = base_bytes.find(path);
            if (known == base_bytes.end()) {
                return false;
            }
            std::string bytes;
            try {
                bytes = read_file(disk_path(root_, path));
            } catch (const std::exception&) {
                return false;
            }
            if (bytes != *known->second) {
                return false;
            }
        }
        // Proven; and once the clock has moved past them they are no longer racy.
        if (now->racy.size() < synced_stamp_->racy.size()) {
            synced_stamp_ = now;
        }
    }
    return true;
}

void Project::mark_synced(const std::optional<DiskStamp>& taken) const {
    // taken was walked before the files it proves were read. A file written
    // after that walk has another stamp now, so the next check finds it: no
    // second walk is needed to take taken as the synced stamp.
    synced_stamp_ = taken;
}

DiskScan Project::scan_disk() const {
    // Nothing on disk changed since the base matched it: no changes, no rows.
    std::optional<DiskStamp> before;
    if (disk_matches_base(&before)) {
        return DiskScan{};
    }
    Comparison compared = compare_disk(&before);
    DiskScan out;
    out.conflicts = std::move(compared.rows);
    out.has_disk_changes = !compared.actions.empty();
    // The disk read just now holds nothing the base lacks: the base matches it.
    if (!out.has_disk_changes && out.conflicts.empty()) {
        mark_synced(before);
    }
    return out;
}

DiskScan Project::apply_disk(const std::vector<DiskChoice>& choices) { return apply_disk_walked(choices, nullptr); }

DiskScan Project::apply_disk(const std::vector<DiskChoice>& choices, const Walk& walk) {
    return apply_disk_walked(choices, &walk.stamp);
}

DiskScan Project::apply_disk_walked(const std::vector<DiskChoice>& choices, const std::optional<DiskStamp>* given) {
    using Type = Comparison::Action::Type;
    DataModel& world = *game_;
    if (world.simulation_running()) {
        fail("changes from disk load in edit mode; stop the test first");
    }
    // Nothing on disk changed since the base matched it, and nothing was
    // chosen: there is nothing to load or settle. (Choices name rows, which
    // only a disk that changed has; they always take the full path.)
    std::optional<DiskStamp> before;
    bool matches = false;
    if (given != nullptr) {
        before = *given;
        matches = disk_matches(before);
    } else {
        matches = disk_matches_base(&before);
    }
    if (matches && choices.empty()) {
        return DiskScan{};
    }
    Comparison compared = compare_disk(&before);
    DiskScan out;
    std::vector<const SaveConflict*> studio_side;
    for (const DiskChoice& choice : choices) {
        if (std::find(compared.rows.begin(), compared.rows.end(), choice.conflict) == compared.rows.end()) {
            out.skipped.push_back(choice.conflict);
            continue;
        }
        if (!choice.disk) {
            studio_side.push_back(&choice.conflict);
            continue;
        }
        Comparison::Action action;
        action.guid = choice.conflict.guid;
        if (choice.conflict.kind == SaveConflict::Kind::DeletedOutside) {
            action.type = Type::Destroy;
        } else if (choice.conflict.key.empty()) {
            action.type = Type::Restore;
        } else if (choice.conflict.key == "class") {
            action.type = Type::Recreate;
        } else {
            action.type = Type::Set;
            action.key = choice.conflict.key;
        }
        compared.actions.push_back(std::move(action));
    }
    // A disk side the rules refuse stays undone; the next scan lists its row again.
    compared.drop_refused();
    // Checked before anything changes, the base included: running out of room
    // partway would stop the studio.
    const std::size_t needed = compared.instances_to_make();
    if (needed > world.room_left()) {
        fail("the changes on disk add " + std::to_string(needed) + " instances, and this place has room for " +
             std::to_string(world.room_left()) + " more");
    }
    for (const SaveConflict* conflict : studio_side) {
        settle(compared, *conflict);
    }
    if (!compared.actions.empty()) {
        // What comes from disk is on disk: applying it leaves the place as dirty as it was.
        const bool was_dirty = world.history().dirty();
        const std::optional<std::string> recording = world.history().try_begin_recording("Changes from Disk");
        apply_changes(compared, out.loaded);
        if (recording) {
            world.history().finish_recording(*recording, FinishRecordingOperation::Commit);
        }
        if (!was_dirty) {
            world.history().mark_saved();
        }
        world.capture_place();
    }
    Comparison after = compare_disk(&before);
    refresh_base(compared, after);
    out.conflicts = std::move(after.rows);
    out.has_disk_changes = !after.actions.empty();
    // In sync when nothing is left to load or ask about and the disk did not
    // change while this ran; otherwise the next check compares in full.
    synced_stamp_.reset();
    if (!out.has_disk_changes && out.conflicts.empty()) {
        mark_synced(before);
    }
    return out;
}

void Project::settle(const Comparison& compared, const SaveConflict& conflict) {
    if (conflict.kind == SaveConflict::Kind::DeletedOutside) {
        // Kept: a save writes it as a new file.
        files_.erase(conflict.guid);
        return;
    }
    const auto disk = compared.on_disk.find(conflict.guid);
    if (disk == compared.on_disk.end()) {
        return;
    }
    const std::size_t index = disk->second;
    if (conflict.key.empty()) {
        // The studio's delete stands. The base takes the disk's files under it
        // too, so a save removes them all.
        for (std::size_t at : compared.subtree(index)) {
            take_from_disk(compared.plan[at], compared.disk_parent(at), compared.docs[at]);
        }
        return;
    }
    const detail::PlanNode& node = compared.plan[index];
    const auto base = files_.find(conflict.guid);
    if (base == files_.end() || conflict.key == "class") {
        take_from_disk(node, compared.disk_parent(index), compared.docs[index]);
        return;
    }
    Files& files = base->second;
    if (conflict.key == "Parent") {
        files.parent = compared.disk_parent(index);
    } else if (conflict.key == "Source") {
        files.source_bytes = node.source;
    } else {
        if (const JsonValue* value = compared.docs[index].find(conflict.key)) {
            files.props.set(conflict.key, *value);
        } else {
            files.props.erase(conflict.key);
        }
        files.props_bytes = write_json(files.props);
    }
}

void Project::apply_changes(const Comparison& compared, std::vector<std::string>& loaded) {
    // Disk edits come in one instance at a time; the stopped tick's follow pass settles them.
    AssemblySuspend suspend(*game_);
    using Type = Comparison::Action::Type;
    using Action = Comparison::Action;
    DataModel& world = *game_;
    // find_guid walks every instance. This walks them once, and the creates
    // below add what they make. An entry for an instance destroyed since reads
    // as stale and is looked up again.
    std::unordered_map<std::string, InstanceId> ids = world.guid_index();
    auto find_guid = [&world, &ids](const std::string& guid) -> std::optional<InstanceId> {
        if (guid.empty()) {
            return std::nullopt;
        }
        const auto found = ids.find(guid);
        if (found == ids.end()) {
            return std::nullopt;
        }
        if ((found->second == 0 || world.alive(found->second)) && world.guid(found->second) == guid) {
            return found->second;
        }
        const std::optional<InstanceId> live = world.find_guid(guid);
        if (live) {
            found->second = *live;
        } else {
            ids.erase(found);
        }
        return live;
    };
    auto create = [&world, &ids](const detail::PlanNode& node) {
        const InstanceId made = create_from(world, node);
        ids[node.guid] = made;
        return made;
    };
    auto note = [&loaded](const std::string& name) {
        if (std::find(loaded.begin(), loaded.end(), name) == loaded.end()) {
            loaded.push_back(name);
        }
    };
    auto each = [&compared](Type type, const std::function<void(const Action&)>& run) {
        for (const Action& action : compared.actions) {
            if (action.type == type) {
                run(action);
            }
        }
    };
    // Where every instance ends up. drop_refused left only actions whose
    // result the placement rules allow.
    const Comparison::Outcome outcome = compared.outcome({});
    auto placed = [&outcome](const std::string& guid) -> const Comparison::Placed* {
        const auto found = outcome.nodes.find(guid);
        return found == outcome.nodes.end() || !found->second.alive ? nullptr : &found->second;
    };
    // Instances out of the tree until the attach below puts them where the
    // outcome has them, by GUID, each with whether the disk gave it that parent.
    std::vector<std::pair<std::string, bool>> pending;
    std::set<std::string> pending_guids;
    auto pend = [&](const std::string& guid, bool from_disk) {
        if (pending_guids.insert(guid).second) {
            pending.emplace_back(guid, from_disk);
        }
    };
    // Parents that took a child from the disk, to order as the disk does.
    std::set<std::string> received;

    // Every instance that moves leaves its parent first, so none carries its
    // children through a place that refuses them on the way to one that takes them.
    for (std::size_t index = 1; index < compared.tree.size(); ++index) {
        const std::string& guid = compared.tree[index].guid;
        const Comparison::Placed* node = placed(guid);
        if (node == nullptr || node->parent == compared.studio_parent(index)) {
            continue;
        }
        const std::optional<InstanceId> id = find_guid(guid);
        if (id && !world.parent_error(*id, DataModel::kNoParent)) {
            world.set_parent(*id, DataModel::kNoParent);
            pend(guid, true);
        }
    }
    // Deleted on disk, with whatever is still under them.
    each(Type::Destroy, [&](const Action& action) {
        if (const std::optional<InstanceId> id = find_guid(action.guid); id && *id != 0) {
            note(world.name(*id));
            world.destroy_tree(*id);
        }
    });
    // A new class is a new instance with the same GUID, children, and place
    // among its siblings. Its children wait out of the tree for it, since
    // they may change class too.
    struct Remade {
        std::string guid;
        InstanceId old = 0;
        InstanceId parent = 0;
        std::vector<InstanceId> siblings;
    };
    std::vector<Remade> remade;
    each(Type::Recreate, [&](const Action& action) {
        const std::optional<InstanceId> old = find_guid(action.guid);
        if (!old || *old == 0) {
            return;
        }
        const detail::PlanNode& node = compared.plan[compared.on_disk.at(action.guid)];
        const InstanceId was = world.parent(*old);
        if (was != DataModel::kNoParent) {
            remade.push_back({action.guid, *old, was, world.get_children(was)});
        }
        for (InstanceId kid : world.get_children(*old)) {
            if (!world.parent_error(kid, DataModel::kNoParent)) {
                world.set_parent(kid, DataModel::kNoParent);
                pend(world.guid(kid), false);
            }
        }
        world.destroy(*old);
        create(node);
        pend(action.guid, false);
        note(node.name);
    });
    // New instances, and ones the studio deleted, back from the disk with
    // everything under them.
    each(Type::Create, [&](const Action& action) {
        const detail::PlanNode& node = compared.plan[compared.on_disk.at(action.guid)];
        if (placed(node.guid) != nullptr && !find_guid(node.guid)) {
            create(node);
            pend(node.guid, true);
            note(node.name);
        }
    });
    each(Type::Restore, [&](const Action& action) {
        for (std::size_t index : compared.subtree(compared.on_disk.at(action.guid))) {
            const detail::PlanNode& node = compared.plan[index];
            if (placed(node.guid) != nullptr && !find_guid(node.guid)) {
                create(node);
                pend(node.guid, true);
                note(node.name);
            }
        }
    });
    // Each into its parent, parents first. Each parent is where it ends up by
    // then, and each child carries only what ends up under it, so every move
    // meets the rules the outcome passed.
    auto depth = [&](const std::string& guid) {
        std::size_t count = 0;
        for (const Comparison::Placed* at = placed(guid); at != nullptr && !at->parent.empty();
             at = placed(at->parent)) {
            ++count;
        }
        return count;
    };
    std::vector<std::pair<std::size_t, std::size_t>> by_depth;
    for (std::size_t at = 0; at < pending.size(); ++at) {
        by_depth.emplace_back(depth(pending[at].first), at);
    }
    std::stable_sort(by_depth.begin(), by_depth.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : by_depth) {
        const std::string& guid = pending[entry.second].first;
        const Comparison::Placed* node = placed(guid);
        const std::optional<InstanceId> id = find_guid(guid);
        if (node == nullptr || !id) {
            continue;
        }
        const std::optional<InstanceId> up = find_guid(node->parent);
        // Asked although the outcome passed, so a case it missed cannot stop the studio.
        if (!up || world.parent(*id) == *up || world.parent_error(*id, *up)) {
            continue;
        }
        world.set_parent(*id, *up);
        const auto disk = compared.on_disk.find(guid);
        if (pending[entry.second].second && disk != compared.on_disk.end()) {
            received.insert(compared.disk_parent(disk->second));
        }
    }
    // A remade instance back in its old parent takes its old place there.
    for (const Remade& entry : remade) {
        const std::optional<InstanceId> made = find_guid(entry.guid);
        if (!made || world.parent(*made) != entry.parent) {
            continue;
        }
        for (InstanceId sibling : entry.siblings) {
            const InstanceId at = sibling == entry.old ? *made : sibling;
            if (world.alive(at) && world.parent(at) == entry.parent) {
                world.set_parent(at, DataModel::kNoParent);
                world.set_parent(at, entry.parent);
            }
        }
    }
    // Keys, shallowest instance first. The moves above took Parent.
    std::vector<const Action*> sets;
    for (const Action& action : compared.actions) {
        if (action.type == Type::Set) {
            sets.push_back(&action);
        }
    }
    auto disk_depth = [&](const Action* action) {
        std::size_t out = 0;
        for (std::size_t at = compared.on_disk.at(action->guid); at != 0; at = compared.plan_parents[at]) {
            ++out;
        }
        return out;
    };
    std::stable_sort(sets.begin(), sets.end(),
                     [&](const Action* a, const Action* b) { return disk_depth(a) < disk_depth(b); });
    for (const Action* action : sets) {
        const std::optional<InstanceId> id = find_guid(action->guid);
        if (!id) {
            continue;
        }
        const std::size_t index = compared.on_disk.at(action->guid);
        const detail::PlanNode& node = compared.plan[index];
        DataModel& object = *id == 0 ? world : *world.instance(*id);
        if (action->key == "Name") {
            world.set_name(*id, node.name);
        } else if (action->key == "Source") {
            if (auto* lua = dynamic_cast<SourceText*>(&object)) {
                lua->set_source(node.source);
            }
        } else if (action->key == "VertexSource") {
            if (auto* lua = dynamic_cast<SourceText*>(&object)) {
                apply_extras(*lua, node);
            }
        } else if (action->key == "children") {
            order_children_as(world, *id, node.doc);
        } else if (action->key != "Parent") {
            set_key(world, *id, object, action->key, node.doc.find(action->key));
        }
        note(node.name);
    }
    for (const std::string& parent : received) {
        const bool open = std::any_of(compared.rows.begin(), compared.rows.end(), [&](const SaveConflict& row) {
            return row.guid == parent && row.key == "children";
        });
        const std::optional<InstanceId> id = find_guid(parent);
        const auto disk = compared.on_disk.find(parent);
        if (!open && compared.keep_order.count(parent) == 0 && id && disk != compared.on_disk.end()) {
            order_children_as(world, *id, compared.plan[disk->second].doc);
        }
    }
}

void Project::refresh_base(const Comparison& before, const Comparison& after) {
    // What after still has open or pending, by GUID: a key, or "" for all of it.
    std::map<std::string, std::set<std::string>> held;
    for (const SaveConflict& row : after.rows) {
        held[row.guid].insert(row.key);
    }
    for (const Comparison::Action& action : after.actions) {
        held[action.guid].insert(action.type == Comparison::Action::Type::Set ? action.key : std::string());
    }
    for (std::size_t index = 0; index < after.plan.size(); ++index) {
        const detail::PlanNode& node = after.plan[index];
        const auto seen = before.on_disk.find(node.guid);
        if (seen == before.on_disk.end()) {
            continue;  // Written during the apply: the next check takes it.
        }
        const detail::PlanNode& was = before.plan[seen->second];
        const bool files_steady = node.props_path == was.props_path && node.source_path == was.source_path;
        const bool steady = files_steady && node.props_bytes == was.props_bytes &&
                            node.source_bytes == was.source_bytes &&
                            after.disk_parent(index) == before.disk_parent(seen->second);
        const auto keys = held.find(node.guid);
        if (keys == held.end()) {
            // Nothing open: the base takes the disk, when the disk still reads as the apply saw it.
            if (!steady) {
                continue;
            }
            // Added on disk under an instance the studio deleted: that instance's row carries it.
            if (files_.count(node.guid) == 0 && after.in_studio.count(node.guid) == 0) {
                continue;
            }
            take_from_disk(node, after.disk_parent(index), after.docs[index]);
            continue;
        }
        // Something open: the base takes the disk key by key, for every key
        // neither open nor pending that the disk held steady through the apply.
        const auto base = files_.find(node.guid);
        if (base == files_.end() || keys->second.count("") != 0) {
            continue;
        }
        Files& files = base->second;
        const JsonValue& disk = after.docs[index];
        const JsonValue& earlier = before.docs[seen->second];
        std::set<std::string> names;
        for (const JsonValue* object : {&files.props.get(), &disk}) {
            for (const JsonValue::Member& member : object->members()) {
                names.insert(member.first);
            }
        }
        for (const std::string& key : names) {
            if (key == "id" || keys->second.count(key) != 0 || !same_value(disk.find(key), earlier.find(key))) {
                continue;
            }
            if (const JsonValue* value = disk.find(key)) {
                files.props.set(key, *value);
            } else {
                files.props.erase(key);
            }
        }
        files.props_bytes = write_json(files.props);
        if (keys->second.count("Parent") == 0 && after.disk_parent(index) == before.disk_parent(seen->second)) {
            files.parent = after.disk_parent(index);
        }
        if (keys->second.count("Source") == 0 && node.source_bytes == was.source_bytes) {
            files.has_source = node.has_source;
            files.source_bytes = node.source_bytes;
        }
        // Where its files are, once no one disagrees about its place or its name.
        if (keys->second.count("Parent") == 0 && keys->second.count("Name") == 0 && files_steady) {
            files.props_path = node.props_path;
            files.source_path = node.source_path;
        }
    }
    // Gone from disk: out of the base only when it was already gone when the
    // apply acted, and nothing holds it. A partial read lists only what
    // changed, and is only taken when nothing is gone.
    if (before.partial || after.partial) {
        return;
    }
    for (auto it = files_.begin(); it != files_.end();) {
        if (after.on_disk.count(it->first) == 0 && before.on_disk.count(it->first) == 0 &&
            held.count(it->first) == 0) {
            it = files_.erase(it);
        } else {
            ++it;
        }
    }
}

// Everything a save writes, taken from the place at once, so writing it needs
// neither the place nor the base: the write can run while the studio goes on.
struct Project::PendingSave {
    fs::path root;
    std::string src;
    bool full = false;
    bool playing = false;
    bool in_sync = false;
    bool keeps_sync = false;
    // In sync: the disk as the save found it, then as it left it.
    std::optional<DiskStamp> stamp;
    // Each instance whose files the save places: its base files, when it had
    // them, and its planned ones.
    struct Entry {
        std::string guid;
        bool had = false;
        Files base;
        Files planned;
    };
    std::vector<Entry> entries;
    // The base files of each instance whose files go.
    std::vector<Files> removed;
    std::set<std::string> left_gone;
    // The conflicts overwrite listed, and every claim read for them.
    std::vector<SaveConflict> conflicts;
    std::map<std::string, std::vector<std::string>> claims;
    // For the base afterwards. Planned from the changes alone (delta), the
    // base keeps every instance not in entries.
    bool delta = false;
    std::vector<std::string> removed_guids;
    std::vector<std::pair<InstanceId, std::string>> ids;
    std::vector<InstanceId> gone_ids;
    // What the capture took from the place, given back when the write fails.
    AuthoredDirty dirty;
    bool cleared = false;
    // The write's own.
    SaveReport report;
    std::vector<std::string> stamped;
};

Project::SaveJob::SaveJob() = default;
Project::SaveJob::~SaveJob() = default;
Project::SaveJob::SaveJob(SaveJob&&) noexcept = default;
Project::SaveJob& Project::SaveJob::operator=(SaveJob&&) noexcept = default;

Project::Walk Project::walk_disk() const {
    Walk walk;
    walk.stamp = disk_stamp();
    return walk;
}

Project::SaveJob Project::begin_save() {
    SaveJob job;
    job.save = capture_save(false, {}, true);
    return job;
}

Project::Written Project::write_save(SaveJob& job, std::string& error) {
    try {
        write_save(*job.save);
        return Written::Yes;
    } catch (const StaleStamp&) {
        return Written::Stale;
    } catch (const std::exception& failure) {
        error = failure.what();
        return Written::Failed;
    }
}

void Project::end_save(SaveJob& job, Written written) {
    finish_save(*job.save, written == Written::Yes);
    if (written == Written::Stale) {
        synced_stamp_.reset();
    }
}

void Project::save_tree(bool full, const std::vector<SaveConflict>& overwrite, bool just_checked) {
    std::unique_ptr<PendingSave> save = capture_save(full, overwrite, just_checked);
    try {
        write_save(*save);
    } catch (...) {
        finish_save(*save, false);
        throw;
    }
    finish_save(*save, true);
}

bool Project::plan_changed(const AuthoredDirty& dirty, PendingSave& save) const {
    const DataModel& world = *game_;
    if (dirty.all || dirty.ids.size() > kMaxChangedSave || id_guid_.empty() || files_.empty()) {
        return false;
    }
    // What a save holds now.
    std::vector<InstanceId> held;
    world.authored_ids(held);
    std::unordered_set<InstanceId> live(held.begin(), held.end());
    // Gone: in the base, and no longer saved, or saved under another GUID.
    for (const auto& [id, guid] : id_guid_) {
        if (live.count(id) != 0 && world.guid(id) == guid) {
            continue;
        }
        const auto base = files_.find(guid);
        if (base != files_.end()) {
            save.removed.push_back(base->second);
            save.removed_guids.push_back(guid);
        }
        save.gone_ids.push_back(id);
    }
    // Changed: each instance the studio changed that a save still holds, and
    // each the base does not have under its GUID.
    std::unordered_set<InstanceId> changed;
    for (InstanceId id : dirty.ids) {
        if (live.count(id) != 0) {
            changed.insert(id);
        }
    }
    std::vector<InstanceId> kids;
    for (InstanceId id : held) {
        const auto known = id_guid_.find(id);
        const auto base =
            known == id_guid_.end() || known->second != world.guid(id) ? files_.end() : files_.find(known->second);
        if (base == files_.end()) {
            changed.insert(id);
            continue;
        }
        // A folder with no children, as a hand-made project can load, saves as a
        // leaf. Left unchanged, a save would keep the folder beside the leaf.
        if (id != 0 && !own_folder(base->second.props_path).empty()) {
            world.authored_children(id, kids);
            if (kids.empty()) {
                changed.insert(id);
            }
        }
    }
    // A leaf that gained a child, as a service the engine adds under one, saves
    // as a folder. The new child is changed, so its parent is found from it;
    // left unchanged, a save would write the folder without its init.json.
    std::vector<InstanceId> grown;
    for (InstanceId id : changed) {
        const InstanceId parent = id == 0 ? 0 : world.parent(id);
        if (parent == 0 || live.count(parent) == 0 || changed.count(parent) != 0) {
            continue;
        }
        const auto known = id_guid_.find(parent);
        const auto base = known == id_guid_.end() ? files_.end() : files_.find(known->second);
        if (base != files_.end() && own_folder(base->second.props_path).empty()) {
            grown.push_back(parent);
        }
    }
    changed.insert(grown.begin(), grown.end());
    // Where each instance's files go, as plan_files names them.
    std::unordered_map<InstanceId, std::string> folder_of;
    std::vector<InstanceId> children;
    std::function<std::string(InstanceId)> own_folder_of = [&](InstanceId id) -> std::string {
        if (id == 0) {
            return save.src;
        }
        const auto found = folder_of.find(id);
        if (found != folder_of.end()) {
            return found->second;
        }
        std::string folder =
            join(own_folder_of(world.parent(id)), sanitize_file_name(world.name(id)) + "." + world.guid(id));
        folder_of.emplace(id, folder);
        return folder;
    };
    auto paths = [&](InstanceId id, Files& files) {
        files.source_path.clear();
        files.extras.clear();
        if (id == 0) {
            files.props_path = join(save.src, "init.json");
            files.has_source = false;
            files.parent.clear();
            return;
        }
        const DataModel* object = world.instance(id);
        const auto* text = dynamic_cast<const SourceText*>(object);
        const bool script = text != nullptr;
        const std::string extension = script ? text->source_extension() : "";
        world.authored_children(id, children);
        const std::string dir = own_folder_of(world.parent(id));
        const std::string stem = sanitize_file_name(world.name(id)) + "." + world.guid(id);
        files.has_source = script;
        if (!children.empty()) {
            const std::string own = join(dir, stem);
            files.props_path = join(own, script ? "init.meta.json" : "init.json");
            files.source_path = script ? join(own, "init." + extension) : std::string();
        } else {
            files.props_path = join(dir, stem + (script ? ".meta.json" : ".json"));
            files.source_path = script ? join(dir, stem + "." + extension) : std::string();
        }
        for (int stage = 1; script && stage < text->stage_count(); ++stage) {
            Files::Extra extra;
            const std::string& held = text->source(stage);
            if (!held.empty()) {
                extra.path = !children.empty() ? join(join(dir, stem), std::string("init.") + text->source_extension(stage))
                                               : join(dir, stem + "." + text->source_extension(stage));
            }
            extra.bytes = held;
            files.extras.push_back(std::move(extra));
        }
        files.parent = world.guid(world.parent(id));
    };
    // Each changed instance's files, as a whole plan would write them.
    std::unordered_set<std::string> guids;
    std::vector<InstanceId> moved;
    for (InstanceId id : held) {
        if (changed.count(id) == 0) {
            continue;
        }
        const AuthoredNode node = world.authored_node(id);
        if (!valid_guid(node.guid) || !guids.insert(node.guid).second) {
            return false;
        }
        const bool known = id == 0 ? node.class_name == "Game" : project_class_known(node.class_name);
        if (!known) {
            return false;
        }
        // A GUID another saved instance still has: the whole plan says so.
        const auto other = guid_id_.find(node.guid);
        if (other != guid_id_.end() && other->second != id && live.count(other->second) != 0 &&
            world.guid(other->second) == node.guid) {
            return false;
        }
        PendingSave::Entry entry;
        entry.guid = node.guid;
        paths(id, entry.planned);
        std::vector<std::string> order;
        for (InstanceId child : children) {
            order.push_back(world.guid(child));
        }
        try {
            const JsonValue doc = instance_doc(node, order);
            entry.planned.props_bytes = instance_bytes(doc, node);
            entry.planned.props = doc;
        } catch (const std::exception&) {
            return false;
        }
        entry.planned.source_bytes = node.source;
        const auto base = files_.find(node.guid);
        entry.had = base != files_.end();
        if (entry.had) {
            entry.base = base->second;
            if (entry.base.props_path != entry.planned.props_path ||
                entry.base.source_path != entry.planned.source_path) {
                moved.push_back(id);
            } else {
                // An extra stage file somewhere else than the base's moves it too.
                for (std::size_t i = 0; i < std::min(entry.base.extras.size(), entry.planned.extras.size()); ++i) {
                    const std::string& was = entry.base.extras[i].path;
                    const std::string& now = entry.planned.extras[i].path;
                    if (!was.empty() && !now.empty() && was != now) {
                        moved.push_back(id);
                        break;
                    }
                }
            }
        }
        save.ids.emplace_back(id, node.guid);
        save.entries.push_back(std::move(entry));
    }
    // Under an instance whose files moved, every file moves too.
    std::vector<InstanceId> below;
    for (InstanceId top : moved) {
        world.authored_children(top, below);
        for (std::size_t at = 0; at < below.size(); ++at) {
            const InstanceId id = below[at];
            std::vector<InstanceId> more;
            world.authored_children(id, more);
            below.insert(below.end(), more.begin(), more.end());
            if (changed.count(id) != 0) {
                continue;
            }
            changed.insert(id);
            const auto base = files_.find(world.guid(id));
            if (base == files_.end()) {
                return false;
            }
            PendingSave::Entry entry;
            entry.guid = base->first;
            entry.had = true;
            entry.base = base->second;
            entry.planned = base->second;
            paths(id, entry.planned);
            entry.planned.parent = base->second.parent;
            save.entries.push_back(std::move(entry));
        }
    }
    // A GUID a saved instance has is not gone, whichever instance has it now:
    // one remade with another class keeps its GUID and its files.
    std::vector<Files> removed;
    std::vector<std::string> removed_guids;
    for (std::size_t at = 0; at < save.removed_guids.size(); ++at) {
        if (guids.count(save.removed_guids[at]) == 0) {
            removed.push_back(std::move(save.removed[at]));
            removed_guids.push_back(std::move(save.removed_guids[at]));
        }
    }
    save.removed = std::move(removed);
    save.removed_guids = std::move(removed_guids);
    // A folder the save moves or deletes may hold a file the base does not
    // know, which only a walk of every file finds: the whole plan does that.
    for (const PendingSave::Entry& entry : save.entries) {
        if (entry.had && !own_folder(entry.base.props_path).empty() &&
            entry.base.props_path != entry.planned.props_path) {
            return false;
        }
    }
    for (const Files& gone : save.removed) {
        if (!own_folder(gone.props_path).empty()) {
            return false;
        }
    }
    save.delta = true;
    g_quick_saves.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::unique_ptr<Project::PendingSave> Project::capture_save(bool full, const std::vector<SaveConflict>& overwrite,
                                                            bool just_checked) {
    DataModel& world = *game_;
    auto save = std::make_unique<PendingSave>();
    save->root = root_;
    save->full = full;
    save->playing = world.simulation_running();
    // A full save writes a fresh tree; any other save keeps the disk in sync
    // only if it was in sync before. A file the disk changed and this save
    // does not touch must still be found by the next check.
    // Proven before anything is written: the disk still matches the base.
    // Just after a check that left it in sync, that check's walk is the proof.
    std::optional<DiskStamp> walked;
    const bool trusted = !full && just_checked && synced_stamp_.has_value();
    save->in_sync = trusted || disk_matches_base(&walked);
    // What the disk holds as this save begins, when in sync: kept up to date
    // with each file the save touches, it is the synced stamp afterwards,
    // without a walk.
    save->stamp = trusted ? synced_stamp_ : (save->in_sync ? walked : std::nullopt);
    save->keeps_sync = full || save->in_sync;
    // What MeshEditors hold and their files do not yet: saved with the place.
    if (!save->playing) {
        world.mesh_editors().write_all(world, false);
    }
    const AuthoredDirty dirty = world.authored_dirty();

    Layout layout;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        layout = read_project_json(root_).layout;
    }
    save->src = layout.src;

    // From the changes alone, when the disk is in sync and nothing calls for
    // the whole tree; otherwise every instance, as the whole plan has it.
    const bool quick = !full && !save->playing && save->in_sync && plan_changed(dirty, *save);
    std::vector<AuthoredNode> tree;
    if (!quick) {
        save->entries.clear();
        save->removed.clear();
        save->removed_guids.clear();
        save->ids.clear();
        save->gone_ids.clear();
        save->delta = false;
        const bool everything = full || save->playing || dirty.all;
        const std::unordered_set<InstanceId> dirty_ids(dirty.ids.begin(), dirty.ids.end());
        auto want = [&](InstanceId id) {
            if (everything || dirty_ids.count(id) != 0) {
                return true;
            }
            const auto known = id_guid_.find(id);
            return known == id_guid_.end() || known->second != world.guid(id) || files_.count(known->second) == 0;
        };
        tree = world.authored_tree(want);
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
        // claims are read from disk: a walk of every file. In sync, only a folder
        // the save moves or deletes can strand a file the base does not know, so
        // without one the walk is skipped and claims stay empty.
        const bool folder_leaves = std::any_of(files_.begin(), files_.end(), [&next](const auto& entry) {
            if (own_folder(entry.second.props_path).empty()) {
                return false;
            }
            const auto planned = next.find(entry.first);
            return planned == next.end() || planned->second.props_path != entry.second.props_path;
        });
        if (!save->in_sync || folder_leaves) {
            save->claims = guid_claims(root_, layout.src);
        }
        std::vector<SaveConflict> conflicts =
            outside_changes(tree, next, save->claims, save->left_gone, save->in_sync);
        // Only the conflicts overwrite lists are written over. Any other, as a file
        // that changed after that list was made, stops the save.
        const bool listed =
            std::all_of(conflicts.begin(), conflicts.end(), [&overwrite](const SaveConflict& conflict) {
                return std::find(overwrite.begin(), overwrite.end(), conflict) != overwrite.end();
            });
        if (!listed) {
            throw ProjectConflict(std::move(conflicts));
        }
        save->conflicts = std::move(conflicts);
        for (const auto& [guid, files] : files_) {
            if (next.count(guid) == 0) {
                save->removed.push_back(files);
                save->removed_guids.push_back(guid);
            }
        }
        save->entries.reserve(next.size());
        for (auto& [guid, files] : next) {
            PendingSave::Entry entry;
            entry.guid = guid;
            const auto base = files_.find(guid);
            entry.had = base != files_.end();
            if (entry.had) {
                entry.base = base->second;
            }
            entry.planned = std::move(files);
            save->entries.push_back(std::move(entry));
        }
        save->ids.reserve(tree.size());
        for (const AuthoredNode& node : tree) {
            save->ids.emplace_back(node.id, node.guid);
        }
    } else if (compare_check()) {
        check_quick_save(*save, dirty, layout.src);
    }

    // Files an instance keeps under resources, such as a Terrain's voxels:
    // after the gate, so a save it stops has written nothing. Each is its own
    // file, already named by the JSON, so the JSON writes below need not undo them.
    const fs::path resources = resources_root();
    if (quick) {
        std::vector<InstanceId> held;
        world.authored_ids(held);
        for (InstanceId id : held) {
            if (id == 0) {
                continue;
            }
            if (std::optional<std::string> reason = world.instance(id)->save_resources(resources)) {
                fail(*reason);
            }
        }
    } else {
        for (const AuthoredNode& node : tree) {
            if (node.id == 0) {
                continue;
            }
            std::optional<std::string> reason;
            if (world.alive(node.id)) {
                reason = world.instance(node.id)->save_resources(resources);
            } else if (save->playing && node.class_name == "Terrain") {
                // Destroyed during play, but in Play's snapshot: its voxels are
                // written from the snapshot as its JSON is.
                reason = Terrain::save_captured(resources, node.name, world.captured_place_bytes(node.id));
            }
            if (reason) {
                fail(*reason);
            }
        }
    }

    // The place as saved, from here: what changes after this is the next
    // save's. Given back if the write fails. Play leaves the edit dirty set
    // alone; Stop marks everything anyway.
    save->dirty = dirty;
    if (!save->playing) {
        world.clear_authored_dirty();
        save->cleared = true;
    }
    // Every file is written. During play that is the place as it was at Play,
    // which is all the edit stack has changed.
    world.history().mark_saved();
    return save;
}

void Project::check_quick_save(const PendingSave& save, const AuthoredDirty& dirty, const std::string& src) const {
    const DataModel& world = *game_;
    const std::unordered_set<InstanceId> dirty_ids(dirty.ids.begin(), dirty.ids.end());
    const std::vector<AuthoredNode> tree = world.authored_tree([&](InstanceId id) {
        if (dirty.all || dirty_ids.count(id) != 0) {
            return true;
        }
        const auto known = id_guid_.find(id);
        return known == id_guid_.end() || known->second != world.guid(id) || files_.count(known->second) == 0;
    });
    const std::map<std::string, Files> next = plan_files(tree, src, files_);
    std::map<std::string, const Files*> planned;
    for (const PendingSave::Entry& entry : save.entries) {
        planned.emplace(entry.guid, &entry.planned);
    }
    auto same = [](const Files& a, const Files& b) {
        return a.props_path == b.props_path && a.props_bytes == b.props_bytes && a.has_source == b.has_source &&
               a.source_path == b.source_path && (!a.has_source || a.source_bytes == b.source_bytes) &&
               a.parent == b.parent && same_extras(a.extras, b.extras);
    };
    const std::set<std::string> removed(save.removed_guids.begin(), save.removed_guids.end());
    for (const auto& [guid, files] : next) {
        const auto found = planned.find(guid);
        const auto base = files_.find(guid);
        const Files* mine = found != planned.end() ? found->second
                            : base != files_.end() && removed.count(guid) == 0 ? &base->second
                                                                                : nullptr;
        if (mine == nullptr || !same(*mine, files)) {
            contract_fail(("a save planned from the changes alone differs from a whole plan at " + guid).c_str());
        }
    }
    for (const auto& [guid, files] : planned) {
        if (next.count(guid) == 0) {
            contract_fail(("a save planned from the changes alone writes " + guid + ", which a whole plan does not")
                              .c_str());
        }
    }
    for (const auto& [guid, files] : files_) {
        if ((next.count(guid) == 0) != (removed.count(guid) != 0)) {
            contract_fail(("a save planned from the changes alone and a whole plan differ on removing " + guid)
                              .c_str());
        }
    }
}

void Project::write_save(PendingSave& save) {
    const fs::path& root = save.root;
    std::optional<DiskStamp>& stamp = save.stamp;
    SaveReport& report = save.report;
    std::error_code error;
    std::set<std::string> vacated;
    auto note_vacated = [&vacated](const std::string& path) {
        const std::size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            vacated.insert(path.substr(0, slash));
        }
    };

    // A save is all or nothing. Every move, write, and removal is noted, so a
    // failure partway puts the disk back as the last save left it. A partial save
    // could leave a folder without its init.json, or two files claiming one GUID,
    // and the project would no longer load.
    struct Undo {
        enum class Kind { Moved, Created, Replaced, Removed };
        Kind kind = Kind::Created;
        fs::path path;
        // Moved: where the file was.
        fs::path from;
        // Replaced and Removed: what the file held.
        std::string bytes;
    };
    std::vector<Undo> undo;
    std::vector<fs::path> made_dirs;
    auto note_new_dirs = [&](const fs::path& target) {
        for (fs::path dir = target.parent_path(); !dir.empty() && !fs::exists(dir, error); dir = dir.parent_path()) {
            made_dirs.push_back(dir);
        }
    };
    auto put_back = [&] {
        // Each step is tried even when one before it failed.
        std::error_code ignored;
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
            if (it->kind == Undo::Kind::Moved) {
                fs::rename(it->path, it->from, ignored);
            } else if (it->kind == Undo::Kind::Created) {
                fs::remove(it->path, ignored);
            } else {
                try {
                    write_file(it->path, it->bytes);
                } catch (const ProjectError&) {
                    // Later steps may still work. The save reports its own error.
                }
            }
        }
        std::sort(made_dirs.begin(), made_dirs.end(),
                  [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
        for (const fs::path& dir : made_dirs) {
            fs::remove(dir, ignored);
        }
    };
    // In sync, every file the save writes over, moves, or deletes must still
    // be as the stamp has it. A racy one proves itself by its bytes.
    auto still = [&](const std::string& path, const std::string* base_bytes) {
        if (!stamp) {
            return;
        }
        const fs::path target = disk_path(root, path);
        const auto known = stamp->files.find(path);
        std::error_code status;
        // fs::status, not a directory_entry: MSVC's entry for a missing path
        // reports no error and a regular file.
        const bool there = fs::is_regular_file(fs::status(target, status)) && !status;
        if (known == stamp->files.end()) {
            if (there) {
                throw StaleStamp{};
            }
            return;
        }
        if (!there) {
            throw StaleStamp{};
        }
        const std::uintmax_t size = fs::file_size(target, status);
        const fs::file_time_type written = status ? fs::file_time_type() : fs::last_write_time(target, status);
        if (status || size != known->second.size ||
            static_cast<std::int64_t>(written.time_since_epoch().count()) != known->second.written) {
            throw StaleStamp{};
        }
        if (std::binary_search(stamp->racy.begin(), stamp->racy.end(), path) &&
            (base_bytes == nullptr || read_file(target) != *base_bytes)) {
            throw StaleStamp{};
        }
    };
    // What a file the save wrote now is on disk, for the stamp.
    auto restamp = [&](const std::string& path) {
        if (stamp) {
            save.stamped.push_back(path);
        }
    };
    auto unstamp = [&](const std::string& path) {
        if (stamp) {
            stamp->files.erase(path);
        }
    };
    // Each removed file's base bytes, by path.
    std::unordered_map<std::string_view, const std::string*> bytes_at;
    for (const Files& files : save.removed) {
        bytes_at.emplace(files.props_path, &files.props_bytes.str());
        if (files.has_source) {
            bytes_at.emplace(files.source_path, &files.source_bytes.str());
        }
        for (const Files::Extra& extra : files.extras) {
            if (!extra.path.empty()) {
                bytes_at.emplace(extra.path, &extra.bytes.str());
            }
        }
    }
    // An extra stage file a kept instance no longer has (its text emptied).
    for (const PendingSave::Entry& entry : save.entries) {
        if (!entry.had) {
            continue;
        }
        for (std::size_t i = 0; i < entry.base.extras.size(); ++i) {
            const Files::Extra& extra = entry.base.extras[i];
            if (!extra.path.empty() && (i >= entry.planned.extras.size() || entry.planned.extras[i].path.empty())) {
                bytes_at.emplace(extra.path, &extra.bytes.str());
            }
        }
    }
    auto place = [&](const std::string& old_path, const std::string& old_bytes, bool had, const std::string& path,
                     const std::string& bytes) {
        const fs::path target = disk_path(root, path);
        bool present = false;
        bool moved_now = false;
        if (had && old_path != path && fs::exists(disk_path(root, old_path), error)) {
            still(old_path, &old_bytes);
            still(path, nullptr);
            moved_now = true;
            note_new_dirs(target);
            move_file(disk_path(root, old_path), target);
            unstamp(old_path);
            restamp(path);
            undo.push_back(Undo{Undo::Kind::Moved, target, disk_path(root, old_path), std::string()});
            report.moved.push_back(old_path + " -> " + path);
            note_vacated(old_path);
            present = true;
        } else {
            present = had && fs::exists(target, error);
        }
        if (!present || old_bytes != bytes) {
            if (!moved_now) {
                still(path, had && old_path == path ? &old_bytes : nullptr);
            }
            Undo step;
            step.path = target;
            if (fs::exists(target, error)) {
                step.kind = Undo::Kind::Replaced;
                step.bytes = read_file(target);
            }
            note_new_dirs(target);
            write_file(target, bytes);
            restamp(path);
            undo.push_back(std::move(step));
            report.written.push_back(path);
        }
    };
    auto remove_path = [&](const std::string& path) {
        const fs::path target = disk_path(root, path);
        if (stamp) {
            const auto base = bytes_at.find(path);
            still(path, base != bytes_at.end() ? base->second : nullptr);
        }
        Undo step;
        step.kind = Undo::Kind::Removed;
        step.path = target;
        step.bytes = read_file(target);
        std::error_code failure;
        fs::remove(target, failure);
        if (failure) {
            fail("cannot remove " + utf8_path(target) + ": " + failure.message());
        }
        undo.push_back(std::move(step));
        report.removed.push_back(path);
        unstamp(path);
        note_vacated(path);
    };
    try {
        for (const PendingSave::Entry& entry : save.entries) {
            if (save.left_gone.count(entry.guid) != 0) {
                // Gone from disk while the studio left it alone: the save leaves it gone.
                continue;
            }
            const Files& files = entry.planned;
            const Files& old = entry.base;
            const bool had = entry.had;
            // In sync, a file the base has at the same path with the same bytes
            // is already on disk as written: placing it would only stat it.
            const bool props_settled = save.in_sync && had && old.props_path == files.props_path &&
                                       old.props_bytes == files.props_bytes;
            if (!props_settled) {
                place(had ? old.props_path : std::string(), had ? old.props_bytes.str() : std::string(), had,
                      files.props_path, files.props_bytes);
            }
            if (files.has_source) {
                const bool had_source = had && old.has_source;
                const bool source_settled = save.in_sync && had_source && old.source_path == files.source_path &&
                                            old.source_bytes == files.source_bytes;
                if (!source_settled) {
                    place(had_source ? old.source_path : std::string(),
                          had_source ? old.source_bytes.str() : std::string(), had_source, files.source_path,
                          files.source_bytes);
                }
            }
            // Each extra stage: placed while it has text, removed once it has none.
            const std::size_t stages = std::max(files.extras.size(), had ? old.extras.size() : 0);
            for (std::size_t i = 0; i < stages; ++i) {
                static const Files::Extra kNone;
                const Files::Extra& planned_extra = i < files.extras.size() ? files.extras[i] : kNone;
                const Files::Extra& old_extra = had && i < old.extras.size() ? old.extras[i] : kNone;
                const bool had_extra = had && !old_extra.path.empty();
                if (planned_extra.path.empty()) {
                    if (had_extra && fs::exists(disk_path(root, old_extra.path), error)) {
                        remove_path(old_extra.path);
                    }
                    continue;
                }
                const bool settled = save.in_sync && had_extra && old_extra == planned_extra;
                if (!settled) {
                    place(had_extra ? old_extra.path : std::string(), had_extra ? old_extra.bytes.str() : std::string(),
                          had_extra, planned_extra.path, planned_extra.bytes);
                }
            }
        }
        // Overwrite: a GUID the save wrote over a conflict keeps only the files it
        // wrote. Any other file claiming it, as one moved outside, would load as a
        // second instance with the same GUID.
        if (!save.conflicts.empty()) {
            std::map<std::string, const Files*> planned_of;
            for (const PendingSave::Entry& entry : save.entries) {
                planned_of.emplace(entry.guid, &entry.planned);
            }
            for (const SaveConflict& conflict : save.conflicts) {
                const auto claimed = save.claims.find(conflict.guid);
                if (claimed == save.claims.end()) {
                    continue;
                }
                const auto planned = planned_of.find(conflict.guid);
                // The same file, by name or by the file system: a name that differs
                // only in case is one file on macOS and Windows.
                auto wrote = [&](const fs::path& target, const std::string& path) {
                    return !path.empty() && fs::equivalent(target, disk_path(root, path), error);
                };
                for (const std::string& path : claimed->second) {
                    const fs::path target = disk_path(root, path);
                    if (planned != planned_of.end() &&
                        (path == planned->second->props_path || path == planned->second->source_path ||
                         wrote(target, planned->second->props_path) || wrote(target, planned->second->source_path) ||
                         std::any_of(planned->second->extras.begin(), planned->second->extras.end(),
                                     [&](const Files::Extra& extra) {
                                         return !extra.path.empty() && (path == extra.path || wrote(target, extra.path));
                                     }))) {
                        continue;
                    }
                    if (fs::exists(target, error)) {
                        remove_path(path);
                    }
                }
            }
        }
        for (const Files& files : save.removed) {
            std::vector<const std::string*> own{&files.props_path, &files.source_path};
            for (const Files::Extra& extra : files.extras) {
                own.push_back(&extra.path);
            }
            for (const std::string* path : own) {
                if (path->empty()) {
                    continue;
                }
                if (fs::exists(disk_path(root, *path), error)) {
                    remove_path(*path);
                } else {
                    note_vacated(*path);
                }
            }
        }
    } catch (...) {
        put_back();
        throw;
    }

    // Folders emptied by a move or a delete go too, up to src/.
    for (auto it = vacated.rbegin(); it != vacated.rend(); ++it) {
        std::string dir = *it;
        while (!dir.empty() && dir != save.src && dir.size() > save.src.size()) {
            const fs::path path = disk_path(root, dir);
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
    std::sort(report.written.begin(), report.written.end());
    std::sort(report.moved.begin(), report.moved.end());
    std::sort(report.removed.begin(), report.removed.end());

    // In sync from the start, the stamp follows every file the save touched,
    // each now racy as just written. A full save walks afterwards instead.
    if (stamp && !save.full) {
        const fs::file_time_type racy_from = fs::file_time_type::clock::now() - std::chrono::seconds(kRacySeconds);
        for (const std::string& path : save.stamped) {
            std::error_code status;
            const fs::path target = disk_path(root, path);
            const std::uintmax_t size = fs::file_size(target, status);
            const fs::file_time_type written = status ? fs::file_time_type() : fs::last_write_time(target, status);
            if (status) {
                stamp.reset();
                break;
            }
            stamp->files[path] =
                DiskStamp::Entry{size, static_cast<std::int64_t>(written.time_since_epoch().count())};
            if (written >= racy_from) {
                stamp->racy.push_back(path);
            }
        }
        if (stamp) {
            std::sort(stamp->racy.begin(), stamp->racy.end());
            stamp->racy.erase(std::unique(stamp->racy.begin(), stamp->racy.end()), stamp->racy.end());
            stamp->racy.erase(std::remove_if(stamp->racy.begin(), stamp->racy.end(),
                                             [&](const std::string& path) { return stamp->files.count(path) == 0; }),
                              stamp->racy.end());
        }
    }
}

void Project::finish_save(PendingSave& save, bool written) {
    DataModel& world = *game_;
    if (!written) {
        // Nothing was written: what the capture took is unsaved again.
        if (save.cleared) {
            if (save.dirty.all) {
                world.mark_all_authored_dirty();
            }
            for (InstanceId id : save.dirty.ids) {
                world.mark_authored_dirty(id);
            }
        }
        world.history().mark_dirty();
        return;
    }
    if (save.delta) {
        for (PendingSave::Entry& entry : save.entries) {
            files_[entry.guid] = std::move(entry.planned);
        }
        for (const std::string& guid : save.removed_guids) {
            files_.erase(guid);
        }
        for (InstanceId id : save.gone_ids) {
            const auto known = id_guid_.find(id);
            if (known != id_guid_.end()) {
                const auto back = guid_id_.find(known->second);
                if (back != guid_id_.end() && back->second == id) {
                    guid_id_.erase(back);
                }
                id_guid_.erase(known);
            }
        }
        for (const auto& [id, guid] : save.ids) {
            id_guid_[id] = guid;
            guid_id_[guid] = id;
        }
    } else {
        std::unordered_map<std::string, Files> base;
        base.reserve(save.entries.size());
        for (PendingSave::Entry& entry : save.entries) {
            base.emplace(std::move(entry.guid), std::move(entry.planned));
        }
        files_ = std::move(base);
        id_guid_.clear();
        guid_id_.clear();
        for (const auto& [id, guid] : save.ids) {
            id_guid_[id] = guid;
            guid_id_[guid] = id;
        }
    }
    last_save_ = std::move(save.report);
    // The base is what was just written: in sync, if the disk was before.
    if (save.stamp && !save.full) {
        synced_stamp_ = std::move(save.stamp);
    } else {
        synced_stamp_ = save.keeps_sync ? disk_stamp() : std::nullopt;
    }
}

}  // namespace engine_core
