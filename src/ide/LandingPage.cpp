#include "LandingPage.hpp"

#include "IdeIcons.hpp"
#include "IdeResources.hpp"

#include <initializer_list>
#include <utility>

namespace ide {
namespace {

// Open Sans has no bold face loaded, so the hierarchy is in size and color.
constexpr const char* kStylesheet = R"CSS(
.landing {
    background-color: var(--ide-window-color);
    font-family: "Open Sans";
}
.landing-body {
    padding: 24px;
    min-height: 100%;
    alignment: center;
}
.landing-card {
    background-color: var(--ide-panel-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--ide-field-border-color);
    border-radius: 12px;
    box-shadow: 0px 8px 28px 0px var(--ide-popup-shadow-color);
    padding: 36px 40px;
    spacing: 28px;
    max-width: 760px;
}
.landing-header {
    spacing: 20px;
    alignment: center-left;
}
.landing-titles {
    spacing: 2px;
}
.landing-title {
    color: var(--ide-text-color);
    font-size: 30px;
}
.landing-tagline {
    color: var(--ide-muted-text-color);
    font-size: 14px;
}
.landing-columns {
    spacing: 48px;
}
.landing-column {
    spacing: 4px;
    min-width: 240px;
}
.landing-heading {
    color: var(--ide-muted-text-color);
    font-size: 11px;
    padding: 0 0 6px 2px;
}
.landing-action {
    background-color: transparent;
    border-width: 0;
    border-radius: 6px;
    color: var(--ide-text-color);
    font-size: 14px;
    padding: 7px 12px 7px 10px;
    alignment: center-left;
    min-width: 240px;
}
.landing-action:hover {
    background-color: var(--ide-ribbon-hover-color);
}
.landing-action:active {
    background-color: var(--ide-ribbon-pressed-color);
}
.landing-shortcut {
    spacing: 12px;
    alignment: center-left;
    padding: 5px 0 5px 2px;
}
.landing-key {
    background-color: var(--ide-field-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--ide-field-border-color);
    border-radius: 4px;
    color: var(--ide-text-color);
    font-size: 12px;
    padding: 1px 7px;
    min-width: 72px;
    alignment: center;
}
.landing-what {
    color: var(--ide-muted-text-color);
    font-size: 13px;
}
.landing-footer {
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--ide-field-border-color);
    padding: 10px 24px;
    alignment: center;
    width: 100%;
}
.landing-footer check-box {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
)CSS";

constexpr double kLogoSize = 72;

// The command key as the menus show it.
#ifdef __APPLE__
constexpr const char* kCommand = "Cmd";
#else
constexpr const char* kCommand = "Ctrl";
#endif

std::shared_ptr<jadefx::Label> Text(const std::string& text, const char* style) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style);
    return label;
}

std::shared_ptr<jadefx::VBox> Column(const char* heading) {
    auto column = jadefx::make<jadefx::VBox>();
    column->getClassList().add("landing-column");
    column->getChildren().add(Text(heading, "landing-heading"));
    return column;
}

// The app icon, or nothing when the resources lack it.
std::shared_ptr<jadefx::ImageView> Logo() {
    const std::filesystem::path path = find_resource("icon/anarchy.png");
    if (path.empty()) {
        return nullptr;
    }
    std::shared_ptr<jadefx::Image> image = jadefx::Image::load(utf8_path(path));
    if (!image) {
        return nullptr;
    }
    auto view = jadefx::make<jadefx::ImageView>(std::move(image));
    view->setPrefSize(kLogoSize, kLogoSize);
    view->setMinSize(kLogoSize, kLogoSize);
    view->setStyle("border-radius: 14px;");
    return view;
}

// Calls action when there is one, so a page made without the studio still works.
std::function<void(jadefx::ActionEvent&)> Run(const std::function<void()>& action) {
    return [action](jadefx::ActionEvent&) {
        if (action) {
            action();
        }
    };
}

}  // namespace

LandingPage::LandingPage(Actions actions, bool show_on_startup)
    : IdePane("Welcome", true), actions_(std::move(actions)) {
    setIconFile("World.png");
    build(show_on_startup);
}

