#include "ide/DockArrange.hpp"
#include "ide/IdeDock.hpp"
#include "ide/IdePane.hpp"

#include "jadefx/jadefx.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

bool Near(double a, double b) { return std::fabs(a - b) < 0.02; }

std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)> Hold(
    std::initializer_list<std::shared_ptr<jadefx::Node>> nodes) {
    std::vector<std::shared_ptr<jadefx::Node>> held(nodes);
    return [held](jadefx::Node* node) -> std::shared_ptr<jadefx::Node> {
        for (const std::shared_ptr<jadefx::Node>& item : held) {
            if (item.get() == node) {
                return item;
            }
        }
        return nullptr;
    };
}

void TestDockMinimum() {
    auto dock = jadefx::make<ide::IdeDock>();
    auto narrow = jadefx::make<ide::IdePane>("Narrow", true);
    auto wide = jadefx::make<ide::IdePane>("Wide", true);
    narrow->setMinSize(100, 40);
    wide->setMinSize(150, 20);
    dock->dock(narrow);
    dock->dock(wide);
    dock->syncMinimum();
    Expect(dock->getMinWidth() == 150, "two tabs keep the wider minimum");
    Expect(dock->getMinHeight() >= 40, "the dock is at least as tall as the taller page");

    auto wider = jadefx::make<ide::IdePane>("Wider", true);
    wider->setMinSize(200, 10);
    dock->dock(wider);
    dock->syncMinimum();
    Expect(dock->getMinWidth() == 200, "a later tab raises the dock minimum");
}

void TestLiftSplit() {
    auto root = jadefx::make<jadefx::BorderPane>();
    auto outer = jadefx::make<jadefx::SplitPane>();
    auto inner = jadefx::make<jadefx::SplitPane>();
    inner->setOrientation(jadefx::Orientation::Vertical);
    auto west = jadefx::make<jadefx::Pane>();
    auto center = jadefx::make<jadefx::Pane>();
    auto south = jadefx::make<jadefx::Pane>();
    west->setMinSize(100, 40);
    center->setMinSize(80, 50);
    south->setMinSize(80, 30);
    inner->getItems().add(center);
    inner->getItems().add(south);
    outer->getItems().add(west);
    outer->getItems().add(inner);
    root->setCenter(outer);

    auto scene = jadefx::make<jadefx::Scene>(root, 600, 400);
    scene->layout(600, 400, 0);
    const ide::Extent need = ide::minimumExtent(outer.get());
    Expect(need.width >= 180, "a horizontal split is at least the sum of its children");
    Expect(need.width < 320, "a nested divider does not inflate the minimum width");
    Expect(need.height >= 80, "a nested vertical split adds its children");
    Expect(need.height < 200, "a nested divider does not inflate the minimum height");

    std::shared_ptr<jadefx::Node> heldOuter = outer;
    std::shared_ptr<jadefx::Node> heldRoot = root;
    auto share = [&](jadefx::Node* node) -> std::shared_ptr<jadefx::Node> {
        if (node == heldRoot.get()) {
            return heldRoot;
        }
        if (node == heldOuter.get()) {
            return heldOuter;
        }
        auto* parent = dynamic_cast<jadefx::SplitPane*>(node->getParent());
        if (parent == nullptr) {
            return nullptr;
        }
        for (const std::shared_ptr<jadefx::Node>& item : parent->getItems().items()) {
            if (item.get() == node) {
                return item;
            }
        }
        return nullptr;
    };
    auto replaced = [&](jadefx::Node&, const std::shared_ptr<jadefx::Node>&, const std::shared_ptr<jadefx::Node>&) {};

    inner->getItems().removeAt(1);
    ide::liftDegenerateSplits(inner, root.get(), share, replaced);
    Expect(outer->getItems().size() == 2, "a one-child split leaves the parent with the remaining page");
    Expect(outer->getItems()[1].get() == center.get(), "the remaining page takes the split's place");

    outer->getItems().removeAt(0);
    ide::liftDegenerateSplits(heldOuter, root.get(), share, replaced);
    Expect(root->getCenter() == center.get(), "the last split disappears when one page remains");
}

