#include "ide/IdeResources.hpp"
#include "jadefx/jadefx.hpp"
#include "runner/GamePack.hpp"
#include "runner/GameView.hpp"
#include "runner/ProfilerOverlay.hpp"
#include "runner/Runner.hpp"

#include "Engine.hpp"
#include "Project.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "profiler/ProfileJson.hpp"

#include <fstream>
#include <ctime>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#if defined(_WIN32)
// As in the studio: run on the discrete GPU of a machine that has two.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

namespace fs = std::filesystem;

// Frames per second the player draws at most, the studio's default.
constexpr double kFrameRate = 120.0;

// The player draws only the game, so its stylesheet is only the view's own
// background and the message shown when there is no game to play.
constexpr const char* kStylesheet = R"(
.ide-viewport {
    background-color: #000000;
}
.player-message {
    background-color: #1e1e1e;
}
.player-message label {
    color: #f2f2f2;
    font-size: 15px;
}
)";

// The project to play: the folder named on the command line, or else the
// game packed into this program, or on a Mac into its bundle, extracted.
fs::path FindGame(int argc, char** argv, std::string& error, bool& packed) {
    packed = false;
    // A folder, for playing a project as the studio saved it. Finder may pass
    // -psn_ arguments; flags are not folders.
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '-') {
        return fs::path(argv[1]);
    }
    const fs::path self = ide::executable_path();
    fs::path packs[] = {self, self.empty() ? fs::path() : self.parent_path() / ".." / "Resources" / runner::kBundlePack};
    for (const fs::path& pack : packs) {
        if (pack.empty()) {
            continue;
        }
        const std::optional<runner::PackLocation> where = runner::find_game_pack(pack);
        if (!where) {
            continue;
        }
        std::error_code failure;
        const fs::path temp = fs::temp_directory_path(failure);
        if (failure || temp.empty()) {
            error = "This system has no temporary folder to unpack the game into.";
            return {};
        }
        fs::path folder;
        if (!runner::unpack_game(pack, *where, temp / "AnarchyEngine" / "games", folder, error)) {
            error = "The game could not be unpacked: " + error;
            return {};
        }
        // The shaders it was exported with come before any beside the program.
        // JadeFX looks for its own in shaders/ under the working folder.
        const fs::path resources = folder / runner::kPackedResources;
        ide::set_resource_override(resources);
        fs::current_path(resources, failure);
        packed = true;
        return folder / runner::kPackedProject;
    }
    error = "There is no game in this player. Export one from the studio with File > Export Game, "
            "or start the player with a project folder: AnarchyPlayer <folder>";
    return {};
}

// Prints and errors from the game's scripts go to the terminal, if there is one.
void PrintOutput(engine_core::ScriptRuntime& scripts) {
    for (const engine_core::ScriptRuntime::OutputLine& line : scripts.drain_output().lines) {
        std::FILE* stream = line.kind == engine_core::ScriptRuntime::OutputKind::Error ? stderr : stdout;
        std::fputs(line.text.c_str(), stream);
    }
    std::fflush(stdout);
}

// Plays one exported game, or a project folder, in a window of its own: no
// studio, only the Scene View, playing from the first frame.
class AnarchyPlayer : public jadefx::Application {
public:
    // root is the project to play, or error says why there is none.
    AnarchyPlayer(fs::path root, std::string error, bool packed)
        : root_(std::move(root)), error_(std::move(error)), packed_(packed) {}

