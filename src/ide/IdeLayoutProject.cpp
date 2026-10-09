// IdeLayout: opening and saving projects, disk checks, and conflicts.

#include "IdeLayout.hpp"
#include "runner/ProfilerOverlay.hpp"
#include "profiler/ProfileJson.hpp"

#include "AssetImport.hpp"
#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "GameExport.hpp"
#include "IdeAssets.hpp"
#include "IdeLayoutInternal.hpp"
#include "LockWaits.hpp"
#include "ModelImport.hpp"
#include "PropertySheet.hpp"
#include "ScratchResources.hpp"
#include "IdeResources.hpp"
#include "InstanceFile.hpp"
#include "TextureImport.hpp"

#include <cctype>
#include <sstream>
#include <fstream>
#include <ctime>
#include <iterator>
#include <thread>

namespace ide {

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
    host.import_assets = [this](engine_core::InstanceId folder, const std::string& kind) {
        choose_import(folder, kind);
    };
    auto pane = jadefx::make<IdeAssets>(runner_.simulation().datamodel(), std::move(host));
    pane->setIconFile("AssetFolder.png");
    return pane;
}

void IdeLayout::add_as_game_objects(std::vector<engine_core::InstanceId> prefabs) {
    runner_.simulation().on_simulation(
        [this, alive = std::weak_ptr<int>(alive_), prefabs = std::move(prefabs)](engine_core::DataModel& world) {
            ScopedRecording step(world, "Add as GameObject");
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

// An instance file or a plugin file, which a drop puts in Workspace.
bool IsInstanceFile(const std::string& file) {
    std::string extension = path_from_utf8(file).extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".aeinst" || extension == kPluginExtension;
}

bool HasImports(const std::vector<std::string>& files) {
    return std::any_of(files.begin(), files.end(),
                       [](const std::string& file) { return is_importable_file(file) || IsInstanceFile(file); });
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
        import_instance_files(event.getDragboard().getFiles());
        import_files(event.getDragboard().getFiles());
        event.setDropCompleted(true);
        event.consume();
    });
}

bool IdeLayout::import_instance_files(const std::vector<std::string>& files) {
    std::vector<engine_core::CopiedNode> roots;
    bool any = false;
    for (const std::string& file : files) {
        if (!IsInstanceFile(file)) {
            continue;
        }
        any = true;
        std::vector<engine_core::CopiedNode> read;
        std::string error;
        if (!engine_core::load_instance_file(path_from_utf8(file), read, error)) {
            show_toast(path_from_utf8(file).filename().u8string() + " could not be read: " + error);
            continue;
        }
        roots.insert(roots.end(), std::make_move_iterator(read.begin()), std::make_move_iterator(read.end()));
    }
    if (roots.empty()) {
        return any;
    }
    runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_),
                                        roots = std::move(roots)](engine_core::DataModel& world) {
        ScopedRecording step(world, "Insert");
        std::vector<engine_core::InstanceId> made;
        std::string refused;
        paste_copies(world, roots, world.scene_service("Workspace"), &made, &refused);
        if (!made.empty()) {
            world.selection().set(made);
        }
        if (!refused.empty()) {
            toast_later(this, alive, std::move(refused));
        }
    });
    return true;
}

void IdeLayout::choose_import(engine_core::InstanceId folder, const std::string& kind) {
    if (dialog_open_ || prompt_open_) {
        return;
    }
    dialog_open_ = true;
    jadefx::FolderDialogOptions options;
    // A Prefab is a model's.
    options.title = "Import " + std::string(kind == "Prefab" ? "Model" : kind);
    options.file = true;
    options.multiple = true;
    options.extensions = kind == "Texture"  ? texture_file_extensions()
                         : kind == "Prefab" ? model_file_extensions()
                                            : sound_file_extensions();
    jadefx::showFilesDialog(std::move(options), [this, alive = std::weak_ptr<int>(alive_), folder](
                                                    jadefx::DialogResult result, const std::vector<std::string>& paths) {
        if (alive.expired()) {
            return;
        }
        dialog_open_ = false;
        if (result == jadefx::DialogResult::Unavailable) {
            show_error("No file dialog",
                       "This system has no file picker. On Linux, install zenity or kdialog. You can also drop files "
                       "on the studio.");
            return;
        }
        if (result == jadefx::DialogResult::Chosen) {
            // Picked to import, so not asked again.
            import_files(paths, folder, false);
        }
    });
}

