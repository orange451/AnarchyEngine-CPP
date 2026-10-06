// IdeLayout: the MCP server, its token, and this studio's registry entry.

#include "IdeLayout.hpp"

#include "AssetImport.hpp"
#include "IdeLayoutInternal.hpp"
#include "McpSetup.hpp"
#include "runner/ProfilerOverlay.hpp"
#include "TextureImport.hpp"

namespace ide {

void IdeLayout::apply_mcp_setting() {
    const McpSwitch setting = decide_mcp(preferences_.mcp_enabled(), engine_core::environment_variable);
    if (setting.on) {
        start_mcp();
    } else {
        stop_mcp();
    }
}

std::string IdeLayout::mcp_status() const {
    if (mcp_) {
        return "Listening on http://127.0.0.1:" + std::to_string(mcp_->port()) + "/mcp";
    }
    return mcp_error_.empty() ? "Off" : "Could not start: " + mcp_error_;
}

void IdeLayout::stop_mcp() {
    mcp_error_.clear();
    if (!mcp_) {
        return;
    }
    // A tool call waiting for the UI thread gives up now, so joining the
    // server's threads below does not wait it out.
    if (ui_calls_) {
        ui_calls_->close();
    }
    if (mcp_identity_ && !mcp_identity_->dir.empty()) {
        remove_studio(mcp_identity_->dir, mcp_identity_->entry);
    }
    mcp_.reset();
    mcp_identity_.reset();
    ui_calls_.reset();
}

void IdeLayout::start_mcp() {
    if (mcp_) {
        return;
    }
    int port = kMcpPort;
    bool pinned = false;
    if (const std::optional<std::string> text = engine_core::environment_variable("ANARCHY_MCP_PORT")) {
        const int asked = std::atoi(text->c_str());
        if (asked > 0 && asked < 65536) {
            port = asked;
            pinned = true;
        }
    }
    // Each hook runs on the UI thread, where the ribbon's own handlers run.
    std::weak_ptr<int> alive = alive_;
    ui_calls_ = std::make_shared<UiCalls>([](std::function<void()> task) { jadefx::runLater(std::move(task)); });
    const std::shared_ptr<UiCalls> calls = ui_calls_;
    auto on_ui = [alive, calls](std::function<void()> fn) {
        calls->run(
            [alive, fn = std::move(fn)] {
                if (alive.expired()) {
                    throw std::runtime_error("The studio is closing.");
                }
                fn();
            },
            kUiWait);
    };
    McpStudio studio;
    studio.start_test = [this, on_ui] { on_ui([this] { start_test(); }); };
    studio.pause_test = [this, on_ui] { on_ui([this] { pause_test(); }); };
    studio.resume_test = [this, on_ui] { on_ui([this] { resume_test(); }); };
    studio.stop_test = [this, on_ui] { on_ui([this] { stop_test(); }); };
    studio.session = [this, on_ui] {
        // Shared, since a task that runs after a timed-out wait still writes it.
        auto state = std::make_shared<std::string>();
        on_ui([this, state] {
            *state = play_ == PlayState::Stopped ? "stopped" : (play_ == PlayState::Running ? "running" : "paused");
        });
        return *state;
    };
    studio.flush_scripts = [this, on_ui] { on_ui([this] { flush_editors(); }); };
    studio.refresh_scripts = [this, on_ui] { on_ui([this] { reapply_editors(); }); };
    // The first Scene View's next paint. The wait is on the server thread, since
    // the paint comes after the UI task that asks for it.
    studio.capture_view = [this, on_ui, calls](int max_size) {
        // Guarded by calls. Shared, since a paint after a timed-out wait still writes it.
        struct Shot {
            bool done = false;
            runner::ViewPixels pixels;
            McpViewSize size;
        };
        auto shot = std::make_shared<Shot>();
        on_ui([this, shot, calls] {
            auto* view = dynamic_cast<runner::GameView*>(scene_view_.get());
            if (view == nullptr) {
                throw std::runtime_error("The studio has no Scene View.");
            }
            // A tab behind another is out of the scene, so it does not paint.
            if (view->getScene() == nullptr) {
                throw std::runtime_error("The Scene View's tab is behind another tab in its dock, so it is not "
                                         "drawing. Bring it forward with tabs (action select, tab Scene View), "
                                         "then try again.");
            }
            calls->update([&] { shot->size = {view->getWidth(), view->getHeight()}; });
            view->requestCapture([shot, calls](runner::ViewPixels pixels) {
                calls->update([&] {
                    shot->pixels = std::move(pixels);
                    shot->done = true;
                });
            });
        });
        if (!calls->wait_until([&] { return shot->done; }, kCaptureWait)) {
            throw std::runtime_error("The Scene View did not draw within 3 seconds. Check that the studio window "
                                     "is not minimized.");
        }
        runner::ViewPixels pixels;
        McpViewSize size;
        calls->update([&] {
            pixels = std::move(shot->pixels);
            size = shot->size;
        });
        if (pixels.empty()) {
            throw std::runtime_error("The Scene View could not be read back.");
        }
        const runner::ViewPixels fitted = runner::FitWithin(pixels, max_size);
        McpImage image;
        image.png = runner::EncodePng(fitted);
        image.width = fitted.width;
        image.height = fitted.height;
        image.view_width = size.width;
        image.view_height = size.height;
        return image;
    };
    // Into the first Scene View's scene, where the window's own events go, so a
    // GUI, the studio's tools, and UserInputService hear them as they do a person's.
    // Where the inputs last left the pointer, in view points. UI thread only.
    auto pointer = std::make_shared<std::optional<std::pair<double, double>>>();
    studio.send_input = [this, on_ui, pointer](const std::vector<McpInput>& inputs) {
        // Shared, since a task that runs after a timed-out wait still writes it.
        auto size = std::make_shared<McpViewSize>();
        on_ui([this, inputs, size, pointer] {
            auto* view = dynamic_cast<runner::GameView*>(scene_view_.get());
            if (view == nullptr) {
                throw std::runtime_error("The studio has no Scene View.");
            }
            jadefx::Scene* scene = view->getScene();
            if (scene == nullptr) {
                throw std::runtime_error("The Scene View's tab is behind another tab in its dock, so it cannot be "
                                         "clicked. Bring it forward with tabs (action select, tab Scene View), "
                                         "then try again.");
            }
            *size = {view->getWidth(), view->getHeight()};
            bool keys = false;
            for (const McpInput& input : inputs) {
                if (input.kind == McpInput::Kind::Key || input.kind == McpInput::Kind::Text) {
                    keys = true;
                } else if (!(input.x >= 0 && input.x < size->width && input.y >= 0 && input.y < size->height)) {
                    throw std::runtime_error("(" + engine_core::format_json_number(input.x) + ", " +
                                             engine_core::format_json_number(input.y) +
                                             ") is outside the Scene View, which is " +
                                             engine_core::format_json_number(size->width) + " by " +
                                             engine_core::format_json_number(size->height) + " points.");
                }
            }
            // Keys go to the focused node, so elsewhere in the studio they could
            // edit the place. Focus inside the view, as a GUI's text field, stays.
            if (keys) {
                const jadefx::Node* node = scene->focusedNode();
                while (node != nullptr && node != view) {
                    node = node->getParent();
                }
                if (node == nullptr) {
                    view->requestFocus();
                }
            }
            // While a script locks the pointer, a person's stays where it is and
            // only its motion reaches GetMouseDelta. So here: no move gets
            // through, and presses land where the pointer was.
            const bool locked = view->pointerWanted();
            const double left = view->getAbsoluteX();
            const double top = view->getAbsoluteY();
            for (const McpInput& input : inputs) {
                // A modifier an earlier call pressed is still held, as the window's own mods would say.
                const int mods = input.mods | scene->modifierMask();
                if (input.kind == McpInput::Kind::Move && locked) {
                    continue;
                }
                if (input.kind == McpInput::Kind::Move || input.kind == McpInput::Kind::Button ||
                    input.kind == McpInput::Kind::Scroll) {
                    if (!locked || !pointer->has_value()) {
                        *pointer = std::make_pair(input.x, input.y);
                    }
                }
                const double x = pointer->has_value() ? left + (*pointer)->first : 0;
                const double y = pointer->has_value() ? top + (*pointer)->second : 0;
                switch (input.kind) {
                    case McpInput::Kind::Move:
                        scene->noteMove(x, y);
                        break;
                    case McpInput::Kind::Button:
                        scene->noteButton(input.code, input.down, x, y, mods);
                        break;
                    case McpInput::Kind::Scroll:
                        scene->noteScroll(x, y, 0, input.amount);
                        break;
                    case McpInput::Kind::Key:
                        scene->noteKey(input.code, input.down, false, mods);
                        break;
                    case McpInput::Kind::Text:
                        scene->noteText(input.text);
                        break;
                }
            }
        });
        return *size;
    };
    // Only where the files go is asked of the UI thread. They are read here, on
    // the server thread, so a large model does not hold the window.
    studio.import_files = [this, on_ui](const std::vector<std::string>& files) -> McpPlaceImports {
        struct Where {
            std::filesystem::path resources;
            bool playing = false;
        };
        auto where = std::make_shared<Where>();
        on_ui([this, where] {
            where->resources = place_resources();
            where->playing = in_test();
        });
        if (where->playing) {
            throw std::runtime_error("Stop the test first (playtest with action stop): a test runs on a copy of the "
                                     "place, and imports go into the place itself.");
        }
        if (where->resources.empty()) {
            throw std::runtime_error("The place has nowhere to keep imported files: it was never saved, and this "
                                     "system has no temporary folder. Save it first.");
        }
        auto prepared = std::make_shared<const std::vector<PreparedAsset>>(prepare_assets(where->resources, files));
        return [prepared, resources = where->resources](engine_core::DataModel& world) {
            // Open, New, or Test may have come while the files were read.
            if (world.resources_root() != resources) {
                throw std::runtime_error("Another place was opened or made while the files were read, so nothing "
                                         "was imported. Try again.");
            }
            if (world.simulation_running()) {
                throw std::runtime_error("A test started while the files were read, so nothing was imported. Stop "
                                         "it and try again.");
            }
            const std::vector<PlacedAsset> placed = place_assets(world, *prepared);
            std::vector<McpImport> imports;
            for (std::size_t i = 0; i < placed.size(); ++i) {
                const PreparedAsset& asset = (*prepared)[i];
                McpImport import;
                import.file = asset.file;
                import.kind = asset.model   ? "model"
                              : asset.sound ? "sound"
                              : is_texture_file(asset.file) ? "texture"
                                                            : "unknown";
                import.error = placed[i].error;
                import.root = placed[i].root;
                import.made = placed[i].made;
                if (asset.model) {
                    import.notes = asset.imported.notes;
                }
                imports.push_back(std::move(import));
            }
            return imports;
        };
    };
    studio.tabs = [this, on_ui] {
        // Shared, since a task that runs after a timed-out wait still writes it.
        auto list = std::make_shared<engine_core::JsonValue>();
        on_ui([this, list] { *list = tab_list(); });
        return *list;
    };
    studio.change_tab = [this, on_ui](const std::string& action, const std::string& tab) {
        on_ui([this, action, tab] {
            if (action == "select") {
                select_tab(tab);
            } else if (action == "close") {
                close_tab(tab);
            } else {
                open_tab(tab);
            }
        });
    };
    studio.save_place = [this, on_ui](const std::string& folder) {
        // Shared, since a task that runs after a timed-out wait still writes it.
        auto out = std::make_shared<engine_core::JsonValue>();
        on_ui([this, folder, out] {
            bool saved = false;
            if (!folder.empty()) {
                saved = save_project_to(path_from_utf8(folder));
            } else if (!project_) {
                throw std::runtime_error("The place has never been saved. Give folder to save it there, as Save "
                                         "As does.");
            } else {
                saved = save_open_project();
            }
            if (!saved) {
                throw std::runtime_error(save_failure_.empty() ? std::string("Not saved.") : save_failure_);
            }
            engine_core::JsonValue result = engine_core::JsonValue::object();
            result.set("project", engine_core::JsonValue::string(project_->name()));
            result.set("folder", engine_core::JsonValue::string(utf8_path(project_->root())));
            if (folder.empty()) {
                const engine_core::Project::SaveReport& report = project_->last_save();
                const std::pair<const char*, const std::vector<std::string>*> lists[] = {
                    {"written", &report.written}, {"moved", &report.moved}, {"removed", &report.removed}};
                for (const auto& [key, files] : lists) {
                    std::vector<engine_core::JsonValue> items;
                    for (const std::string& file : *files) {
                        items.push_back(engine_core::JsonValue::string(file));
                    }
                    result.set(key, engine_core::JsonValue::array(std::move(items)));
                }
            }
            *out = std::move(result);
        });
        return *out;
    };
    studio.show_profiler = [on_ui](std::optional<bool> shown) {
        // Shared, since a task that runs after a timed-out wait still writes it.
        auto now = std::make_shared<bool>(false);
        on_ui([shown, now] {
            runner::ProfilerUi& ui = runner::ProfilerUi::get();
            ui.setShown(shown.value_or(!ui.shown()));
            *now = ui.shown();
        });
        return *now;
    };
    auto identity = std::make_shared<McpIdentity>();
    studio.info = [identity] {
        std::lock_guard<std::mutex> guard(identity->mu);
        engine_core::JsonValue out = engine_core::JsonValue::object();
        out.set("project", engine_core::JsonValue::string(identity->entry.project));
        out.set("root", engine_core::JsonValue::string(identity->entry.root));
        out.set("pid", engine_core::JsonValue::number(static_cast<double>(identity->entry.pid)));
        out.set("port", engine_core::JsonValue::number(identity->entry.port));
        return out;
    };

    auto server = std::make_unique<McpServer>();
    // Every client needs this studio's token. The bridge reads it from the
    // registry entry; a client that connects directly needs ANARCHY_MCP_TOKEN
    // set to a secret it knows too.
    const std::optional<std::string> fixed = engine_core::environment_variable("ANARCHY_MCP_TOKEN");
    const std::string token = fixed && !fixed->empty() ? *fixed : session_token();
    server->set_token(token);
    identity->entry.token = token;
    add_engine_tools(*server, runner_.simulation(), std::move(studio));
    engine_core::ScriptRuntime& scripts = runner_.simulation().scripts();
    std::string error;
    // A second studio finds 7777 taken and listens on any free port. The
    // registry entry is how the bridge finds it there.
    if (!server->start(port, error) && (pinned || !server->start(0, error))) {
        scripts.append_output(engine_core::ScriptRuntime::OutputKind::Error, "MCP server: " + error);
        mcp_error_ = error;
        return;
    }
    mcp_error_.clear();
    identity->entry.pid = current_pid();
    identity->entry.port = server->port();
    identity->dir = studio_registry_dir();
    show_toast("MCP server listening on http://127.0.0.1:" + std::to_string(server->port()) + "/mcp",
               jadefx::Toast::LENGTH_LONG);
    mcp_ = std::move(server);
    mcp_identity_ = std::move(identity);
    publish_studio();
}

void IdeLayout::publish_studio() {
    if (!mcp_identity_) {
        return;
    }
    const std::string project = project_ ? project_->name() : std::string("Untitled");
    std::string root;
    if (project_) {
        std::error_code ignored;
        const std::filesystem::path absolute = std::filesystem::absolute(project_->root(), ignored);
        root = utf8_path(std::filesystem::weakly_canonical(absolute, ignored));
    }
    StudioEntry entry;
    {
        std::lock_guard<std::mutex> guard(mcp_identity_->mu);
        StudioEntry& current = mcp_identity_->entry;
        if (mcp_identity_->published && current.project == project && current.root == root) {
            return;
        }
        current.project = project;
        current.root = root;
        mcp_identity_->published = true;
        entry = current;
    }
    if (mcp_identity_->dir.empty()) {
        return;
    }
    std::string error;
    if (!write_studio(mcp_identity_->dir, entry, error)) {
        runner_.simulation().scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error,
                                                     "MCP studio registry: " + error);
    }
}

}  // namespace ide
