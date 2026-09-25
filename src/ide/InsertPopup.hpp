#pragma once

#include <functional>
#include <memory>
#include <string>

namespace jadefx {
class Node;
}

namespace ide {

// The insert list anchored to an explorer row's + button.
// Typing filters the classes Instance.new can create. Enter or a click
// creates the highlighted class.
class InsertPopup {
public:
    InsertPopup();
    ~InsertPopup();

    void setOnCreate(std::function<void(const std::string& class_name)> handler);

    bool isOpen() const;
    void show(jadefx::Node& anchor);
    void hide();

private:
    struct List;

    std::shared_ptr<List> list_;
};

}  // namespace ide
