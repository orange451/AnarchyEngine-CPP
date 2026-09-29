#include "ide/ConsoleLog.hpp"
#include "ide/IdeTheme.hpp"

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

TableField Field(std::string key, std::string value, std::shared_ptr<const TableSnapshot> table = nullptr,
                 std::string note = {}) {
    TableField field;
    field.key = std::move(key);
    field.value = std::move(value);
    field.table = std::move(table);
    field.note = std::move(note);
    return field;
}

std::shared_ptr<const TableSnapshot> MakeTable() {
    auto inner = std::make_shared<TableSnapshot>();
    inner->fields.push_back(Field("x", "1"));
    auto outer = std::make_shared<TableSnapshot>();
    outer->fields.push_back(Field("[1]", "10"));
    outer->fields.push_back(Field("name", "\"bob\""));
    outer->fields.push_back(Field("pos", "table: 0x2", inner));
    outer->fields.push_back(Field("none", "table: 0x3", std::make_shared<TableSnapshot>()));
    outer->fields.push_back(Field("self", "table: 0x1", nullptr, "cycle"));
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

ScriptRuntime::OutputLine ScriptLine(std::string text, std::uint32_t script, int line,
                                     std::vector<ScriptRuntime::OutputValue> values = {}) {
    ScriptRuntime::OutputLine out = PrintLine(std::move(text), std::move(values));
    out.script = script;
    out.line = line;
    return out;
}

// A link underlines while the pointer is on any run of it.
bool Underlined(const ide::ConsoleLog& log, int paragraph, int column) {
    const std::string href = log.linkAt(log.absolutePosition(paragraph, column));
    return !href.empty() && href == log.hoveredLink();
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
    log->appendLine(PrintLine("t\ttable: 0x1\ttable: 0x4\n",
                              {{"t", nullptr}, {"table: 0x1", MakeTable()}, {"table: 0x4", std::make_shared<TableSnapshot>()}}));
    log->appendLine(PrintLine("after\n", {}));

    Expect(log->paragraphCount() == 4, "three rows and the empty last paragraph");
    Expect(Body(*log, 1) == "t\t{...}\t{}", "a table prints closed, and an empty one as {}");

    Expect(!log->toggleAt(1, 0), "the stamp is not a toggle");
    Expect(!log->toggleAt(1, Column(*log, 1, "{}")), "an empty table does not open");
    Expect(log->toggleAt(1, Column(*log, 1, "...")), "the ... opens the table");
    Expect(log->paragraphCount() == 11, "one row per field, one for the rest, and the closing brace");
    Expect(Body(*log, 1) == "t\t{\t{}", "an open table leaves its brace on the row");
    Expect(Body(*log, 2) == "    [1] = 10,", "a field is key = value with a comma");
    Expect(Body(*log, 3) == "    name = \"bob\",", "a string field keeps its quotes");
    Expect(Body(*log, 4) == "    pos = {...},", "a table field shows closed");
    Expect(Body(*log, 5) == "    none = {},", "an empty table field shows {}");
    Expect(Body(*log, 6) == "    self = <cycle>,", "a table inside itself is named");
    Expect(Body(*log, 7) == "    ... 3 more", "fields left out of the copy are counted");
    Expect(Body(*log, 8) == "}", "the closing brace lines up with the row that opened it");
    Expect(Body(*log, 9) == "after", "later rows move down");
    Expect(!log->toggleAt(5, Column(*log, 5, "{}")), "an empty table field does not open");

    Expect(log->toggleAt(4, Column(*log, 4, "{...}") + 1), "a table field opens from its braces");
    Expect(log->paragraphCount() == 13, "the nested field and its brace go in");
    Expect(Body(*log, 4) == "    pos = {", "the field's comma moves to its closing brace");
    Expect(Body(*log, 5) == "        x = 1,", "a nested field is one level deeper");
    Expect(Body(*log, 6) == "    },", "the nested brace keeps the field's comma");
    Expect(Body(*log, 7) == "    none = {},", "the rest of the fields follow");

    Expect(log->toggleAt(4, Column(*log, 4, "{")), "the open brace of a field closes it");
    Expect(log->paragraphCount() == 11 && Body(*log, 4) == "    pos = {...},", "closing puts {...}, back");
    Expect(log->toggleAt(4, Column(*log, 4, "{...}")), "the field opens again");
    Expect(log->toggleAt(1, Column(*log, 1, "{")), "the open brace closes the table");
    Expect(log->paragraphCount() == 4 && Body(*log, 2) == "after", "closing takes the nested rows with it");
    Expect(Body(*log, 1) == "t\t{...}\t{}", "a closed table reads as it did");
    Expect(log->toggleAt(1, Column(*log, 1, "{...}")), "the table opens again");
    Expect(log->paragraphCount() == 11 && Body(*log, 4) == "    pos = {...},", "reopening starts with the fields closed");
    log->toggleAt(1, Column(*log, 1, "{"));

    // A click on {...} goes through the same toggle.
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
    Expect(clicked, "{...} shows a pointer");
    Expect(log->paragraphCount() == 11, "a click opens the table");

    // The hand shows over the braces' own glyphs: from the left edge of { to the right edge of },
    // never over the half glyph in front of them.
    {
        log->clearLog();
        log->appendLine(PrintLine("t\ttable: 0x1\n", {{"t", nullptr}, {"table: 0x1", MakeTable()}}));
        scene->layout(600, 300, 1);
        const int brace = Column(*log, 0, "{...}");
        auto gapX = [&](int column, double& x, double& y) {
            log->moveTo(0, column);
            const jadefx::TextBounds bounds = log->caretBounds();
            x = bounds.x;
            y = bounds.y + bounds.height * 0.5;
            return bounds.valid;
        };
        double left = 0;
        double right = 0;
        double y = 0;
        const bool measured = gapX(brace, left, y) && gapX(brace + 5, right, y);
        Expect(measured, "the braces are on screen");
        double first = -1;
        double last = -1;
        for (double x = 0; x < 600; x += 0.25) {
            if (log->cursorAt(x, y) == jadefx::Cursor::Pointer) {
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        Expect(first >= left && first < left + 1, "the hand starts at the left edge of {");
        Expect(last < right && last > right - 1, "the hand ends at the right edge of }");
        const int at = log->absolutePosition(0, brace);
        const jadefx::StyleSpans spans = log->getStyleSpans(at, at + 5);
        const jadefx::IndexRange link = log->linkRange(at + 1);
        Expect(spans.spans().size() == 1 && spans.spans()[0].style.styleClass == "toggle",
               "all of {...} takes the toggle style");
        Expect(link.start == at && link.end == at + 5, "and is one link");
    }

    // Toggles and printed text underline only while the pointer is on them.
    // A line a script printed opens that script at its line.
    {
        auto probe = jadefx::make<ide::ConsoleLog>();
        auto probeScene = jadefx::make<jadefx::Scene>(probe, 600, 300);
        probe->setPrefWidthRatio(1);
        probe->setPrefHeightRatio(1);
        std::uint32_t opened = 0;
        int openedLine = 0;
        probe->setOnOpenScript([&](std::uint32_t script, int line) {
            opened = script;
            openedLine = line;
        });
        probe->appendLine(ScriptLine("hello\n", 7, 3));
        probe->appendLine(PrintLine("host\n", {}));
        probe->appendLine(ScriptLine("t\ttable: 0x1\n", 9, 5, {{"t", nullptr}, {"table: 0x1", MakeTable()}}));
        ScriptRuntime::OutputLine error = ScriptLine("oops\n", 7, 1);
        error.kind = ScriptRuntime::OutputKind::Error;
        probe->appendLine(error);
        probeScene->layout(600, 300, 0);

        const int text = Column(*probe, 0, "hello");
        const int brace = Column(*probe, 2, "{...}");
        Expect(!Underlined(*probe, 0, text) && !Underlined(*probe, 2, brace), "nothing is underlined at rest");

        auto centre = [&](int paragraph, int column, double& x, double& y) {
            probe->moveTo(paragraph, column);
            const jadefx::TextBounds left = probe->caretBounds();
            probe->moveTo(paragraph, column + 1);
            const jadefx::TextBounds right = probe->caretBounds();
            x = (left.x + right.x) * 0.5;
            y = left.y + left.height * 0.5;
        };
        double x = 0;
        double y = 0;
        centre(0, text + 1, x, y);
        probeScene->noteMove(x, y);
        Expect(probe->cursorAt(x, y) == jadefx::Cursor::Pointer, "printed text shows a pointer");
        Expect(Underlined(*probe, 0, text) && Underlined(*probe, 0, text + 4), "the hovered print underlines");
        Expect(!Underlined(*probe, 0, 0), "the stamp does not underline");
        Expect(!Underlined(*probe, 2, brace), "another row does not underline");

        centre(2, brace + 1, x, y);
        probeScene->noteMove(x, y);
        Expect(probe->cursorAt(x, y) == jadefx::Cursor::Pointer, "a toggle still shows a pointer");
        Expect(Underlined(*probe, 2, brace) && !Underlined(*probe, 2, Column(*probe, 2, "t\t")),
               "a hovered toggle underlines, and not the text around it");
        Expect(!Underlined(*probe, 0, text), "moving off a print takes its underline away");

        centre(1, Column(*probe, 1, "host"), x, y);
        probeScene->noteMove(x, y);
        Expect(probe->cursorAt(x, y) == jadefx::Cursor::Text, "a line no script printed is plain text");
        Expect(probe->hoveredLink().empty(), "and hovers nothing");
        centre(3, Column(*probe, 3, "oops"), x, y);
        Expect(probe->cursorAt(x, y) == jadefx::Cursor::Text, "an error is not a link");

        Expect(!probe->openAt(0, 0), "the stamp does not open the script");
        Expect(!probe->openAt(1, Column(*probe, 1, "host")), "a host line opens nothing");
        Expect(probe->openAt(2, Column(*probe, 2, "t\t")) && opened == 9 && openedLine == 5,
               "the text beside a table opens its script");
        Expect(!probe->openAt(2, brace), "the table's braces toggle, not open");

        opened = 0;
        centre(0, text + 1, x, y);
        probeScene->noteButton(0, true, x, y, 0);
        probeScene->noteButton(0, false, x, y, 0);
        Expect(opened == 7 && openedLine == 3, "a click on printed text opens the script at its line");

        opened = 0;
        double endX = 0;
        double endY = 0;
        centre(0, text + 4, endX, endY);
        probeScene->noteButton(0, true, x, y, 0);
        probeScene->noteMove(endX, endY);
        probeScene->noteButton(0, false, endX, endY, 0);
        Expect(opened == 0, "a drag across printed text selects it instead");
    }

    // Toggles and printed text draw in the theme's text color, not link blue.
    {
        struct Styled : ide::ConsoleLog {
            using jadefx::StyleClassedTextArea::resolveStyle;
        };
        auto probe = jadefx::make<Styled>();
        auto probeScene = jadefx::make<jadefx::Scene>(probe, 600, 300);
        probe->appendLine(ScriptLine("t\ttable: 0x1\n", 9, 5, {{"t", nullptr}, {"table: 0x1", MakeTable()}}));
        const int brace = probe->absolutePosition(0, Column(*probe, 0, "{...}"));
        const jadefx::StyleSpans spans = probe->getStyleSpans(brace, brace + 5);
        Expect(!probe->resolveStyle(spans.spans().front().style).hasFill, "a toggle sets no fill of its own");
        auto linksTakeText = [](const ide::ConsoleLog& console, const char* message) {
            const jadefx::Color link = console.themeColor(jadefx::ThemeColor::Link);
            const jadefx::Color text = console.computedStyle().color;
            Expect(link.r == text.r && link.g == text.g && link.b == text.b && link.a == text.a, message);
        };
        for (const char* theme : {jadefx::Theme::LIGHT, jadefx::Theme::DARK}) {
            probeScene->setUserAgentStylesheet(theme);
            probeScene->layout(600, 300, 0);
            linksTakeText(*probe, "links take the theme's text color");
        }

        // In the studio the console pane sizes the log with an inline style, and each studio
        // theme sets its own --link-color. Only the console's links ignore it.
        auto console = jadefx::make<ide::ConsoleLog>();
        console->setStyle("width: 100%; height: 100%;");
        auto elsewhere = jadefx::make<jadefx::StyledTextArea>();
        auto studio = jadefx::make<jadefx::HBox>();
        studio->getChildren().add(console);
        studio->getChildren().add(elsewhere);
        auto studioScene = jadefx::make<jadefx::Scene>(studio, 600, 300);
        for (const char* theme : {"light", "dark", "classic-studio", "dracula", "monokai", "nord", "one-dark",
                                  "solarized-dark", "solarized-light"}) {
            ide::set_current_theme(ide::shipped_theme(theme));
            studioScene->layout(600, 300, 0);
            linksTakeText(*console, "links keep the text color under a studio theme and a sizing style");
            const jadefx::Color link = elsewhere->themeColor(jadefx::ThemeColor::Link);
            const jadefx::Color themed = ide::theme_color("--link-color");
            Expect(link.r == themed.r && link.g == themed.g && link.b == themed.b,
                   "a text area outside the console keeps the theme's link color");
        }
        ide::set_current_theme(ide::shipped_theme("light"));
    }

    log->clearLog();
    Expect(log->paragraphCount() == 1 && log->getText().empty(), "clearing empties the log");
    Expect(!log->toggleAt(0, 0), "a cleared log has no toggles");

    {
        // A long session keeps the newest rows, and their links still lead home.
        auto capped = jadefx::make<ide::ConsoleLog>();
        capped->setMaxRows(10);
        std::uint32_t opened = 0;
        int openedLine = 0;
        capped->setOnOpenScript([&](std::uint32_t script, int line) {
            opened = script;
            openedLine = line;
        });
        for (int i = 0; i < 30; ++i) {
            capped->appendLine(ScriptLine("line " + std::to_string(i) + "\n", static_cast<std::uint32_t>(100 + i), i));
        }
        const int rows = capped->paragraphCount() - 1;
        Expect(rows <= 10 && rows > 0, "the log keeps at most its rows");
        Expect(Body(*capped, rows - 1) == "line 29", "the newest row stays");
        Expect(Body(*capped, 0) != "line 0", "the oldest rows go");
        const int first = 30 - rows;
        Expect(Body(*capped, 0) == "line " + std::to_string(first), "rows go from the top");
        Expect(capped->openAt(0, Column(*capped, 0, "line")) && opened == static_cast<std::uint32_t>(100 + first) &&
                   openedLine == first,
               "a kept row still opens its own script");
    }

    if (gFailures == 0) {
        std::printf("console log tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", gFailures);
    return 1;
}
