#pragma once

#include "ScriptRuntime.hpp"

#include "jadefx/jadefx.hpp"

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
class ConsoleLog : public jadefx::StyleClassedTextArea {
public:
    ConsoleLog();

    void appendLine(const engine_core::ScriptRuntime::OutputLine& line);
    void clearLog();

    // Opens or closes the table whose braces cover this spot. False when none does.
    bool toggleAt(int paragraph, int column);

    void handleMousePressed(const jadefx::MouseEvent& event) override;
    jadefx::Cursor cursorAt(double x, double y) const override;

private:
    struct Toggle {
        // Column of the opening brace in the paragraph.
        int begin = 0;
        std::shared_ptr<const engine_core::TableSnapshot> table;
        bool open = false;
        // A field's table ends with a comma: after `{...}` when closed, after `}` when open.
        bool comma = false;
    };

    // One per paragraph, including the empty one after the last newline.
    // A field row is one deeper than the row it opened from; slot is which of
    // that row's toggles it belongs to.
    struct Row {
        int depth = 0;
        int slot = -1;
        std::string stamp;
        std::vector<Toggle> toggles;
    };

    struct Span {
        int begin = 0;
        int end = 0;
        const char* style = "";
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
    const Toggle* toggleUnder(double x, double y) const;
    int findToggle(int paragraph, int column) const;
    void syncRows();

    std::vector<Row> rows_;
};

}  // namespace ide
