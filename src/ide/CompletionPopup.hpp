#pragma once

#include "LuauComplete.hpp"

#include <cstdint>
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
    // The typed quote matches an argument string and one row is the choice.
    // `unclosed_only` ignores a string whose closer is already in the buffer.
    // The editor passes false when that closer is the next character, so typing
    // it still chooses the row and steps past the quote.
    bool commitsQuote(char quote, bool unclosed_only = true) const;
    bool keyAccepts() const;
    bool accepting() const;

    void dismiss();
    void move(int delta);
    // Copies the highlighted row and closes the list. `text` is the buffer the
    // caret sits in, so a call that already has '(' does not gain another pair.
    std::optional<CompletionEdit> take(bool parentheses, std::string_view text);
    void finish();

    void present(const CompletionList& list, bool force, jadefx::Node& owner, double caret_x, double caret_y,
                 double caret_height);
    // While the list for the text as it is now is on its way: keeps the rows
    // shown that still start with `frame.prefix` and takes the frame's range,
    // so accepting in the meantime replaces the right text. Closes when the
    // frame is another site or starts elsewhere, or no row is left.
    void narrow(const CompletionList& frame, jadefx::Node& owner, double caret_x, double caret_y,
                double caret_height);
    // How many times the list has closed. A list asked for before a close is
    // not shown after it.
    std::uint64_t dismissals() const;
    void moveTo(jadefx::Node& owner, double caret_x, double caret_y, double caret_height);

private:
    struct State;

    void fill();
    void place(jadefx::Node& owner, double caret_x, double caret_y, double caret_height);
    const CompletionItem* highlighted() const;

    std::unique_ptr<State> state_;
};

// What completion_world read last, and at which revisions of the place. A
// keystroke that changed nothing in the place copies nothing from it. One per
// editor or console.
struct CompletionWorldCache {
    const void* world = nullptr;
    std::uint64_t tree = ~std::uint64_t{0};
    std::uint64_t authored = ~std::uint64_t{0};
    std::uint64_t sources = ~std::uint64_t{0};
    std::vector<engine_core::LuaNode> nodes;
    // Each node's LuaSource::source_version when copied; 0 for a node that is
    // not a script.
    std::vector<std::uint64_t> versions;
};

// The instance tree completion reads, kept in `cache` and read again only when
// the tree or an authored value changed. When only Sources did, as the editor's
// own typing does, only the scripts that changed are copied again. `buffer` replaces the
// source of `script_id` when that script is the one open in an editor. A read
// that cannot take the lock in time, as while a game runs, answers with the
// last tree read, so completion never sees an empty place. UI thread only.
const std::vector<engine_core::LuaNode>& completion_world(engine_core::Engine& engine, std::uint32_t script_id,
                                                          const std::string* buffer, CompletionWorldCache& cache);

}  // namespace ide
