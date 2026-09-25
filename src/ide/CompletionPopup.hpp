#pragma once

#include "LuauComplete.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {
class Engine;
}

namespace jadefx {
class Node;
}

namespace ide {

// The text an accepted completion writes back into the editor.
struct CompletionEdit {
    int begin = 0;
    int end = 0;
    std::string text;
    int caret = 0;
};

// The suggestion list shared by the script editor and the command line.
class CompletionPopup {
public:
    CompletionPopup();
    ~CompletionPopup();

    void setOnAccept(std::function<void()> handler);

    bool isOpen() const;
    bool commitsName() const;
    bool keyAccepts() const;
    bool accepting() const;
    int replaceEnd() const;

    void dismiss();
    void move(int delta);
    // Copies the highlighted row and closes the list. `text` is the buffer the
    // caret sits in, so a call that already has '(' does not gain another pair.
    std::optional<CompletionEdit> take(bool parentheses, std::string_view text);
    void finish();

    void present(const CompletionList& list, bool force, jadefx::Node& owner, double caret_x, double caret_y,
                 double caret_height);
    void moveTo(jadefx::Node& owner, double caret_x, double caret_y, double caret_height);

private:
    struct State;

    void fill();
    void place(jadefx::Node& owner, double caret_x, double caret_y, double caret_height);
    const CompletionItem* highlighted() const;

    std::unique_ptr<State> state_;
};

// The instance tree completion reads. `buffer` replaces the source of `script_id`
// when that script is the one open in an editor.
std::vector<engine_core::LuaNode> completion_world(engine_core::Engine& engine, std::uint32_t script_id,
                                                   const std::string* buffer);

}  // namespace ide
