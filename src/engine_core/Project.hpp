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
struct PlanReaderNote;
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

    // The most instances a project read may hold: DataModel::kMaxInstances
    // unless lowered. Reading more is a ProjectError, never a full world.
    // Process-wide; for tests that check that limit and hosts that want a
    // smaller one.
    static std::size_t instance_limit();
    static void set_instance_limit(std::size_t limit);
    // On, every disk check that reads only the files that changed also reads
    // the whole tree and fails, with a contract violation, unless both find
    // the same rows and actions. The sandbox turns it on; so does
    // ANARCHY_VERIFY_DISK_COMPARE=1 in the studio.
    static void set_compare_check(bool on);
    // How many disk checks read only the changed files, since the program started. For tests.
    static std::size_t partial_reads();
    // How many saves were planned from the changes alone, since the program started. For tests.
    static std::size_t quick_saves();

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
    // just_checked: a disk check (scan_disk or apply_disk) ran just before,
    // with nothing in between, so the save takes the stamp it walked instead
    // of walking the tree again. Every file the save writes over, moves, or
    // deletes is still checked against that stamp just before it is touched.
    void save(const std::vector<SaveConflict>& overwrite = {}, bool just_checked = false);

    // A save that writes off the place's thread. On any thread, walk_disk
    // reads the tree's directory entries. Then, with the place to itself,
    // apply_disk with that walk loads what changed only on disk (and finds
    // rows, which stop the save), and begin_save takes what the save writes.
    // write_save then writes it on any thread, while the place goes on, and
    // end_save makes the base what was written, or gives the changes back as
    // unsaved when nothing was. Nothing else may use the project in between.
    class Walk;
    class SaveJob;
    enum class Written { Yes, Stale, Failed };
    Walk walk_disk() const;
    DiskScan apply_disk(const std::vector<DiskChoice>& choices, const Walk& walk);
    SaveJob begin_save();
    // Stale: a file the save was to touch changed since the walk; nothing was
    // written. A save with save() then finds it as a conflict. Failed: error says why.
    static Written write_save(SaveJob& job, std::string& error);
    void end_save(SaveJob& job, Written written);
    // Writes the whole project under a new root, copies resources/, and binds there.
    void save_as(const std::filesystem::path& root);

    // A hash of every path and every byte a save of this place would write.
    // Equal fingerprints mean a save has nothing to write, so an edit that is
    // undone back to the saved state no longer counts. Needs no project folder.
    // During play it covers the place captured at Test, like a save does.
    static std::uint64_t place_fingerprint(const DataModel& game);
    // The place has changed since the last load or save: ChangeHistoryService::dirty().
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
    // Every place service back at its class defaults, with no extras; their
    // children stay. A client clears its world with this before it connects,
    // since a snapshot carries only the service values that differ from them.
    static void reset_services(DataModel& game);
    // The Camera a new place starts with, in Workspace: 9 up and 21 back from
    // the origin, looking at it, 60 degrees high: the view the Scene View had
    // before there were Cameras. create and reset_place add it. Returns its id.
    static InstanceId add_default_camera(DataModel& game);
    // The DynamicSky a new place starts with, in Lighting, as it is made.
    // create and reset_place add it. Returns its id.
    static InstanceId add_default_light(DataModel& game);

    const std::filesystem::path& root() const { return root_; }
    // Where Mesh, Texture, and Sound Paths point: project.json's resources root.
    std::filesystem::path resources_root() const;
    // Where the instance files are: project.json's tree root, src/ unless it names another.
    std::filesystem::path tree_root() const;
    const std::string& name() const { return name_; }
    // project.json's gameId: 32 hex characters that name this game to the
    // network handshake. Made at create, added at load to a project without
    // one, kept by Save As.
    const std::string& game_id() const { return game_id_; }
    // project.json's version, "1.0.0" by default. The user will set it.
    const std::string& version() const { return version_; }
    DataModel& datamodel() { return *game_; }
    const DataModel& datamodel() const { return *game_; }
    // Each instance's GUID and its properties file, as last loaded or saved. For benchmarks.
    std::vector<std::pair<std::string, std::string>> base_paths_for_bench() const;

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

    // A file's bytes, held once and shared by every copy. A save plans every
    // instance's files from the base; sharing makes that a count per
    // instance, not a copy of the whole project.
    class SharedText {
    public:
        SharedText() = default;
        SharedText(std::string text) : text_(std::make_shared<const std::string>(std::move(text))) {}
        SharedText& operator=(std::string text) {
            text_ = std::make_shared<const std::string>(std::move(text));
            return *this;
        }
        const std::string& str() const { return text_ ? *text_ : none(); }
        // The bytes as held, for a reader on another thread. Null when empty.
        const std::shared_ptr<const std::string>& shared() const { return text_; }
        operator const std::string&() const { return str(); }
        bool empty() const { return str().empty(); }
        std::size_t size() const { return str().size(); }
        // The same bytes: at once when both share them.
        friend bool operator==(const SharedText& a, const SharedText& b) {
            return a.text_ == b.text_ || a.str() == b.str();
        }
        friend bool operator!=(const SharedText& a, const SharedText& b) { return !(a == b); }
        friend bool operator==(const SharedText& a, const std::string& b) { return a.str() == b; }
        friend bool operator!=(const SharedText& a, const std::string& b) { return a.str() != b; }
        friend bool operator==(const std::string& a, const SharedText& b) { return a == b.str(); }
        friend bool operator!=(const std::string& a, const SharedText& b) { return a != b.str(); }

    private:
        static const std::string& none() {
            static const std::string nothing;
            return nothing;
        }
        std::shared_ptr<const std::string> text_;
    };
    // A parsed file, shared the same way: a change makes this copy its own.
    class SharedJson {
    public:
        SharedJson() = default;
        SharedJson(JsonValue doc) : doc_(std::make_shared<JsonValue>(std::move(doc))) {}
        SharedJson& operator=(JsonValue doc) {
            doc_ = std::make_shared<JsonValue>(std::move(doc));
            return *this;
        }
        const JsonValue& get() const { return doc_ ? *doc_ : none(); }
        operator const JsonValue&() const { return get(); }
        const JsonValue* find(std::string_view key) const { return get().find(key); }
        const std::vector<JsonValue::Member>& members() const { return get().members(); }
        void set(std::string key, JsonValue value) { own().set(std::move(key), std::move(value)); }
        bool erase(std::string_view key) { return own().erase(key); }

    private:
        JsonValue& own() {
            if (!doc_ || doc_.use_count() > 1) {
                doc_ = std::make_shared<JsonValue>(get());
            }
            return *doc_;
        }
        static const JsonValue& none() {
            static const JsonValue nothing;
            return nothing;
        }
        std::shared_ptr<JsonValue> doc_;
    };

    // Files on disk for one instance, as last loaded or written.
    struct Files {
        std::string props_path;
        SharedText props_bytes;
        bool has_source = false;
        std::string source_path;
        SharedText source_bytes;
        // Stages past the first (a Shader's vertex text), stage 1 first: each
        // its path and bytes; one with no text has an empty path and no file.
        struct Extra {
            std::string path;
            SharedText bytes;
            friend bool operator==(const Extra& a, const Extra& b) { return a.path == b.path && a.bytes == b.bytes; }
            friend bool operator!=(const Extra& a, const Extra& b) { return !(a == b); }
        };
        std::vector<Extra> extras;
        // The properties file parsed: the base each key is compared against. A
        // key settled for the studio's side is patched here, and props_bytes
        // rewritten to match.
        SharedJson props;
        // The parent's GUID. Empty for the root.
        std::string parent;
    };

    void bind(DataModel* game, std::unique_ptr<DataModel> owned);
    // Reads the project at root into the bound world. replace clears a world
    // that already has content, after a dry run so a bad file leaves it as it was.
    void read_into_game(const std::filesystem::path& root, bool replace);
    void write_skeleton(const std::filesystem::path& root);
    void save_tree(bool full, const std::vector<SaveConflict>& overwrite = {}, bool just_checked = false);
    struct PendingSave;
    // A save in three steps. capture_save takes from the place everything the
    // save writes, and runs the conflict gate; write_save touches only the
    // disk, all or nothing; finish_save makes the base what was written, or,
    // when nothing was, gives the place's changes back as unsaved.
    std::unique_ptr<PendingSave> capture_save(bool full, const std::vector<SaveConflict>& overwrite,
                                              bool just_checked);
    static void write_save(PendingSave& save);
    void finish_save(PendingSave& save, bool written);
    // The save planned from the changes alone: what the studio changed since
    // the last save, what is gone, and the files under anything whose files
    // moved. False, for a whole plan, when that cannot be done exactly.
    bool plan_changed(const AuthoredDirty& dirty, PendingSave& save) const;
    // With the compare check on: a contract violation unless a whole plan
    // writes, moves, and removes exactly what save does.
    void check_quick_save(const PendingSave& save, const AuthoredDirty& dirty, const std::string& src) const;
    // The files next would write over, delete, or put back that changed on disk
    // since the last load or save, and files it does not know inside a folder
    // it moves or deletes, sorted by path. A GUID whose files are all gone while
    // the studio left it alone goes in left_gone instead: the save leaves it
    // gone. claims is every src/ file by the GUID in its name. in_sync: the
    // disk is proven to match the base (disk_matches_base), so every base
    // file is present with its base bytes and only files the base does not
    // know can matter.
    std::vector<SaveConflict> outside_changes(const std::vector<AuthoredNode>& tree,
                                              const std::map<std::string, Files>& next,
                                              const std::map<std::string, std::vector<std::string>>& claims,
                                              std::set<std::string>& left_gone, bool in_sync) const;
    // Base, disk, and studio side by side, and what differs. Defined in Project.cpp.
    // Every file under the tree root, and project.json: each path with its
    // size and modified time. Two stamps with the same entries mean no file
    // was added, removed, or rewritten in between, and the entries that differ
    // name the files that were. A file
    // modified within kRacySeconds of the walk is "racy": the clock may not
    // have moved between two writes, so its stamp cannot prove it unchanged
    // (git's racy-clean problem); racy files are checked by their bytes.
    struct DiskStamp {
        // One file as the walk saw it.
        struct Entry {
            std::uintmax_t size = 0;
            std::int64_t written = 0;
            bool operator==(const Entry& other) const { return size == other.size && written == other.written; }
            bool operator!=(const Entry& other) const { return !(*this == other); }
        };
        // Every file, by path relative to root, with '/'.
        std::unordered_map<std::string, Entry> files;
        // Paths relative to root, with '/', sorted.
        std::vector<std::string> racy;
    };
    static constexpr int kRacySeconds = 2;
    // A load writes the load pack (ProjectPack.hpp) when it read this many
    // files from disk: the first load, or once edits outside the pack add up.
    // A tree smaller than kPackMinFiles reads fast enough without one.
    static constexpr std::size_t kPackRewrite = 256;
    static constexpr std::size_t kPackMinFiles = 1000;
    // The stamp from the files the load's read noted, with project.json.
    std::optional<DiskStamp> stamp_from(const std::unordered_map<std::string, detail::PlanReaderNote>& noted,
                                        std::filesystem::file_time_type racy_from) const;
    // The load pack from the base and the stamp, written in the background.
    void write_pack() const;
