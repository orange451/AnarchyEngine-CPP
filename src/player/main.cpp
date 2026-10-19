#include "ide/IdeResources.hpp"
#include "player/PlayerArgs.hpp"
#include "runner/CommandLine.hpp"
#include "jadefx/jadefx.hpp"
#include "runner/GamePack.hpp"
#include "runner/GameView.hpp"
#include "runner/ProfilerOverlay.hpp"
#include "runner/Runner.hpp"
#include "runner/UiFrameProfile.hpp"

#include "Engine.hpp"
#include "Project.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "profiler/ProfileJson.hpp"

#include "ClientJoinHost.hpp"
#include "ProjectIdentity.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"

#include <fstream>
#include <ctime>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

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

// Shown over the game while it joins a server.
constexpr const char* kConnecting = "Connecting…";

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
fs::path FindGame(const fs::path& folder, std::string& error, bool& packed) {
    packed = false;
    // A folder, for playing a project as the studio saved it.
    if (!folder.empty()) {
        return folder;
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
        fs::path unpacked;
        if (!runner::unpack_game(pack, *where, temp / "AnarchyEngine" / "games", unpacked, error)) {
            error = "The game could not be unpacked: " + error;
            return {};
        }
        // The shaders it was exported with come before any beside the program.
        // JadeFX looks for its own in shaders/ under the working folder.
        const fs::path resources = unpacked / runner::kPackedResources;
        ide::set_resource_override(resources);
        fs::current_path(resources, failure);
        packed = true;
        return unpacked / runner::kPackedProject;
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
    // root is the project to play, or error says why there is none. connect is
    // the server to join, "host:port"; empty, the game plays on its own.
    AnarchyPlayer(fs::path root, std::string error, bool packed, std::string connect)
        : root_(std::move(root)), error_(std::move(error)), packed_(packed), connect_(std::move(connect)) {}

    // The host and its session never outlive the Game they write to, and the
    // engine never pumps a session that is gone: the engine stops, the Game and
    // the engine let go of the host, then it goes.
    ~AnarchyPlayer() override {
        if (host_ != nullptr) {
            engine_core::Engine& engine = runner_.simulation();
            engine.stop();
            engine.datamodel().set_join_host(nullptr);
            engine.set_network(nullptr);
            host_.reset();
        }
    }

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
        // Connections:Join, and --connect, go through one host, made before the
        // engine starts since it binds a job. A game that cannot say which it is
        // gets none, and a script's Join then says only a client can join.
        network::GameIdentity identity;
        fs::path resources;
        std::string identityError;
        if (engine_network::project_identity_unhashed(*project_, identity, resources, identityError)) {
            host_ = std::make_unique<engine_network::ClientJoinHost>(engine, identity, resources);
            // A packed game's empty resources folder comes out missing; made again,
            // just before the first join hashes it, it hashes as the original.
            if (packed_) {
                host_->set_before_hash([](const fs::path& folder) { runner::restore_packed_resources(folder); });
            }
            engine.datamodel().set_join_host(host_.get());
        }
        // Joining a server: the world is the server's, so the project only says
        // which game this is, and its own place is cleared as it connects.
        if (!connect_.empty() && !joinServer(identityError, error)) {
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
        if (connecting_) {
            // The game, under a message that says how the connection is going.
            auto stack = jadefx::make<jadefx::StackPane>();
            stack->setPrefWidthRatio(1);
            stack->setPrefHeightRatio(1);
            stack->getChildren().add(std::move(view));
            auto overlay = jadefx::make<jadefx::VBox>();
            overlay->getClassList().add("player-message");
            overlay->setPrefWidthRatio(1);
            overlay->setPrefHeightRatio(1);
            overlay->setAlignment(jadefx::Pos::Center);
            auto label = jadefx::make<jadefx::Label>(kConnecting);
            label_ = label.get();
            overlay->getChildren().add(std::move(label));
            overlay_ = overlay.get();
            stack->getChildren().add(std::move(overlay));
            scene->setRoot(std::move(stack));
        } else {
            scene->setRoot(std::move(view));
        }
        stage.setScene(std::move(scene));
        stage.setTitle(project_->name());
        // Paced as the studio paces it: no swap wait, frames capped at kFrameRate.
        // Pacing by the swap interval held the player near 60 on a Mac laptop.
        stage.setMaxFrameRate(kFrameRate);
        engine_core::ScriptRuntime* scripts = &engine.scripts();
        stage.setFrameTail([this, scripts] {
            PrintOutput(*scripts);
            showStatus();
        });
        // The window's own work each frame, on the profiler's UI row.
        runner::profile_ui_frames(stage);
        // The view's first paint is what lets the engine loop leave its wait.
        runner_.start();
        // Joining, the session starts play itself once the server lets it in.
        if (!connecting_) {
            // As Test does in the studio: the place as loaded is what plays.
            engine.on_simulation([](engine_core::DataModel& game) {
                if (!game.simulation_running()) {
                    game.capture_place();
                    game.start_simulation();
                }
            });
        }
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
    // Joins connect_ through the host, with the engine not yet started: the
    // server's address, then an empty world and a FromEmpty session the engine
    // pumps, which connects on the engine's first step, once the window is up.
    // False, with why, when the game cannot say which it is (identityError), the
    // server is not found, the resources cannot be read, or there is no socket. A
    // connect the endpoint refuses still goes on: the overlay shows "Could not
    // connect to the server" in place of "Connecting…".
    bool joinServer(const std::string& identityError, std::string& error) {
        if (host_ == nullptr) {
            error = identityError;
            return false;
        }
        // The resolver blocks, once, before the game's first frame.
        std::string why;
        const std::optional<network::Address> found = network::resolve(connect_, why);
        if (!found) {
            error = "Could not find the server: " + why;
            return false;
        }
        // The connect itself, and its timeout, wait for the engine's first step.
        if (!host_->connect_from_empty(*found, error)) {
            return false;
        }
        connecting_ = true;
        return true;
    }

    // Each frame, the overlay follows the session: shown while connecting, gone
    // once the world has arrived, and back with the reason when the connection ends.
    void showStatus() {
        if (host_ == nullptr || overlay_ == nullptr) {
            return;
        }
        const engine_network::ClientStatus status = host_->status();
        if (status.state == shownState_ && status.reason == shownReason_) {
            return;
        }
        shownState_ = status.state;
        shownReason_ = status.reason;
        switch (status.state) {
        case engine_network::ClientState::Joined:
            overlay_->setVisible(false);
            break;
        case engine_network::ClientState::Disconnected:
            label_->setText(status.reason);
            overlay_->setVisible(true);
            break;
        default:
            label_->setText(kConnecting);
            overlay_->setVisible(true);
            break;
        }
    }

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
    std::string connect_;
    runner::Runner runner_;
    std::unique_ptr<engine_core::Project> project_;
    // Connections:Join and --connect, through one session it owns. Null when
    // the game cannot say which it is.
    std::unique_ptr<engine_network::ClientJoinHost> host_;
    // --connect only: the overlay shows how the connection is going. A script's
    // join shows none, since the game draws its own loading screen.
    bool connecting_ = false;
    jadefx::Node* overlay_ = nullptr;
    jadefx::Label* label_ = nullptr;
    engine_network::ClientState shownState_ = engine_network::ClientState::Idle;
    std::string shownReason_;
};

}  // namespace

int main(int argc, char** argv) {
    // Unpacked before launch: JadeFX loads its shaders as it opens the window,
    // before start, and a packed game's are only on disk once this is done.
    std::string error;
    bool packed = false;
    fs::path root;
    std::string connect;
    // UTF-8, so a project folder whose name is outside the Windows code page still opens.
    std::vector<std::string> utf8 = runner::utf8_arguments(argc, argv);
    std::vector<char*> pointers;
    for (std::string& arg : utf8) {
        pointers.push_back(arg.data());
    }
    pointers.push_back(nullptr);
    if (const std::optional<player::PlayerArgs> args =
            player::parse_player_args(static_cast<int>(utf8.size()), pointers.data(), error)) {
        root = FindGame(args->folder, error, packed);
        connect = args->connect;
    }
    return jadefx::Application::launch(
        std::make_unique<AnarchyPlayer>(std::move(root), std::move(error), packed, std::move(connect)), argc, argv);
}
