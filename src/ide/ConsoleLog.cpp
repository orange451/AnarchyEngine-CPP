#include "ConsoleLog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <utility>

namespace ide {
namespace {

using engine_core::ScriptRuntime;
using engine_core::TableField;
using engine_core::TableSnapshot;

constexpr std::string_view kClosed = "{...}";
constexpr std::string_view kOpen = "{";
constexpr std::string_view kEmpty = "{}";
constexpr std::string_view kIndent = "    ";

struct Editable {
    jadefx::StyledTextArea& area;
    bool previous;
    explicit Editable(jadefx::StyledTextArea& area) : area(area), previous(area.isEditable()) { area.setEditable(true); }
    ~Editable() { area.setEditable(previous); }
};

// Code points, the unit the text area counts in.
int units(std::string_view text) {
    int count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

std::string formatStamp(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) {
        return "00:00:00.000";
    }
#else
    if (localtime_r(&seconds, &local) == nullptr) {
        return "00:00:00.000";
    }
#endif
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) % 1000;
    int ms = static_cast<int>(millis.count());
    if (ms < 0) {
        ms += 1000;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, ms);
    return buffer;
}

// A field row is one paragraph, so a __tostring with line breaks is flattened.
std::string oneLine(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    return out;
}

bool isEmpty(const TableSnapshot& table) { return table.fields.empty() && table.omitted == 0; }

std::string indentFor(int depth) {
    std::string out;
    for (int i = 0; i < depth; ++i) {
        out += kIndent;
    }
    return out;
}

}  // namespace

ConsoleLog::ConsoleLog() {
    setEditable(false);
    setWrapText(true);
    setShowCaret(CaretVisibility::Off);
    setFollowCaret(true);
    // New lines take the plain style. Error ranges are painted after they are inserted,
    // so a following print does not inherit the red.
    setUseInitialStyleForInsertion(true);
    suspendUndo();
    auto define = [this](const char* name, jadefx::Color fill) {
        jadefx::TextStyle style;
        style.hasFill = true;
        style.fill = fill;
        defineStyleClass(name, style);
    };
    define("error", jadefx::Color::rgb8(176, 0, 32));
    define("command", jadefx::Color::rgb8(18, 78, 148));
    define("time", jadefx::Color::rgb8(120, 124, 130));
    // A field row repeats its table's stamp so the columns line up and a copy keeps it,
    // but only the first row of a print shows one.
    define("gutter", jadefx::Color::rgb8(0, 0, 0, 0));
    define("toggle", jadefx::Color::rgb8(18, 78, 148));
    define("key", jadefx::Color::rgb8(136, 19, 145));
    define("note", jadefx::Color::rgb8(120, 124, 130));
    rows_.assign(static_cast<std::size_t>(paragraphCount()), Row{});
}

std::string ConsoleLog::toggleText(const Toggle& toggle) {
    std::string text(toggle.open ? kOpen : kClosed);
    if (toggle.comma && !toggle.open) {
        text.push_back(',');
    }
    return text;
}

int ConsoleLog::clickEnd(const Toggle& toggle) {
    return toggle.begin + units(toggle.open ? kOpen : kClosed);
}

void ConsoleLog::clearLog() {
    Editable editing(*this);
    clear();
    rows_.assign(static_cast<std::size_t>(paragraphCount()), Row{});
}

void ConsoleLog::syncRows() {
    // Only this class edits the text, so this is a guard, not a path that runs.
    const std::size_t count = static_cast<std::size_t>(paragraphCount());
    if (rows_.size() != count) {
        rows_.resize(count);
    }
}