void TestLiftKeepsColumns() {
    auto root = jadefx::make<jadefx::BorderPane>();
    auto outer = jadefx::make<jadefx::SplitPane>();
    auto inner = jadefx::make<jadefx::SplitPane>();
    inner->setOrientation(jadefx::Orientation::Vertical);
    auto west = jadefx::make<jadefx::Pane>();
    auto center = jadefx::make<jadefx::Pane>();
    auto south = jadefx::make<jadefx::Pane>();
    auto east = jadefx::make<jadefx::Pane>();
    west->setMinSize(150, 80);
    east->setMinSize(150, 80);
    east->setPrefWidth(9999999);
    center->setMinSize(64, 64);
    south->setMinSize(80, 64);
    inner->getItems().add(center);
    inner->getItems().add(south);
    inner->setDividerPositions({0.75});
    jadefx::SplitPane::setResizableWithParent(*south, false);
    outer->getItems().add(west);
    outer->getItems().add(inner);
    outer->getItems().add(east);
    outer->setDividerPositions({0.2, 0.8});
    jadefx::SplitPane::setResizableWithParent(*west, false);
    jadefx::SplitPane::setResizableWithParent(*east, false);
    root->setCenter(outer);

    auto scene = jadefx::make<jadefx::Scene>(root, 1000, 600);
    scene->layout(1000, 600, 0);
    const double westWidth = west->getWidth();
    const double eastWidth = east->getWidth();
    const double centerWidth = center->getWidth();
    const std::vector<double> before = outer->getDividerPositions();
    Expect(westWidth > 140 && westWidth < 260, "the left column starts near a fifth of the row");
    Expect(eastWidth > 140 && eastWidth < 260, "the right column starts near a fifth of the row");
    Expect(centerWidth > 400, "the middle column keeps the rest of the row");

    std::shared_ptr<jadefx::Node> heldOuter = outer;
    std::shared_ptr<jadefx::Node> heldRoot = root;
    auto share = [&](jadefx::Node* node) -> std::shared_ptr<jadefx::Node> {
        if (node == heldRoot.get()) {
            return heldRoot;
        }
        if (node == heldOuter.get()) {
            return heldOuter;
        }
        auto* parent = dynamic_cast<jadefx::SplitPane*>(node->getParent());
        if (parent == nullptr) {
            return nullptr;
        }
        for (const std::shared_ptr<jadefx::Node>& item : parent->getItems().items()) {
            if (item.get() == node) {
                return item;
            }
        }
        return nullptr;
    };
    auto replaced = [](jadefx::Node&, const std::shared_ptr<jadefx::Node>&, const std::shared_ptr<jadefx::Node>&) {};
    inner->getItems().removeAt(1);
    ide::liftDegenerateSplits(inner, root.get(), share, replaced);
    Expect(outer->getItems().size() == 3 && outer->getItems()[1].get() == center.get(),
           "the scene takes the place of the bottom split");
    scene->layout(1000, 600, 0);
    Expect(std::fabs(west->getWidth() - westWidth) < 2, "closing the bottom split keeps the left column");
    Expect(std::fabs(east->getWidth() - eastWidth) < 2, "closing the bottom split keeps the right column");
    Expect(std::fabs(center->getWidth() - centerWidth) < 2, "the scene column keeps its width");
    const std::vector<double> after = outer->getDividerPositions();
    Expect(before.size() == 2 && after.size() == 2 && Near(after[0], before[0]) && Near(after[1], before[1]),
           "the row dividers stay where they were");
}

