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

// One CSS instance's Source, docked beside the scene view as the legacy
// engine's CSS editor was. Double-clicking a CSS, or Edit, opens it. The text
// is highlighted as CSS: selectors, property names, numbers with their units,
// hex colors, strings, and comments. Enter keeps the line's indent, and adds a
// level after {; typing { adds its }. Typing writes Source back half a second
// after the last keystroke, as one undo step, and the GUIs restyle; while the
// simulation is stopped that becomes the authored place, so Stop keeps it.
// Ctrl/Cmd+Z undoes the typing itself.
class IdeCssEditor : public IdePane {
public:
    IdeCssEditor(engine_core::Engine& engine, std::uint32_t id);
    ~IdeCssEditor() override;

    std::uint32_t instanceId() const { return id_; }
    bool isLoaded() const { return loaded_; }
    std::string text() const;
    // Typed text Source does not have yet. flush() writes it.
    bool hasUnflushedText() const { return dirty_; }

    void focus();
    // Writes the buffer to Source.
    void flush();

protected:
    void layoutChildren() override;
    void onOpen() override;
    void onClose() override;

private:
    class Area;
    struct Commit;

    // False when the DataModel was busy. alive is false when the instance is
    // gone or is no longer a CSS.
    bool readSource(std::string& text, std::string& name, bool& alive, std::uint32_t* world = nullptr) const;
    void load();
    void paint(const std::string& text);
    void noteText();
    void push(const std::string& text);
    // After an undo, Stop, or another writer changed Source under the buffer.
    void reapply();
    void showSource(std::string text);
    void setTitleText(const std::string& name);

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
    std::uint32_t world_ = 0;
    std::uint64_t seenAuthored_ = 0;
    std::uint64_t seenTree_ = 0;
};

}  // namespace ide
