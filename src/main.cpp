#include "ide/IdeLayout.hpp"
#include "jadefx/jadefx.hpp"

#include "Engine.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"

#include <memory>
#include <string>

namespace {

// Desktop shell. JadeFX owns the window, the GL context, and the frame loop.
class AnarchyEngine : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int, char**) override {
        const jadefx::Size size = defaultWindowSize();
        // The shell owns the runner, which owns the Lua engine. It has to outlive
        // start(); the scene keeps the widgets, and this member keeps the engine.
        layout_ = std::make_unique<ide::IdeLayout>(size.width, size.height);
        engine_core::Engine& simulation = layout_->simulation();
        engine_core::DataModel& model = simulation.datamodel();
        // View-space positions. Positive z is toward the camera.
        const float kPositions[][3] = {
            {-0.58f, 0.38f, 0.f},
            {0.58f, 0.38f, 0.15f},
            {0.f, 0.02f, 0.55f},
            {-0.58f, -0.48f, -0.4f},
            {0.58f, -0.48f, -0.15f},
        };
        const double kStartSeconds[] = {0.0, 0.4, 0.8, 1.2, 1.6};
        constexpr int kCount = 5;
        for (int index = 0; index < kCount; ++index) {
            engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
            model.set_name(triangle.id(), "Tri" + std::to_string(index));
            model.set_parent(triangle.id(), model.id());
            triangle.set_position(kPositions[index][0], kPositions[index][1], kPositions[index][2]);
            triangle.step(kStartSeconds[index]);
        }
        // Play-solo scripts. Test starts them; Stop restores these poses.
        // HopSlow and HopFast wait on different clocks so one wait cannot freeze the other.
        auto add_script = [&](const char* name, const char* source) {
            engine_core::Script& script = model.create<engine_core::Script>();
            model.set_name(script.id(), name);
            script.set_source(source);
            model.set_parent(script.id(), model.id());
        };
        add_script("HopSlow", R"(
local tri = game:FindFirstChild("Tri0")
assert(tri)
local home = tri.Position
local n = 0
while true do
    task.wait(0.5)
    n = n + 1
    local hop = (n % 2 == 1) and 0.45 or 0
    tri.Position = home + Vector3.new(hop, 0, 0)
end
)");
        add_script("HopFast", R"(
local tri = game:FindFirstChild("Tri1")
assert(tri)
local home = tri.Position
local n = 0
while true do
    task.wait(0.2)
    n = n + 1
    local hop = (n % 2 == 1) and 0.35 or 0
    tri.Position = home + Vector3.new(0, hop, 0)
end
)");
        layout_->start();
        auto scene = jadefx::make<jadefx::Scene>(nullptr, size.width, size.height);
        layout_->mount(*scene);
        layout_->attachFrame(stage);
        stage.setScene(std::move(scene));
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
