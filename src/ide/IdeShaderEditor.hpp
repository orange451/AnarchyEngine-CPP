#pragma once

#include "IdePane.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace engine_core {
class Engine;
}

namespace ide {

class ThemeListener;

// One Shader's Source, docked beside the scene view as the CSS editor is.
// Double-clicking a Shader in the Assets pane, or Edit, opens it. The text is
// highlighted as GLSL (GlslHighlight). Enter keeps the line's indent, and adds
// a level after {; typing { adds its }. Typing writes Source back half a
// second after the last keystroke, as one undo step named "Edit Shader";
// while the simulation is stopped that becomes the authored place, so Stop
// keeps it. The Shader's Error is marked across its line and on the scroll
// bar's right side, and moves as Error does.
class IdeShaderEditor : public IdePane {
public:
    IdeShaderEditor(engine_core::Engine& engine, std::uint32_t id);
    ~IdeShaderEditor() override;

    std::uint32_t instanceId() const { return id_; }
    bool isLoaded() const { return loaded_; }
    std::string text() const;
    // Typed text Source does not have yet. flush() writes it.
    bool hasUnflushedText() const { return dirty_; }

    void focus();
    // Writes the buffer to Source.
    void flush();
    // Puts the caret at the start of a 1-based line and scrolls to it.
    void showLine(int line);
    // The 1-based line the error mark covers, or 0 for none.
    int errorLine() const { return errorLine_; }

protected:
    void layoutChildren() override;
    void onOpen() override;
    void onClose() override;

private:
    class Area;
    struct Commit;

    // False when the DataModel was busy. alive is false when the instance is
    // gone or is no longer a Shader.
    bool readSource(std::string& text, std::string& name, std::string& error, bool& alive,
                    std::uint32_t* world = nullptr) const;
    void load();
    void paint(const std::string& text);
    void noteText();
    void push(const std::string& text);
    // After an undo, Stop, or another writer changed Source under the buffer.
    void reapply();
    void showSource(std::string text);
    void setTitleText(const std::string& name);
    // Marks error's line, "<line>: <message>", or clears the mark for "".
    void markError(const std::string& error);

    engine_core::Engine& engine_;
    std::uint32_t id_ = 0;
    std::shared_ptr<Area> area_;
    std::shared_ptr<Commit> commit_;
    std::unique_ptr<ThemeListener> themeListener_;
    bool loaded_ = false;
    bool loading_ = false;
    bool missing_ = false;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point dirtyAt_{};
    std::string shownName_;
    std::string shownError_;
    int errorLine_ = 0;
    std::uint32_t world_ = 0;
    std::uint64_t seenAuthored_ = 0;
    std::uint64_t seenTree_ = 0;
};

}  // namespace ide
