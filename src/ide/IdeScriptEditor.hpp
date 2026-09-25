#pragma once

#include "CompletionPopup.hpp"
#include "IdePane.hpp"
#include "LuaApi.hpp"

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
    IdeScriptEditor(engine_core::Engine& engine, std::uint32_t id);

    std::uint32_t instanceId() const { return id_; }

    // The tab title, including the .lua suffix.
    void setOnTitle(std::function<void(const std::string&)> handler);
    void setTitleText(const std::string& name);

    bool isLoaded() const { return loaded_; }
    std::string text() const;

    void focus();
    // Writes the buffer to the instance. While stopped, captures the place.
    void flush();
    // After Stop restores the place, put this buffer back when it differs.
    void reapply();

protected:
    void layoutChildren() override;
    void onOpen() override;
    void onClose() override;

private:
    struct Commit;

    bool read_source(std::string& text, std::string& name, bool& alive) const;
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
    bool completion_commits_quote(char quote) const;
    // Enter and Tab accept when the highlighted name would change the text.
    // A finished name keeps those keys, so a newline or indent still works.
    bool completion_key_accepts() const;
    std::vector<engine_core::LuaNode> world() const;

    engine_core::Engine& engine_;
    std::uint32_t id_ = 0;
    std::shared_ptr<jadefx::CodeArea> area_;
    std::shared_ptr<jadefx::Label> status_;
    CompletionPopup completion_;
    std::shared_ptr<Commit> commit_;
    std::function<void(const std::string&)> on_title_;
    std::string shown_name_;
    bool loading_ = false;
    bool loaded_ = false;
    bool missing_ = false;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point dirty_at_{};
};

}  // namespace ide
