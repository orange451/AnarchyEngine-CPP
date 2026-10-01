// IdeLayout: opening and saving projects, disk checks, and conflicts.

#include "IdeLayout.hpp"

#include "AssetImport.hpp"
#include "AssetInstances.hpp"
#include "IdeAssets.hpp"
#include "IdeLayoutInternal.hpp"
#include "LockWaits.hpp"
#include "ModelImport.hpp"
#include "PropertySheet.hpp"
#include "ScratchResources.hpp"
#include "TextureImport.hpp"

#include <chrono>
#include <iterator>

namespace ide {
namespace {

// How often refresh_modified checks the place while it keeps changing.
constexpr std::chrono::milliseconds kModifiedCheckInterval{250};

}  // namespace

const std::shared_ptr<IdeConflicts>& IdeLayout::conflicts_pane() {
    window_page(*conflicts_window_);
    return conflicts_pane_;
}

std::shared_ptr<IdePane> IdeLayout::make_conflicts() {
    ConflictsHost host;
    host.apply = [this](const std::vector<engine_core::DiskChoice>& choices) { check_disk(choices); };
    host.refresh = [this] { check_disk(); };
    host.select = [this](const std::string& guid) { select_guid(guid); };
    host.class_of = [this](const std::string& guid) {
        std::string name;
        run_now([&](engine_core::DataModel& game) {
            const std::optional<engine_core::InstanceId> id = game.find_guid(guid);
            if (id && *id != 0 && game.instance(*id) != nullptr) {
                name = game.instance(*id)->class_name();
            }
        });
        return name;
    };
    conflicts_pane_ = jadefx::make<IdeConflicts>(std::move(host));
    conflicts_pane_->setConflicts(conflicts_);
    conflicts_pane_->setApplyEnabled(!in_test());
    if (!disk_problem_.empty()) {
        conflicts_pane_->setProblem("Can't read the project on disk: " + disk_problem_);
    }
    return conflicts_pane_;
}

std::shared_ptr<IdePane> IdeLayout::make_terminal() {
    TerminalHost host;
    // The project open when the shell starts; this process's folder before one is.
    host.folder = [this] { return project_ ? project_->root().string() : std::string(); };
    return jadefx::make<IdeTerminal>(std::move(host));
}

std::shared_ptr<IdePane> IdeLayout::make_assets() {
    AssetsHost host;
    host.actions = explorer_host_;
    host.saved_view = [this] { return preferences_.assets_view(); };
    host.save_view = [this](const std::string& view) {
        preferences_.set_assets_view(view);
        std::string error;
        preferences_.save(error);
    };
    host.add_as_game_object = [this](engine_core::InstanceId prefab) { add_as_game_objects({prefab}); };
    host.import_sound = [this](engine_core::InstanceId folder) { choose_sound_import(folder); };
    auto pane = jadefx::make<IdeAssets>(runner_.simulation().datamodel(), std::move(host));
    pane->setIconFile("AssetFolder.png");
    return pane;
}

void IdeLayout::add_as_game_objects(std::vector<engine_core::InstanceId> prefabs) {
    runner_.simulation().on_simulation(
        [this, alive = std::weak_ptr<int>(alive_), prefabs = std::move(prefabs)](engine_core::DataModel& world) {
            world.history().set_pending_gesture("Add as GameObject");
            std::vector<engine_core::InstanceId> made;
            std::string error;
            for (engine_core::InstanceId prefab : prefabs) {
                std::string refused;
                if (const engine_core::InstanceId id = add_prefab_instance(world, prefab, refused)) {
                    made.push_back(id);
                } else if (error.empty()) {
                    error = std::move(refused);
                }
            }
            CloseGesture(world);
            if (made.empty()) {
                toast_later(this, alive, std::move(error));
                return;
            }
            world.selection().set(std::move(made));
        });
}

namespace {

// The Prefabs among ids. Callers hold the world's read lock.
std::vector<engine_core::InstanceId> PrefabsIn(const engine_core::DataModel& world,
                                              const std::vector<engine_core::InstanceId>& ids) {
    std::vector<engine_core::InstanceId> prefabs;
    for (engine_core::InstanceId id : ids) {
        if (dynamic_cast<const engine_core::Prefab*>(world.instance(id)) != nullptr) {
            prefabs.push_back(id);
        }
    }
    return prefabs;
}

// The image files among a drop's files.
std::vector<std::string> ImageFiles(const std::vector<std::string>& files) {
    std::vector<std::string> images;
    std::copy_if(files.begin(), files.end(), std::back_inserter(images), is_texture_file);
    return images;
}

// The model files among a drop's files.
std::vector<std::string> ModelFiles(const std::vector<std::string>& files) {
    std::vector<std::string> models;
    std::copy_if(files.begin(), files.end(), std::back_inserter(models), is_model_file);
    return models;
}

// The sound files among a drop's files.
std::vector<std::string> SoundFiles(const std::vector<std::string>& files) {
    std::vector<std::string> sounds;
    std::copy_if(files.begin(), files.end(), std::back_inserter(sounds), is_sound_file);
    return sounds;
}

bool HasImports(const std::vector<std::string>& files) {
    return std::any_of(files.begin(), files.end(), is_importable_file);
}

// "1 model", "3 textures".
std::string Count(std::size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// How many file names the import question lists before it counts the rest.
constexpr std::size_t kListedImports = 12;

}  // namespace

void IdeLayout::accept_prefab_drops(jadefx::Node& view) {
    view.setOnDragOver([this](jadefx::DragEvent& event) {
        if (event.dragboard == nullptr || !event.dragboard->has(kInstanceDragFormat)) {
            return;
        }
        engine_core::DataModel& world = runner_.simulation().datamodel();
        // A busy place refuses this over; the next one asks again.
        engine_core::DataModelLock lock(world, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns() && !PrefabsIn(world, instance_drag_ids(event.dragboard->get(kInstanceDragFormat))).empty()) {
            // A copy, so the pointer shows a plus: the drop adds a GameObject, and the Prefab stays where it is.
            event.acceptTransferModes(jadefx::TransferMode::Copy);
            event.consume();
        }
    });
    view.setOnDragDropped([this](jadefx::DragEvent& event) {
        if (event.dragboard == nullptr || !event.dragboard->has(kInstanceDragFormat)) {
            return;
        }
        engine_core::DataModel& world = runner_.simulation().datamodel();
        std::vector<engine_core::InstanceId> prefabs;
        {
            engine_core::DataModelLock lock(world, engine_core::DataModelLock::Read, kDropLockWait);
            if (!lock.owns()) {
                show_toast("The place is busy, so the drop did nothing. Try again.");
                return;
            }
            prefabs = PrefabsIn(world, instance_drag_ids(event.dragboard->get(kInstanceDragFormat)));
        }
        if (prefabs.empty()) {
            return;
        }
        add_as_game_objects(std::move(prefabs));
        event.setDropCompleted(true);
        event.consume();
    });
}

void IdeLayout::accept_file_drops(jadefx::Node& node) {
    node.setOnDragOver([](jadefx::DragEvent& event) {
        if (event.dragboard != nullptr && HasImports(event.getDragboard().getFiles())) {
            event.acceptTransferModes(jadefx::TransferMode::Copy);
            event.consume();
        }
    });
    node.setOnDragDropped([this](jadefx::DragEvent& event) {
        if (event.dragboard == nullptr || !HasImports(event.getDragboard().getFiles())) {
            return;
        }
        // Taken even when it asks nothing: the reason shows as a toast.
        import_files(event.getDragboard().getFiles());
        event.setDropCompleted(true);
        event.consume();
    });
}

void IdeLayout::choose_sound_import(engine_core::InstanceId folder) {
    if (dialog_open_ || prompt_open_) {
        return;
    }
    dialog_open_ = true;
    jadefx::FolderDialogOptions options;
    options.title = "Import Sound";
    options.file = true;
    options.extensions = sound_file_extensions();
    jadefx::showFolderDialog(std::move(options), [this, alive = std::weak_ptr<int>(alive_),
                                                  folder](jadefx::DialogResult result, const std::string& path) {
        if (alive.expired()) {
            return;
        }
        dialog_open_ = false;
        if (result == jadefx::DialogResult::Unavailable) {
            show_error("No file dialog",
                       "This system has no file picker. On Linux, install zenity or kdialog. You can also drop sound "
                       "files on the studio.");
            return;
        }
        if (result == jadefx::DialogResult::Chosen) {
            import_files({path}, folder);
        }
    });
}

void IdeLayout::import_files(const std::vector<std::string>& files, engine_core::InstanceId sound_folder) {
    std::vector<std::string> images = ImageFiles(files);
    std::vector<std::string> models = ModelFiles(files);
    std::vector<std::string> sounds = SoundFiles(files);
    if ((images.empty() && models.empty() && sounds.empty()) || scene_ == nullptr || prompt_open_ || dialog_open_) {
        return;
    }
    const int kinds = !images.empty() + !models.empty() + !sounds.empty();
    const std::string what = kinds > 1            ? "files"
                             : !models.empty()    ? "models"
                             : !sounds.empty()    ? "sounds"
                                                  : "textures";
    if (in_test()) {
        show_toast("Stop the test to import " + what);
        return;
    }
    // A place never saved imports into its scratch folder.
    const std::filesystem::path resources = place_resources();
    if (resources.empty()) {
        show_toast("Nowhere to import " + what + ": this system has no temporary folder. Save the place first");
        return;
    }

    // Models first, as the question names them.
    std::vector<std::string> named = models;
    named.insert(named.end(), images.begin(), images.end());
    named.insert(named.end(), sounds.begin(), sounds.end());
    const std::size_t count = named.size();
    std::string listed;
    for (std::size_t i = 0; i < count && i < kListedImports; ++i) {
        listed += (i == 0 ? "" : "\n") + utf8_path(path_from_utf8(named[i]).filename());
    }
    if (count > kListedImports) {
        listed += "\nand " + std::to_string(count - kListedImports) + " more";
    }
    // "Import 1 model, 2 textures and 1 sound".
    std::vector<std::string> parts;
    if (!models.empty()) {
        parts.push_back(Count(models.size(), "model", "models"));
    }
    if (!images.empty()) {
        parts.push_back(Count(images.size(), "texture", "textures"));
    }
    if (!sounds.empty()) {
        parts.push_back(Count(sounds.size(), "sound", "sounds"));
    }
    std::string header = "Import ";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        header += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    }