void ConsoleLog::appendLine(const ScriptRuntime::OutputLine& line) {
    const std::string stamp = formatStamp(line.time);
    const char* style = line.kind == ScriptRuntime::OutputKind::Error     ? "error"
                        : line.kind == ScriptRuntime::OutputKind::Command ? "command"
                                                                          : nullptr;
    std::vector<Pending> rows;
    auto next = [&] {
        Pending pending;
        pending.row.stamp = stamp;
        pending.textStyle = style;
        rows.push_back(std::move(pending));
    };
    next();
    // Text is split at its newlines. A trailing newline does not start an empty row.
    auto addText = [&](std::string_view text) {
        std::size_t begin = 0;
        while (begin < text.size()) {
            const std::size_t newline = text.find('\n', begin);
            const std::size_t stop = newline == std::string_view::npos ? text.size() : newline;
            std::string_view segment = text.substr(begin, stop - begin);
            if (!segment.empty() && segment.back() == '\r') {
                segment.remove_suffix(1);
            }
            rows.back().text.append(segment);
            if (newline == std::string_view::npos || newline + 1 == text.size()) {
                break;
            }
            next();
            begin = newline + 1;
        }
    };
    if (line.values.empty()) {
        addText(line.text);
    } else {
        for (std::size_t i = 0; i < line.values.size(); ++i) {
            const ScriptRuntime::OutputValue& value = line.values[i];
            if (i > 0) {
                rows.back().text.push_back('\t');
            }
            if (!value.table) {
                addText(value.text);
                continue;
            }
            Pending& row = rows.back();
            if (isEmpty(*value.table)) {
                row.text.append(kEmpty);
                continue;
            }
            Toggle toggle;
            toggle.table = value.table;
            toggle.begin = units(row.text);
            row.text += toggleText(toggle);
            row.spans.push_back(Span{toggle.begin, clickEnd(toggle), "toggle"});
            row.row.toggles.push_back(std::move(toggle));
        }
    }
    syncRows();
    insertRows(paragraphCount() - 1, std::move(rows));
}

std::vector<ConsoleLog::Pending> ConsoleLog::fieldRows(const Row& parent, int slot) const {
    const Toggle& opened = parent.toggles[static_cast<std::size_t>(slot)];
    const TableSnapshot& table = *opened.table;
    std::vector<Pending> rows;
    auto make = [&](int indent) {
        Pending pending;
        pending.row.depth = parent.depth + 1;
        pending.row.slot = slot;
        pending.row.stamp = parent.stamp;
        pending.stampStyle = "gutter";
        pending.text = indentFor(indent);
        return pending;
    };
    for (const TableField& field : table.fields) {
        Pending pending = make(parent.depth + 1);
        std::string& text = pending.text;
        const int keyAt = units(text);
        text += oneLine(field.key);
        pending.spans.push_back(Span{keyAt, units(text), "key"});
        text += " = ";
        if (field.table && !isEmpty(*field.table)) {
            Toggle toggle;
            toggle.table = field.table;
            toggle.comma = true;
            toggle.begin = units(text);
            text += toggleText(toggle);
            pending.spans.push_back(Span{toggle.begin, clickEnd(toggle), "toggle"});
            pending.row.toggles.push_back(std::move(toggle));
            rows.push_back(std::move(pending));
            continue;
        }
        if (field.table) {
            text.append(kEmpty);
        } else if (!field.note.empty()) {
            const int noteAt = units(text);
            text += "<" + field.note + ">";
            pending.spans.push_back(Span{noteAt, units(text), "note"});
        } else {
            text += oneLine(field.value);
        }
        text.push_back(',');
        rows.push_back(std::move(pending));
    }
    if (table.omitted > 0) {
        Pending pending = make(parent.depth + 1);
        const int at = units(pending.text);
        pending.text += "... " + std::to_string(table.omitted) + " more";
        pending.spans.push_back(Span{at, units(pending.text), "note"});
        rows.push_back(std::move(pending));
    }
    // The brace lines up with the row that opened it. A field keeps its comma after it.
    Pending close = make(parent.depth);
    close.text.push_back('}');
    if (opened.comma) {
        close.text.push_back(',');
    }
    rows.push_back(std::move(close));
    return rows;
}

void ConsoleLog::insertRows(int paragraph, std::vector<Pending> rows) {
    if (rows.empty()) {
        return;
    }
    // Each row is stamp, space, text, newline, put in front of the paragraph at column 0.
    std::string text;
    for (Pending& pending : rows) {
        const int prefix = units(pending.row.stamp) + 1;
        for (Toggle& toggle : pending.row.toggles) {
            toggle.begin += prefix;
        }
        text += pending.row.stamp;
        text.push_back(' ');
        text += pending.text;
        text.push_back('\n');
    }
    Editable editing(*this);
    int offset = absolutePosition(paragraph, 0);
    insertText(offset, text);
    std::vector<Row> inserted;
    inserted.reserve(rows.size());
    for (Pending& pending : rows) {
        const int stampEnd = offset + units(pending.row.stamp);
        const int textAt = stampEnd + 1;
        const int end = textAt + units(pending.text) + 1;
        if (stampEnd > offset) {
            setStyleClass(offset, stampEnd, pending.stampStyle);
        }
        if (pending.textStyle != nullptr) {
            setStyleClass(textAt, end, pending.textStyle);
        }
        for (const Span& span : pending.spans) {
            if (span.end > span.begin) {
                setStyleClass(textAt + span.begin, textAt + span.end, span.style);
            }
        }
        inserted.push_back(std::move(pending.row));
        offset = end;
    }
    rows_.insert(rows_.begin() + paragraph, std::make_move_iterator(inserted.begin()),
                 std::make_move_iterator(inserted.end()));
}

