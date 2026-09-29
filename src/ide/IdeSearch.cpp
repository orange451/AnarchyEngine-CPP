#include "IdeSearch.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "IdeIcons.hpp"
#include "LuaSource.hpp"
#include "NodeClasses.hpp"
#include "Strings.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ide {
namespace {

constexpr std::chrono::milliseconds kLockWait(5);
// Typing waits this long before a search, and results are checked against edits this often.
constexpr double kTypingPause = 0.15;
constexpr double kRefreshEvery = 1.0;
// A line shows this much before its first match, and this much in all.
constexpr int kLeadIn = 24;
constexpr int kPreviewLength = 160;
constexpr double kRowHeight = 22;

constexpr const char* kSearchRules = R"CSS(
.search-pane {
    background-color: var(--ide-panel-color);
}
.search-header {
    padding: 8px 8px 4px 2px;
    spacing: 2px;
}
.search-summary {
    color: var(--ide-search-status-color);
    font-size: 12px;
    padding: 2px 8px 6px 8px;
}
.search-summary.error {
    color: var(--ide-search-status-error-color);
}
.search-results {
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.search-path, .search-line-number {
    color: var(--ide-search-path-color);
    font-size: 12px;
}
.search-badge {
    background-color: var(--ide-search-badge-color);
    color: var(--ide-search-badge-text-color);
    border-radius: 8px;
    font-size: 11px;
    padding: 0 6px;
}
.search-hit {
    background-color: var(--ide-find-match-color);
    border-radius: 2px;
}
)CSS";

#if defined(__APPLE__)
constexpr const char* kReplaceAllKey = "Cmd+Enter";
#else
constexpr const char* kReplaceAllKey = "Ctrl+Enter";
#endif

// A script as it is in the place, gathered under one read lock.
struct Source {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    std::string path;
    std::string text;
};

// Every Script and ModuleScript under parent, depth first in sibling order, the
// order the explorer lists them.
void collect(const engine_core::DataModel& game, engine_core::InstanceId parent, const std::string& path,
             std::vector<Source>& out, int depth) {
    for (engine_core::InstanceId child = game.first_child(parent); child != 0; child = game.next_sibling(child)) {
        std::string name = game.name(child);
        if (const auto* script = dynamic_cast<const engine_core::LuaSource*>(game.instance(child))) {
            out.push_back(Source{child, name, script->class_name(), path, script->source()});
        }
        if (depth < 512) {
            collect(game, child, path + "." + name, out, depth + 1);
        }
    }
}

std::string slice(std::string_view text, int begin, int end) {
    const std::size_t from = CodePointByte(text, begin);
    const std::size_t to = CodePointByte(text, end);
    std::string out(text.substr(from, to > from ? to - from : 0));
    // A tab would be as wide as a space in a label anyway, and a stray \r shows as a box.
    for (char& unit : out) {
        if (unit == '\t' || unit == '\r') {
            unit = ' ';
        }
    }
    return out;
}

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    if (style_class != nullptr) {
        label->getClassList().add(style_class);
    }
    label->setMouseTransparent(true);
    return label;
}

// The icon, the name, where it is, and how many matches.
std::shared_ptr<jadefx::Node> script_graphic(const ScriptHits& script) {
    auto box = jadefx::make<jadefx::HBox>();
    box->setSpacing(6);
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_view(script.class_name)) {
        icon->setPrefSize(16, 16);
        box->getChildren().add(icon);
    }
    box->getChildren().add(text_label(script.name, nullptr));
    box->getChildren().add(text_label(script.path, "search-path"));
    box->getChildren().add(text_label(std::to_string(script.matches), "search-badge"));
    return box;
}

