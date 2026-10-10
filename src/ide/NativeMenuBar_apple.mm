// The studio's menus in the macOS menu bar. See NativeMenuBar.hpp.

#include "NativeMenuBar.hpp"

#include "jadefx/event/Events.hpp"
#include "jadefx/scene/controls/Menu.hpp"
#include "jadefx/scene/controls/MenuBar.hpp"
#include "jadefx/scene/controls/MenuItem.hpp"

#include <memory>
#include <string>

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#if !__has_feature(objc_arc)
#error "NativeMenuBar_apple.mm is built with -fobjc-arc (CMakeLists.txt)."
#endif

// Runs one JadeFX item. The NSMenuItem keeps it as its representedObject,
// since an NSMenuItem's target is a weak reference.
@interface AnarchyMenuAction : NSObject {
@public
    std::weak_ptr<jadefx::MenuItem> item;
}
- (void)fire:(id)sender;
@end

@implementation AnarchyMenuAction
- (void)fire:(id)sender {
    (void)sender;
    if (std::shared_ptr<jadefx::MenuItem> held = item.lock()) {
        held->fire();
    }
}

// Asked as the menu opens and before a key equivalent runs, so a disabled
// item neither shows enabled nor takes its shortcut.
- (BOOL)validateMenuItem:(NSMenuItem*)menuItem {
    (void)menuItem;
    std::shared_ptr<jadefx::MenuItem> held = item.lock();
    return held != nullptr && !held->isDisable();
}
@end

// Fills one NSMenu from one JadeFX Menu whenever AppKit is about to read it.
@interface AnarchyMenuSource : NSObject <NSMenuDelegate> {
@public
    std::weak_ptr<jadefx::Menu> menu;
    BOOL windowMenu;
}
@end

namespace {

// The NSMenu keeps its delegate through this association; delegate is weak.
char kSourceKey;

const jadefx::MenuItem* gSettings = nullptr;
const jadefx::MenuItem* gQuit = nullptr;
const jadefx::Menu* gWindow = nullptr;
// What Install added, for Remove to take out.
NSMutableArray<NSMenuItem*>* gAdded = nil;
// GLFW's Window menu, set aside while ours is in.
NSMenuItem* gDisplacedWindow = nil;
NSInteger gDisplacedIndex = -1;

NSString* Text(const std::string& text) {
    NSString* string = [NSString stringWithUTF8String:text.c_str()];
    return string != nil ? string : @"";
}

NSString* Character(unichar character) { return [NSString stringWithCharacters:&character length:1]; }

// JadeFX key codes are GLFW's: letters and digits are ASCII.
NSString* KeyEquivalent(int key) {
    namespace Key = jadefx::Key;
    if (key >= Key::A && key <= Key::Z) {
        return Character(static_cast<unichar>('a' + (key - Key::A)));
    }
    if ((key >= Key::Digit0 && key <= Key::Digit9) || key == Key::Space || key == Key::Apostrophe ||
        key == Key::Comma || key == Key::Minus || key == Key::Period || key == Key::Slash ||
        key == Key::Semicolon || key == Key::Equal || key == Key::LeftBracket || key == Key::Backslash ||
        key == Key::RightBracket) {
        return Character(static_cast<unichar>(key));
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return Character(static_cast<unichar>(NSF1FunctionKey + (key - Key::F1)));
    }
    switch (key) {
        case Key::Escape:
            return Character(0x1b);
        case Key::Enter:
        case Key::KpEnter:
            return Character('\r');
        case Key::Tab:
            return Character('\t');
        case Key::Backspace:
            return Character(NSBackspaceCharacter);
        case Key::Delete:
            return Character(NSDeleteFunctionKey);
        case Key::Left:
            return Character(NSLeftArrowFunctionKey);
        case Key::Right:
            return Character(NSRightArrowFunctionKey);
        case Key::Up:
            return Character(NSUpArrowFunctionKey);
        case Key::Down:
            return Character(NSDownArrowFunctionKey);
        case Key::Home:
            return Character(NSHomeFunctionKey);
        case Key::End:
            return Character(NSEndFunctionKey);
        case Key::PageUp:
            return Character(NSPageUpFunctionKey);
        case Key::PageDown:
            return Character(NSPageDownFunctionKey);
        default:
            return @"";
    }
}

// ModControl is Ctrl or Command in JadeFX, which on a Mac means Command.
NSEventModifierFlags ModifierMask(int mods) {
    NSEventModifierFlags mask = 0;
    if ((mods & (jadefx::Key::ModControl | jadefx::Key::ModSuper)) != 0) {
        mask |= NSEventModifierFlagCommand;
    }
    if ((mods & jadefx::Key::ModShift) != 0) {
        mask |= NSEventModifierFlagShift;
    }
    if ((mods & jadefx::Key::ModAlt) != 0) {
        mask |= NSEventModifierFlagOption;
    }
    return mask;
}

NSMenuItem* ActionItem(NSString* title, const std::shared_ptr<jadefx::MenuItem>& item) {
    AnarchyMenuAction* action = [[AnarchyMenuAction alloc] init];
    action->item = item;
    NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:title
                                                   action:@selector(fire:)
                                            keyEquivalent:KeyEquivalent(item->getAcceleratorKey())];
    entry.keyEquivalentModifierMask = ModifierMask(item->getAcceleratorMods());
    entry.target = action;
    entry.representedObject = action;
    return entry;
}

