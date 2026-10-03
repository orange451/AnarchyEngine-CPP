#pragma once

#include "DataModel.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine_core {

namespace detail {
struct PlanNode;
}  // namespace detail

// Every load and save failure. The message names the file.
class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A file that changed on disk since the last load or save, which a save would
// have written over, deleted, or put back.
struct SaveConflict {
    enum class Kind {
        EditedOutside,   // its bytes differ from the last load or save
        DeletedOutside,  // it is gone, and no other file claims its GUID
        MovedOutside,    // it is gone, and a file elsewhere claims its GUID
        AddedOutside,    // it names an instance the studio does not know, inside a folder the save moves or deletes
    };
    std::string guid;
    // As last loaded or saved: relative to the project root, written with '/'.
    // An added file's path is where it is now.
    std::string path;
    Kind kind = Kind::EditedOutside;
    // What the row is about: a key of the instance's file, "Parent", "Source",
    // or "class". Empty for the whole instance.
    std::string key;
    // Each side as a person reads it: a value, or a word such as "deleted".
    std::string studio;
    std::string disk;
    // The instance's Name, and where it sits, as the explorers show them
    // ("game.Box"). Empty when neither side has it.
    std::string name;
    std::string where;
};

inline bool operator==(const SaveConflict& a, const SaveConflict& b) {
    return a.guid == b.guid && a.path == b.path && a.kind == b.kind && a.key == b.key && a.studio == b.studio &&
           a.disk == b.disk && a.name == b.name && a.where == b.where;
}

// "src/Part.3f2a.json changed on disk", "... was deleted on disk",
// "... was moved or renamed on disk", or "... was added on disk".
std::string describe_conflict(const SaveConflict& conflict);

// Thrown by a guarded save before it touches disk. Sorted by path; the message
// names the first.
class ProjectConflict : public ProjectError {
public:
    explicit ProjectConflict(std::vector<SaveConflict> conflicts);
    const std::vector<SaveConflict>& conflicts() const { return conflicts_; }

private:
    std::vector<SaveConflict> conflicts_;
};

// What the disk holds that the place does not, against the last load or save.
struct DiskScan {
    // Instances apply_disk changed from the disk, by Name. Empty from scan_disk.
    std::vector<std::string> loaded;
    // Choices apply_disk did not apply: their row changed since it was listed.
    std::vector<SaveConflict> skipped;
    // Rows that need a person's choice, by where, name, then key.
    std::vector<SaveConflict> conflicts;
    // True when the disk has changes the studio did not make, which apply_disk loads.
    bool has_disk_changes = false;
};

// A person's pick for one row: disk true takes the disk's side.
struct DiskChoice {
    SaveConflict conflict;
    bool disk = false;
};

// Classes a project file may name. The built-ins are DataModel, GameObject,
// Camera, PointLight, SpotLight, DirectionalLight, Script, ModuleScript, Folder, the asset classes, and the scene services,
// whose factory gives the world's own service back at its defaults. A later
// class registers here.
using ProjectFactory = DataModel& (*)(DataModel& world);
void register_project_class(const char* class_name, ProjectFactory factory);
bool project_class_known(std::string_view class_name);

// One filename component from a Name. Characters illegal on Windows, macOS,
// or Linux become '_'. Empty, ".", and ".." become "_". A leading '.' becomes
// '_' so the file is not hidden. A Windows device name gains a leading '_'.
// Long names are cut on a UTF-8 boundary. The Name itself is never changed.
std::string sanitize_file_name(std::string_view name);

// A directory on disk is the source of truth. The DataModel is a working copy
// of src/. Each instance is a file named <SanitizedName>.<guid>, so siblings
// may share a Name and adding one never renames another.
//
//   project.json     format, name, tree root, resources root
//   src/init.json    the root DataModel
//   src/X.<guid>.json                 a leaf instance
//   src/X.<guid>/init.json            an instance with children, inside that folder
//   src/X.<guid>.luau + .meta.json    a Script or ModuleScript leaf
//   src/X.<guid>/init.luau + init.meta.json   a script with children
//   resources/textures|meshes|audio   files that instances name by relative path
//
// Every call runs on the thread that may mutate the DataModel: the caller when
// the engine threads are stopped, otherwise inside Engine::on_simulation.
class Project {
public:
    // Writes project.json, src/init.json, resources/<kind>/.gitkeep, .gitignore,
    // .gitattributes, and RESOURCES.md. root must be missing or an empty directory.
    // The bound DataModel is cleared first.
    static Project create(const std::filesystem::path& root);
    static Project create(const std::filesystem::path& root, DataModel& into);
    // Stops a running simulation, rebuilds the DataModel from src/, captures
    // that as the place, and drops undo history. On error the DataModel is untouched.
    // Save As for a place that has no project yet: writes the skeleton and the
    // current tree. The DataModel is not cleared and its undo history stays.
    static Project adopt(const std::filesystem::path& root, DataModel& game);
    static Project load(const std::filesystem::path& root);
    static Project load(const std::filesystem::path& root, DataModel& into);

    Project(Project&&);
    Project& operator=(Project&&);
    ~Project();

    // Edit mode writes the live tree. Play writes the place snapshot, never
    // an instance created during play. Only files whose bytes changed are
    // written. A renamed or moved instance's files move; a destroyed one's are deleted.
    // A save first compares every file it would write or delete with the last
    // load or save. One that changed on disk stops the save with
    // ProjectConflict, writing nothing, unless overwrite lists that conflict:
    // then the studio's version is written over it.
    void save(const std::vector<SaveConflict>& overwrite = {});
    // Writes the whole project under a new root, copies resources/, and binds there.
    void save_as(const std::filesystem::path& root);

