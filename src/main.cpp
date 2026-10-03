#include "ide/IdeLayout.hpp"
#include "ide/IdeResources.hpp"
#include "jadefx/jadefx.hpp"

#include "Engine.hpp"
#include "Project.hpp"

#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
// On a machine with integrated and discrete GPUs, the NVIDIA and AMD drivers
// read these from the exe and run it on the discrete one. Without them Windows
// may hand the GL context to the integrated GPU. They must be exported by the
// exe itself; the same symbols in a DLL are ignored.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

// Desktop shell. JadeFX owns the window, the GL context, and the frame loop.
class AnarchyEngine : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        const jadefx::Size size = defaultWindowSize();
        // The shell owns the runner, which owns the Lua engine. It has to outlive
        // start(); the scene keeps the widgets, and this member keeps the engine.
        // Preferences and themes live in the user's config folder.
        layout_ = std::make_unique<ide::IdeLayout>(size.width, size.height, ide::config_directory());
        engine_core::Engine& simulation = layout_->simulation();
        // A new studio holds a new place, the one File > New makes.
        engine_core::Project::reset_place(simulation.datamodel());
        layout_->start();
        layout_->apply_mcp_setting();
        auto scene = jadefx::make<jadefx::Scene>(nullptr, size.width, size.height);
        layout_->mount(*scene);
        // Setting the scene sizes the window to it, so the saved size goes on after.
        stage.setScene(std::move(scene));
        layout_->attachFrame(stage);
        // AnarchyStudio <folder> opens that project on the first frame.
        // Finder may pass -psn_ arguments; flags are not folders.
        if (argc > 1 && argv[1] != nullptr && argv[1][0] != '-') {
            const std::filesystem::path root(argv[1]);
            jadefx::runLater([this, root]() { layout_->open_project_at(root); });
        }
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {1280, 800}; }

    std::string defaultTitle() const override { return "Anarchy Engine"; }

    // The scene draws as fast as the frame allows. The simulation stays at 60 Hz.
    int swapInterval() const override { return 0; }

private:
    std::unique_ptr<ide::IdeLayout> layout_;
};

}  // namespace

int main(int argc, char** argv) {
    return jadefx::Application::launch(std::make_unique<AnarchyEngine>(), argc, argv);
}
