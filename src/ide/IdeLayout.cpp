#include "IdeLayout.hpp"

#include "Engine.hpp"
#include "IdeConsole.hpp"
#include "IdeDock.hpp"
#include "IdeExplorer.hpp"
#include "TestTriangle.hpp"
#include "../runner/GameView.hpp"

#include <cmath>

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
.ide-fps {
    color: #f2f2f2;
    padding: 6px 8px;
    background-color: rgba(0, 0, 0, 0.45);
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

// The click handler runs on the open menu's row. Hiding first keeps that row
// alive: a visibility change on an open menu rebuilds its rows.
void ShowOne(jadefx::MenuItem& show, jadefx::MenuItem& hide) {
    if (jadefx::Menu* menu = show.getParentMenu()) {
        menu->hide();
    }
    hide.setVisible(false);
    show.setVisible(true);
}

}  // namespace

IdeLayout::IdeLayout(double windowWidth, double windowHeight) {
    runner_.prepare();

    auto file = jadefx::make<jadefx::Menu>("File");
    AddItem(*file, "New", jadefx::Key::N, jadefx::Key::ModControl);
    AddItem(*file, "Open", jadefx::Key::O, jadefx::Key::ModControl);
    AddItem(*file, "Save", jadefx::Key::S, jadefx::Key::ModControl);
    AddItem(*file, "Save As", jadefx::Key::S, jadefx::Key::ModControl | jadefx::Key::ModShift);

    auto edit = jadefx::make<jadefx::Menu>("Edit");
    auto test = jadefx::make<jadefx::MenuItem>("Test");
    auto stop = jadefx::make<jadefx::MenuItem>("Stop");
    jadefx::MenuItem* testItem = test.get();
    jadefx::MenuItem* stopItem = stop.get();
    test->setAccelerator(kKeyF5, 0);
    stop->setAccelerator(kKeyF5, 0);
    stop->setVisible(false);
    test->setOnAction([this, testItem, stopItem](jadefx::ActionEvent&) {
        runner_.simulation().resume();
        ShowOne(*stopItem, *testItem);
    });
    stop->setOnAction([this, testItem, stopItem](jadefx::ActionEvent&) {
        runner_.simulation().pause();
        ShowOne(*testItem, *stopItem);
    });

    auto insert = jadefx::make<jadefx::MenuItem>("Insert Triangle");
    insert->setOnAction([this](jadefx::ActionEvent&) {
        runner_.simulation().on_simulation([](engine_core::DataModel& model) {
            int existing = 0;
            for (engine_core::InstanceId id = model.first_child(model.id()); id != 0; id = model.next_sibling(id)) {
                if (dynamic_cast<engine_core::TestTriangle*>(model.instance(id)) != nullptr) {
                    ++existing;
                }
            }
            engine_core::TestTriangle& triangle = model.create<engine_core::TestTriangle>();
            model.set_parent(triangle.id(), model.id());
            // Spread repeats around the view so they do not stack on one point.
            const float angle = static_cast<float>(existing) * 0.9f;
            constexpr float kRadius = 0.42f;
            triangle.set_position(std::cos(angle) * kRadius, std::sin(angle) * kRadius, 0.15f);
        });
    });
    edit->getItems().add(std::move(insert));
    edit->getItems().add(std::move(test));
    edit->getItems().add(std::move(stop));

    auto view = jadefx::make<jadefx::Menu>("View");
    AddItem(*view, "Maybe :)", 0, 0);

    auto menuBar = jadefx::make<jadefx::MenuBar>();
    menuBar->getMenus().add(file);
    menuBar->getMenus().add(edit);
    menuBar->getMenus().add(view);

    engine_core::DataModel& model = runner_.simulation().datamodel();

    auto west = jadefx::make<IdeDock>();
    west->setMinSize(160, 80);
    // IdeTreeTest is the sample tree page. The Java shell left that dock commented out.
    west->dock(jadefx::make<IdeExplorer>(model, "Game Explorer"));

    auto center = jadefx::make<IdeDock>();
    center->setMinSize(64, 64);
    sceneDock_ = center.get();

    auto south = jadefx::make<IdeDock>();
    south->setMinSize(80, 96);
    south->dock(jadefx::make<IdeConsole>());

    auto east = jadefx::make<IdeDock>();
    east->setMinSize(160, 80);
    east->dock(jadefx::make<IdeExplorer>(model, "Current Scene"));

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

engine_core::Engine& IdeLayout::simulation() { return runner_.simulation(); }

void IdeLayout::start() {
    // Dock the view before the threads start. Its first paint is what lets the
    // uncapped render thread leave its wait.
    sceneDock_->dock(jadefx::make<runner::GameView>(runner_));
    runner_.start();
}

void IdeLayout::mount(jadefx::Scene& scene) {
    scene.setPadding(jadefx::Insets{});
    scene.setStylesheet(kStylesheet);
    scene.setRoot(root_);
}

}  // namespace ide
