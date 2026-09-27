#pragma once

#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ide {

// The console's read-only log. Each row starts with the wall time it was recorded.
// A printed table shows as `{...}`, or `{}` when it is empty. Clicking `{...}` opens it
// the way Luau writes a table: `{` on that row, one `key = value,` row per field, then `}`.
// A field that is a table shows `{...}` and opens the same way. Clicking the `{` of an
// open table closes it and everything opened inside it.
// Opening reads the copy print made, so it shows the table as it was when printed.
// The rest of a line a script printed is a link to that script: clicking it opens the
// script at the line that printed. Toggles and links underline while the pointer is on them.
class ConsoleLog : public jadefx::StyleClassedTextArea {
public:
    ConsoleLog();

    void appendLine(const engine_core::ScriptRuntime::OutputLine& line);
    void clearLog();

    // Opens or closes the table whose braces cover this spot. False when none does.
    bool toggleAt(int paragraph, int column);
    // Called with the script and line when a printed line is clicked.
    void setOnOpenScript(std::function<void(std::uint32_t script, int line)> handler) {
        onOpenScript_ = std::move(handler);
    }
    // Opens the script that printed the text at this spot. False when no script did.
    bool openAt(int paragraph, int column);

    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent& event) override;
    jadefx::Cursor cursorAt(double x, double y) const override;

    // The hover mark of what the pointer is on, -1 for none. The spans carrying it underline.
    int hoveredMark() const { return hovered_; }

protected:
    jadefx::TextStyle resolveStyle(const jadefx::TextStyle& style) const override;

private:
    struct Toggle {
        // Column of the opening brace in the paragraph.
        int begin = 0;
        std::shared_ptr<const engine_core::TableSnapshot> table;
        bool open = false;
        // A field's table ends with a comma: after `{...}` when closed, after `}` when open.
        bool comma = false;
        // Its hover mark, a style class that underlines while the pointer is on it.
        int mark = -1;
    };

    // Where the rest of a printed row leads.
    struct Link {
        std::uint32_t script = 0;
        int line = 0;
        int mark = -1;
    };

    // One per paragraph, including the empty one after the last newline.
    // A field row is one deeper than the row it opened from; slot is which of
    // that row's toggles it belongs to.
    struct Row {
        int depth = 0;
        int slot = -1;
        std::string stamp;
        std::vector<Toggle> toggles;
        // Index into links_, -1 for a row nothing links from.
        int link = -1;
    };

    struct Span {
        int begin = 0;
        int end = 0;
        std::string style;
    };

    // A row that is about to go in: its text without the stamp or the newline.
    struct Pending {
        Row row;
        std::string text;
        std::vector<Span> spans;
        const char* stampStyle = "time";
        const char* textStyle = nullptr;
    };

    void insertRows(int paragraph, std::vector<Pending> rows);
    // The fields of parent's table at slot, then the closing brace.
    std::vector<Pending> fieldRows(const Row& parent, int slot) const;
    // What the toggle shows in its row, and where its clickable part ends.
    static std::string toggleText(const Toggle& toggle);
    static int clickEnd(const Toggle& toggle);
    bool spotUnder(double x, double y, int& paragraph, int& column) const;
    int findToggle(int paragraph, int column) const;
    // The link of the printed text at this spot, or null. Toggles and the stamp are not in it.
    const Link* findLink(int paragraph, int column) const;
    // The mark of what is under the pointer, -1 for none.
    int markUnder(double x, double y) const;
    static std::string markClass(int mark);
    void syncRows();

    std::vector<Row> rows_;
    std::vector<Link> links_;
    std::function<void(std::uint32_t, int)> onOpenScript_;
    mutable int nextMark_ = 0;
    // Set while the pointer moves, so it is mutable for cursorAt.
    mutable int hovered_ = -1;
    // The link a press landed on. A release on the same link without a drag opens it.
    int pressedLink_ = -1;
};

}  // namespace ide
