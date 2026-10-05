#include "ide/IdeDock.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"
#include "ide/LandingPage.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// The Welcome page: its buttons and checkbox alone, then where the studio docks it.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

void TestPage() {
    std::vector<std::string> called;
    std::vector<bool> shown;
    ide::LandingPage::Actions actions;
    actions.new_place = [&] { called.push_back("new"); };
    actions.open_project = [&] { called.push_back("open"); };
    actions.show_assets = [&] { called.push_back("assets"); };
    actions.open_preferences = [&] { called.push_back("preferences"); };
    actions.set_show_on_startup = [&](bool show) { shown.push_back(show); };
    auto page = jadefx::make<ide::LandingPage>(std::move(actions), true);

    Expect(page->name() == "Welcome", "the page is named Welcome");
    Expect(page->closable(), "its tab can be closed");
    Expect(page->hide_box() != nullptr && !page->hide_box()->isSelected(),
           "Don't show on startup is unchecked while the page shows at startup");

    for (const char* text : {"New Place", "Open Project…", "Assets", "Preferences…"}) {
        jadefx::Button* button = page->button(text);
        Expect(button != nullptr, "each start action has a button");
        if (button != nullptr) {
            button->fire();
        }
    }
    Expect(called == std::vector<std::string>{"new", "open", "assets", "preferences"},
           "each button runs its action");

    page->hide_box()->fire();
    Expect(page->hide_box()->isSelected(), "a click checks the box");
    Expect(shown == std::vector<bool>{false}, "checking it turns the page off at startup");
    page->hide_box()->fire();
    Expect(shown == std::vector<bool>{false, true}, "unchecking it turns it back on");

    auto hidden = jadefx::make<ide::LandingPage>(ide::LandingPage::Actions{}, false);
    Expect(hidden->hide_box()->isSelected(), "the box starts checked when the page is off at startup");
    hidden->hide_box()->fire();
    Expect(!hidden->hide_box()->isSelected(), "a page without actions still toggles");
}

// A page too short for the card scrolls it, and the checkbox stays at the bottom.
void TestShortPage() {
    auto page = jadefx::make<ide::LandingPage>(ide::LandingPage::Actions{}, true);
    jadefx::Scene scene(page, 900, 700);

    scene.layout(900, 700, 0);
    jadefx::ScrollPane* scroll = page->scroll_pane();
    Expect(scroll->getContentBounds().height <= scroll->getViewportBounds().height,
           "a tall page has nothing to scroll");
    const double card_y = page->button("New Place")->getAbsoluteY();

    scene.layout(900, 240, 0.1);
    Expect(scroll->getContentBounds().height > scroll->getViewportBounds().height, "a short page scrolls the card");
    jadefx::CheckBox* box = page->hide_box();
    Expect(box->getAbsoluteY() >= 0 && box->getAbsoluteY() + box->getHeight() <= 240,
           "the checkbox stays inside a short page");
    Expect(box->getAbsoluteY() > page->getHeight() - 60, "and sits at its bottom");

    scene.layout(900, 700, 0.2);
    Expect(page->button("New Place")->getAbsoluteY() == card_y, "the card is centered again once the page is tall");
}

ide::IdeDock* DockOf(jadefx::Node* node) {
    for (jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (auto* dock = dynamic_cast<ide::IdeDock*>(cursor)) {
            return dock;
        }
    }
    return nullptr;
}

std::vector<std::string> TabNames(ide::IdeDock& dock) {
    std::vector<std::string> names;
    for (const std::shared_ptr<jadefx::Tab>& tab : dock.tabs()->getTabs().items()) {
        auto* pane = tab ? dynamic_cast<ide::IdePane*>(tab->getContent()) : nullptr;
        names.push_back(pane != nullptr ? pane->name() : std::string());
    }
    return names;
}