    // A hash of every path and every byte a save of this place would write.
    // Equal fingerprints mean a save has nothing to write, so an edit that is
    // undone back to the saved state no longer counts. Needs no project folder.
    // During play it covers the place captured at Test, like a save does.
    static std::uint64_t place_fingerprint(const DataModel& game);
    // A save would write, move, or remove something: the place differs from the
    // last load or save, compared key by key, so a file on disk that is only
    // formatted differently does not count.
    bool unsaved() const;
    // Reads src/ and compares it, key by key, with the last load or save and
    // with the place. Touches nothing. Throws ProjectError when src/ does not
    // read as a project, or holds a value its class rejects.
    DiskScan scan_disk() const;
    // Edit mode. Loads every change only the disk made, and the disk's side of
    // each choice, into the place as one undo step named "Changes from Disk".
    // The studio's side of a choice is settled in the base, so the next save
    // writes the studio's value. Scans first: a choice whose row changed since
    // it was listed is skipped, and listed again. Then the base takes the
    // disk's files wherever no row is left. Returns the rows still open.
    DiskScan apply_disk(const std::vector<DiskChoice>& choices = {});
    // File > New: stops a running simulation, destroys every instance, gives the
    // root a fresh GUID, captures the empty place, and drops undo history.
    static void reset_place(DataModel& game);
    // The Camera a new place starts with, in Workspace: 3 up and 7 back from
    // the origin, looking at it, 60 degrees high: the view the Scene View had
    // before there were Cameras. create and reset_place add it. Returns its id.
    static InstanceId add_default_camera(DataModel& game);

    const std::filesystem::path& root() const { return root_; }
    // Where Mesh, Texture, and Sound Paths point: project.json's resources root.
    std::filesystem::path resources_root() const;
    // Where the instance files are: project.json's tree root, src/ unless it names another.
    std::filesystem::path tree_root() const;
    const std::string& name() const { return name_; }
    DataModel& datamodel() { return *game_; }
    const DataModel& datamodel() const { return *game_; }

    // The runtime id for a GUID as of the last load or save.
    std::optional<InstanceId> instance_for(std::string_view guid) const;

    // Paths relative to root, written with '/'. Sorted.
    struct SaveReport {
        std::vector<std::string> written;
        std::vector<std::string> moved;    // "from -> to"
        std::vector<std::string> removed;
    };
    const SaveReport& last_save() const { return last_save_; }

private:
    Project();

    // Tells the bound game where its resources folder is now.
    void publish_resources_root() const;

    // Files on disk for one instance, as last loaded or written.
    struct Files {
        std::string props_path;
        std::string props_bytes;
        bool has_source = false;
        std::string source_path;
        std::string source_bytes;
        // The properties file parsed: the base each key is compared against. A
        // key settled for the studio's side is patched here, and props_bytes
        // rewritten to match.
        JsonValue props;
        // The parent's GUID. Empty for the root.
        std::string parent;
    };

    void bind(DataModel* game, std::unique_ptr<DataModel> owned);
    // Reads the project at root into the bound world. replace clears a world
    // that already has content, after a dry run so a bad file leaves it as it was.
    void read_into_game(const std::filesystem::path& root, bool replace);
    void write_skeleton(const std::filesystem::path& root) const;
    void save_tree(bool full, const std::vector<SaveConflict>& overwrite = {});
    // The files next would write over, delete, or put back that changed on disk
    // since the last load or save, and files it does not know inside a folder
    // it moves or deletes, sorted by path. A GUID whose files are all gone while
    // the studio left it alone goes in left_gone instead: the save leaves it
    // gone. claims is every src/ file by the GUID in its name.
    std::vector<SaveConflict> outside_changes(const std::vector<AuthoredNode>& tree,
                                              const std::map<std::string, Files>& next,
                                              const std::map<std::string, std::vector<std::string>>& claims,
                                              std::set<std::string>& left_gone) const;
    // Base, disk, and studio side by side, and what differs. Defined in Project.cpp.
    struct Comparison;
    Comparison compare_disk() const;
    // The studio's side of a row: the base takes the disk's value there.
    void settle(const Comparison& compared, const SaveConflict& conflict);
    // The changes only the disk made, and the disk's side of chosen rows.
    void apply_changes(const Comparison& compared, std::vector<std::string>& loaded);
    // After an apply: the base takes what the disk had when before was read
    // and still has in after, wherever after holds nothing open or pending.
    void refresh_base(const Comparison& before, const Comparison& after);
    // The files for each GUID. A node without properties takes its bytes from cache.
    static std::map<std::string, Files> plan_files(const std::vector<AuthoredNode>& tree, const std::string& src,
                                                   const std::unordered_map<std::string, Files>& cache);
    // An instance's files as the disk has them, parent its parent's GUID, and
    // props its file as the class stores it.
    static Files from_disk(const detail::PlanNode& node, std::string parent, JsonValue props);
    // The base takes node's files from disk. A scene service the read made has
    // none: the base has no entry for it, so a save writes it as new.
    void take_from_disk(const detail::PlanNode& node, std::string parent, JsonValue props);

    std::filesystem::path root_;
    std::string name_;
    // The tree folder project.json named at the load; create and Save As write "src".
    std::string src_ = "src";
    // project.json's resources.root, relative to root_.
    std::string resources_ = "resources";
    std::unique_ptr<DataModel> owned_;
    DataModel* game_ = nullptr;
    std::unordered_map<std::string, Files> files_;
    std::unordered_map<InstanceId, std::string> id_guid_;
    std::unordered_map<std::string, InstanceId> guid_id_;
    SaveReport last_save_;
};

}  // namespace engine_core
