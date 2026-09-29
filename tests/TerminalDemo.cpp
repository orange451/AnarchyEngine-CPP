#include "ide/IdeTerminal.hpp"
#include "ide/IdeTheme.hpp"
#include "ide/Utf8.hpp"
#include "runner/ViewCapture.hpp"
#include "runner/gl.hpp"

#include "jadefx/jadefx.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

// The Terminal page alone in a window, drawing the user's shell. It types a
// command, waits, saves the window as a PNG, and quits: a look by hand at what
// the page draws for a real program.
//   terminal-demo <png> <seconds> <theme> <command...>
namespace {

class TerminalDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        png_ = argc > 1 ? argv[1] : "terminal.png";
        seconds_ = argc > 2 ? std::atof(argv[2]) : 8.0;
        ide::set_current_theme(ide::shipped_theme(argc > 3 ? argv[3] : "light"));
        for (int i = 4; i < argc; ++i) {
            command_ += (command_.empty() ? "" : " ") + std::string(argv[i]);
        }
        // @file types the file's UTF-8 text, which Windows' ANSI arguments can't carry.
        if (!command_.empty() && command_[0] == '@') {
            std::ifstream file(command_.substr(1), std::ios::binary);
            command_.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        auto terminal = jadefx::make<ide::IdeTerminal>();
        terminal->setPrefWidthRatio(1);
        terminal->setPrefHeightRatio(1);
        terminal_ = terminal.get();
        stage.setScene(jadefx::make<jadefx::Scene>(terminal, 1000, 640));
        terminal_->view().requestFocus();
        stage_ = &stage;
        begin_ = std::chrono::steady_clock::now();
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {1000, 640}; }
    std::string defaultTitle() const override { return "Terminal demo"; }

private:
    void frame(int width, int height) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin_).count();
        if (!typed_ && elapsed > 2.5) {
            typed_ = true;
            ide::TerminalScreen& screen = terminal_->view().screen();
            for (const char32_t c : ide::Utf32(command_)) {
                screen.character(c, ide::TerminalMods{});
            }
            screen.key(ide::TerminalKey::Enter, ide::TerminalMods{});
        }
        if (saved_ || elapsed < seconds_) {
            return;
        }
        saved_ = true;
        runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); });
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
        std::ofstream(png_, std::ios::binary) << runner::EncodePng(pixels);
        std::printf("saved %s (%dx%d), title \"%s\"\n", png_.c_str(), width, height, terminal_->title().c_str());
        stage_->close();
    }

    ide::IdeTerminal* terminal_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string png_;
    std::string command_;
    double seconds_ = 8.0;
    bool typed_ = false;
    bool saved_ = false;
    std::chrono::steady_clock::time_point begin_;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<TerminalDemo>(), argc, argv); }