    prompt_open_ = true;
    const jadefx::ButtonType import("Import", jadefx::ButtonType::Data::OkDone);
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Confirmation, listed,
                                                 std::vector<jadefx::ButtonType>{import, jadefx::ButtonType::Cancel()});
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(header + "?");
    alert->setOnClosed([this, import, resources, sound_folder,
                        named = std::move(named)](const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice == nullptr || !(*choice == import)) {
            return;
        }
        // The place may have been replaced, or a test started, while it asked.
        if (place_resources() != resources || in_test()) {
            show_toast("Nothing was imported: the place changed while it asked");
            return;
        }
        // Read here, on the UI thread: a large model holds the window until it is read.
        std::vector<PreparedAsset> prepared = prepare_assets(resources, named);
        std::string problem;
        for (const PreparedAsset& asset : prepared) {
            if (!asset.error.empty()) {
                problem = "Could not import " + utf8_path(path_from_utf8(asset.file).filename()) + ": " + asset.error;
                break;
            }
        }
        if (std::all_of(prepared.begin(), prepared.end(), [](const PreparedAsset& asset) { return !asset.error.empty(); })) {
            show_toast(std::move(problem));
            return;
        }
        engine_core::ScriptRuntime* scripts = &runner_.simulation().scripts();
        runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_), scripts, sound_folder,
                                            prepared = std::move(prepared),
                                            problem = std::move(problem)](engine_core::DataModel& world) mutable {
            const bool models = std::any_of(prepared.begin(), prepared.end(),
                                            [](const PreparedAsset& asset) { return asset.model; });
            const bool sounds = std::any_of(prepared.begin(), prepared.end(),
                                            [](const PreparedAsset& asset) { return asset.sound; });
            const bool images = std::any_of(prepared.begin(), prepared.end(), [](const PreparedAsset& asset) {
                return !asset.model && !asset.sound && asset.error.empty();
            });
            world.history().set_pending_gesture(models            ? "Import Models"
                                                : sounds && images ? "Import Assets"
                                                : sounds          ? "Import Sounds"
                                                                  : "Import Textures");
            // The folder the import was asked for may be gone by now; Assets.Audio then takes them.
            const engine_core::InstanceId into = sound_folder != 0 && world.alive(sound_folder) ? sound_folder : 0;
            const std::vector<PlacedAsset> placed = place_assets(world, prepared, into);
            CloseGesture(world);
            std::vector<engine_core::InstanceId> made;
            std::size_t model_count = 0;
            std::string summary;
            bool noted = false;
            for (std::size_t i = 0; i < placed.size(); ++i) {
                const PreparedAsset& asset = prepared[i];
                if (placed[i].root == 0) {
                    if (problem.empty()) {
                        problem = "Could not import " + utf8_path(path_from_utf8(asset.file).filename()) + ": " +
                                  placed[i].error;
                    }
                    continue;
                }
                made.push_back(placed[i].root);
                if (!asset.model) {
                    continue;
                }
                const ImportedModel& model = asset.imported;
                for (const std::string& note : model.notes) {
                    scripts->append_output(engine_core::ScriptRuntime::OutputKind::Print, model.name + ": " + note);
                }
                noted = noted || !model.notes.empty();
                ++model_count;
                summary = "Imported " + model.name + ": " + Count(model.meshes.size(), "mesh", "meshes") + ", " +
                          Count(model.materials.size(), "material", "materials") + ", " +
                          Count(model.textures.size(), "texture", "textures");
            }
            if (!problem.empty()) {
                toast_later(this, alive, std::move(problem));
            } else if (!summary.empty()) {
                if (model_count > 1) {
                    summary = "Imported " + Count(model_count, "model", "models");
                }
                toast_later(this, alive, summary + (noted ? ". The console says what was left out" : ""));
            }
            if (!made.empty()) {
                world.selection().set(std::move(made));
                // The Assets pane comes to the front, opened if it was closed, to show what came in.
                jadefx::runLater([this, alive] {
                    if (!alive.expired()) {
                        open_window(*assets_window_);
                    }
                });
            }
        });
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene_);
    // Stable names for the two answers, so a test can find them.
    if (jadefx::Button* button = alert->lookupButton(import)) {
        button->setElementId("import-files-import");
    }
    if (jadefx::Button* button = alert->lookupButton(jadefx::ButtonType::Cancel())) {
        button->setElementId("import-files-cancel");
    }
    alerts_.push_back(std::move(alert));
}