int ConsoleLog::findToggle(int paragraph, int column) const {
    if (paragraph < 0 || paragraph >= static_cast<int>(rows_.size())) {
        return -1;
    }
    const std::vector<Toggle>& toggles = rows_[static_cast<std::size_t>(paragraph)].toggles;
    for (std::size_t i = 0; i < toggles.size(); ++i) {
        if (column >= toggles[i].begin && column < clickEnd(toggles[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ConsoleLog::toggleAt(int paragraph, int column) {
    syncRows();
    const int slot = findToggle(paragraph, column);
    if (slot < 0) {
        return false;
    }
    const std::size_t at = static_cast<std::size_t>(paragraph);
    const int depth = rows_[at].depth;
    // The rows under this one: its own fields, grouped by slot, each followed by what they opened.
    int last = paragraph + 1;
    while (last < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(last)].depth > depth) {
        ++last;
    }
    auto childOf = [&](int index, bool after) {
        const Row& row = rows_[static_cast<std::size_t>(index)];
        return row.depth == depth + 1 && (after ? row.slot > slot : row.slot >= slot);
    };
    int begin = paragraph + 1;
    while (begin < last && !childOf(begin, false)) {
        ++begin;
    }
    int end = begin;
    while (end < last && !childOf(end, true)) {
        ++end;
    }

    // Opening a table far up the log leaves the view where it is.
    const double scrollX = getScrollX();
    const double scrollY = getScrollY();
    const bool follow = isFollowCaret();
    setFollowCaret(false);
    {
        Editable editing(*this);
        std::vector<Toggle>& toggles = rows_[at].toggles;
        Toggle& toggle = toggles[static_cast<std::size_t>(slot)];
        const std::string before = toggleText(toggle);
        toggle.open = !toggle.open;
        const bool open = toggle.open;
        const std::string after = toggleText(toggle);
        const int marker = absolutePosition(paragraph, toggle.begin);
        replaceText(marker, marker + units(before), after);
        setStyleClass(marker, marker + clickEnd(toggle) - toggle.begin, "toggle");
        // A later table on the same row moves with the text in front of it.
        const int shift = units(after) - units(before);
        for (std::size_t i = static_cast<std::size_t>(slot) + 1; i < toggles.size(); ++i) {
            toggles[i].begin += shift;
        }
        if (open) {
            insertRows(begin, fieldRows(rows_[at], slot));
        } else if (end > begin) {
            deleteText(absolutePosition(begin, 0), absolutePosition(end, 0));
            rows_.erase(rows_.begin() + begin, rows_.begin() + end);
        }
    }
    // A new line scrolls the log to the bottom, as it did before the click.
    moveTo(length());
    setFollowCaret(follow);
    scrollTo(scrollX, scrollY);
    return true;
}

const ConsoleLog::Toggle* ConsoleLog::toggleUnder(double x, double y) const {
    const jadefx::CharacterHit hitAt = hit(x, y);
    if (!hitAt.valid || hitAt.characterIndex < 0) {
        return nullptr;
    }
    const int slot = findToggle(hitAt.paragraph, hitAt.column);
    if (slot < 0) {
        return nullptr;
    }
    return &rows_[static_cast<std::size_t>(hitAt.paragraph)].toggles[static_cast<std::size_t>(slot)];
}

void ConsoleLog::handleMousePressed(const jadefx::MouseEvent& event) {
    // cursorAt leaves the scroll bars out, so a press on a bar over a brace still scrolls.
    if (event.button == 0 && !event.shift() && !event.shortcut() &&
        cursorAt(event.x, event.y) == jadefx::Cursor::Pointer) {
        const jadefx::CharacterHit hitAt = hit(event.x, event.y);
        toggleAt(hitAt.paragraph, hitAt.column);
        return;
    }
    jadefx::StyleClassedTextArea::handleMousePressed(event);
}

jadefx::Cursor ConsoleLog::cursorAt(double x, double y) const {
    const jadefx::Cursor base = jadefx::StyleClassedTextArea::cursorAt(x, y);
    if (base == jadefx::Cursor::Text && toggleUnder(x, y) != nullptr) {
        return jadefx::Cursor::Pointer;
    }
    return base;
}

}  // namespace ide
