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
#include <optional>
#include <utility>

namespace ide {
namespace {

constexpr double kRowHeight = 22;
// FindButton is an icon-sized square by default; these toggles show text instead.
constexpr double kToggleWidth = 92;
constexpr double kToggleHeight = 22;

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
    box->getChildren().add(text_label("Line " + std::to_string(problem.line), "problems-detail"));
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

// Every Script and ModuleScript under parent with published diagnostics,
// except the studio's own under Core. path is the chain below game.
void gather(const engine_core::DataModel& game, const engine_core::ScriptAnalysis& analysis,
            engine_core::InstanceId parent, const std::string& path, std::vector<ProblemSource>& out, int depth) {
    for (engine_core::InstanceId child = game.first_child(parent); child != 0; child = game.next_sibling(child)) {
        if (child == game.core()) {
            continue;
        }
        const std::string name = game.name(child);
        if (const auto* script = dynamic_cast<const engine_core::LuaSource*>(game.instance(child))) {
            const std::optional<std::string> checked = analysis.analyzed_source(child);
            if (checked) {
                ProblemSource source;
                source.id = child;
                source.name = name;
                source.class_name = script->class_name();
                source.path = path;
                source.problems = problems_from(*checked, analysis.diagnostics(child));
                if (!source.problems.empty()) {
                    out.push_back(std::move(source));
                }
            }
        }
        if (depth < 512) {
            gather(game, analysis, child, path.empty() ? name : path + "." + name, out, depth + 1);
        }
    }
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
        toggle->setPrefSize(kToggleWidth, kToggleHeight);
        toggle->setMaxSize(kToggleWidth * 2, kToggleHeight);
        toggle->setOnAction([this] { changed_.set(); });
    }
    auto toggles = jadefx::make<jadefx::HBox>();
    toggles->setSpacing(4);
    toggles->setAlignment(jadefx::Pos::CenterLeft);
    toggles->getChildren().add(errors_);
    toggles->getChildren().add(warnings_);
    toggles->getChildren().add(info_);

    auto header = jadefx::make<jadefx::VBox>();
    header->getClassList().add("problems-header");
    header->setSpacing(4);
    header->setStyle("width: 100%;");
    header->getChildren().add(filter_);
    header->getChildren().add(toggles);

    summary_ = text_label("", "problems-summary");
    summary_->setStyle("width: 100%;");
    play_note_ = text_label("Checking resumes when the playtest stops.", "problems-note");
    off_notice_ = text_label("Script analysis is off.", "problems-note");
    notices_ = jadefx::make<jadefx::VBox>();
    notices_->setStyle("width: 100%;");

    auto top = jadefx::make<jadefx::VBox>();
    top->setStyle("width: 100%;");
    top->getChildren().add(header);
    top->getChildren().add(summary_);
    top->getChildren().add(notices_);

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
    column->setTop(top);
    column->setCenter(tree_);
    getChildren().add(column);

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
bool IdeProblems::playNoteShown() const { return shown_playing_; }
bool IdeProblems::offNoticeShown() const { return !shown_enabled_; }

void IdeProblems::refresh() {
    engine_core::DataModel& game = engine_.datamodel();
    std::vector<ProblemSource> gathered;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionLockWait);
        if (!lock.owns()) {
            // The place is busy. The next layout tries again.
            changed_.set();
            return;
        }
        gather(game, engine_.analysis(), game.id(), "", gathered, 0);
    }
    sources_ = std::move(gathered);
    rebuild();
}

void IdeProblems::rebuild() {
    const ProblemFilter wanted = filter();
    list_ = build_problems(sources_, wanted);
    shown_filter_ = wanted;
    built_once_ = true;
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
    summary_->setText(problems_summary(list_));
    const std::string title = problems_title(list_);
    setTitle(title == "Problems" ? std::string() : title);
    update_toggles();
}

void IdeProblems::update_toggles() {
    errors_->setText("Errors (" + std::to_string(list_.matching.errors) + ")");
    warnings_->setText("Warnings (" + std::to_string(list_.matching.warnings) + ")");
    info_->setText("Info (" + std::to_string(list_.matching.info) + ")");
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

void IdeProblems::layoutChildren() {
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
    const ProblemFilter wanted = filter();
    const bool filter_changed = wanted.text != shown_filter_.text || wanted.errors != shown_filter_.errors ||
                                wanted.warnings != shown_filter_.warnings || wanted.info != shown_filter_.info;
    if (changed_.take() || !built_once_) {
        refresh();
    } else if (filter_changed) {
        rebuild();
    }
    IdePane::layoutChildren();
}

}  // namespace ide