void TestDockZones() {
    const ide::Box dock{0, 0, 200, 120};
    Expect(ide::dockZone(dock, 20, 30, 8) == ide::DockZone::Header, "the tab strip is the header zone");
    Expect(ide::dockZone(dock, 20, 100, 50) == ide::DockZone::Center, "the upper middle merges into the tab pane");
    Expect(ide::dockZone(dock, 20, 100, 90) == ide::DockZone::Bottom, "the lower half of a dock splits below it");
    Expect(ide::dockZone(dock, 20, 100, 28) == ide::DockZone::Top, "the top of the content splits above it");
    Expect(ide::dockZone(dock, 20, 12, 70) == ide::DockZone::Left, "the left quarter splits to the left");
    Expect(ide::dockZone(dock, 20, 180, 70) == ide::DockZone::Right, "the right quarter splits to the right");
    Expect(ide::dockZone(dock, 20, -4, 40) == ide::DockZone::Outside, "a point outside the dock is not a zone");

    const ide::Box bottom = ide::dockPreview(dock, 20, ide::DockZone::Bottom);
    Expect(Near(bottom.y, 60) && Near(bottom.height, 60), "a bottom drop previews the lower half");
    const ide::Box merge = ide::dockPreview(dock, 20, ide::DockZone::Center);
    Expect(Near(merge.y, 20) && Near(merge.height, 100), "a merge previews the content under the strip");

    const ide::Box work{10, 30, 1000, 700};
    Expect(ide::screenEdge(work, 80, 20, 400) == ide::DockZone::Left, "the left screen edge is a left drop");
    Expect(ide::screenEdge(work, 80, 20, 40) == ide::DockZone::Left, "a corner belongs to the vertical edge");
    Expect(ide::screenEdge(work, 80, 500, 400) == ide::DockZone::Outside, "the middle of the work area is not an edge");
    const ide::Box band = ide::edgePreview(work, ide::DockZone::Left, 240);
    Expect(Near(band.x, 10) && Near(band.width, 240) && Near(band.height, 700), "the left preview is a full-height band");
}

void TestSplitBeside() {
    auto root = jadefx::make<jadefx::BorderPane>();
    auto outer = jadefx::make<jadefx::SplitPane>();
    auto west = jadefx::make<jadefx::Pane>();
    auto center = jadefx::make<jadefx::Pane>();
    outer->getItems().add(west);
    outer->getItems().add(center);
    outer->setDividerPositions({0.4});
    root->setCenter(outer);
    auto extra = jadefx::make<jadefx::Pane>();
    auto replaced = [](jadefx::Node&, const std::shared_ptr<jadefx::Node>&, const std::shared_ptr<jadefx::Node>&) {};
    Expect(ide::splitBeside(*west, extra, ide::DropSide::Bottom, Hold({west, center, outer, extra, root}), replaced),
           "a dock can be split on the bottom");
    Expect(outer->getItems().size() == 2, "splitting a section leaves the outer split's children");
    Expect(outer->getItems()[1].get() == center.get(), "the neighbor stays in its slot");
    Expect(!outer->getDividerPositions().empty() && Near(outer->getDividerPositions()[0], 0.4),
           "the outer divider stays where it was");
    auto* nested = dynamic_cast<jadefx::SplitPane*>(outer->getItems()[0].get());
    Expect(nested != nullptr && nested->getOrientation() == jadefx::Orientation::Vertical,
           "the bottom of a section becomes a vertical split");
    if (nested != nullptr) {
        Expect(nested->getItems().size() == 2 && nested->getItems()[0].get() == west.get() &&
                   nested->getItems()[1].get() == extra.get(),
               "the new page occupies the bottom half");
        Expect(!nested->getDividerPositions().empty() && Near(nested->getDividerPositions()[0], 0.5),
               "the new split is even");
    }
}

