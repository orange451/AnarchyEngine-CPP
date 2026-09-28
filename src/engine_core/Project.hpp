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
    };
    std::string guid;
    // As last loaded or saved: relative to the project root, written with '/'.
    std::string path;
    Kind kind = Kind::EditedOutside;
};

// "src/Part.3f2a.json changed on disk", "... was deleted on disk", or
// "... was moved or renamed on disk".
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

// Guarded stops at files changed on disk. Overwrite writes the studio's version anyway.
enum class SaveMode { Guarded, Overwrite };

// Classes a project file may name. The built-ins are DataModel, GameObject,
// Script, ModuleScript, Folder, and TestTriangle. A later class registers here.
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

    Project(Project&&) noexcept;
    Project& operator=(Project&&) noexcept;
    ~Project();

    // Edit mode writes the live tree. Play writes the place snapshot, never
    // an instance created during play. Only files whose bytes changed are
    // written. A renamed or moved instance's files move; a destroyed one's are deleted.
    // A guarded save first compares every file it would write or delete with
    // the last load or save, and throws ProjectConflict, writing nothing, when
    // one changed on disk.
    void save(SaveMode mode = SaveMode::Guarded);
    // Writes the whole project under a new root, copies resources/, and binds there.
    void save_as(const std::filesystem::path& root);

    // A hash of every path and every byte a save of this place would write.
    // Equal fingerprints mean a save has nothing to write, so an edit that is
    // undone back to the saved state no longer counts. Needs no project folder.
    // During play it covers the place captured at Test, like a save does.
    static std::uint64_t place_fingerprint(const DataModel& game);
    // File > New: stops a running simulation, destroys every instance, gives the
    // root a fresh GUID, captures the empty place, and drops undo history.
    static void reset_place(DataModel& game);

    const std::filesystem::path& root() const { return root_; }
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

    // Files on disk for one instance, as last loaded or written.
    struct Files {
        std::string props_path;
        std::string props_bytes;
        bool has_source = false;
        std::string source_path;
        std::string source_bytes;
    };

    void bind(DataModel* game, std::unique_ptr<DataModel> owned);
    void write_skeleton(const std::filesystem::path& root) const;
    void save_tree(bool full, SaveMode mode = SaveMode::Guarded);
    // The files next would write over or delete that changed on disk since the
    // last load or save, sorted by path.
    std::vector<SaveConflict> outside_changes(const std::map<std::string, Files>& next) const;
    // The files for each GUID. A node without properties takes its bytes from cache.
    static std::map<std::string, Files> plan_files(const std::vector<AuthoredNode>& tree, const std::string& src,
                                                   const std::unordered_map<std::string, Files>& cache);

    std::filesystem::path root_;
    std::string name_;
    std::unique_ptr<DataModel> owned_;
    DataModel* game_ = nullptr;
    std::unordered_map<std::string, Files> files_;
    std::unordered_map<InstanceId, std::string> id_guid_;
    std::unordered_map<std::string, InstanceId> guid_id_;
    SaveReport last_save_;
};

}  // namespace engine_core
