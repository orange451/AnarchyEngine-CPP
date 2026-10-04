#include "IdeProblems.hpp"
#include "LockWaits.hpp"

#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "IdeIcons.hpp"
#include "LuaSource.hpp"
#include "ScriptAnalysis.hpp"
#include "Strings.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace ide {
namespace {

constexpr double kRowHeight = 22;
// FindButton is an icon-sized square by default; these toggles show text
// instead, as wide as it with this much room on each side.
constexpr double kToggleHeight = 22;
constexpr double kTogglePadding = 8;
// While a long check batch runs, the rows are rebuilt at most this often.
constexpr double kBusyRebuildInterval = 0.25;

constexpr const char* kProblemsRules = R"CSS(
.problems-pane {
    background-color: var(--ide-panel-color);
}
.problems-header {
    padding: 8px 8px 4px 8px;
    spacing: 2px;
}
.problems-summary {
    color: var(--ide-search-status-color);
    font-size: 12px;
    padding: 2px 8px 6px 8px;
}
.problems-note {
    color: var(--ide-problems-note-color);
    font-size: 12px;
    padding: 0 8px 6px 8px;
}
.problems-results {
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.problems-detail {
    color: var(--ide-problems-detail-color);
    font-size: 12px;
}
.problems-badge {
    color: var(--ide-problems-badge-text-color);
    border-radius: 8px;
    font-size: 11px;
    padding: 0 6px;
}
.problems-badge.error {
    background-color: var(--ide-problems-error-badge-color);
}
.problems-badge.warning {
    background-color: var(--ide-problems-warning-badge-color);
    color: var(--ide-problems-warning-badge-text-color);
}
.problems-glyph {
    min-width: 8px;
    min-height: 8px;
    max-width: 8px;
    max-height: 8px;
}
.problems-glyph.error {
    background-color: var(--ide-problems-error-color);
    border-radius: 4px;
}
.problems-glyph.warning {
    background-color: var(--ide-problems-warning-color);
    border-radius: 1px;
}
.problems-glyph.info {
    border-width: 2px;
    border-style: solid;
    border-color: var(--ide-problems-info-color);
    border-radius: 4px;
}
)CSS";

const char* severity_class(engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return "error";
    case engine_core::Severity::Warning:
        return "warning";
    default:
        return "info";
    }
}

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    if (style_class != nullptr) {
        label->getClassList().add(style_class);
    }
    label->setMouseTransparent(true);
    return label;
}

std::shared_ptr<jadefx::Node> glyph(engine_core::Severity severity) {
    auto shape = jadefx::make<jadefx::StackPane>();
    shape->getClassList().add("problems-glyph");
    shape->getClassList().add(severity_class(severity));
    shape->setPrefSize(8, 8);
    shape->setMouseTransparent(true);
    return shape;
}

std::shared_ptr<jadefx::Node> script_graphic(const ScriptProblems& script) {
    auto box = jadefx::make<jadefx::HBox>();
    box->setSpacing(6);
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_view(script.class_name)) {
        icon->setPrefSize(16, 16);
        box->getChildren().add(icon);
    }
    box->getChildren().add(text_label(script.name, nullptr));
    if (!script.path.empty()) {
        box->getChildren().add(text_label(script.path, "problems-detail"));
    }
    if (script.errors > 0) {
        auto badge = text_label(std::to_string(script.errors), "problems-badge");
        badge->getClassList().add("error");
        box->getChildren().add(badge);
    }
    if (script.warnings > 0) {
        auto badge = text_label(std::to_string(script.warnings), "problems-badge");
        badge->getClassList().add("warning");
        box->getChildren().add(badge);
    }
    return box;
}

std::shared_ptr<jadefx::Node> problem_graphic(const Problem& problem) {
    auto box = jadefx::make<jadefx::HBox>();
    box->setSpacing(6);
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    box->getChildren().add(glyph(problem.severity));
    box->getChildren().add(text_label("Line " + std::to_string(problem.line), nullptr));
    box->getChildren().add(text_label(problem.message, nullptr));
    box->getChildren().add(text_label(problem.code, "problems-detail"));
    return box;
}

