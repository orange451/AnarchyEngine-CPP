#include "ide/IdeLayout.hpp"
#include "ide/IdeTheme.hpp"
#include "runner/GameView.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <string>

// The game UI's cascade: its own default sheet in a SubScene, so the studio's
// theme and stylesheets never reach it, with Gui service CSS over it.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

bool Same(const jadefx::Color& a, const jadefx::Color& b) { return jadefx::near(a, b); }

runner::GameView* FindGameView(jadefx::Scene& scene) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* view = dynamic_cast<runner::GameView*>(node)) {
            return view;
        }
    }
    return nullptr;
}

}  // namespace

int RunGuiStyleTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    double time = scene.timeSeconds() + 0.01;
    // The layer syncs with the tree during layout, after the style pass, so a
    // change shows in the frame after the one that picks it up.
    auto frame = [&] {
        for (int i = 0; i < 2; ++i) {
            scene.layout(1280, 800, time);
            time += 0.02;
        }
    };
    frame();
    runner::GameView* view = FindGameView(scene);
    Expect(view != nullptr, "the studio has a Scene View");
    if (view == nullptr) {
        return gFailures;
    }

    engine_core::Engine& engine = layout.simulation();
    engine_core::InstanceId screen = 0;
    engine_core::InstanceId button = 0;
    engine_core::InstanceId field = 0;
    engine_core::InstanceId serviceCss = 0;
    engine_core::InstanceId screenCss = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        const engine_core::InstanceId gui = game.scene_service("Gui");
        screen = engine_core::lua_create_instance(game, "ScreenGui")->id();
        game.set_parent(screen, gui);
        button = engine_core::lua_create_instance(game, "Button")->id();
        game.set_parent(button, screen);
        field = engine_core::lua_create_instance(game, "TextField")->id();
        game.set_parent(field, screen);
    });
    auto setSource = [&](engine_core::InstanceId id, const char* css) {
        engine.on_simulation([&](engine_core::DataModel& game) {
            if (auto* sheet = dynamic_cast<engine_core::Css*>(game.instance(id))) {
                sheet->set_text(engine_core::GuiProperty::Source, css);
            }
        });
    };

    const ide::IdeTheme before = ide::current_theme();
    ide::set_current_theme(ide::IdeTheme(":root { --theme-base: dark; }"));
    frame();
    jadefx::Node* node = view->guiLayer().nodeFor(button);
    jadefx::Node* text = view->guiLayer().nodeFor(field);
    Expect(node != nullptr && text != nullptr, "the Button and TextField are drawn");
    if (node == nullptr || text == nullptr) {
        ide::set_current_theme(before);
        return gFailures;
    }

    // The blank default, whatever the studio's theme.
    const jadefx::ComputedStyle& style = node->computedStyle();
    Expect(style.background.color.a == 0.f, "in the dark studio theme a game Button has no background");
    Expect(node->themeColor(jadefx::ThemeColor::Border).a == 0.f && text->themeColor(jadefx::ThemeColor::Border).a == 0.f,
           "nor do a Button and a TextField have the built-in outline");
    Expect(style.padding.top == 0 && style.padding.left == 0 && text->computedStyle().padding.left == 0,
           "nor padding");
    Expect(Same(style.color, jadefx::Color::black()), "its text is black, not the studio's");
    Expect(node->themeColor(jadefx::ThemeColor::Wash).a > 0.f, "it keeps the hover wash");
    Expect(Same(node->themeColor(jadefx::ThemeColor::Outline), jadefx::Color::rgb8(0x1a, 0x73, 0xe8)),
           "and the focus ring, in the light accent");
    Expect(text->themeColor(jadefx::ThemeColor::TextSelection).a > 0.f, "a TextField keeps its selection color");

    // A stylesheet in the studio does not reach in.
    view->setStylesheet("button { color: #ff0000; } :root { --border-color: #ff0000; }");
    frame();
    Expect(Same(node->computedStyle().color, jadefx::Color::black()), "a studio stylesheet's rule does not reach a game Button");
    Expect(node->themeColor(jadefx::ThemeColor::Border).a == 0.f, "nor do its custom properties");
    view->setStylesheet("");

    // CSS under the Gui service styles every ScreenGui, under the ScreenGui's own.
    engine.on_simulation([&](engine_core::DataModel& game) {
        serviceCss = engine_core::lua_create_instance(game, "CSS")->id();
        game.set_parent(serviceCss, game.scene_service("Gui"));
    });
    setSource(serviceCss, ":root { --accent-color: #ff00ff; } button { color: #00ff00; }");
    frame();
    Expect(Same(node->computedStyle().color, jadefx::Color::rgb8(0, 255, 0)), "CSS under the Gui service styles a game Button");
    Expect(Same(node->themeColor(jadefx::ThemeColor::Outline), jadefx::Color::rgb8(255, 0, 255)),
           "its :root accent colors the focus ring");

    engine.on_simulation([&](engine_core::DataModel& game) {
        screenCss = engine_core::lua_create_instance(game, "CSS")->id();
        game.set_parent(screenCss, screen);
    });
    setSource(screenCss, "button { color: #0000ff; }");
    frame();
    Expect(Same(node->computedStyle().color, jadefx::Color::rgb8(0, 0, 255)), "a ScreenGui's CSS wins over the service's");

    setSource(screenCss, "");
    setSource(serviceCss, "button { color: #00ffff; padding: 4px; }");
    frame();
    Expect(Same(node->computedStyle().color, jadefx::Color::rgb8(0, 255, 255)), "editing the service CSS restyles the next frame");
    Expect(node->computedStyle().padding.left == 4, "and its padding wins over the default's");

    // TextScaled fills the box Size gives, and a stylesheet's font-size does not stop it.
    engine_core::InstanceId label = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        label = engine_core::lua_create_instance(game, "Label")->id();
        game.set_parent(label, screen);
        engine_core::LuaSlot size;
        size.kind = engine_core::LuaSlot::Kind::Vec2;
        size.vec = engine_core::Vec3{300.f, 120.f, 0.f};
        engine_core::LuaSlot scaled;
        scaled.kind = engine_core::LuaSlot::Kind::Bool;
        scaled.flag = true;
        for (const engine_core::InstanceId id : {label, button}) {
            auto* gui = dynamic_cast<engine_core::GuiValues*>(game.instance(id));
            gui->set_value(engine_core::GuiProperty::Size, size);
            gui->set_value(engine_core::GuiProperty::TextScaled, scaled);
        }
    });
    setSource(serviceCss, "button, label { font-size: 10px; }");
    frame();
    auto* scaledLabel = dynamic_cast<jadefx::Label*>(view->guiLayer().nodeFor(label));
    auto* scaledButton = dynamic_cast<jadefx::Button*>(view->guiLayer().nodeFor(button));
    Expect(scaledLabel != nullptr && scaledLabel->isTextScaled() && scaledLabel->displayedFontSize() > 40.f,
           "a TextScaled Label's text fills its Size");
    Expect(scaledButton != nullptr && scaledButton->isTextScaled() && scaledButton->displayedFontSize() > 40.f,
           "so does a TextScaled Button's");

    engine.on_simulation([&](engine_core::DataModel& game) {
        game.destroy_tree(serviceCss);
        game.destroy_tree(screen);
    });
    frame();
    Expect(view->guiLayer().nodeFor(button) == nullptr, "removing the ScreenGui takes its nodes away");
    ide::set_current_theme(before);
    frame();
    if (gFailures == 0) {
        std::printf("gui style tests passed\n");
    }
    return gFailures;
}
