#pragma once

#include "CollisionGroupsModel.hpp"
#include "IdePane.hpp"

#include "DataModel.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ide {

// A New Group on its way through the simulation thread: the name it made, or
// why it made none. Both are written before done.
struct NewGroupResult {
    std::string name;
    std::string error;
    std::atomic<bool> done{false};
};

// What the Collision Groups pane asks of the studio. Each write is one undo
// step (CollisionGroupsModel.hpp's run_*), on the simulation thread.
struct CollisionGroupsHost {
    std::function<void(std::shared_ptr<NewGroupResult> result)> add;
    std::function<void(std::string from, std::string to)> rename;
    // Moves the group's bodies to Default, then unregisters it.
    std::function<void(std::string name)> remove;
    std::function<void(std::string name, engine_core::CollisionGroups::FilterType filter_type)> set_filter_type;
    std::function<void(std::string a, std::string b, bool collidable)> set_collidable;
    // Puts the selected bodies, and those under selected instances, in the group.
    std::function<void(std::string name)> assign_selection;
    // Selects every body in the group.
    std::function<void(std::string name)> select_bodies;
    std::function<void(std::vector<std::string> names)> register_names;
    // A short message for the person, such as why a rename did nothing.
    std::function<void(std::string text)> notice;
};

// The place's collision groups, and which pairs collide
// (docs/superpowers/specs/2026-10-12-collision-groups-editor-design.md §3).
//
// The header holds the pane's badge, its title, "N of 62 groups · N bodies",
// a legend of the two cell looks, and New Group, which adds "New Group" and
// starts renaming it. Under it, while bodies name groups no one registered,
// a row lists them with Register; during play, a banner says changes last
// until Stop.
//
// Each group is a row: its color, its name, how many bodies are in it, its
// filter type as two segments (Exclude and Include, a click switches), and,
// while the row is hovered or selected, Select, Assign, and delete. Right of
// those is the matrix: a lower triangle of cells, row i holding one cell for
// each group up to and including itself (the diagonal is the group against
// itself, drawn round). A filled cell with a check collides; a hollow one
// does not; a click switches it. Each column's group name sits on the
// staircase just above the column's first cell, so no text is turned. The
// cell under the pointer lights the cells between it and the two names, and
// the names themselves.
//
// A click on a row selects it. Delete removes the selected group, asking
// first when bodies are in it; F2, Enter, or a double-click on its name
// renames it in place. Default has no rename and no delete. Rows and matrix
// scroll together.
class IdeCollisionGroups : public IdePane {
public:
    IdeCollisionGroups(engine_core::DataModel& world, CollisionGroupsHost host = {});
    ~IdeCollisionGroups() override;

    // Frames between body counts during play, when bodies change groups with
    // no edit to watch.
    static constexpr int kPlayPollFrames = 30;

    const CollisionGroupsView& view() const { return view_; }

    // New Group: adds one, then renames it in place once its row shows.
    void addGroup();
    // Removes the group, asking first when bodies are in it.
    void requestRemove(const std::string& name);
    void beginRename(const std::string& name);
    // The selected row's group, or empty.
    const std::string& selectedRow() const { return selected_; }
    void selectRow(const std::string& name);

    // For tests: the widgets a person would click. Null when there is none.
    jadefx::Node* rowNode(const std::string& name) const;
    // The cell for two groups, in either order.
    jadefx::Node* cellNode(const std::string& a, const std::string& b) const;
    jadefx::Node* filterSegment(const std::string& name, engine_core::CollisionGroups::FilterType type) const;
    jadefx::Node* selectButton(const std::string& name) const;
    jadefx::Node* assignButton(const std::string& name) const;
    // Null for Default, which cannot be deleted.
    jadefx::Node* deleteButton(const std::string& name) const;
    jadefx::Label* nameLabel(const std::string& name) const;
    // The name over the group's column.
    jadefx::Node* columnLabel(const std::string& name) const;
    jadefx::TextField* renameField() const;
    jadefx::Node* newButton() const;
    jadefx::Label* subtitle() const;
    // Null while every body names a registered group.
    jadefx::Node* unregisteredRow() const;
    jadefx::Node* registerButton() const;
    // Null while stopped.
    jadefx::Node* playBanner() const;
    // Null while there are groups besides Default.
    jadefx::Node* emptyHint() const;
    // Null when not asking.
    jadefx::Alert* removeAlert() const;
    // Whether the pointer's cell lights this node: a cell on the way to the names, or a name.
    static bool lit(const jadefx::Node* node);

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;

private:
    struct Row;

    // Reads the world when the groups, the tree, an edit, the selection, or
    // (during play) the poll moved.
    void refresh();
    // Makes the rows again when the groups' names changed; otherwise fills them.
    void sync_rows();
    std::shared_ptr<Row> make_row(std::size_t index);
    void fill_row(Row& row, std::size_t index);
    std::shared_ptr<jadefx::Node> make_header_row();
    void show_header();
    void show_notices();
    void show_selection();
    void light(int row, int column);
    void unlight();
    void clicked_cell(std::size_t row, std::size_t column);
    void finish_rename(bool apply);
    const Row* row_named(const std::string& name) const;
    double table_width() const;

    engine_core::DataModel& world_;
    CollisionGroupsHost host_;

    CollisionGroupsView view_;
    bool read_once_ = false;
    std::uint64_t seen_groups_ = ~std::uint64_t{0};
    std::uint64_t seen_tree_ = ~std::uint64_t{0};
    std::uint64_t seen_authored_ = ~std::uint64_t{0};
    std::uint64_t seen_selection_ = ~std::uint64_t{0};
    int poll_in_ = 0;
    bool has_selection_ = false;

    std::shared_ptr<jadefx::Label> subtitle_;
    std::shared_ptr<jadefx::Button> new_button_;
    std::shared_ptr<jadefx::VBox> top_;
    std::shared_ptr<jadefx::HBox> banner_;
    std::shared_ptr<jadefx::HBox> unregistered_;
    std::shared_ptr<jadefx::Label> unregistered_text_;
    std::shared_ptr<jadefx::Button> register_button_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    std::shared_ptr<jadefx::VBox> table_;
    std::shared_ptr<jadefx::Node> header_row_;
    std::shared_ptr<jadefx::Node> empty_;
    // The label over column 0 sits in the header row; the others in the row above their column.
    std::vector<std::shared_ptr<jadefx::Node>> column_labels_;
    std::vector<std::shared_ptr<Row>> rows_;
    // The names the rows were made for.
    std::vector<std::string> made_for_;

    std::string selected_;
    std::string renaming_;
    // A New Group on its way, and the name to rename once its row shows.
    std::shared_ptr<NewGroupResult> pending_add_;
    std::string rename_when_shown_;
    // The lit cell, or -1.
    int lit_row_ = -1;
    int lit_column_ = -1;

    std::shared_ptr<jadefx::Alert> alert_;
    std::string asking_;
    // A row's own control took the click that bubbles on to its row.
    bool took_click_ = false;
};

}  // namespace ide