// Enter on a row opens it.
class ProblemsTree : public jadefx::TreeView {
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

// A script in the place, as read under the DataModel lock.
struct ScriptEntry {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    std::string path;
};

// Every Script and ModuleScript under parent, except the studio's own under
// Core. path is the chain below game. Only the tree is read here: the
// diagnostics are read after the lock is let go.
void gather(const engine_core::DataModel& game, engine_core::InstanceId parent, const std::string& path,
            std::vector<ScriptEntry>& out, int depth) {
    for (engine_core::InstanceId child = game.first_child(parent); child != 0; child = game.next_sibling(child)) {
        if (child == game.core()) {
            continue;
        }
        std::string name = game.name(child);
        if (const auto* script = dynamic_cast<const engine_core::LuaSource*>(game.instance(child))) {
            out.push_back(ScriptEntry{child, name, script->class_name(), path});
        }
        if (depth < 512) {
            gather(game, child, path.empty() ? name : path + "." + name, out, depth + 1);
        }
    }
}

// The chain of names from game's child down to id's parent, as gather builds
// it. False when id is not in the place, or is under Core.
bool path_of(const engine_core::DataModel& game, engine_core::InstanceId id, std::string& path) {
    if (game.core_holds(id)) {
        return false;
    }
    std::vector<engine_core::InstanceId> chain;
    engine_core::InstanceId at = game.parent(id);
    for (int depth = 0; at != game.id(); ++depth) {
        if (at == engine_core::DataModel::kNoParent || depth > 512) {
            return false;
        }
        chain.push_back(at);
        at = game.parent(at);
    }
    path.clear();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!path.empty()) {
            path += '.';
        }
        path += game.name(*it);
    }
    return true;
}

}  // namespace

IdeProblems::IdeProblems(engine_core::Engine& engine, ProblemsHost host)
    : IdePane("Problems", true), engine_(engine), host_(std::move(host)) {
    setIconFile("Warning.png");
    getClassList().add("problems-pane");
    setStylesheet(std::string(kFindStylesheet) + kProblemsRules);
    setMinSize(150, 120);

    filter_ = jadefx::make<SearchInput>("Filter");
    filter_->setStyle("width: 100%;");
    errors_ = jadefx::make<FindButton>("", "Errors", "Show errors", true);
    warnings_ = jadefx::make<FindButton>("", "Warnings", "Show warnings", true);
    info_ = jadefx::make<FindButton>("", "Info", "Show info", true);
    for (const auto& toggle : {errors_, warnings_, info_}) {
        toggle->setChecked(true);
        // These toggles carry a word and a count, not an icon, so they need
        // more than FindButton's default 22x22 icon-button box.
        toggle->setMinSize(0, kToggleHeight);
        toggle->setOnAction([this] { changed_.set(); });
    }
    auto toggles = jadefx::make<jadefx::HBox>();
    toggles->setSpacing(4);
    toggles->setAlignment(jadefx::Pos::CenterLeft);
    toggles->getChildren().add(errors_);
    toggles->getChildren().add(warnings_);
    toggles->getChildren().add(info_);

    header_ = jadefx::make<jadefx::VBox>();
    header_->getClassList().add("problems-header");
    header_->setSpacing(4);
    header_->setStyle("width: 100%;");
    header_->getChildren().add(filter_);
    header_->getChildren().add(toggles);

    summary_ = text_label("", "problems-summary");
    summary_->setStyle("width: 100%;");
    play_note_ = text_label("Checking resumes when the playtest stops.", "problems-note");
    off_notice_ = text_label("Script analysis is off.", "problems-note");
    notices_ = jadefx::make<jadefx::VBox>();
    notices_->setStyle("width: 100%;");

    top_ = jadefx::make<jadefx::VBox>();
    top_->setStyle("width: 100%;");
    top_->getChildren().add(header_);
    top_->getChildren().add(summary_);
    top_->getChildren().add(notices_);

    root_ = jadefx::make<jadefx::TreeItem>("");
    root_->setExpanded(true);
    auto tree = jadefx::make<ProblemsTree>();
    tree->open = [this](jadefx::TreeItem* item) { return openRow(item); };
    tree_ = tree;
    tree_->setRoot(root_);
    tree_->getClassList().add("problems-results");
    tree_->setShowRoot(false);
    tree_->setFixedCellSize(kRowHeight);
    tree_->setOnItemActivated([this](jadefx::TreeItem& item) { return openRow(&item); });
    tree_->setOnMouseClicked([this](const jadefx::MouseEvent& event) { clicked(event); });

    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setTop(top_);
    column->setCenter(tree_);
    getChildren().add(column);
    update_toggles();

    hook_ = engine_.analysis().diagnostics_changed().connect(
        [changed = changed_.setter()](engine_core::InstanceId) { changed(); });
}

