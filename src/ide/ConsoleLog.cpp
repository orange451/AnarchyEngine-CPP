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

// The marker in front of a table. Open Sans has no triangles; these two are the same width.
constexpr std::string_view kClosed = "+";
constexpr std::string_view kOpen = "−";
// Where a field row's key starts when it has no marker, so keys line up with the tables.
constexpr std::string_view kLeafGap = "   ";
constexpr std::string_view kIndent = "    ";
constexpr std::size_t kPreviewBytes = 60;

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

// {1, 2, x = 3, ...}: the array part without keys, then named fields, until it is long enough.
std::string preview(const TableSnapshot& table) {
    if (table.fields.empty() && table.omitted == 0) {
        return "{}";
    }
    std::string out = "{";
    std::size_t index = 0;
    bool first = true;
    bool cut = table.omitted > 0;
    for (const TableField& field : table.fields) {
        ++index;
        std::string item = field.table ? "{...}" : oneLine(field.value);
        if (field.key != "[" + std::to_string(index) + "]") {
            item = field.key + " = " + item;
        }
        if (!first && out.size() + item.size() > kPreviewBytes) {
            cut = true;
            break;
        }
        if (!first) {
            out += ", ";
        }
        out += item;
        first = false;
    }
    if (cut) {
        out += ", ...";
    }
    out += "}";
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
    define("preview", jadefx::Color::rgb8(120, 124, 130));
    rows_.assign(static_cast<std::size_t>(paragraphCount()), Row{});
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
            Toggle toggle;
            toggle.table = value.table;
            toggle.begin = units(row.text);
            row.text.append(kClosed);
            row.text.push_back(' ');
            row.text += oneLine(value.text);
            toggle.end = units(row.text);
            row.spans.push_back(Span{toggle.begin, toggle.begin + 1, "toggle"});
            row.text.push_back(' ');
            const int previewAt = units(row.text);
            row.text += preview(*value.table);
            row.spans.push_back(Span{previewAt, units(row.text), "preview"});
            row.row.toggles.push_back(std::move(toggle));
        }
    }
    syncRows();
    insertRows(paragraphCount() - 1, std::move(rows));
}

std::vector<ConsoleLog::Pending> ConsoleLog::fieldRows(const Row& parent, int slot, const TableSnapshot& table) const {
    std::vector<Pending> rows;
    std::string indent;
    for (int i = 0; i <= parent.depth; ++i) {
        indent += kIndent;
    }
    auto make = [&] {
        Pending pending;
        pending.row.depth = parent.depth + 1;
        pending.row.slot = slot;
        pending.row.stamp = parent.stamp;
        pending.stampStyle = "gutter";
        pending.text = indent;
        return pending;
    };
    for (const TableField& field : table.fields) {
        Pending pending = make();
        std::string& text = pending.text;
        Toggle toggle;
        if (field.table) {
            toggle.table = field.table;
            toggle.begin = units(text);
            text.append(kClosed);
            text.push_back(' ');
            pending.spans.push_back(Span{toggle.begin, toggle.begin + 1, "toggle"});
        } else {
            text.append(kLeafGap);
        }
        const int keyAt = units(text);
        text += oneLine(field.key);
        pending.spans.push_back(Span{keyAt, units(text), "key"});
        text += " = ";
        text += oneLine(field.value);
        if (field.table) {
            toggle.end = units(text);
            text.push_back(' ');
            const int previewAt = units(text);
            text += preview(*field.table);
            pending.spans.push_back(Span{previewAt, units(text), "preview"});
            pending.row.toggles.push_back(std::move(toggle));
        }
        rows.push_back(std::move(pending));
    }
    if (table.omitted > 0) {
        Pending pending = make();
        pending.text.append(kLeafGap);
        const int at = units(pending.text);
        pending.text += "... " + std::to_string(table.omitted) + " more";
        pending.spans.push_back(Span{at, units(pending.text), "preview"});
        rows.push_back(std::move(pending));
    }
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
            toggle.end += prefix;
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
        if (column >= toggles[i].begin && column < toggles[i].end) {
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
        Toggle& toggle = rows_[at].toggles[static_cast<std::size_t>(slot)];
        toggle.open = !toggle.open;
        const bool open = toggle.open;
        const std::shared_ptr<const TableSnapshot> table = toggle.table;
        const int marker = absolutePosition(paragraph, toggle.begin);
        replaceText(marker, marker + 1, std::string(open ? kOpen : kClosed));
        setStyleClass(marker, marker + 1, "toggle");
        if (open) {
            insertRows(begin, fieldRows(rows_[at], slot, *table));
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
    // cursorAt leaves the scroll bars out, so a press on a bar over a label still scrolls.
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
