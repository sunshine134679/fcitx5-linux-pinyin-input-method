#include "modernime/settings/pages/input_page.h"

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include <cassert>
#include <iostream>
#include <string>

namespace {

void testModifierKeyDetection() {
    using namespace modernime::settings;
    assert(isModifierKey(GDK_KEY_Control_L));
    assert(isModifierKey(GDK_KEY_Control_R));
    assert(isModifierKey(GDK_KEY_Alt_L));
    assert(isModifierKey(GDK_KEY_Alt_R));
    assert(isModifierKey(GDK_KEY_Super_L));
    assert(isModifierKey(GDK_KEY_Super_R));
    assert(isModifierKey(GDK_KEY_Shift_L));
    assert(isModifierKey(GDK_KEY_Shift_R));

    assert(!isModifierKey(GDK_KEY_space));
    assert(!isModifierKey(GDK_KEY_Return));
    assert(!isModifierKey(GDK_KEY_Escape));
    assert(!isModifierKey(GDK_KEY_a));
}

void testModifierPromptFormatting() {
    using namespace modernime::settings;
    assert(formatModifierPrompt(0, 0) == "请按快捷键... (Esc取消)");
    assert(formatModifierPrompt(GDK_KEY_Control_L, 0) == "Ctrl + ...");
    assert(formatModifierPrompt(GDK_KEY_Control_R, 0) == "Ctrl + ...");
    assert(formatModifierPrompt(0, GDK_CONTROL_MASK) == "Ctrl + ...");
    assert(formatModifierPrompt(GDK_KEY_Shift_L, GDK_CONTROL_MASK) ==
           "Ctrl + Shift + ...");
    assert(formatModifierPrompt(0, GDK_MOD1_MASK) == "Alt + ...");
    assert(formatModifierPrompt(0, GDK_SUPER_MASK) == "Super + ...");
    assert(formatModifierPrompt(0, GDK_MOD4_MASK) == "Super + ...");
}

void testShortcutAssembly() {
    using namespace modernime::settings;
    assert(buildShortcutString(GDK_KEY_space, GDK_CONTROL_MASK) ==
           "Ctrl+Space");
    assert(buildShortcutString(GDK_KEY_space, GDK_MOD1_MASK) == "Alt+Space");
    assert(buildShortcutString(GDK_KEY_space, GDK_SUPER_MASK) == "Super+Space");
    assert(buildShortcutString(GDK_KEY_space, GDK_MOD4_MASK) == "Super+Space");
    assert(buildShortcutString(GDK_KEY_space,
                               GDK_CONTROL_MASK | GDK_SHIFT_MASK) ==
           "Ctrl+Shift+Space");

    // Letters & Numbers
    assert(buildShortcutString(GDK_KEY_a, GDK_CONTROL_MASK) == "Ctrl+A");
    assert(buildShortcutString(GDK_KEY_Z, GDK_CONTROL_MASK) == "Ctrl+Z");
    assert(buildShortcutString(GDK_KEY_1, GDK_CONTROL_MASK) == "Ctrl+1");
    assert(buildShortcutString(GDK_KEY_F12, GDK_CONTROL_MASK) == "Ctrl+F12");
}

} // namespace

int main(int argc, char **argv) {
    gtk_init_check(&argc, &argv);

    testModifierKeyDetection();
    testModifierPromptFormatting();
    testShortcutAssembly();

    std::cout << "All shortcut recorder unit tests passed!\n";
    return 0;
}