void IdeLayout::show_conflicts() {
    open_window(*conflicts_window_);
}

void IdeLayout::forget_conflicts() {
    conflicts_.clear();
    disk_problem_.clear();
    check_pending_ = false;
    noted_play_check_ = false;
    if (conflicts_pane_) {
        conflicts_pane_->setProblem("");
    }
    show_conflict_count();
}

void IdeLayout::show_conflict_count() {
    if (conflicts_pane_) {
        conflicts_pane_->setConflicts(conflicts_);
        conflicts_pane_->setApplyEnabled(!in_test());
    }
    if (conflict_count_ != nullptr) {
        conflict_count_->setVisible(!conflicts_.empty());
    }
    if (conflict_count_text_ != nullptr) {
        conflict_count_text_->setText(std::to_string(conflicts_.size()));
    }
    if (conflict_tip_) {
        conflict_tip_->setText(Counted(conflicts_.size(), "conflict", "conflicts") + " with files on disk");
    }
}

void IdeLayout::select_guid(const std::string& guid) {
    bool found = false;
    run_now([&](engine_core::DataModel& game) {
        const std::optional<engine_core::InstanceId> id = game.find_guid(guid);
        if (id && *id != 0) {
            game.selection().set({*id});
            found = true;
        }
    });
    if (!found) {
        return;
    }
    for (const std::weak_ptr<IdeExplorer>& weak : explorers_) {
        const std::shared_ptr<IdeExplorer> explorer = weak.lock();
        if (explorer && dockContaining(explorer.get()) != nullptr) {
            explorer->reveal_selection();
        }
    }
}