void IdeLayout::import_files(const std::vector<std::string>& files, engine_core::InstanceId folder, bool ask) {
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
    if (!ask) {
        place_imports(resources, named, folder);
        return;
    }
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
    alert->setOnClosed([this, import, resources, folder, named = std::move(named)](const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice == nullptr || !(*choice == import)) {
            return;
        }
        // The place may have been replaced, or a test started, while it asked.
        if (place_resources() != resources || in_test()) {
            show_toast("Nothing was imported: the place changed while it asked");
            return;
        }
        place_imports(resources, named, folder);
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene_);
    // A drop comes from another program, over this window, which may be behind it and hide the question.
    if (mainStage_ != nullptr) {
        mainStage_->toFront();
    }
    // Stable names for the two answers, so a test can find them.
    if (jadefx::Button* button = alert->lookupButton(import)) {
        button->setElementId("import-files-import");
    }
    if (jadefx::Button* button = alert->lookupButton(jadefx::ButtonType::Cancel())) {
        button->setElementId("import-files-cancel");
    }
    alerts_.push_back(std::move(alert));
}

void IdeLayout::place_imports(const std::filesystem::path& resources, const std::vector<std::string>& files,
                              engine_core::InstanceId folder) {
    // Read here, on the UI thread: a large model holds the window until it is read.
    std::vector<PreparedAsset> prepared = prepare_assets(resources, files);
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
    runner_.simulation().on_simulation([this, alive = std::weak_ptr<int>(alive_), scripts, folder,
                                        prepared = std::move(prepared),
                                        problem = std::move(problem)](engine_core::DataModel& world) mutable {
        const bool models = std::any_of(prepared.begin(), prepared.end(),
                                        [](const PreparedAsset& asset) { return asset.model; });
        const bool sounds = std::any_of(prepared.begin(), prepared.end(),
                                        [](const PreparedAsset& asset) { return asset.sound; });
        const bool images = std::any_of(prepared.begin(), prepared.end(), [](const PreparedAsset& asset) {
            return !asset.model && !asset.sound && asset.error.empty();
        });
        ScopedRecording step(world, models            ? "Import Models"
                                   : sounds && images ? "Import Assets"
                                   : sounds          ? "Import Sounds"
                                                     : "Import Textures");
        // The folder the import was asked for may be gone by now; each category then takes its own.
        const std::vector<PlacedAsset> placed = place_assets(world, prepared, folder);
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
            // The Assets pane comes to the front, opened if it was closed, on the folder the first one went into.
            const engine_core::InstanceId shown = world.parent(made.front());
            world.selection().set(std::move(made));
            jadefx::runLater([this, alive, shown] {
                if (alive.expired()) {
                    return;
                }
                open_window(*assets_window_);
                if (auto* assets = dynamic_cast<IdeAssets*>(assets_window_->pane.get())) {
                    assets->openFolder(shown);
                }
            });
        }
    });
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

void IdeLayout::show_problem_count(const ProblemCounts& total) {
    if (problem_errors_text_ == nullptr || (total.errors == shown_errors_ && total.warnings == shown_warnings_)) {
        return;
    }
    shown_errors_ = total.errors;
    shown_warnings_ = total.warnings;
    problem_errors_text_->setText(std::to_string(total.errors));
    problem_warnings_text_->setText(std::to_string(total.warnings));
    problem_tip_->setText(total.errors == 0 && total.warnings == 0
                              ? std::string("No problems")
                              : Counted(total.errors, "error", "errors") + ", " +
                                    Counted(total.warnings, "warning", "warnings"));
}