// The line number, then the line from a little before its first match, with every match marked.
std::shared_ptr<jadefx::Node> line_graphic(const ScriptHits::Line& line) {
    auto box = jadefx::make<jadefx::HBox>();
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    auto number = text_label(std::to_string(line.line), "search-line-number");
    number->setPadding(jadefx::Insets{0, 8, 0, 0});
    box->getChildren().add(number);

    const int length = CodePoints(line.text);
    int indent = 0;
    while (indent < length && (line.text[static_cast<std::size_t>(indent)] == ' ' ||
                               line.text[static_cast<std::size_t>(indent)] == '\t')) {
        ++indent;
    }
    const int first = line.ranges.empty() ? 0 : line.ranges.front().first;
    int begin = std::min(indent, first);
    std::string lead;
    if (first - begin > kLeadIn * 2) {
        begin = first - kLeadIn;
        lead = "…";
    }
    const int end = std::min(length, begin + kPreviewLength);
    int at = begin;
    auto plain = [&](int to) {
        if (to > at) {
            box->getChildren().add(text_label(lead + slice(line.text, at, to), nullptr));
            lead.clear();
            at = to;
        }
    };
    for (const auto& [from, to] : line.ranges) {
        if (from >= end) {
            break;
        }
        plain(std::max(at, from));
        const int stop = std::min(to, end);
        if (stop > at) {
            if (!lead.empty()) {
                box->getChildren().add(text_label(lead, nullptr));
                lead.clear();
            }
            box->getChildren().add(text_label(slice(line.text, at, stop), "search-hit"));
            at = stop;
        }
    }
    plain(end);
    if (end < length) {
        box->getChildren().add(text_label("…", nullptr));
    }
    return box;
}

// Enter on a line's row opens it. Enter on a script's row still opens or closes it.
class ResultsTree : public jadefx::TreeView {
public:
    std::function<bool(jadefx::TreeItem*)> open;

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && (event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) && open &&
            open(getSelectedItem())) {
            event.consume();
            return;
        }
        TreeView::handleKey(event);
    }
};

}  // namespace

IdeSearch::IdeSearch(engine_core::Engine& engine, SearchHost host)
    : IdePane("Search", true), engine_(engine), host_(std::move(host)) {
    setIconFile("Search.png");
    getClassList().add("search-pane");
    setStylesheet(std::string(kFindStylesheet) + kSearchRules);
    setMinSize(150, 120);

    chevron_ = jadefx::make<FindButton>("FindCollapsed.png", ">", "Toggle Replace");
    chevron_->setMinSize(16, 22);
    chevron_->setPrefSize(16, 22);
    chevron_->setMaxSize(16, 22);
    chevron_->setOnAction([this] { setReplaceShown(!replace_shown_); });

    find_ = jadefx::make<SearchInput>("Search");
    find_->setStyle("width: 100%;");
    toggles_.attach(*find_, [] {});

    replace_ = jadefx::make<SearchInput>("Replace");
    replace_->setStyle("width: 100%;");
    replace_every_ = jadefx::make<FindButton>("ReplaceAll.png", "A", std::string("Replace All (") + kReplaceAllKey + ")");
    replace_every_->setOnAction([this] { replaceAll(); });
    replace_row_ = jadefx::make<jadefx::HBox>();
    replace_row_->setSpacing(2);
    replace_row_->setAlignment(jadefx::Pos::CenterLeft);
    replace_row_->setStyle("width: 100%;");
    replace_row_->getChildren().add(replace_);
    replace_row_->getChildren().add(replace_every_);

    rows_ = jadefx::make<jadefx::VBox>();
    rows_->setSpacing(4);
    rows_->setStyle("width: 100%;");
    rows_->getChildren().add(find_);
    auto header = jadefx::make<jadefx::HBox>();
    header->getClassList().add("search-header");
    header->setAlignment(jadefx::Pos::TopLeft);
    header->setStyle("width: 100%;");
    header->getChildren().add(chevron_);
    header->getChildren().add(rows_);

    summary_ = jadefx::make<jadefx::Label>("");
    summary_->getClassList().add("search-summary");
    summary_->setAlignment(jadefx::Pos::CenterLeft);
    summary_->setStyle("width: 100%;");

    auto top = jadefx::make<jadefx::VBox>();
    top->setStyle("width: 100%;");
    top->getChildren().add(header);
    top->getChildren().add(summary_);

    root_ = jadefx::make<jadefx::TreeItem>("");
    root_->setExpanded(true);
    auto tree = jadefx::make<ResultsTree>();
    tree->open = [this](jadefx::TreeItem* item) { return openRow(item); };
    tree_ = tree;
    tree_->setRoot(root_);
    tree_->getClassList().add("search-results");
    tree_->setShowRoot(false);
    tree_->setFixedCellSize(kRowHeight);
    tree_->setOnItemActivated([this](jadefx::TreeItem& item) { return openRow(&item); });
    // Row clicks bubble here after the row has selected itself.
    tree_->setOnMouseClicked([this](const jadefx::MouseEvent& event) { clicked(event); });

    row_replace_ = jadefx::make<FindButton>("Replace.png", "R", "");
    row_replace_tip_ = jadefx::make<jadefx::Tooltip>("Replace");
    jadefx::Tooltip::install(row_replace_.get(), row_replace_tip_);
    row_replace_->setOnAction([this] { accessory_clicked(); });

    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setTop(top);
    column->setCenter(tree_);
    getChildren().add(column);
    show_summary("");
}