std::optional<std::vector<engine_core::SaveConflict>> IdeLayout::check_disk(
    const std::vector<engine_core::DiskChoice>& choices) {
    if (!project_) {
        check_pending_ = false;
        return std::vector<engine_core::SaveConflict>();
    }
    if (in_test()) {
        // A test runs on the place captured at Test. Changes on disk wait for Stop.
        check_pending_ = true;
        if (!noted_play_check_) {
            bool waiting = false;
            std::string problem;
            run_now([&](engine_core::DataModel&) {
                try {
                    const engine_core::DiskScan scan = project_->scan_disk();
                    waiting = scan.has_disk_changes || !scan.conflicts.empty();
                } catch (const std::exception& failure) {
                    problem = failure.what();
                }
            });
            if (!problem.empty()) {
                // Said once. The check at Stop reads the disk again.
                if (problem != disk_problem_) {
                    show_toast("Can't read the project on disk: " + problem);
                }
                disk_problem_ = problem;
                noted_play_check_ = true;
            } else if (waiting) {
                show_toast("Changes on disk will load when the test stops");
                noted_play_check_ = true;
            }
        }
        return std::nullopt;
    }
    // Typing an editor has not written yet is the studio's side.
    flush_editors();
    engine_core::DiskScan scan;
    std::string problem;
    run_now([&](engine_core::DataModel&) {
        try {
            scan = project_->apply_disk(choices);
        } catch (const std::exception& failure) {
            problem = failure.what();
        }
    });
    check_pending_ = false;
    noted_play_check_ = false;
    // The base may have moved without the place moving, so the title looks again.
    seen_revision_ = ~std::uint64_t{0};
    if (!problem.empty()) {
        if (problem != disk_problem_) {
            show_toast("Can't read the project on disk: " + problem);
        }
        disk_problem_ = problem;
        if (conflicts_pane_) {
            conflicts_pane_->setProblem("Can't read the project on disk: " + problem);
        }
        return std::nullopt;
    }
    disk_problem_.clear();
    if (conflicts_pane_) {
        conflicts_pane_->setProblem("");
    }
    if (!scan.loaded.empty()) {
        show_toast(LoadedText(scan.loaded));
    }
    if (!scan.skipped.empty()) {
        show_toast(Counted(scan.skipped.size(), "row", "rows") + " changed on disk and " +
                   (scan.skipped.size() == 1 ? "wasn't" : "weren't") + " applied");
    }
    conflicts_ = scan.conflicts;
    show_conflict_count();
    return scan.conflicts;
}

