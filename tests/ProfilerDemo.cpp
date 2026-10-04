#include "ide/IdeTheme.hpp"
#include "runner/GameView.hpp"
#include "runner/ProfilerOverlay.hpp"
#include "runner/Runner.hpp"
#include "runner/ViewCapture.hpp"
#include "runner/gl.hpp"

#include "Engine.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "profiler/Profiler.hpp"

#include "jadefx/jadefx.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// By hand, not ctest: profiler-demo out-dir plays a small place with a slow
// script and falling boxes under the profiler, and saves what it shows: the
// live Timeline, the slowest frame paused with a scope's tooltip, and the
// Scopes table.
namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 760;

const char* kEnemy = R"(
local RunService = game:GetService("RunService")
local frame = 0
RunService.Heartbeat:Connect(function()
    frame += 1
    debug.profilebegin("pathfind")
    local steps = if frame % 45 == 0 then 900000 else 40000
    local total = 0
    for i = 1, steps do
        total += math.sqrt(i)
    end
    debug.profileend()
    debug.profilebegin("steer")
    for i = 1, 8000 do
        total += i
    end
    debug.profileend()
end)
)";

const char* kSpawner = R"(
local floor = Instance.new("PhysicsObject")
floor.Anchored = true
floor.Size = Vector3.new(60, 1, 60)
floor.Parent = workspace
for i = 1, 80 do
    local box = Instance.new("PhysicsObject")
    box.Size = Vector3.new(1, 1, 1)
    box.Transform = box.Transform + Vector3.new((i % 9) * 1.4 - 6, 4 + i * 0.5, math.floor(i / 9) * 1.4 - 6)
    box.Parent = workspace
end
while true do
    task.wait(0.2)
    local total = 0
    for i = 1, 60000 do
        total += i
    end
end
)";

const char* kCamera = R"(
game:GetService("RunService").RenderStepped:Connect(function()
    local total = 0
    for i = 1, 6000 do
        total += i
    end
end)
)";

class ProfilerDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        ide::set_current_theme(ide::shipped_theme("dark"));
        runner_.prepare();
        engine_core::Engine& engine = runner_.simulation();
        engine.analysis().set_enabled(false);
        runner_.setSceneGrid(true);
        auto view = jadefx::make<runner::GameView>(runner_, "Scene View");
        view->setPrefWidthRatio(1);
        view->setPrefHeightRatio(1);
        view_ = view.get();
        auto scene = jadefx::make<jadefx::Scene>(view, kWidth, kHeight);
        scene_ = scene.get();
        stage.setScene(scene);
        stage.setTitle("Profiler demo");
        stage_ = &stage;
        runner_.start();
        engine.on_simulation([](engine_core::DataModel& game) {
            const engine_core::InstanceId workspace = game.scene_service("Workspace");
            const std::pair<const char*, const char*> scripts[] = {
                {"EnemyAI", kEnemy}, {"Spawner", kSpawner}, {"CameraScript", kCamera}};
            for (const auto& [name, source] : scripts) {
                engine_core::Script& script = game.create<engine_core::Script>();
                game.set_name(script.id(), name);
                script.set_source(source);
                game.set_parent(script.id(), workspace);
            }
            game.capture_place();
            game.start_simulation();
        });
        engine.resume();
        runner::ProfilerUi::get().setShown(true);
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
        stage.setFrameTail([this] {
            for (const auto& line : runner_.simulation().scripts().drain_output().lines) {
                std::fputs(line.text.c_str(), stdout);
            }
        });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {kWidth, kHeight}; }
    std::string defaultTitle() const override { return "Profiler demo"; }

private:
    void click(double x, double y) {
        scene_->noteMove(x, y);
        scene_->noteButton(0, true, x, y);
        scene_->noteButton(0, false, x, y);
    }

    void frame(int width, int height) {
        ++frames_;
        runner::ProfilerOverlay& overlay = view_->profilerOverlay();
        if (frames_ == 420) {
            save(width, height, "1-timeline-live.png");
            // The slowest recent frame: its bar, clicked, pauses on it.
            std::size_t slowest = 0;
            std::size_t count = 0;
            profiler::with_view([&](const profiler::History& history) {
                count = history.frames.size();
                double longest = 0;
                for (std::size_t index = 0; index < count; ++index) {
                    if (profiler::frame_ms(history.frames[index]) > longest) {
                        longest = profiler::frame_ms(history.frames[index]);
                        slowest = index;
                    }
                }
            });
            if (count > 0) {
                const runner::ProfilerOverlay::Rect bar = overlay.barRect(slowest, count);
                click(bar.x + bar.w * 0.5, bar.y + bar.h * 0.5);
            }
        } else if (frames_ == 432) {
            runner::ProfilerOverlay::Rect block;
            if (overlay.blockRect(profiler::intern("pathfind", profiler::Group::User), block)) {
                scene_->noteMove(block.x + block.w * 0.5, block.y + block.h * 0.5);
            }
        } else if (frames_ == 440) {
            save(width, height, "2-timeline-paused-spike.png");
            const runner::ProfilerOverlay::Rect tab = overlay.tabRect(runner::ProfilerUi::Tab::Scopes);
            click(tab.x + tab.w * 0.5, tab.y + tab.h * 0.5);
            scene_->noteMove(10, kHeight - 10);
        } else if (frames_ == 450) {
            save(width, height, "3-scopes.png");
        } else if (frames_ == 452) {
            stage_->close();
        }
    }

    void save(int width, int height, const std::string& name) {
        runner::LoadGl([](const char* symbol) { return reinterpret_cast<void*>(glfwGetProcAddress(symbol)); });
        runner::ViewPixels pixels;
        pixels.width = width;
        pixels.height = height;
        std::vector<unsigned char> bottom_up(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, bottom_up.data());
        pixels.rgba.resize(bottom_up.size());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y) {
            std::copy_n(bottom_up.data() + row * static_cast<std::size_t>(height - 1 - y), row,
                        pixels.rgba.data() + row * static_cast<std::size_t>(y));
        }
        const std::string path = out_dir_ + "/" + name;
        std::ofstream(path, std::ios::binary) << runner::EncodePng(pixels);
        std::printf("saved %s (%dx%d)\n", path.c_str(), width, height);
    }

    runner::Runner runner_;
    runner::GameView* view_ = nullptr;
    jadefx::Scene* scene_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    int frames_ = 0;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<ProfilerDemo>(), argc, argv); }
