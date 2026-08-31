#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <cassert>
#include <string>
#include <string_view>

namespace {

void drainEvents() {
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
}

void dispatchTab(GtkWidget *window, bool reverse) {
    assert(gtk_widget_child_focus(
        window, reverse ? GTK_DIR_TAB_BACKWARD : GTK_DIR_TAB_FORWARD));
    drainEvents();
}

void assertFocusChainsUseDirectChildren(GtkWidget *widget) {
    if (!GTK_IS_CONTAINER(widget)) {
        return;
    }
    GList *chain = nullptr;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    const bool hasChain =
        gtk_container_get_focus_chain(GTK_CONTAINER(widget), &chain);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (hasChain) {
        for (auto *item = chain; item != nullptr; item = item->next) {
            assert(gtk_widget_get_parent(GTK_WIDGET(item->data)) == widget);
        }
    }
    g_list_free(chain);

    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (auto *item = children; item != nullptr; item = item->next) {
        assertFocusChainsUseDirectChildren(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

} // namespace

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
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
    drainEvents();
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
    drainEvents();
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

    auto *nestedWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto *firstGroup = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto *secondGroup = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto *first = gtk_entry_new();
    auto *second = gtk_button_new_with_label("第二项");
    auto *more = gtk_button_new_with_label("更多");
    auto *third = gtk_button_new_with_label("第三项");
    gtk_container_add(GTK_CONTAINER(nestedWindow), root);
    gtk_box_pack_start(GTK_BOX(root), firstGroup, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), more, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), secondGroup, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(firstGroup), first, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(firstGroup), second, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(secondGroup), third, FALSE, FALSE, 0);
    setSettingsFocusChain(root, {first, second, more, third});
    gtk_widget_show_all(nestedWindow);
    gtk_window_present(GTK_WINDOW(nestedWindow));
    drainEvents();

    assertFocusChainsUseDirectChildren(root);
    gtk_widget_grab_focus(first);
    assert(gtk_widget_has_focus(first));
    dispatchTab(nestedWindow, false);
    assert(gtk_widget_has_focus(second));
    dispatchTab(nestedWindow, false);
    assert(gtk_widget_has_focus(more));
    dispatchTab(nestedWindow, false);
    assert(gtk_widget_has_focus(third));
    dispatchTab(nestedWindow, true);
    assert(gtk_widget_has_focus(more));

    gtk_widget_destroy(nestedWindow);

    auto *dialog = gtk_message_dialog_new(
        nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
        "继续操作吗？");
    setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_YES,
                                   "继续", "确认并执行当前操作");
    setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                   "取消", "关闭对话框且不执行操作");
    const auto assertResponseAccessibility = [dialog](
                                                 int response,
                                                 std::string_view name,
                                                 std::string_view description) {
        auto *button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                          response);
        assert(button != nullptr);
        auto *accessible = gtk_widget_get_accessible(button);
        assert(std::string_view(atk_object_get_name(accessible)) == name);
        assert(std::string_view(atk_object_get_description(accessible)) ==
               description);
    };
    assertResponseAccessibility(GTK_RESPONSE_YES, "继续",
                                "确认并执行当前操作");
    assertResponseAccessibility(GTK_RESPONSE_NO, "取消",
                                "关闭对话框且不执行操作");
    gtk_widget_destroy(dialog);

    auto *chooser = gtk_file_chooser_dialog_new(
        "选择文件", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "取消",
        GTK_RESPONSE_CANCEL, "打开", GTK_RESPONSE_ACCEPT, nullptr);
    setDialogResponseAccessibility(GTK_DIALOG(chooser), GTK_RESPONSE_CANCEL,
                                   "取消", "关闭文件选择器且不选择文件");
    setDialogResponseAccessibility(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT,
                                   "打开", "打开当前选中的文件");
    auto *chooserCancel = gtk_dialog_get_widget_for_response(
        GTK_DIALOG(chooser), GTK_RESPONSE_CANCEL);
    auto *chooserAccept = gtk_dialog_get_widget_for_response(
        GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
    assert(chooserCancel != nullptr && chooserAccept != nullptr);
    assert(std::string_view(atk_object_get_description(
               gtk_widget_get_accessible(chooserCancel))) ==
           "关闭文件选择器且不选择文件");
    assert(std::string_view(atk_object_get_description(
               gtk_widget_get_accessible(chooserAccept))) ==
           "打开当前选中的文件");
    gtk_widget_destroy(chooser);
}