SearchQuery IdeSearch::query() const { return toggles_.query(find_->text()); }

void IdeSearch::setFindText(const std::string& text) { find_->field().setText(text); }

void IdeSearch::focusFind() { find_->focusAll(); }

void IdeSearch::focusReplace() {
    setReplaceShown(true);
    replace_->focusAll();
}

void IdeSearch::setReplaceShown(bool shown) {
    if (replace_shown_ == shown) {
        return;
    }
    replace_shown_ = shown;
    if (shown) {
        rows_->getChildren().add(replace_row_);
    } else {
        const bool had_focus = replace_row_->isFocusWithin();
        const jadefx::HBox* row = replace_row_.get();
        rows_->getChildren().removeIf([row](const std::shared_ptr<jadefx::Node>& child) { return child.get() == row; });
        if (had_focus) {
            find_->focusAll();
        }
    }
    chevron_->setGraphic(icon_graphic(shown ? "FindExpanded.png" : "FindCollapsed.png"));
    // Replacing one row is a button on the row under the pointer, while replace is shown.
    tree_->setHoverAccessory(shown ? row_replace_ : nullptr);
}

std::string IdeSearch::summary() const { return summary_->getText(); }

void IdeSearch::show_summary(const std::string& error) {
    std::string text;
    if (!error.empty()) {
        text = "Invalid regular expression";
    } else if (searched_.pattern.empty()) {
        text = "Search every Script and ModuleScript";
    } else if (results_.empty()) {
        text = "No results";
    } else {
        std::size_t matches = 0;
        for (const ScriptHits& script : results_) {
            matches += static_cast<std::size_t>(script.matches);
        }
        text = counted(matches, "result", "results") + " in " + counted(results_.size(), "script", "scripts");
        if (capped_) {
            text += ", showing the first " + std::to_string(kMatchLimit);
        }
    }
    summary_->setText(text);
    const bool bad = !error.empty() || (!searched_.pattern.empty() && results_.empty());
    set_class(*summary_, "error", bad);
}

void IdeSearch::refresh() {
    const SearchQuery wanted = query();
    const TextSearch search(wanted);
    std::vector<Source> sources;
    if (search.ready()) {
        engine_core::DataModel& game = engine_.datamodel();
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kLockWait);
        if (!lock.owns()) {
            // The place is busy. The next layout tries again.
            return;
        }
        collect(game, game.id(), "game", sources, 0);
    }
    searched_ = wanted;
    pending_ = wanted;
    searched_once_ = true;
    if (const jadefx::Scene* scene = getScene()) {
        searched_at_ = scene->timeSeconds();
    }

    std::vector<ScriptHits> hits;
    bool capped = false;
    std::size_t left = static_cast<std::size_t>(kMatchLimit);
    for (const Source& source : sources) {
        if (left == 0) {
            capped = true;
            break;
        }
        std::optional<std::string> open;
        if (host_.editor_text) {
            open = host_.editor_text(source.id);
        }
        const std::string& text = open ? *open : source.text;
        std::vector<TextMatch> matches = search.find_all(text, left + 1);
        if (matches.size() > left) {
            matches.resize(left);
            capped = true;
        }
        if (matches.empty()) {
            continue;
        }
        left -= matches.size();
        ScriptHits script;
        script.id = source.id;
        script.name = source.name;
        script.class_name = source.class_name;
        script.path = source.path;
        script.matches = static_cast<int>(matches.size());
        for (const TextMatch& match : matches) {
            if (script.lines.empty() || script.lines.back().line != match.line + 1) {
                ScriptHits::Line line;
                line.line = match.line + 1;
                std::size_t stop = text.find('\n', match.line_byte);
                if (stop == std::string::npos) {
                    stop = text.size();
                }
                line.text = text.substr(match.line_byte, stop - match.line_byte);
                script.lines.push_back(std::move(line));
            }
            script.lines.back().ranges.emplace_back(match.column, match.column + (match.end - match.start));
        }
        hits.push_back(std::move(script));
    }
    error_ = search.error();
    find_->setInvalid(!error_.empty());
    capped_ = capped;
    if (hits != results_) {
        results_ = std::move(hits);
        rebuild();
    }
    show_summary(error_);
}