IdeProblems::~IdeProblems() { engine_.analysis().diagnostics_changed().disconnect(hook_); }

ProblemFilter IdeProblems::filter() const {
    ProblemFilter out;
    out.text = filter_->text();
    out.errors = errors_->isChecked();
    out.warnings = warnings_->isChecked();
    out.info = info_->isChecked();
    return out;
}

std::string IdeProblems::summary() const { return summary_->getText(); }

bool IdeProblems::playNoteShown() const {
    for (const auto& child : notices_->getChildren().items()) {
        if (child.get() == play_note_.get()) {
            return true;
        }
    }
    return false;
}

bool IdeProblems::offNoticeShown() const {
    for (const auto& child : notices_->getChildren().items()) {
        if (child.get() == off_notice_.get()) {
            return true;
        }
    }
    return false;
}

bool IdeProblems::headerShown() const {
    for (const auto& child : top_->getChildren().items()) {
        if (child.get() == header_.get()) {
            return true;
        }
    }
    return false;
}

void IdeProblems::refresh() {
    engine_core::DataModel& game = engine_.datamodel();
    std::vector<ScriptEntry> scripts;
    std::uint64_t tree = 0;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kFrameLockWait);
        if (!lock.owns()) {
            // The place is busy. The next layout tries again.
            changed_.set();
            return;
        }
        tree = game.tree_revision();
        gather(game, game.id(), "", scripts, 0);
    }
    // Analysis keeps its own lock: what it published is read without holding the place's.
    const engine_core::ScriptAnalysis& analysis = engine_.analysis();
    std::vector<ProblemSource> gathered;
    for (ScriptEntry& script : scripts) {
        const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id);
        // Hints are dropped by problems_from anyway; skip the source copy
        // analyzed_source() makes when there is nothing else to show.
        const bool any_shown = std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.severity != engine_core::Severity::Hint;
        });
        if (!any_shown) {
            continue;
        }
        const std::optional<std::string> checked = analysis.analyzed_source(script.id);
        if (!checked) {
            continue;
        }
        ProblemSource source;
        source.id = script.id;
        source.name = std::move(script.name);
        source.class_name = std::move(script.class_name);
        source.path = std::move(script.path);
        source.problems = problems_from(*checked, diagnostics);
        if (!source.problems.empty()) {
            gathered.push_back(std::move(source));
        }
    }
    sources_ = std::move(gathered);
    seen_tree_ = tree;
    rebuild();
}

void IdeProblems::refresh_paths() {
    engine_core::DataModel& game = engine_.datamodel();
    bool shown_moved = false;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kFrameLockWait);
        if (!lock.owns()) {
            // seen_tree_ stays behind, so the next tick tries again.
            return;
        }
        seen_tree_ = game.tree_revision();
        std::vector<ProblemSource> kept;
        kept.reserve(sources_.size());
        for (ProblemSource& source : sources_) {
            std::string path;
            if (!path_of(game, source.id, path)) {
                shown_moved = true;
                continue;
            }
            std::string name = game.name(source.id);
            if (name != source.name || path != source.path) {
                source.name = std::move(name);
                source.path = std::move(path);
                shown_moved = true;
            }
            kept.push_back(std::move(source));
        }
        sources_ = std::move(kept);
    }
    // Most tree changes touch nothing listed: a part made or destroyed, a
    // folder no listed script is under renamed. The rows stay as they are.
    if (shown_moved) {
        rebuild();
    }
}

