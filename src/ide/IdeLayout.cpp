#include "IdeLayout.hpp"

#include "IdeConsole.hpp"
#include "IdeDock.hpp"
#include "IdeExplorer.hpp"
#include "IdeGameView.hpp"

namespace ide {
namespace {

// MenuBar's title row is 28 points. The status strip below is 24, matching the Java toolbar.
constexpr double kMenuHeight = 28;
constexpr double kStatusHeight = 24;
constexpr double kSideWidth = 240;
constexpr double kConsoleHeight = 150;

// JadeFX key codes match GLFW. This is GLFW_KEY_F5.
constexpr int kKeyF5 = 294;

constexpr const char* kStylesheet = R"CSS(
scene {
    background-color: #d0d0d0;
    font-family: "Open Sans";
    font-size: 13px;
    color: #202124;
}
.ide-status {
    background-color: #eceff1;
}
.ide-viewport {
    background-color: #1e1e1e;
}
textfield {
    background-color: #ffffff;
    border-width: 1px 0 0 0;
    border-color: #c8c8c8;
    padding: 6px 8px;
}
split-pane:horizontal > .split-pane-divider,
split-pane:vertical > .split-pane-divider {
    padding: 0 2px;
    background-color: #b0b0b0;
}
)CSS";

void AddItem(jadefx::Menu& menu, const char* label, int key, int mods) {
    auto item = jadefx::make<jadefx::MenuItem>(label);
    if (key != 0) {
        item->setAccelerator(key, mods);
    }
    menu.getItems().add(std::move(item));
}

double Fraction(double part, double whole, double limit) {
    if (whole <= 1.0) {
        return limit;
    }
    double fraction = part / whole;
    if (fraction < 0.05) {
        fraction = 0.05;
    }
    if (fraction > limit) {
        fraction = limit;
    }
    return fraction;
}

}  // namespace

IdeLayout::IdeLayout(double windowWidth, double windowHeight) {
    auto file = jadefx::make<jadefx::Menu>("File");
    AddItem(*file, "New", jadefx::Key::N, jadefx::Key::ModControl);
    AddItem(*file, "Open", jadefx::Key::O, jadefx::Key::ModControl);
    AddItem(*file, "Save", jadefx::Key::S, jadefx::Key::ModControl);
    AddItem(*file, "Save As", jadefx::Key::S, jadefx::Key::ModControl | jadefx::Key::ModShift);

    auto edit = jadefx::make<jadefx::Menu>("Edit");
    AddItem(*edit, "Test", kKeyF5, 0);

    auto view = jadefx::make<jadefx::Menu>("View");
    AddItem(*view, "Maybe :)", 0, 0);

    auto menuBar = jadefx::make<jadefx::MenuBar>();
    menuBar->getMenus().add(file);
    menuBar->getMenus().add(edit);
    menuBar->getMenus().add(view);

    auto west = jadefx::make<IdeDock>();
    west->setMinSize(160, 80);
    // IdeTreeTest is the sample tree page. The Java shell left that dock commented out.
    west->dock(jadefx::make<IdeExplorer>("Game Explorer"));

    auto center = jadefx::make<IdeDock>();
    center->setMinSize(64, 64);
    center->dock(jadefx::make<IdeGameView>());

    auto south = jadefx::make<IdeDock>();
    south->setMinSize(80, 96);
    south->dock(jadefx::make<IdeConsole>());

    auto east = jadefx::make<IdeDock>();
    east->setMinSize(160, 80);
    east->dock(jadefx::make<IdeExplorer>("Current Scene"));

    auto vertical = jadefx::make<jadefx::SplitPane>();
    vertical->setOrientation(jadefx::Orientation::Vertical);
    vertical->getItems().add(center);
    vertical->getItems().add(south);
    const double contentHeight = windowHeight - kMenuHeight - kStatusHeight;
    vertical->setDividerPositions({1.0 - Fraction(kConsoleHeight, contentHeight, 0.6)});
    jadefx::SplitPane::setResizableWithParent(*south, false);

    auto horizontal = jadefx::make<jadefx::SplitPane>();
    horizontal->getItems().add(west);
    horizontal->getItems().add(vertical);
    horizontal->getItems().add(east);
    const double side = Fraction(kSideWidth, windowWidth, 0.35);
    horizontal->setDividerPositions({side, 1.0 - side});
    jadefx::SplitPane::setResizableWithParent(*west, false);
    jadefx::SplitPane::setResizableWithParent(*east, false);

    auto status = jadefx::make<jadefx::Pane>();
    status->getClassList().add("ide-status");
    status->setMinSize(0, kStatusHeight);
    status->setPrefHeight(kStatusHeight);
    status->setBackground(jadefx::Color::rgb8(236, 239, 241));

    root_ = jadefx::make<jadefx::BorderPane>();
    root_->setPrefWidthRatio(1);
    root_->setPrefHeightRatio(1);
    root_->setBackground(jadefx::Color::rgb8(208, 208, 208));
    root_->setTop(menuBar);
    root_->setCenter(horizontal);
    root_->setBottom(status);
}

void IdeLayout::mount(jadefx::Scene& scene) {
    scene.setPadding(jadefx::Insets{});
    scene.setStylesheet(kStylesheet);
    scene.setRoot(root_);
}

}  // namespace ide