void IdeSearch::rebuild() {
    // The selected row stays selected when its line is still a result.
    std::optional<Target> selected;
    if (const Target* target = target_of(tree_->getSelectedItem())) {
        selected = *target;
    }
    jadefx::TreeItem* reselect = nullptr;
    targets_.clear();
    std::vector<std::shared_ptr<jadefx::TreeItem>> rows;
    rows.reserve(results_.size());
    for (const ScriptHits& script : results_) {
        auto row = jadefx::make<jadefx::TreeItem>("", script_graphic(script));
        const std::uint32_t id = script.id;
        row->setExpanded(collapsed_.count(id) == 0);
        row->setOnCollapsed([this, id](jadefx::TreeItem&) { collapsed_.insert(id); });
        row->setOnExpanded([this, id](jadefx::TreeItem&) { collapsed_.erase(id); });
        targets_[row.get()] = Target{id, 0, 0, 0};
        if (selected && selected->id == id && selected->line == 0) {
            reselect = row.get();
        }
        for (const ScriptHits::Line& line : script.lines) {
            auto child = jadefx::make<jadefx::TreeItem>("", line_graphic(line));
            const std::pair<int, int> first = line.ranges.front();
            targets_[child.get()] = Target{id, line.line, first.first, first.second};
            if (selected && selected->id == id && selected->line == line.line) {
                reselect = child.get();
            }
            row->getChildren().add(std::move(child));
        }
        rows.push_back(std::move(row));
    }
    root_->getChildren().setAll(std::move(rows));
    if (reselect != nullptr) {
        tree_->select(reselect);
    }
}

const IdeSearch::Target* IdeSearch::target_of(const jadefx::TreeItem* item) const {
    if (item == nullptr) {
        return nullptr;
    }
    const auto found = targets_.find(item);
    return found == targets_.end() ? nullptr : &found->second;
}

bool IdeSearch::openRow(const jadefx::TreeItem* item) {
    const Target* target = target_of(item);
    if (target == nullptr || target->line == 0) {
        return false;
    }
    if (host_.open) {
        // Copied first: opening a script can refresh the results and drop this row.
        const Target where = *target;
        host_.open(where.id, where.line, where.column, where.column_end);
    }
    return true;
}

void IdeSearch::clicked(const jadefx::MouseEvent& event) {
    if (event.button != 0 || (event.mods & (jadefx::Key::ModShift | jadefx::Key::ModControl | jadefx::Key::ModSuper)) != 0) {
        return;
    }
    // The row's own handler already selected it. The disclosure arrow, the
    // scroll bar, and the space under the rows are not row clicks.
    for (jadefx::Node* node = tree_->pick(event.x, event.y); node != nullptr && node != tree_.get();
         node = node->getParent()) {
        const std::string_view type = node->getElementType();
        if (type == "tree-disclosure-node") {
            return;
        }
        if (type == "tree-cell") {
            openRow(tree_->getSelectedItem());
            return;
        }
    }
}

void IdeSearch::accessory_clicked() {
    if (const Target* target = target_of(tree_->getHoveredItem())) {
        replaceIn(target->id, target->line);
    }
}

void IdeSearch::replaceIn(std::uint32_t id, int line) {
    if (id != 0) {
        replace(id, line);
    }
}

void IdeSearch::replaceAll(bool ask) {
    refresh();
    std::size_t matches = 0;
    for (const ScriptHits& script : results_) {
        matches += static_cast<std::size_t>(script.matches);
    }
    if (matches == 0) {
        return;
    }
    jadefx::Scene* scene = getScene();
    if (!ask || scene == nullptr) {
        replace(0, 0);
        return;
    }
    const jadefx::ButtonType go("Replace", jadefx::ButtonType::Data::OkDone);
    std::string question = "Replace " + counted(matches, "match", "matches") + " in " +
                           counted(results_.size(), "script", "scripts");
    question += replace_->text().empty() ? " with nothing?" : " with \"" + replace_->text() + "\"?";
    alert_ = std::make_shared<jadefx::Alert>(jadefx::AlertType::Confirmation, "Open editors can undo their own changes.",
                                             std::vector<jadefx::ButtonType>{go, jadefx::ButtonType::Cancel()});
    alert_->setTitle("Anarchy Engine");
    alert_->setHeaderText(question);
    const SearchQuery asked = searched_;
    alert_->setOnClosed([this, go, asked](const jadefx::ButtonType* choice) {
        // What was asked about is still what the results show.
        if (choice != nullptr && *choice == go && asked == searched_) {
            replace(0, 0);
        }
    });
    alert_->show(*scene);
}

