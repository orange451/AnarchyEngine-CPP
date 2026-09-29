#pragma once

#include "IdeTheme.hpp"
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
// script at the line that printed. Toggles and printed lines are text area links, so they
// underline while the pointer is on them; every row of one print underlines together.
class ConsoleLog : public jadefx::StyleClassedTextArea {
public:
    ConsoleLog();

    void appendLine(const engine_core::ScriptRuntime::OutputLine& line);
    void clearLog();

    // The most rows the log keeps, open tables included. Past it the oldest go,
    // a tenth of the limit at a time, so trimming is not paid on every line.
    static constexpr int kMaxRows = 5000;
    void setMaxRows(int rows) { maxRows_ = rows; }

    // Opens or closes the table whose braces cover this spot. False when none does.
    bool toggleAt(int paragraph, int column);
    // Called with the script and line when a printed line is clicked.
    void setOnOpenScript(std::function<void(std::uint32_t script, int line)> handler) {
        onOpenScript_ = std::move(handler);
    }
    // Opens the script that printed the text at this spot. False when no script did.
    bool openAt(int paragraph, int column);

private:
    struct Toggle {
        // Column of the opening brace in the paragraph.
        int begin = 0;
        std::shared_ptr<const engine_core::TableSnapshot> table;
        bool open = false;
        // A field's table ends with a comma: after `{...}` when closed, after `}` when open.
        bool comma = false;
        // Its link, unique to it.
        std::string href;
    };

    // Where the rest of a printed row leads.
    struct Link {
        std::uint32_t script = 0;
        int line = 0;
        // Shared by every row of the print.
        std::string href;
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
    int findToggle(int paragraph, int column) const;
    // The link of the printed text at this spot, or null. Toggles and the stamp are not in it.
    const Link* findLink(int paragraph, int column) const;
    // A new href, so that no two toggles or prints hover together.
    std::string nextHref(const char* kind) const;
    // Styles a toggle's braces and links them.
    void styleToggle(int offset, const Toggle& toggle);
    void syncRows();
    // Drops the oldest rows past maxRows_, and the links only they used.
    void trimOldest();
    // The text colors, from the theme.
    void defineStyles();

    std::vector<Row> rows_;
    std::vector<Link> links_;
    std::function<void(std::uint32_t, int)> onOpenScript_;
    mutable int nextLink_ = 0;
    int maxRows_ = kMaxRows;
    ThemeListener themeListener_{[this] { defineStyles(); }};
};

}  // namespace ide