void TestLayout(ide::IdeLayout& layout, jadefx::Scene& scene) {
    auto shown_pane = [&scene](const std::string& name) -> ide::IdePane* {
        for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
            auto* pane = dynamic_cast<ide::IdePane*>(node);
            if (pane != nullptr && pane->name() == name) {
                return pane;
            }
        }
        return nullptr;
    };
    Expect(shown_pane("Welcome") == nullptr, "a studio that was never started shows no Welcome page");
    ide::IdePane* view = shown_pane("Scene View");
    ide::IdeDock* dock = DockOf(view);
    Expect(dock != nullptr, "the scene view is docked");
    if (dock == nullptr) {
        return;
    }

    auto close_welcome = [dock] {
        const std::vector<std::shared_ptr<jadefx::Tab>> tabs = dock->tabs()->getTabs().items();
        for (const std::shared_ptr<jadefx::Tab>& tab : tabs) {
            auto* pane = tab ? dynamic_cast<ide::IdePane*>(tab->getContent()) : nullptr;
            if (pane != nullptr && pane->name() == "Welcome") {
                dock->tabs()->close(tab);
            }
        }
    };

    layout.open_landing();
    scene.layout(1280, 800, 0.1);
    std::vector<std::string> names = TabNames(*dock);
    Expect(names.size() >= 2 && names[0] == "Scene View" && names[1] == "Welcome",
           "the Welcome page is the second tab, beside the scene view");
    Expect(shown_pane("Welcome") != nullptr && shown_pane("Scene View") == nullptr, "and it is in front");

    layout.open_landing();
    scene.layout(1280, 800, 0.2);
    std::size_t welcomes = 0;
    for (const std::string& name : TabNames(*dock)) {
        welcomes += name == "Welcome" ? 1 : 0;
    }
    Expect(welcomes == 1, "opening it again brings the open page forward");

    // Closed, the Window menu brings it back. A copy of the list, since closing takes the tab out of it.
    close_welcome();
    scene.layout(1280, 800, 0.3);
    Expect(shown_pane("Welcome") == nullptr, "closing the tab closes the page");
    jadefx::MenuItem* item = nullptr;
    if (auto* root = dynamic_cast<jadefx::BorderPane*>(scene.getRoot())) {
        if (auto* top = dynamic_cast<jadefx::VBox*>(root->getTop()); top != nullptr && !top->getChildren().empty()) {
            if (auto* bar = dynamic_cast<jadefx::MenuBar*>(top->getChildren()[0].get())) {
                for (const std::shared_ptr<jadefx::Menu>& menu : bar->getMenus().items()) {
                    for (const std::shared_ptr<jadefx::MenuItem>& entry : menu->getItems().items()) {
                        if (menu->getText() == "Window" && entry && entry->getText() == "Welcome Page") {
                            item = entry.get();
                        }
                    }
                }
            }
        }
    }
    Expect(item != nullptr, "the Window menu has Welcome Page");
    if (item != nullptr) {
        item->fire();
    }
    scene.layout(1280, 800, 0.4);
    names = TabNames(*dock);
    Expect(names.size() >= 2 && names[1] == "Welcome" && shown_pane("Welcome") != nullptr,
           "Window > Welcome Page opens it again as the second tab");

    // New Place starts the work the page was for, so the page closes. That also
    // leaves the scene view in front for the tests after this one.
    ide::LandingPage* page = dynamic_cast<ide::LandingPage*>(shown_pane("Welcome"));
    jadefx::Button* new_place = page != nullptr ? page->button("New Place") : nullptr;
    Expect(new_place != nullptr, "the docked page has New Place");
    if (new_place != nullptr) {
        new_place->fire();
    }
    // The place the earlier tests edited asks first; dropping it goes on.
    if (auto* discard = dynamic_cast<jadefx::Button*>(scene.getElementById("unsaved-discard"))) {
        discard->fire();
    }
    scene.layout(1280, 800, 0.5);
    Expect(shown_pane("Welcome") == nullptr, "New Place closes the Welcome page");
    close_welcome();
    scene.layout(1280, 800, 0.6);
}

}  // namespace

int RunLandingPageTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    TestPage();
    TestShortPage();
    TestLayout(layout, scene);
    if (gFailures == 0) {
        std::printf("landing page tests passed\n");
    }
    return gFailures;
}