void IdeSearch::replace(std::uint32_t id, int line) {
    const TextSearch search(searched_);
    if (!search.ready()) {
        return;
    }
    const SearchQuery query = searched_;
    const std::string replacement = replace_->text();
    std::vector<std::uint32_t> in_source;
    for (const ScriptHits& script : results_) {
        if (id != 0 && script.id != id) {
            continue;
        }
        const int done = host_.replace_in_editor ? host_.replace_in_editor(script.id, query, replacement, line) : -1;
        if (done < 0) {
            in_source.push_back(script.id);
        }
    }
    if (!in_source.empty()) {
        // Found again in the Source as it is when the write runs, which is now
        // while the place is stopped and the next step during play.
        engine_.on_simulation([ids = std::move(in_source), query, replacement, line](engine_core::DataModel& game) {
            const TextSearch again(query);
            if (!again.ready()) {
                return;
            }
            std::optional<std::string> recording;
            if (!game.simulation_running()) {
                recording = game.history().try_begin_recording("Replace in Scripts");
            }
            bool changed = false;
            for (const std::uint32_t script : ids) {
                auto* source = dynamic_cast<engine_core::LuaSource*>(game.instance(script));
                if (source == nullptr) {
                    continue;
                }
                int count = 0;
                std::string next = again.replace_all(source->source(), replacement, line - 1, &count);
                if (count > 0) {
                    source->set_source(std::move(next));
                    changed = true;
                }
            }
            if (recording) {
                game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
            }
            if (changed && !game.simulation_running()) {
                game.capture_place();
            }
        });
    }
    refresh();
}

void IdeSearch::layoutChildren() {
    const jadefx::Scene* scene = getScene();
    const double now = scene != nullptr ? scene->timeSeconds() : 0;
    const SearchQuery wanted = query();
    if (!searched_once_ || wanted != searched_) {
        if (wanted != pending_) {
            pending_ = wanted;
            changed_at_ = now;
        }
        if (wanted.pattern.empty() || now - changed_at_ >= kTypingPause) {
            refresh();
        }
    } else if (!wanted.pattern.empty() && now - searched_at_ >= kRefreshEvery) {
        refresh();
    }
    if (replace_shown_) {
        const Target* hovered = target_of(tree_->getHoveredItem());
        const bool script = hovered != nullptr && hovered->line == 0;
        if (hovered != nullptr && script != hover_script_) {
            hover_script_ = script;
            row_replace_->setGraphic(icon_graphic(script ? "ReplaceAll.png" : "Replace.png"));
            row_replace_tip_->setText(script ? "Replace All in This Script" : "Replace in This Line");
        }
    }
    IdePane::layoutChildren();
}

void IdeSearch::handleKey(jadefx::KeyEvent& event) {
    // Keys bubble here from the fields and the results.
    if (!event.pressed) {
        IdePane::handleKey(event);
        return;
    }
    const bool in_find = find_->field().isFocused();
    const bool in_replace = replace_->field().isFocused();
    const bool enter = event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter;
    if (enter && in_replace && event.shortcut()) {
        replaceAll();
        event.consume();
        return;
    }
    if (enter && (in_find || in_replace)) {
        refresh();
        event.consume();
        return;
    }
    if (event.key == jadefx::Key::Tab && !event.shortcut() && (in_find || in_replace)) {
        if (event.shift || in_replace) {
            find_->focusAll();
        } else if (replace_shown_) {
            replace_->focusAll();
        }
        event.consume();
        return;
    }
    // Down from a field walks into the results.
    if (event.key == jadefx::Key::Down && (in_find || in_replace) && !root_->getChildren().empty()) {
        refresh();
        if (!root_->getChildren().empty()) {
            tree_->requestFocus();
            if (tree_->getSelectedItem() == nullptr) {
                tree_->select(0);
            }
        }
        event.consume();
        return;
    }
    const FindChord chord = find_chord(event);
    if (chord == FindChord::Find || chord == FindChord::FindInScripts) {
        find_->focusAll();
        event.consume();
        return;
    }
    if (chord == FindChord::Replace || chord == FindChord::ReplaceInScripts) {
        focusReplace();
        event.consume();
        return;
    }
    if (toggles_.handleKey(event)) {
        event.consume();
        return;
    }
    IdePane::handleKey(event);
}

}  // namespace ide
