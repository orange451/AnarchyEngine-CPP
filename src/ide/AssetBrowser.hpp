#pragma once

#include "DataModel.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ide {

enum class AssetView { Icons, List, Columns };
const char* asset_view_name(AssetView view);  // "icons", "list", "columns"
bool asset_view_from(std::string_view name, AssetView& out);

// One row in a view: an asset or a container (Folder, category) under Assets. A
// Prefab is one item: its Models are never rows.
struct AssetRow {
    engine_core::InstanceId id = 0;
    std::string name;
    std::string class_name;
    std::string path;  // Path for Texture, Mesh, Sound; empty otherwise
    std::string where;  // search_rows only: names from the folder shown down to it, joined by '/'
    int depth = 0;       // List view: 0 for a child of the folder shown
    bool opens = false;  // a Folder: double-click opens it
    bool expanded = false;  // List view only
    bool operator==(const AssetRow&) const;
};

enum class AssetSort { Name, Kind, Path };

// What the Assets pane shows, without widgets: the folder, back and forward,
// crumbs, and each view's rows. Callers hold the world's read lock.
class AssetBrowser {
public:
    explicit AssetBrowser(engine_core::DataModel& world);

    // The folder shown: a category or a Folder under Assets. Starts
    // at Materials, the first category.
    engine_core::InstanceId folder() const;
    // Opens id, pushing the current folder on back. False when id cannot be opened
    // (dead, not under Assets, or not a category or Folder).
    bool open(engine_core::InstanceId id);
    bool back();
    bool forward();
    bool can_back() const;
    bool can_forward() const;
    // Assets down to the folder shown, with names: {Assets, Textures, Walls}.
    std::vector<std::pair<engine_core::InstanceId, std::string>> crumbs() const;
    // The five categories, in order.
    std::vector<AssetRow> categories() const;

    // Icons view: the folder's children in sibling order.
    std::vector<AssetRow> children() const;
    // List view: the folder's children, sorted, with expanded Folders' children under them.
    std::vector<AssetRow> list_rows() const;
    void set_expanded(engine_core::InstanceId id, bool expanded);
    void set_sort(AssetSort sort, bool descending);
    AssetSort sort() const;
    bool descending() const;
    // Columns view: one column per level from the categories to the folder shown.
    std::vector<std::vector<AssetRow>> columns() const;

    // A non-empty search replaces every view with the matches under the folder,
    // not counting a Prefab's Models: name contains the text, ignoring case; where holds "Walls/Brick" from the folder.
    void set_search(std::string text);
    const std::string& search() const;
    std::vector<AssetRow> search_rows() const;

    // Moves back to a live folder when the one shown was destroyed, or left
    // Assets. Returns true when anything the views show may have changed since
    // the last call (the tree moved or the folder changed).
    bool refresh();

    // The class New <kind> makes in the folder shown: Material in Materials or
    // a Folder under it, and so on. Empty when there is none.
    std::string new_kind() const;

private:
    AssetRow row_of(engine_core::InstanceId id, int depth) const;
    bool can_open(engine_core::InstanceId id) const;
    void add_list_rows(engine_core::InstanceId parent, int depth, std::vector<AssetRow>& out) const;
    void sort_rows(std::vector<AssetRow>& rows) const;
    // The ancestors of folder_ from its category down, not counting folder_.
    void remember_chain();

    engine_core::DataModel& world_;
    engine_core::InstanceId folder_ = 0;
    std::vector<engine_core::InstanceId> back_;
    std::vector<engine_core::InstanceId> forward_;
    std::vector<engine_core::InstanceId> chain_;
    std::unordered_set<engine_core::InstanceId> expanded_;
    AssetSort sort_ = AssetSort::Name;
    bool descending_ = false;
    std::string search_;
    std::uint64_t seen_tree_ = 0;
    engine_core::InstanceId seen_folder_ = 0;
};

}  // namespace ide
