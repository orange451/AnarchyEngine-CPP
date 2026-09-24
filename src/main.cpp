#include "ide/IdeLayout.hpp"
#include "jadefx/jadefx.hpp"

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
        auto scene = jadefx::make<jadefx::Scene>(nullptr, size.width, size.height);
        layout_->mount(*scene);
        stage.setScene(std::move(scene));
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {1280, 800}; }

    std::string defaultTitle() const override { return "Anarchy Engine"; }

private:
    std::unique_ptr<ide::IdeLayout> layout_;
};

}  // namespace

int main(int argc, char** argv) {
    return jadefx::Application::launch(std::make_unique<AnarchyEngine>(), argc, argv);
}