void TestSplitEdge() {
    auto root = jadefx::make<jadefx::BorderPane>();
    auto row = jadefx::make<jadefx::SplitPane>();
    auto left = jadefx::make<jadefx::Pane>();
    auto right = jadefx::make<jadefx::Pane>();
    row->getItems().add(left);
    row->getItems().add(right);
    row->setDividerPositions({0.5});
    root->setCenter(row);
    auto fresh = jadefx::make<jadefx::Pane>();
    std::shared_ptr<jadefx::Node> area = row;
    auto share = [&](jadefx::Node* node) -> std::shared_ptr<jadefx::Node> {
        if (node == root.get()) {
            return root;
        }
        if (node == area.get()) {
            return area;
        }
        return nullptr;
    };
    auto replaced = [&](jadefx::Node& owner, const std::shared_ptr<jadefx::Node>&,
                        const std::shared_ptr<jadefx::Node>& replacement) {
        if (&owner == root.get()) {
            area = replacement;
        }
    };
    Expect(ide::splitEdge(*area, fresh, ide::DropSide::Left, 0.25, share, replaced), "the left edge accepts a page");
    Expect(area.get() == row.get(), "a matching split gains a child instead of wrapping");
    Expect(row->getItems().size() == 3 && row->getItems()[0].get() == fresh.get(), "the new page is the left column");
    const std::vector<double> positions = row->getDividerPositions();
    Expect(positions.size() == 2 && Near(positions[0], 0.25) && Near(positions[1], 0.25 + 0.75 * 0.5),
           "the old columns keep their share of the remaining space");

    auto below = jadefx::make<jadefx::Pane>();
    Expect(ide::splitEdge(*area, below, ide::DropSide::Bottom, 0.2, share, replaced), "the bottom edge wraps the row");
    auto* vertical = dynamic_cast<jadefx::SplitPane*>(root->getCenter());
    Expect(vertical != nullptr && vertical->getOrientation() == jadefx::Orientation::Vertical,
           "a bottom edge on a horizontal split stacks a new row");
    if (vertical != nullptr) {
        Expect(vertical->getItems().size() == 2 && vertical->getItems()[0].get() == row.get() &&
                   vertical->getItems()[1].get() == below.get(),
               "the new page sits under the old work area");
    }
}

void TestSceneRootSplit() {
    auto dock = jadefx::make<jadefx::Pane>();
    auto scene = jadefx::make<jadefx::Scene>(dock, 300, 200);
    auto extra = jadefx::make<jadefx::Pane>();
    auto share = [&](jadefx::Node* node) -> std::shared_ptr<jadefx::Node> {
        if (node == dock.get()) {
            return dock;
        }
        return nullptr;
    };
    auto replaced = [](jadefx::Node&, const std::shared_ptr<jadefx::Node>&, const std::shared_ptr<jadefx::Node>&) {};
    Expect(ide::splitBeside(*dock, extra, ide::DropSide::Bottom, share, replaced), "a window root can be split");
    auto* split = dynamic_cast<jadefx::SplitPane*>(scene->getRoot());
    Expect(split != nullptr && split->getOrientation() == jadefx::Orientation::Vertical,
           "the window root becomes the new split");
    if (split != nullptr) {
        Expect(split->getItems().size() == 2 && split->getItems()[0].get() == dock.get() &&
                   split->getItems()[1].get() == extra.get(),
               "the original page stays above the new one");
    }
}

void TestDockFillsScene() {
    auto dock = jadefx::make<ide::IdeDock>();
    dock->setPrefWidthRatio(1);
    dock->setPrefHeightRatio(1);
    auto page = jadefx::make<ide::IdePane>("Page", true);
    page->setMinSize(100, 40);
    dock->dock(page);
    auto scene = jadefx::make<jadefx::Scene>(dock, 500, 400);
    scene->layout(500, 400, 0);
    Expect(std::fabs(dock->getWidth() - 500) < 2, "a stretched dock fills the window width");
    Expect(std::fabs(dock->getHeight() - 400) < 2, "a stretched dock fills the window height");
    Expect(page->getHeight() > 300, "the page fills the dock under the tab strip");
}

}  // namespace

int main() {
    TestDockMinimum();
    TestLiftSplit();
    TestLiftKeepsColumns();
    TestDockZones();
    TestSplitBeside();
    TestSplitEdge();
    TestSceneRootSplit();
    TestDockFillsScene();
    if (gFailures == 0) {
        std::printf("dock arrange tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d dock arrange tests failed\n", gFailures);
    return 1;
}
