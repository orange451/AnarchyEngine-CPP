#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jadefx {
class Scene;
}

namespace ide {

// One instance Quick Open can list: its name, class, and the names of its
// ancestors below the root, joined by dots.
struct QuickOpenEntry {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    std::string path;
};

// The entries query matches, ignoring case, best first: names that start with
// it, then names that hold it, then paths that hold it. Shorter names first in
// each group. At most limit. An empty query matches nothing.
std::vector<std::size_t> quick_open_filter(const std::vector<QuickOpenEntry>& entries, std::string_view query,
                                           std::size_t limit);

// Edit > Quick Open (Ctrl+Shift+T): a field over the top of the window that
// finds any instance by name. Enter or a click chooses the highlighted one.
class QuickOpen {
public:
    QuickOpen();
    ~QuickOpen();

    void setOnChoose(std::function<void(std::uint32_t id)> handler);

    void show(jadefx::Scene& scene, std::vector<QuickOpenEntry> entries);
    void hide();

private:
    struct List;

    std::shared_ptr<List> list_;
};

}  // namespace ide