void IdeLayout::mark_saved() {
    engine_core::DataModel& game = runner_.simulation().datamodel();
    run_now([this](engine_core::DataModel& world) { saved_fingerprint_ = engine_core::Project::place_fingerprint(world); });
    seen_revision_ = game.authored_revision();
    place_modified_ = false;
    update_title();
}

void IdeLayout::refresh_modified(bool force) {
    engine_core::DataModel& game = runner_.simulation().datamodel();
    const std::uint64_t revision = game.authored_revision();
    const auto checked = std::chrono::steady_clock::now();
    // Only an authored change moves the revision; play steps do not. Flying the
    // scene camera moves it every step, though, and the check below reads the
    // whole place. So while it keeps moving the check waits out the interval.
    // seen_revision_ stays behind meanwhile, so a later frame checks the place
    // as it ends up.
    const bool due = force || checked - modified_checked_at_ >= kModifiedCheckInterval;
    if (revision != seen_revision_ && due) {
        seen_revision_ = revision;
        modified_checked_at_ = checked;
        // A project compares key by key, so a file on disk that is only
        // formatted differently does not count. An untitled place has no files.
        std::uint64_t now = 0;
        bool unsaved = false;
        run_now([&](engine_core::DataModel& world) {
            if (project_) {
                unsaved = project_->unsaved();
            } else {
                now = engine_core::Project::place_fingerprint(world);
            }
        });
        place_modified_ = project_ ? unsaved : now != saved_fingerprint_;
    }
    if ((place_modified_ || editors_unflushed()) != title_modified_) {
        update_title();
    }
}

