#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <cassert>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) {
        return 77;
    }

    using namespace modernime::settings;

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto *target = gtk_entry_new();
    auto *fallback = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *next = gtk_button_new_with_label("下一个控件");
    setAccessibleWidgetText(next, "下一个控件", "将焦点移动到下一个控件");
    auto *nextAccessible = gtk_widget_get_accessible(next);
    assert(std::string_view(atk_object_get_name(nextAccessible)) ==
           "下一个控件");
    assert(std::string_view(atk_object_get_description(nextAccessible)) ==
           "将焦点移动到下一个控件");
    gtk_container_add(GTK_CONTAINER(window), box);
    gtk_box_pack_start(GTK_BOX(box), target, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), fallback, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), next, FALSE, FALSE, 0);
    setSettingsFocusChain(box, {target, next});
    GList *focusChain = nullptr;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    assert(gtk_container_get_focus_chain(GTK_CONTAINER(box), &focusChain));
    G_GNUC_END_IGNORE_DEPRECATIONS
    assert(g_list_length(focusChain) == 2);
    assert(focusChain->data == target);
    assert(focusChain->next->data == next);
    g_list_free(focusChain);
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    gtk_widget_grab_focus(next);
    assert(gtk_widget_has_focus(next));

    gtk_widget_set_sensitive(target, FALSE);
    assert(!gtk_widget_get_can_focus(fallback));
    assert(focusWidgetOrFallback(target, fallback));
    assert(gtk_widget_has_focus(fallback));
    assert(gtk_widget_get_can_focus(fallback));
    assert(gtk_style_context_has_class(
        gtk_widget_get_style_context(fallback),
        std::string(kSettingsFocusFallbackClass).c_str()));

    gtk_widget_grab_focus(next);
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    assert(gtk_widget_has_focus(next));
    assert(!gtk_widget_get_can_focus(fallback));
    assert(!gtk_style_context_has_class(
        gtk_widget_get_style_context(fallback),
        std::string(kSettingsFocusFallbackClass).c_str()));

    gtk_widget_set_sensitive(target, TRUE);
    assert(focusWidgetOrFallback(target, fallback));
    assert(gtk_widget_has_focus(target));
    assert(!gtk_widget_get_can_focus(fallback));

    gtk_widget_grab_focus(next);
    auto *unattachedTarget = gtk_entry_new();
    gtk_widget_show(unattachedTarget);
    assert(gtk_widget_get_visible(unattachedTarget));
    assert(gtk_widget_is_sensitive(unattachedTarget));
    assert(gtk_widget_get_can_focus(unattachedTarget));
    assert(!gtk_widget_get_mapped(unattachedTarget));
    assert(focusWidgetOrFallback(unattachedTarget, fallback));
    assert(gtk_widget_has_focus(fallback));
    assert(gtk_widget_get_can_focus(fallback));

    g_object_ref_sink(unattachedTarget);
    gtk_widget_destroy(unattachedTarget);
    g_object_unref(unattachedTarget);

    gtk_widget_destroy(window);
}
