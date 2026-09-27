#pragma once

#include "ColorLiterals.hpp"
#include "CompletionPopup.hpp"
#include "IdePane.hpp"
#include "LuaApi.hpp"
#include "ide/TextUndoStack.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine_core {
class Engine;
}

namespace ide {

class ScriptCodeArea;

// One Luau source, docked beside the scene view. Typing writes Source back
// onto the instance. While the simulation is stopped that write also becomes
// the authored place, so Stop keeps the edit. A double-click or Edit opens it.
class IdeScriptEditor : public IdePane {
    friend class ScriptCodeArea;

public:
    // Watches its script in analysis until the editor is destroyed.
    IdeScriptEditor(engine_core::Engine& engine, std::uint32_t id);
    ~IdeScriptEditor() override;

    std::uint32_t instanceId() const { return id_; }

    // The tab title, including the .lua suffix.
    void setOnTitle(std::function<void(const std::string&)> handler);
    void setTitleText(const std::string& name);

    bool isLoaded() const { return loaded_; }
    std::string text() const;
    // Typed text the script's Source does not have yet. flush() writes it.
    bool hasUnflushedText() const { return dirty_; }

    void focus();
    // Puts the caret at the start of a 1-based line and scrolls to it.
    void showLine(int line);
    // Writes the buffer to the instance. While stopped, captures the place.
    void flush();
    // The document stack Ctrl/Cmd-Z edits. Null until the shell binds one.
    void bindUndo(TextUndoStack* stack);
    // Copies the stack into the buffer after InputRouter has undone or redone it.
    void applyUndoText();
    // After Stop restores the place, put this buffer back when it differs.
    void reapply();

protected:
    void layoutChildren() override;
    void onOpen() override;
    void onClose() override;

private:
    struct Commit;

    bool read_source(std::string& text, std::string& name, bool& alive, std::uint32_t* world = nullptr) const;
    void show_source(std::string text);
    void load();
    void paint();
    void refresh_marks();
    void note_text();
    void push(const std::string& text);
    void refresh_completion(bool force);
    void dismiss_completion();
    void accept_completion(bool parentheses);
    void move_completion(int delta);
    void place_completion();
    bool completion_open() const;
    bool completion_commits_name() const;
    bool completion_commits_quote(char quote, bool unclosed_only = true) const;
    // Enter and Tab accept when the highlighted name would change the text.
    // A finished name keeps those keys, so a newline or indent still works.
    bool completion_key_accepts() const;
    std::vector<engine_core::LuaNode> world() const;
    // A swatch before each Color3 literal. A click on one opens the color picker on it.
    void refresh_color_swatches();
    void open_color_picker(std::size_t index);
    // Ends the picker: keep writes one undo step for the whole session, and
    // otherwise the literal goes back to how it was.
    void close_color_picker(bool keep);
    void write_color(const engine_core::Color3& color);

    engine_core::Engine& engine_;
    std::uint32_t id_ = 0;
    std::shared_ptr<jadefx::CodeArea> area_;
    std::shared_ptr<jadefx::Label> status_;
    CompletionPopup completion_;
    std::shared_ptr<Commit> commit_;
    std::function<void(const std::string&)> on_title_;
    std::string shown_name_;
    TextUndoStack* undo_stack_ = nullptr;
    bool mute_undo_ = false;
    bool loading_ = false;
    bool loaded_ = false;
    bool missing_ = false;
    bool dirty_ = false;
    // world_generation the buffer last matched. Stop bumps it.
    std::uint32_t world_ = 0;
    std::chrono::steady_clock::time_point dirty_at_{};

    // The literal the picker is editing, as it was when the picker opened.
    struct ColorEdit {
        Color3Literal literal;
        std::string original;
        int key_hook = 0;
    };
    std::vector<Color3Literal> color_literals_;
    std::vector<std::shared_ptr<jadefx::Pane>> color_swatches_;
    std::shared_ptr<jadefx::ColorChooser> color_chooser_;
    std::unique_ptr<ColorEdit> color_edit_;
};

}  // namespace ide