void IdeLayout::select_guid(const std::string& guid) {
    engine_core::InstanceId found = 0;
    run_now([&](engine_core::DataModel& game) {
        const std::optional<engine_core::InstanceId> id = game.find_guid(guid);
        if (id) {
            found = *id;
        }
    });
    if (found != 0) {
        select_instance(found);
    }
}

void IdeLayout::add_select_to_tab_menu(IdePane& pane, std::uint32_t id) {
    pane.setOnTabMenu([this, id](jadefx::Menu& menu) {
        auto item = jadefx::make<jadefx::MenuItem>("Select In Explorer");
        item->setOnAction([this, id](jadefx::ActionEvent&) { select_instance(id); });
        menu.getItems().add(std::move(item));
    });
}

void IdeLayout::select_instance(std::uint32_t id) {
    bool found = false;
    run_now([&](engine_core::DataModel& game) {
        if (id != 0 && game.alive(id)) {
            game.selection().set({id});
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

void IdeLayout::refresh_modified() {
    const bool unsaved = has_unsaved_changes();
    if (unsaved != title_modified_) {
        update_title();
    }
    show_save_state(unsaved);
}

bool IdeLayout::has_unsaved_changes() {
    return runner_.simulation().datamodel().history().dirty() || editors_unflushed();
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
    update_title();
    show_toast("New place");
}

void IdeLayout::open_project() {
    confirm_discard("Save changes before opening another project?", [this] {
        jadefx::FolderDialogOptions options;
        options.title = "Open Project";
        pick_folder(std::move(options), " You can also start the studio with a project folder: AnarchyStudio <folder>",
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

bool IdeLayout::open_project_at(const std::filesystem::path& root) {
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
        return false;
    }
    // Every instance id changed. Editors and the clipboard pointed at the old ones.
    close_script_editors();
    if (clip_) {
        clip_->held = false;
    }
    project_ = std::move(loaded);
    end_scratch();
    forget_conflicts();
    update_title();
    // The project is the work the Welcome page was for, however it was opened.
    close_landing();
    show_toast("Opened " + project_->name());
    return true;
}

bool IdeLayout::save_open_project(std::function<void()> then,
                                  const std::vector<engine_core::SaveConflict>* overwrite) {
    save_failure_.clear();
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
        save_failure_ = error;
        show_error("Could not save project", error);
        return false;
    }
    update_title();
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
    save_failure_ = "Not saved: " + engine_core::describe_conflict(conflicts.front()) +
                    (conflicts.size() > 1 ? " (and " + std::to_string(conflicts.size() - 1) + " more)" : std::string());
    runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error, save_failure_);
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

bool IdeLayout::export_needs_save() {
    if (has_unsaved_changes()) {
        return true;
    }
    bool written = false;
    run_now([&](engine_core::DataModel& game) {
        const engine_core::AuthoredDirty dirty = game.authored_dirty();
        written = dirty.all || !dirty.ids.empty();
    });
    return written;
}

void IdeLayout::export_game() {
    if (exporting_ || dialog_open_ || prompt_open_) {
        return;
    }
    // The game is the project as it is on disk, so changes, with what writes
    // outside any recording changed, are saved first, once asked.
    if (project_ && !export_needs_save()) {
        export_saved_game();
        return;
    }
    if (scene_ == nullptr) {
        return;
    }
    prompt_open_ = true;
    const jadefx::ButtonType save("Save", jadefx::ButtonType::Data::OkDone);
    auto alert = std::make_shared<jadefx::Alert>(
        jadefx::AlertType::Warning,
        project_ ? "The game is exported from the project as it is saved on disk."
                 : "The game is exported from a project folder, so the place is saved as a project first.",
        std::vector<jadefx::ButtonType>{save, jadefx::ButtonType::Cancel()});
    alert->setTitle("Anarchy Engine");
    alert->setHeaderText(project_ ? "Save changes before exporting?" : "Save the place before exporting?");
    alert->setOnClosed([this, save](const jadefx::ButtonType* choice) {
        prompt_open_ = false;
        if (choice == nullptr || *choice != save) {
            return;
        }
        // A test holds changes back from disk.
        if (in_test()) {
            stop_test();
        }
        save_project([this] { export_saved_game(); });
    });
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(),
                                 [](const std::shared_ptr<jadefx::Alert>& item) {
                                     return !item || item->getResult() != nullptr;
                                 }),
                  alerts_.end());
    alert->show(*scene_);
    // Stable names for the answers, so a test can find them.
    const std::pair<const jadefx::ButtonType*, const char*> ids[] = {{&save, "export-save"},
                                                                     {&jadefx::ButtonType::Cancel(), "export-cancel"}};
    for (const auto& [type, id] : ids) {
        if (jadefx::Button* button = alert->lookupButton(*type)) {
            button->setElementId(id);
        }
    }
    alerts_.push_back(std::move(alert));
}

void IdeLayout::export_saved_game() {
    if (!project_ || exporting_ || dialog_open_ || prompt_open_) {
        return;
    }
    // A save dialog: the name typed there is the game's file.
    jadefx::FolderDialogOptions options;
    options.title = "Export Game";
    options.save = true;
    options.name = game_file_name(project_->name());
    pick_folder(std::move(options), std::string(), [this](const std::filesystem::path& output) {
        if (!project_ || exporting_) {
            return;
        }
        GameExportRequest request;
        request.name = project_->name();
        request.project_root = project_->root();
        request.tree_root = project_->tree_root();
        request.resources_root = project_->resources_root();
        request.output = output;
        exporting_ = true;
        show_toast("Exporting " + request.name + "…");
        // A game with large resources takes a while to copy; the studio keeps drawing meanwhile.
        std::thread([this, alive = std::weak_ptr<int>(alive_), request = std::move(request)] {
            std::filesystem::path written;
            std::string error;
            const bool done = ide::export_game(request, written, error);
            jadefx::runLater([this, alive, done, written, error] {
                if (alive.expired()) {
                    return;
                }
                exporting_ = false;
                if (!done) {
                    show_error("Could not export the game", error);
                    return;
                }
                show_toast("Exported " + utf8_path(written.filename()) + ". Send it to a friend!",
                           jadefx::Toast::LENGTH_LONG);
                reveal_folder(written.parent_path());
            });
        }).detach();
    });
}

bool IdeLayout::save_project_to(const std::filesystem::path& root) {
    save_failure_.clear();
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
        save_failure_ = error;
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
    update_title();
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



bool IdeLayout::save_profile_capture(const std::filesystem::path& file, std::string& error) {
    std::string text;
    profiler::with_view([&](const profiler::History& history) {
        text = profiler::write_capture_html(history, project_ ? project_->name() : std::string("Untitled"),
                                            profiler::utc_stamp(std::time(nullptr)));
    });
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (!out) {
        error = "Could not write " + file.string() + ".";
        return false;
    }
    return true;
}

void IdeLayout::save_profile_capture_as() {
    jadefx::FolderDialogOptions options;
    options.title = "Save Profile";
    options.save = true;
    options.name = profiler::capture_file_name(std::time(nullptr));
    if (project_) {
        options.directory = project_->root().string();
    }
    jadefx::showFolderDialog(std::move(options), [this, alive = std::weak_ptr<int>(alive_)](
                                                     jadefx::DialogResult result, const std::string& path) {
        if (alive.expired() || result != jadefx::DialogResult::Chosen) {
            return;
        }
        const std::filesystem::path file = std::filesystem::u8path(path);
        std::string error;
        if (save_profile_capture(file, error)) {
            show_toast("Saved " + file.filename().u8string());
        } else {
            runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                         "Profile: " + error);
        }
    });
}

}  // namespace ide
