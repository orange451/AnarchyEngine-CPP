#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jadefx {
class Node;
}

namespace ide {

// True for a class the explorer's Insert list shows: one Instance.new makes
// that is not an asset, since assets go in the Assets pane.
bool insert_offers(const std::string& class_name);

// True when child_class belongs under holder_class, the class whose rule
// decides the new parent (DataModel::placement_holder). The list draws the
// rest faded, after these. An empty holder suits every class.
bool insert_suits(std::string_view holder_class, std::string_view child_class);

struct InsertAction {
    std::string label;
    std::string icon;
    bool enabled = true;
    std::function<void()> run;
};

// The insert list anchored to an explorer row's + button.
// Typing filters the classes Instance.new can create. Enter or a click
// creates the highlighted class.
class InsertPopup {
public:
    InsertPopup();
    ~InsertPopup();

    void setOnCreate(std::function<void(const std::string& class_name)> handler);

    // holder is the class whose rule decides the new parent; see insert_suits.
    void show(jadefx::Node& anchor, std::string holder = {});
    void show_at(jadefx::Node& owner, double x, double y, std::vector<InsertAction> actions,
                 std::string holder = {});
    void hide();

private:
    struct List;

    std::shared_ptr<List> list_;
};

}  // namespace ide
