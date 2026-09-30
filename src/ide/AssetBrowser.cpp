#include "AssetBrowser.hpp"

#include "AssetInstances.hpp"
#include "Containment.hpp"

#include <algorithm>
#include <cctype>

namespace ide {

namespace {

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

bool AssetRow::operator==(const AssetRow& other) const {
    return id == other.id && name == other.name && class_name == other.class_name && path == other.path &&
           where == other.where && depth == other.depth && opens == other.opens && expanded == other.expanded;
}

const char* asset_view_name(AssetView view) {
    switch (view) {
        case AssetView::Icons:
            return "icons";
        case AssetView::List:
            return "list";
        case AssetView::Columns:
            return "columns";
    }
    return "icons";
}

bool asset_view_from(std::string_view name, AssetView& out) {
    if (name == "icons") {
        out = AssetView::Icons;
        return true;
    }
    if (name == "list") {
        out = AssetView::List;
        return true;
    }
    if (name == "columns") {
        out = AssetView::Columns;
        return true;
    }
    return false;
}

AssetBrowser::AssetBrowser(engine_core::DataModel& world) : world_(world) {
    folder_ = world_.service(engine_core::kServices[5].class_name);  // Materials
    remember_chain();
}

engine_core::InstanceId AssetBrowser::folder() const { return folder_; }

AssetRow AssetBrowser::row_of(engine_core::InstanceId id, int depth) const {
    AssetRow row;
    row.id = id;
    row.depth = depth;
    const engine_core::DataModel* object = world_.instance(id);
    if (object == nullptr) {
        return row;
    }
    row.name = world_.name(id);
    row.class_name = object->class_name();
    if (const auto* file = dynamic_cast<const engine_core::FileAsset*>(object)) {
        row.path = file->path();
    }
    row.opens = row.class_name == "Folder" || row.class_name == "Prefab";
    row.expanded = expanded_.count(id) != 0;
    return row;
}

bool AssetBrowser::can_open(engine_core::InstanceId id) const {
    const engine_core::DataModel* object = world_.instance(id);
    if (object == nullptr) {
        return false;
    }
    const std::string klass = object->class_name();
    const bool container = klass == "Folder" || klass == "Prefab" ||
                            (object->is_service() && world_.parent(id) == world_.service("Assets"));
    if (!container) {
        return false;
    }
    // Under Assets.
    const engine_core::InstanceId assets = world_.service("Assets");
    if (id == assets) {
        return false;
    }
    for (engine_core::InstanceId at = world_.parent(id); at != engine_core::DataModel::kNoParent;
         at = world_.parent(at)) {
        if (at == assets) {
            return true;
        }
        if (at == 0) {
            return false;
        }
    }
    return false;
}

bool AssetBrowser::open(engine_core::InstanceId id) {
    if (id == folder_ || !can_open(id)) {
        return false;
    }
    back_.push_back(folder_);
    forward_.clear();
    folder_ = id;
    remember_chain();
    return true;
}

bool AssetBrowser::back() {
    while (!back_.empty()) {
        const engine_core::InstanceId previous = back_.back();
        back_.pop_back();
        if (can_open(previous)) {
            forward_.push_back(folder_);
            folder_ = previous;
            remember_chain();
            return true;
        }
    }
    return false;
}

bool AssetBrowser::forward() {
    while (!forward_.empty()) {
        const engine_core::InstanceId next = forward_.back();
        forward_.pop_back();
        if (can_open(next)) {
            back_.push_back(folder_);
            folder_ = next;
            remember_chain();
            return true;
        }
    }
    return false;
}

bool AssetBrowser::can_back() const { return !back_.empty(); }

bool AssetBrowser::can_forward() const { return !forward_.empty(); }

std::vector<std::pair<engine_core::InstanceId, std::string>> AssetBrowser::crumbs() const {
    std::vector<std::pair<engine_core::InstanceId, std::string>> path;
    const engine_core::InstanceId assets = world_.service("Assets");
    for (engine_core::InstanceId at = folder_; at != engine_core::DataModel::kNoParent && at != 0;
         at = world_.parent(at)) {
        path.emplace_back(at, world_.name(at));
        if (at == assets) {
            break;
        }
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<AssetRow> AssetBrowser::categories() const {
    std::vector<AssetRow> rows;
    for (const engine_core::ServiceSpec& spec : engine_core::kServices) {
        if (spec.parent_class != nullptr && std::string(spec.parent_class) == "Assets") {
            rows.push_back(row_of(world_.service(spec.class_name), 0));
        }
    }
    return rows;
}

std::vector<AssetRow> AssetBrowser::children() const {
    std::vector<AssetRow> rows;
    for (engine_core::InstanceId id : world_.get_children(folder_)) {
        rows.push_back(row_of(id, 0));
    }
    return rows;
}

void AssetBrowser::sort_rows(std::vector<AssetRow>& rows) const {
    std::sort(rows.begin(), rows.end(), [this](const AssetRow& a, const AssetRow& b) {
        bool less = false;
        switch (sort_) {
            case AssetSort::Name:
                less = to_lower(a.name) < to_lower(b.name);
                break;
            case AssetSort::Kind:
                if (a.class_name != b.class_name) {
                    less = a.class_name < b.class_name;
                } else {
                    less = to_lower(a.name) < to_lower(b.name);
                }
                break;
            case AssetSort::Path:
                if (a.path != b.path) {
                    less = a.path < b.path;
                } else {
                    less = to_lower(a.name) < to_lower(b.name);
                }
                break;
        }
        return descending_ ? !less : less;
    });
}

void AssetBrowser::add_list_rows(engine_core::InstanceId parent, int depth, std::vector<AssetRow>& out) const {
    std::vector<AssetRow> rows;
    for (engine_core::InstanceId id : world_.get_children(parent)) {
        rows.push_back(row_of(id, depth));
    }
    sort_rows(rows);
    for (const AssetRow& row : rows) {
        out.push_back(row);
        if (row.opens && row.expanded) {
            add_list_rows(row.id, depth + 1, out);
        }
    }
}

std::vector<AssetRow> AssetBrowser::list_rows() const {
    std::vector<AssetRow> rows;
    add_list_rows(folder_, 0, rows);
    return rows;
}

void AssetBrowser::set_expanded(engine_core::InstanceId id, bool expanded) {
    if (expanded) {
        expanded_.insert(id);
    } else {
        expanded_.erase(id);
    }
}

void AssetBrowser::set_sort(AssetSort sort, bool descending) {
    sort_ = sort;
    descending_ = descending;
}

AssetSort AssetBrowser::sort() const { return sort_; }

bool AssetBrowser::descending() const { return descending_; }

std::vector<std::vector<AssetRow>> AssetBrowser::columns() const {
    std::vector<std::vector<AssetRow>> result;
    result.push_back(categories());
    // chain_ holds the ancestors from the category down, not including folder_.
    for (engine_core::InstanceId id : chain_) {
        std::vector<AssetRow> rows;
        for (engine_core::InstanceId child : world_.get_children(id)) {
            rows.push_back(row_of(child, 0));
        }
        result.push_back(rows);
    }
    result.push_back(children());
    return result;
}

void AssetBrowser::set_search(std::string text) { search_ = std::move(text); }

const std::string& AssetBrowser::search() const { return search_; }

namespace {

void collect_search(const engine_core::DataModel& world, engine_core::InstanceId parent, const std::string& lower,
                     const std::vector<std::string>& where_stack, std::vector<AssetRow>& out) {
    for (engine_core::InstanceId id : world.get_children(parent)) {
        const engine_core::DataModel* object = world.instance(id);
        if (object == nullptr) {
            continue;
        }
        const std::string name = world.name(id);
        std::vector<std::string> next_where = where_stack;
        next_where.push_back(name);
        std::string lower_name(name);
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_name.find(lower) != std::string::npos) {
            AssetRow row;
            row.id = id;
            row.name = name;
            row.class_name = object->class_name();
            if (const auto* file = dynamic_cast<const engine_core::FileAsset*>(object)) {
                row.path = file->path();
            }
            row.opens = row.class_name == "Folder" || row.class_name == "Prefab";
            std::string joined;
            for (std::size_t i = 0; i < next_where.size(); ++i) {
                if (i != 0) {
                    joined += '/';
                }
                joined += next_where[i];
            }
            row.where = joined;
            out.push_back(row);
        }
        collect_search(world, id, lower, next_where, out);
    }
}

}  // namespace

std::vector<AssetRow> AssetBrowser::search_rows() const {
    std::vector<AssetRow> rows;
    if (search_.empty()) {
        return rows;
    }
    const std::string lower = to_lower(search_);
    collect_search(world_, folder_, lower, {}, rows);
    return rows;
}

// The ancestors of folder_ from its category down, not counting folder_.
void AssetBrowser::remember_chain() {
    chain_.clear();
    const engine_core::InstanceId assets = world_.service("Assets");
    for (engine_core::InstanceId at = world_.parent(folder_);
         at != assets && at != 0 && at != engine_core::DataModel::kNoParent; at = world_.parent(at)) {
        chain_.insert(chain_.begin(), at);
    }
}

bool AssetBrowser::refresh() {
    if (!can_open(folder_)) {
        engine_core::InstanceId fallback = world_.service("Materials");
        for (auto it = chain_.rbegin(); it != chain_.rend(); ++it) {
            if (can_open(*it)) {
                fallback = *it;
                break;
            }
        }
        folder_ = fallback;
        remember_chain();
    }
    const std::uint64_t tree = world_.tree_revision();
    const bool changed = tree != seen_tree_ || folder_ != seen_folder_;
    seen_tree_ = tree;
    seen_folder_ = folder_;
    return changed;
}

std::string AssetBrowser::new_kind() const {
    const engine_core::DataModel* object = world_.instance(folder_);
    if (object == nullptr) {
        return {};
    }
    if (std::string(object->class_name()) == "Prefab") {
        return "Model";
    }
    // The category above: its asset class is the one whose home it is.
    engine_core::InstanceId at = folder_;
    while (at != 0 && at != engine_core::DataModel::kNoParent) {
        const engine_core::DataModel* here = world_.instance(at);
        if (here != nullptr && here->is_service()) {
            for (const char* klass : {"Material", "Prefab", "Mesh", "Texture", "Sound"}) {
                const char* home = engine_core::asset_home(klass);
                if (home != nullptr && std::string(home) == here->class_name()) {
                    return klass;
                }
            }
            return {};
        }
        at = world_.parent(at);
    }
    return {};
}

}  // namespace ide