bool IdeLayout::has_unsaved_changes() {
    seen_revision_ = ~std::uint64_t{0};
    // Asked before a save or a discard, so it checks now, whatever the pace.
    refresh_modified(true);
    return place_modified_ || editors_unflushed();
}

void IdeLayout::confirm_discard(const std::string& question, std::function<void()> proceed) {
    if (prompt_open_ || dialog_open_) {
        return;
    }
    if (!has_unsaved_changes() || scene_ == nullptr) {
        proceed();
        return;
    }
    prompt_open_ = true;
    const jadefx::ButtonType save("Save", jadefx::ButtonType::Data::OkDone);
    const jadefx::ButtonType discard("Don't Save", jadefx::ButtonType::Data::Left);
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Warning,
                                                 "Your changes will be lost if you don't save them.",
                                                 std::vector<jadefx::ButtonType>{save, discard, jadefx::ButtonType::Cancel()});
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(question);
    alert->setOnClosed([this, save, discard, proceed = std::move(proceed)](const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice == nullptr) {
            return;
        }
        if (*choice == discard) {
            // Don't Save: the editors' text is dropped with the place.
            proceed();
        } else if (*choice == save) {
            // The editors' text is part of what is saved.
            if (in_test()) {
                stop_test();
            }
            save_project(proceed);
        }
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene_);
    // Stable names for the three answers, so a test can find them.
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {
        {&save, "unsaved-save"}, {&discard, "unsaved-discard"}, {&jadefx::ButtonType::Cancel(), "unsaved-cancel"}};
    for (const auto& [type, id] : ids) {
        if (jadefx::Button* button = alert->lookupButton(*type)) {
            button->setElementId(id);
        }
    }
    alerts_.push_back(std::move(alert));
}

void IdeLayout::new_place() {
    if (in_test()) {
        stop_test();
    }
    close_script_editors();
    if (clip_) {
        clip_->held = false;
    }
    run_now([](engine_core::DataModel& game) { engine_core::Project::reset_place(game); });
    project_.reset();
    // Whatever the last untitled place imported went with it.
    begin_scratch();
    forget_conflicts();
    mark_saved();
    load_plugins();
    show_toast("New place");
}

void IdeLayout::open_project() {
    confirm_discard("Save changes before opening another project?", [this] {
        jadefx::FolderDialogOptions options;
        options.title = "Open Project";
        pick_folder(std::move(options), " You can also start the studio with a project folder: AnarchyEngine-CPP <folder>",
                    [this](const std::filesystem::path& root) { open_project_at(root); });
    });
}

void IdeLayout::pick_folder(jadefx::FolderDialogOptions options, const std::string& hint,
                            std::function<void(const std::filesystem::path&)> chosen) {
    if (dialog_open_) {
        return;
    }
    dialog_open_ = true;
    options.directory = dialog_directory().u8string();
    jadefx::showFolderDialog(std::move(options), [this, hint, chosen = std::move(chosen)](jadefx::DialogResult result,
                                                                                         const std::string& path) {
        dialog_open_ = false;
        if (result == jadefx::DialogResult::Unavailable) {
            show_error("No folder dialog", "This system has no folder picker. On Linux, install zenity or kdialog." + hint);
            return;
        }
        if (result == jadefx::DialogResult::Chosen) {
            chosen(std::filesystem::u8path(path));
        }
    });
}