NSMenu* MakeMenu(NSString* title, const std::shared_ptr<jadefx::Menu>& menu);

void Fill(NSMenu* native, AnarchyMenuSource* source) {
    [native removeAllItems];
    std::shared_ptr<jadefx::Menu> menu = source->menu.lock();
    if (!menu) {
        return;
    }
    // A separator shows only between two shown items, so items left out
    // here leave no doubled or trailing lines.
    bool any = false;
    bool separator = false;
    if (source->windowMenu) {
        [native addItem:[[NSMenuItem alloc] initWithTitle:@"Minimize"
                                                   action:@selector(performMiniaturize:)
                                            keyEquivalent:@"m"]];
        [native addItem:[[NSMenuItem alloc] initWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""]];
        any = true;
        separator = true;
    }
    for (const std::shared_ptr<jadefx::MenuItem>& item : menu->getItems().items()) {
        if (!item || !item->isVisible() || item.get() == gSettings || item.get() == gQuit) {
            continue;
        }
        if (dynamic_cast<const jadefx::SeparatorMenuItem*>(item.get()) != nullptr) {
            separator = any;
            continue;
        }
        if (separator) {
            [native addItem:[NSMenuItem separatorItem]];
            separator = false;
        }
        NSString* title = Text(item->getText());
        if (std::shared_ptr<jadefx::Menu> sub = std::dynamic_pointer_cast<jadefx::Menu>(item)) {
            NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            entry.submenu = MakeMenu(title, sub);
            [native addItem:entry];
        } else {
            [native addItem:ActionItem(title, item)];
        }
        any = true;
    }
}