void IdeProblems::rebuild() {
    const ProblemFilter wanted = filter();
    list_ = build_problems(sources_, wanted);
    shown_filter_ = wanted;
    built_once_ = true;
    built_at_ = now_;
    ++rebuilds_;

    std::optional<Target> selected;
    if (const jadefx::TreeItem* item = tree_->getSelectedItem()) {
        const auto found = targets_.find(item);
        if (found != targets_.end()) {
            selected = found->second;
        }
    }
    jadefx::TreeItem* reselect = nullptr;
    targets_.clear();
    std::vector<std::shared_ptr<jadefx::TreeItem>> rows;
    rows.reserve(list_.scripts.size());
    for (const ScriptProblems& script : list_.scripts) {
        auto row = jadefx::make<jadefx::TreeItem>("", script_graphic(script));
        const std::uint32_t id = script.id;
        row->setExpanded(collapsed_.count(id) == 0);
        row->setOnCollapsed([this, id](jadefx::TreeItem&) { collapsed_.insert(id); });
        row->setOnExpanded([this, id](jadefx::TreeItem&) { collapsed_.erase(id); });
        const Problem& first = script.problems.front();
        targets_[row.get()] = Target{id, 0, first.column, first.column_end};
        if (selected && selected->id == id && selected->line == 0) {
            reselect = row.get();
        }
        for (const Problem& problem : script.problems) {
            auto child = jadefx::make<jadefx::TreeItem>("", problem_graphic(problem));
            targets_[child.get()] = Target{id, problem.line, problem.column, problem.column_end};
            if (selected && selected->id == id && selected->line == problem.line &&
                selected->column == problem.column) {
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
    // While analysis is off, "No problems" would contradict the notice below it.
    summary_->setText(shown_enabled_ ? problems_summary(list_) : std::string());
    const std::string title = problems_title(list_);
    setTitle(title == "Problems" ? std::string() : title);
    update_toggles();
}

void IdeProblems::update_toggles() {
    errors_->setText("Errors (" + std::to_string(list_.matching.errors) + ")");
    warnings_->setText("Warnings (" + std::to_string(list_.matching.warnings) + ")");
    info_->setText("Info (" + std::to_string(list_.matching.info) + ")");
    fit_toggles();
}

void IdeProblems::fit_toggles() {
    for (const auto& toggle : {errors_, warnings_, info_}) {
        // Measured as Labeled measures its text, so the word and count fit.
        const jadefx::ComputedStyle& style = toggle->computedStyle();
        const jadefx::Font font(style.fontFamily, style.fontSize);
        const double width = std::ceil(static_cast<double>(font.measureWidth(toggle->getText()))) +
                             2 * kTogglePadding + style.padding.width() + style.border.width();
        if (width != toggle->getPrefWidth()) {
            toggle->setPrefSize(width, kToggleHeight);
            toggle->setMaxSize(width, kToggleHeight);
        }
    }
}

void IdeProblems::update_notices() {
    std::vector<std::shared_ptr<jadefx::Node>> rows;
    if (shown_playing_) {
        rows.push_back(play_note_);
    }
    if (!shown_enabled_) {
        rows.push_back(off_notice_);
    }
    notices_->getChildren().setAll(std::move(rows));
    std::vector<std::shared_ptr<jadefx::Node>> top;
    if (shown_enabled_) {
        top.push_back(header_);
        top.push_back(summary_);
    } else {
        summary_->setText("");
    }
    top.push_back(notices_);
    top_->getChildren().setAll(std::move(top));
}

bool IdeProblems::openRow(const jadefx::TreeItem* item) {
    if (item == nullptr) {
        return false;
    }
    const auto found = targets_.find(item);
    if (found == targets_.end()) {
        return false;
    }
    Target where = found->second;
    if (where.line == 0) {
        // A script row opens its first shown problem.
        for (const ScriptProblems& script : list_.scripts) {
            if (script.id == where.id && !script.problems.empty()) {
                where.line = script.problems.front().line;
                break;
            }
        }
    }
    if (host_.open && where.line > 0) {
        // Copied first: opening a script can refresh the list and drop this row.
        host_.open(where.id, where.line, where.column, where.column_end);
    }
    return true;
}

void IdeProblems::clicked(const jadefx::MouseEvent& event) {
    if (event.button != 0 ||
        (event.mods & (jadefx::Key::ModShift | jadefx::Key::ModControl | jadefx::Key::ModSuper)) != 0) {
        return;
    }
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

void IdeProblems::tick(double now) {
    now_ = now;
    const bool playing = engine_.datamodel().simulation_running();
    const bool enabled = engine_.analysis().enabled();
    bool notices_changed = false;
    if (playing != shown_playing_) {
        shown_playing_ = playing;
        notices_changed = true;
    }
    if (enabled != shown_enabled_) {
        shown_enabled_ = enabled;
        tree_->setVisible(enabled);
        changed_.set();
        notices_changed = true;
    }
    if (notices_changed) {
        update_notices();
    }
    bool changed = changed_.take() || !built_once_;
    // A playtest churns the tree every frame as it spawns and destroys things,
    // and Stop puts the authored tree back, ids and all. The pane keeps the
    // last Edit-mode results through play, so it does not follow the tree
    // until Stop; the first tick after it reads the restored names and paths.
    bool moved = !playing && engine_.datamodel().tree_revision() != seen_tree_;
    // A long check batch publishes a little every frame. Rebuilding every row
    // each time is wasted work no one can read, so while it runs the rows
    // follow at most every kBusyRebuildInterval; what waits is kept for the
    // first tick after that, or after the batch ends. A clock that went back
    // (a new scene) does not hold anything up.
    if ((changed || moved) && built_once_ && engine_.analysis().busy() && now >= built_at_ &&
        now - built_at_ < kBusyRebuildInterval) {
        if (changed) {
            changed_.set();
        }
        changed = false;
        moved = false;
    }
    const ProblemFilter wanted = filter();
    const bool filter_changed = wanted.text != shown_filter_.text || wanted.errors != shown_filter_.errors ||
                                wanted.warnings != shown_filter_.warnings || wanted.info != shown_filter_.info;
    if (changed) {
        refresh();
    } else if (moved) {
        refresh_paths();
    } else if (filter_changed) {
        rebuild();
    }
}

void IdeProblems::sync_filter() {
    if (!built_once_) {
        refresh();
        return;
    }
    const ProblemFilter wanted = filter();
    if (wanted.text != shown_filter_.text || wanted.errors != shown_filter_.errors ||
        wanted.warnings != shown_filter_.warnings || wanted.info != shown_filter_.info) {
        rebuild();
    }
}

void IdeProblems::filterKey(jadefx::KeyEvent& event) {
    // Down from the filter walks into the list. Taken on the way down: the
    // field would move its caret with it and never let it bubble.
    if (event.pressed && event.key == jadefx::Key::Down && !event.shortcut() && !event.shift &&
        filter_->field().isFocused()) {
        sync_filter();
        if (!root_->getChildren().empty()) {
            tree_->requestFocus();
            if (tree_->getSelectedItem() == nullptr) {
                tree_->select(0);
            }
        }
        event.consume();
        return;
    }
    IdePane::filterKey(event);
}

void IdeProblems::handleKey(jadefx::KeyEvent& event) {
    // Keys bubble here from the filter and the list.
    if (!event.pressed || !filter_->field().isFocused()) {
        IdePane::handleKey(event);
        return;
    }
    // Enter opens the first problem shown.
    if ((event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) && !event.shortcut()) {
        sync_filter();
        if (!root_->getChildren().empty()) {
            const std::shared_ptr<jadefx::TreeItem>& first = root_->getChildren().items().front();
            openRow(first->getChildren().empty() ? first.get() : first->getChildren().items().front().get());
        }
        event.consume();
        return;
    }
    // Escape clears the filter. With nothing to clear it goes on up, as Search's does.
    if (event.key == jadefx::Key::Escape && !filter_->text().empty()) {
        filter_->field().clear();
        sync_filter();
        event.consume();
        return;
    }
    IdePane::handleKey(event);
}

void IdeProblems::layoutChildren() {
    jadefx::Scene* scene = getScene();
    tick(scene != nullptr ? scene->timeSeconds() : now_);
    fit_toggles();
    IdePane::layoutChildren();
}

}  // namespace ide
