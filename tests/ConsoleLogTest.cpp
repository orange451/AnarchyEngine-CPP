#include "ide/ConsoleLog.hpp"

#include "ScriptRuntime.hpp"
#include "TableSnapshot.hpp"
#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

// The console log opens and closes a printed table in place, through toggleAt and through a click.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

using engine_core::ScriptRuntime;
using engine_core::TableField;
using engine_core::TableSnapshot;

std::shared_ptr<const TableSnapshot> MakeTable() {
    auto inner = std::make_shared<TableSnapshot>();
    inner->fields.push_back(TableField{"x", "1", nullptr});
    auto outer = std::make_shared<TableSnapshot>();
    outer->fields.push_back(TableField{"[1]", "10", nullptr});
    outer->fields.push_back(TableField{"pos", "table: 0x2", inner});
    outer->omitted = 3;
    return outer;
}

ScriptRuntime::OutputLine PrintLine(std::string text, std::vector<ScriptRuntime::OutputValue> values) {
    ScriptRuntime::OutputLine line;
    line.text = std::move(text);
    line.values = std::move(values);
    return line;
}

// The paragraph's text after the stamp and its space.
std::string Body(const ide::ConsoleLog& log, int paragraph) {
    const std::string text = log.getText(paragraph);
    const std::size_t space = text.find(' ');
    return space == std::string::npos ? text : text.substr(space + 1);
}

int Column(const ide::ConsoleLog& log, int paragraph, const std::string& needle) {
    const std::string text = log.getText(paragraph);
    const std::size_t at = text.find(needle);
    return at == std::string::npos ? -1 : static_cast<int>(at);
}

}  // namespace

int main() {
    auto log = jadefx::make<ide::ConsoleLog>();
    log->appendLine(PrintLine("before\n", {}));
    log->appendLine(PrintLine("t\ttable: 0x1\n", {{"t", nullptr}, {"table: 0x1", MakeTable()}}));
    log->appendLine(PrintLine("after\n", {}));

    Expect(log->paragraphCount() == 4, "three rows and the empty last paragraph");
    Expect(Body(*log, 1) == "t\t+ table: 0x1 {10, pos = {...}, ...}", "a table prints closed with a preview");

    Expect(!log->toggleAt(1, 0), "the stamp is not a toggle");
    Expect(!log->toggleAt(1, Column(*log, 1, "{10")), "the preview is not a toggle");
    Expect(log->toggleAt(1, Column(*log, 1, "+")), "the + opens the table");
    Expect(log->paragraphCount() == 7, "one row per field, and one for the rest");
    Expect(Body(*log, 1).rfind("t\t− table: 0x1", 0) == 0, "an open table shows the minus");
    Expect(Body(*log, 2) == "       [1] = 10", "a plain field lines up past the marker");
    Expect(Body(*log, 3) == "    + pos = table: 0x2 {x = 1}", "a table field can open too");
    Expect(Body(*log, 4) == "       ... 3 more", "fields left out of the copy are counted");
    Expect(Body(*log, 5) == "after", "later rows move down");

    Expect(log->toggleAt(3, Column(*log, 3, "pos")), "the key of a table field opens it");
    Expect(log->paragraphCount() == 8 && Body(*log, 4) == "           x = 1", "a nested field is one level deeper");
    Expect(Body(*log, 5) == "       ... 3 more", "the nested rows go under their own table");

    Expect(log->toggleAt(1, Column(*log, 1, "table: 0x1")), "the label closes the table");
    Expect(log->paragraphCount() == 4 && Body(*log, 2) == "after", "closing takes the nested rows with it");
    Expect(log->toggleAt(1, Column(*log, 1, "+")), "the table opens again");
    Expect(log->paragraphCount() == 7 && Body(*log, 3).rfind("    + pos", 0) == 0,
           "reopening starts with the fields closed");
    log->toggleAt(1, Column(*log, 1, "−"));

    // A click on the + goes through the same toggle.
    auto scene = jadefx::make<jadefx::Scene>(log, 600, 300);
    log->setPrefWidthRatio(1);
    log->setPrefHeightRatio(1);
    scene->layout(600, 300, 0);
    bool clicked = false;
    for (double y = 0; y < 300 && !clicked; y += 2) {
        for (double x = 0; x < 600 && !clicked; x += 2) {
            if (log->cursorAt(x, y) == jadefx::Cursor::Pointer) {
                scene->noteButton(0, true, x, y, 0);
                scene->noteButton(0, false, x, y, 0);
                clicked = true;
            }
        }
    }
    Expect(clicked, "the table label shows a pointer");
    Expect(log->paragraphCount() == 7, "a click opens the table");

    log->clearLog();
    Expect(log->paragraphCount() == 1 && log->getText().empty(), "clearing empties the log");
    Expect(!log->toggleAt(0, 0), "a cleared log has no toggles");

    if (gFailures == 0) {
        std::printf("console log tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", gFailures);
    return 1;
}