jadefx::Button* LandingPage::button(const std::string& text) const {
    for (const std::shared_ptr<jadefx::Button>& button : buttons_) {
        if (button->getText() == text) {
            return button.get();
        }
    }
    return nullptr;
}

void LandingPage::build(bool show_on_startup) {
    setStylesheet(kStylesheet);
    getClassList().add("landing");

    auto card = jadefx::make<jadefx::VBox>();
    card->getClassList().add("landing-card");

    auto header = jadefx::make<jadefx::HBox>();
    header->getClassList().add("landing-header");
    if (std::shared_ptr<jadefx::ImageView> logo = Logo()) {
        header->getChildren().add(logo);
    }
    auto titles = jadefx::make<jadefx::VBox>();
    titles->getClassList().add("landing-titles");
    titles->getChildren().add(Text("Anarchy Engine", "landing-title"));
    titles->getChildren().add(Text("Build, script, and test 3D places in one studio.", "landing-tagline"));
    header->getChildren().add(titles);
    card->getChildren().add(header);

    auto columns = jadefx::make<jadefx::HBox>();
    columns->getClassList().add("landing-columns");
    columns->getChildren().add(start_column());
    columns->getChildren().add(shortcut_column());
    card->getChildren().add(columns);

    // The card scrolls when the page is too short for it, and stays centered when not:
    // the body is at least as tall as the viewport.
    auto body = jadefx::make<jadefx::StackPane>();
    body->getClassList().add("landing-body");
    body->getChildren().add(card);
    scroll_ = jadefx::make<jadefx::ScrollPane>(body);
    scroll_->setFitToWidth(true);

    // The checkbox stays pinned to the bottom of the page, outside the scroll.
    auto footer = jadefx::make<jadefx::HBox>();
    footer->getClassList().add("landing-footer");
    hide_box_ = jadefx::make<jadefx::CheckBox>("Don't show this page on startup");
    hide_box_->setSelected(!show_on_startup);
    hide_box_->setOnAction([this](jadefx::ActionEvent&) {
        if (actions_.set_show_on_startup) {
            actions_.set_show_on_startup(!hide_box_->isSelected());
        }
    });
    footer->getChildren().add(hide_box_);

    auto frame = jadefx::make<jadefx::BorderPane>();
    Fill(*frame);
    frame->setCenter(scroll_);
    frame->setBottom(footer);
    getChildren().add(frame);
}

std::shared_ptr<jadefx::Node> LandingPage::start_column() {
    std::shared_ptr<jadefx::VBox> column = Column("START");
    auto add = [&](const char* text, const char* icon, const std::function<void()>& action) {
        auto button = jadefx::make<jadefx::Button>(text);
        button->getClassList().add("landing-action");
        if (std::shared_ptr<jadefx::ImageView> graphic = icon_graphic(icon)) {
            button->setGraphic(std::move(graphic));
            button->setGraphicTextGap(10);
        }
        button->setOnAction(Run(action));
        column->getChildren().add(button);
        buttons_.push_back(std::move(button));
    };
    add("New Place", "New.png", actions_.new_place);
    add("Open Project…", "Folder.png", actions_.open_project);
    add("Assets", "AssetFolder.png", actions_.show_assets);
    add("Preferences…", "Properties.png", actions_.open_preferences);
    return column;
}

std::shared_ptr<jadefx::Node> LandingPage::shortcut_column() {
    std::shared_ptr<jadefx::VBox> column = Column("SHORTCUTS");
    auto add = [&](const std::string& keys, const char* what) {
        auto row = jadefx::make<jadefx::HBox>();
        row->getClassList().add("landing-shortcut");
        row->getChildren().add(Text(keys, "landing-key"));
        row->getChildren().add(Text(what, "landing-what"));
        column->getChildren().add(row);
    };
    add("F5", "Test the place, or resume");
    add("Shift+F5", "Stop the test");
    add(std::string(kCommand) + "+S", "Save the project");
    add(std::string(kCommand) + "+K", "Clear the output");
    add(std::string(kCommand) + "+,", "Preferences");
    return column;
}

}  // namespace ide
