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
        ide::IdeLayout layout(size.width, size.height);
        auto scene = jadefx::make<jadefx::Scene>(nullptr, size.width, size.height);
        layout.mount(*scene);
        stage.setScene(std::move(scene));
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {1280, 800}; }

    std::string defaultTitle() const override { return "Anarchy Engine"; }
};

}  // namespace

int main(int argc, char** argv) {
    return jadefx::Application::launch(std::make_unique<AnarchyEngine>(), argc, argv);
}