void IdeLayout::open_project_at(const std::filesystem::path& root) {
    // The load replaces the tree that Stop would restore.
    if (in_test()) {
        stop_test();
    }
    std::unique_ptr<engine_core::Project> loaded;
    std::string error;
    run_now([&](engine_core::DataModel& game) {
        try {
            loaded = std::make_unique<engine_core::Project>(engine_core::Project::load(root, game));
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!loaded) {
        show_error("Could not open project", error);
        return;
    }
    // Every instance id changed. Editors and the clipboard pointed at the old ones.
    close_script_editors();
    if (clip_) {
        clip_->held = false;
    }
    project_ = std::move(loaded);
    end_scratch();
    forget_conflicts();
    mark_saved();
    load_plugins();
    show_toast("Opened " + project_->name());
}

bool IdeLayout::save_open_project(std::function<void()> then,
                                  const std::vector<engine_core::SaveConflict>* overwrite) {
    // A check first: what only the disk changed loads, and what both changed
    // stops the save and asks. When the check cannot run, during a test or with
    // src/ unreadable, the save's own guard still stops at a changed file.
    if (overwrite == nullptr) {
        const std::optional<std::vector<engine_core::SaveConflict>> open = check_disk();
        if (open && !open->empty()) {
            confirm_overwrite(*open, std::move(then), GateRows::Checked);
            return false;
        }
    }
    // Open editors write Source first. During play the save writes the place
    // captured at Test, so play edits stay out of it either way.
    flush_editors();
    std::string error;
    std::vector<engine_core::SaveConflict> conflicts;
    run_now([&](engine_core::DataModel&) {
        try {
            project_->save(overwrite != nullptr ? *overwrite : std::vector<engine_core::SaveConflict>());
        } catch (const engine_core::ProjectConflict& conflict) {
            conflicts = conflict.conflicts();
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!conflicts.empty()) {
        confirm_overwrite(conflicts, std::move(then), in_test() ? GateRows::DuringTest : GateRows::Guarded);
        return false;
    }
    if (!error.empty()) {
        show_error("Could not save project", error);
        return false;
    }
    mark_saved();
    const engine_core::Project::SaveReport& report = project_->last_save();
    const std::size_t changed = report.written.size() + report.moved.size() + report.removed.size();
    show_toast("Saved " + project_->name() +
               (changed == 0 ? std::string(" (no changes)")
                             : " (" + std::to_string(changed) + (changed == 1 ? " file" : " files") + " changed)"));
    if (then) {
        then();
    }
    return true;
}

void IdeLayout::confirm_overwrite(const std::vector<engine_core::SaveConflict>& conflicts,
                                  std::function<void()> then, GateRows from) {
    runner_.simulation().scripts().append_output(
        engine_core::ScriptRuntime::OutputKind::Error,
        "Not saved: " + engine_core::describe_conflict(conflicts.front()) +
            (conflicts.size() > 1 ? " (and " + std::to_string(conflicts.size() - 1) + " more)" : std::string()));
    if (scene_ == nullptr || prompt_open_) {
        return;
    }
    prompt_open_ = true;
    std::string detail;
    const std::size_t shown = std::min<std::size_t>(conflicts.size(), 5);
    for (std::size_t index = 0; index < shown; ++index) {
        detail += ConflictLine(conflicts[index]) + "\n";
    }
    if (conflicts.size() > shown) {
        detail += "and " + std::to_string(conflicts.size() - shown) + " more\n";
    }
    const jadefx::ButtonType show("Show Conflicts", jadefx::ButtonType::Data::Left);
    const jadefx::ButtonType overwrite("Overwrite All", jadefx::ButtonType::Data::OkDone);
    std::vector<jadefx::ButtonType> buttons{show, overwrite, jadefx::ButtonType::Cancel()};
    if (from == GateRows::DuringTest) {
        // A test holds changes on disk back, so nothing here can be settled yet.
        detail += "\nStop the test to load changes from disk, then see any conflicts in the Conflicts window.";
        buttons = {jadefx::ButtonType::Cancel()};
    } else {
        detail += "\nTo see more information, check the Conflicts window.";
    }
    auto alert = std::make_shared<jadefx::Alert>(jadefx::AlertType::Warning, detail, std::move(buttons));
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(Counted(conflicts.size(), "conflict", "conflicts") + " with files changed on disk.");
    // Overwrite All takes the studio's side of exactly these. A file that
    // changed while the alert was up is asked about again.
    alert->setOnClosed([this, show, overwrite, listed = conflicts, from, then = std::move(then)](
                           const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice == nullptr) {
            return;
        }
        if (*choice == show) {
            show_conflicts();
            return;
        }
        if (*choice != overwrite || !project_) {
            return;
        }
        if (from == GateRows::Checked) {
            std::vector<engine_core::DiskChoice> keep;
            for (const engine_core::SaveConflict& row : listed) {
                keep.push_back({row, false});
            }
            check_disk(keep);
            save_open_project(then);
        } else {
            save_open_project(then, &listed);
        }
    });
    // Finished alerts are not dropped here: this may run inside the closed
    // handler of the alert that asked last.
    alert->show(*scene_);
    // Stable names for the answers, so a test can find them.
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {{&show, "save-conflict-show"},
                                                                     {&overwrite, "save-conflict-overwrite"},
                                                                     {&jadefx::ButtonType::Cancel(), "save-conflict-cancel"}};
    for (const auto& [type, id] : ids) {
        if (jadefx::Button* button = alert->lookupButton(*type)) {
            button->setElementId(id);
        }
    }
    alerts_.push_back(std::move(alert));
}

void IdeLayout::save_project(std::function<void()> then) {
    if (!project_) {
        save_project_as(std::move(then));
        return;
    }
    save_open_project(std::move(then));
}

void IdeLayout::save_project_as(std::function<void()> then) {
    jadefx::FolderDialogOptions options;
    options.title = "Save Project As";
    options.save = true;
    options.name = project_ ? project_->name() : std::string("MyPlace");
    pick_folder(std::move(options), std::string(), [this, then = std::move(then)](const std::filesystem::path& root) {
        if (save_project_to(root) && then) {
            then();
        }
    });
}

bool IdeLayout::save_project_to(const std::filesystem::path& root) {
    flush_editors();
    std::string error;
    run_now([&](engine_core::DataModel& game) {
        try {
            if (project_) {
                project_->save_as(root);
            } else {
                project_ = std::make_unique<engine_core::Project>(engine_core::Project::adopt(root, game));
            }
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    if (!error.empty()) {
        show_error("Could not save project", error);
        return false;
    }
    // A first save: what the place added to resources so far joins the project.
    if (!scratch_resources_.empty()) {
        if (!move_scratch_resources(scratch_resources_, project_->resources_root(), error)) {
            // Left where it is, and no longer deleted, so the files can still be found.
            show_error("Some resources were not moved into the project",
                       "They are still in " + utf8_path(scratch_resources_) + ": " + error);
        }
        scratch_resources_.clear();
    }
    // Another folder, which this save wrote whole.
    forget_conflicts();
    mark_saved();
    show_toast("Saved " + project_->name());
    return true;
}

void leave_field_on_escape(jadefx::Scene& scene, int key, bool pressed) {
    if (!pressed || key != jadefx::Key::Escape) {
        return;
    }
    jadefx::Node* focused = scene.focusedNode();
    if (dynamic_cast<jadefx::TextField*>(focused) != nullptr) {
        scene.releaseFocus(focused);
    }
}

}  // namespace ide