public:
    class Walk {
    private:
        friend class Project;
        std::optional<DiskStamp> stamp;
    };
    class SaveJob {
    public:
        SaveJob();
        ~SaveJob();
        SaveJob(SaveJob&&) noexcept;
        SaveJob& operator=(SaveJob&&) noexcept;

    private:
        friend class Project;
        std::unique_ptr<PendingSave> save;
    };

private:
    struct Comparison;
    // walked: the stamp of a walk just taken, so the check walks the tree once.
    Comparison compare_disk(const std::optional<DiskStamp>* walked = nullptr) const;
    // The disk side and the studio side, read whole: every file parsed and
    // made in a scratch place, every instance with its properties.
    void read_whole(Comparison& out) const;
    // The disk side from only the files whose stamp changed since the disk
    // last matched the base, each with the instances it is under, when that is
    // all that changed: no file added, removed, or renamed, project.json the
    // same, and each changed file keeps its class and its children list.
    // False, with out untouched, when any of that does not hold. compare lists
    // the plan indexes the rules must look at.
    bool read_changed(Comparison& out, std::vector<std::size_t>& compare,
                      const std::optional<DiskStamp>* walked) const;
    // The rows and actions, over every plan index, or over only those listed.
    void find_rows(Comparison& out, const std::vector<std::size_t>* only) const;

    // nullopt when the tree cannot be read. Reads directory entries only.
    std::optional<DiskStamp> disk_stamp() const;
    // The stamp as of the last moment the base matched the disk (a load, a
    // save that started in sync, an apply that left nothing pending). While
    // the disk still has it, nothing on disk changed: a disk check has
    // nothing to load and no row to ask about, so it skips the full compare,
    // which reads and rebuilds every file.
    mutable std::optional<DiskStamp> synced_stamp_;
    // True when the disk still matches synced_stamp_: the same entries, and every
    // racy file holds the bytes the base has for it.
    // walked, when given, gets the stamp the check walked, for the rest of
    // the check to use instead of walking again.
    bool disk_matches_base(std::optional<DiskStamp>* walked = nullptr) const;
    // The same, against a walk already made.
    bool disk_matches(const std::optional<DiskStamp>& now) const;
    DiskScan apply_disk_walked(const std::vector<DiskChoice>& choices, const std::optional<DiskStamp>* given);
    // Marks the base in sync with the disk as stamped by taken, when the
    // disk still has that stamp; otherwise leaves it out of sync.
    void mark_synced(const std::optional<DiskStamp>& taken) const;
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
    std::string game_id_;
    std::string version_ = "1.0.0";
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