    void start(jadefx::Stage& stage, int, char**) override {
        const jadefx::Size size = defaultWindowSize();
        auto scene = jadefx::make<jadefx::Scene>(nullptr, size.width, size.height);
        scene->setPadding(jadefx::Insets{});
        scene->setStylesheet(kStylesheet);
        std::string error = error_;
        const fs::path& root = root_;
        runner_.prepare();
        engine_core::Engine& engine = runner_.simulation();
        // Nothing in the player reads diagnostics, so its scripts are not checked.
        engine.analysis().set_enabled(false);
        if (error.empty()) {
            try {
                project_ = std::make_unique<engine_core::Project>(engine_core::Project::load(root, engine.datamodel()));
            } catch (const std::exception& failure) {
                error = std::string("The game could not be opened: ") + failure.what();
            }
        }
        if (!error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            showMessage(stage, std::move(scene), error);
            return;
        }
        // Grid, outlines, and the camera list belong to the studio.
        runner_.setSceneGrid(false);
        auto view = jadefx::make<runner::GameView>(runner_, project_->name());
        view->setPlayerView(true);
        view->setPrefWidthRatio(1);
        view->setPrefHeightRatio(1);
        runner::GameView* shown = view.get();
        scene->setRoot(std::move(view));
        stage.setScene(std::move(scene));
        stage.setTitle(project_->name());
        // Paced as the studio paces it: no swap wait, frames capped at kFrameRate.
        // Pacing by the swap interval held the player near 60 on a Mac laptop.
        stage.setMaxFrameRate(kFrameRate);
        engine_core::ScriptRuntime* scripts = &engine.scripts();
        stage.setFrameTail([scripts] { PrintOutput(*scripts); });
        // The view's first paint is what lets the render thread leave its wait.
        runner_.start();
        // As Test does in the studio: the place as loaded is what plays.
        engine.on_simulation([](engine_core::DataModel& game) {
            if (!game.simulation_running()) {
                game.capture_place();
                game.start_simulation();
            }
        });
        engine.resume();
        // Cmd+F6 shows the profiler; Save, while paused, writes beside the game.
        runner::ProfilerUi::get().save = [this] {
            const fs::path file = runner::player_capture_folder(root_, packed_, ide::executable_path()) /
                                  profiler::capture_file_name(std::time(nullptr));
            std::string text;
            profiler::with_view([&](const profiler::History& history) {
                text = profiler::write_capture_html(history, project_ ? project_->name() : std::string("Game"),
                                                    profiler::utc_stamp(std::time(nullptr)));
            });
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
            out.close();
            std::printf(out ? "Saved a profile to %s\n" : "Could not save a profile to %s\n", file.string().c_str());
            std::fflush(stdout);
        };
        // The game has the keyboard from the start, with no click first.
        jadefx::runLater([shown] { shown->requestFocus(); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {1280, 720}; }

    std::string defaultTitle() const override { return "Anarchy Player"; }

    // The scene draws as fast as the cap allows, and the simulation steps with it.
    int swapInterval() const override { return 0; }

private:
    static void showMessage(jadefx::Stage& stage, std::shared_ptr<jadefx::Scene> scene, const std::string& text) {
        // A label does not wrap, so each sentence gets a line of its own.
        auto pane = jadefx::make<jadefx::VBox>();
        pane->getClassList().add("player-message");
        pane->setPrefWidthRatio(1);
        pane->setPrefHeightRatio(1);
        pane->setPadding(jadefx::Insets{24, 24, 24, 24});
        pane->setSpacing(6);
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find(". ", start);
            end = end == std::string::npos ? text.size() : end + 1;
            pane->getChildren().add(jadefx::make<jadefx::Label>(text.substr(start, end - start)));
            start = end;
            while (start < text.size() && text[start] == ' ') {
                ++start;
            }
        }
        scene->setRoot(std::move(pane));
        stage.setScene(std::move(scene));
    }

    // Declared before the project, which reads and writes its engine's place.
    fs::path root_;
    std::string error_;
    bool packed_ = false;
    runner::Runner runner_;
    std::unique_ptr<engine_core::Project> project_;
};

}  // namespace

int main(int argc, char** argv) {
    // Unpacked before launch: JadeFX loads its shaders as it opens the window,
    // before start, and a packed game's are only on disk once this is done.
    std::string error;
    bool packed = false;
    fs::path root = FindGame(argc, argv, error, packed);
    return jadefx::Application::launch(std::make_unique<AnarchyPlayer>(std::move(root), std::move(error), packed),
                                       argc, argv);
}