NSMenu* MakeMenu(NSString* title, const std::shared_ptr<jadefx::Menu>& menu) {
    NSMenu* native = [[NSMenu alloc] initWithTitle:title];
    // Enabled state comes from AnarchyMenuAction's validateMenuItem:.
    native.autoenablesItems = YES;
    AnarchyMenuSource* source = [[AnarchyMenuSource alloc] init];
    source->menu = menu;
    source->windowMenu = menu.get() == gWindow;
    objc_setAssociatedObject(native, &kSourceKey, source, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    native.delegate = source;
    // Filled now as well, so shortcuts work before the menu is first opened.
    Fill(native, source);
    return native;
}

// GLFW makes the application menu. Made here only if nothing did.
NSMenu* ApplicationMenu(NSMenu* main) {
    if (main.numberOfItems > 0 && [main itemAtIndex:0].submenu != nil) {
        return [main itemAtIndex:0].submenu;
    }
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    [menu addItem:[[NSMenuItem alloc] initWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"]];
    NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    holder.submenu = menu;
    [main insertItem:holder atIndex:0];
    [gAdded addObject:holder];
    return menu;
}

void AddSettings(NSMenu* app, const std::shared_ptr<jadefx::MenuItem>& settings) {
    NSString* title = @"Preferences…";
    if (@available(macOS 13.0, *)) {
        title = @"Settings…";
    }
    NSMenuItem* entry = ActionItem(title, settings);
    entry.keyEquivalent = @",";
    entry.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    // After About, in its own group, as in Apple's applications.
    NSInteger at = 0;
    if (app.numberOfItems > 0 && [app itemAtIndex:0].action == @selector(orderFrontStandardAboutPanel:)) {
        NSMenuItem* line = [NSMenuItem separatorItem];
        [app insertItem:line atIndex:1];
        [gAdded addObject:line];
        at = 2;
    }
    [app insertItem:entry atIndex:at];
    [gAdded addObject:entry];
    if (at + 1 < app.numberOfItems && ![app itemAtIndex:at + 1].isSeparatorItem) {
        NSMenuItem* line = [NSMenuItem separatorItem];
        [app insertItem:line atIndex:at + 1];
        [gAdded addObject:line];
    }
}

}  // namespace

@implementation AnarchyMenuSource
- (void)menuNeedsUpdate:(NSMenu*)native {
    Fill(native, self);
}
@end

namespace ide {

void InstallNativeMenuBar(const jadefx::MenuBar& bar, const NativeMenuSetup& setup) {
    @autoreleasepool {
        if (NSApp == nil) {
            return;
        }
        RemoveNativeMenuBar();
        gSettings = setup.settings;
        gQuit = setup.quit;
        gWindow = setup.window;
        gAdded = [NSMutableArray array];

        NSMenu* main = NSApp.mainMenu;
        if (main == nil) {
            main = [[NSMenu alloc] initWithTitle:@""];
            NSApp.mainMenu = main;
        }
        NSMenu* app = ApplicationMenu(main);

        std::shared_ptr<jadefx::MenuItem> settings;
        for (const std::shared_ptr<jadefx::Menu>& menu : bar.getMenus().items()) {
            if (!menu) {
                continue;
            }
            for (const std::shared_ptr<jadefx::MenuItem>& item : menu->getItems().items()) {
                if (item && item.get() == gSettings) {
                    settings = item;
                }
            }
        }
        if (settings) {
            AddSettings(app, settings);
        }

        // Ours takes the place of GLFW's Window menu.
        if (gWindow != nullptr) {
            for (NSInteger i = 1; i < main.numberOfItems; ++i) {
                NSMenuItem* item = [main itemAtIndex:i];
                if (item.submenu != nil && [item.submenu.title isEqualToString:@"Window"]) {
                    gDisplacedWindow = item;
                    gDisplacedIndex = i;
                    [main removeItemAtIndex:i];
                    break;
                }
            }
        }

        NSInteger at = 1;
        for (const std::shared_ptr<jadefx::Menu>& menu : bar.getMenus().items()) {
            if (!menu) {
                continue;
            }
            NSString* title = Text(menu->getText());
            NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            holder.submenu = MakeMenu(title, menu);
            holder.hidden = !menu->isVisible();
            [main insertItem:holder atIndex:at++];
            [gAdded addObject:holder];
        }
    }
}

void RemoveNativeMenuBar() {
    @autoreleasepool {
        for (NSMenuItem* item in gAdded) {
            [item.menu removeItem:item];
        }
        gAdded = nil;
        NSMenu* main = NSApp != nil ? NSApp.mainMenu : nil;
        if (gDisplacedWindow != nil && main != nil) {
            [main insertItem:gDisplacedWindow atIndex:MIN(gDisplacedIndex, main.numberOfItems)];
        }
        gDisplacedWindow = nil;
        gDisplacedIndex = -1;
        gSettings = nullptr;
        gQuit = nullptr;
        gWindow = nullptr;
    }
}

}  // namespace ide
