#include "modernime/settings/settings_shell.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

void drainEvents() {
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
}

void dispatchKey(GtkWidget *window, guint keyval, GdkModifierType state) {
    GdkEventKey event{};
    event.type = GDK_KEY_PRESS;
    event.state = state;
    event.keyval = keyval;
    gboolean handled = FALSE;
    g_signal_emit_by_name(window, "key-press-event", &event, &handled);
    drainEvents();
}

void traverseFocus(GtkWidget *window, bool reverse) {
    assert(gtk_widget_child_focus(
        window, reverse ? GTK_DIR_TAB_BACKWARD : GTK_DIR_TAB_FORWARD));
    drainEvents();
}

GtkWidget *findTarget(GtkWidget *widget, std::string_view target) {
    const auto *id = static_cast<const char *>(g_object_get_data(
        G_OBJECT(widget), "modernime-settings-target"));
    if (id != nullptr && target == id) {
        return widget;
    }
    if (!GTK_IS_CONTAINER(widget)) {
        return nullptr;
    }
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *item = children; item != nullptr && result == nullptr;
         item = item->next) {
        result = findTarget(GTK_WIDGET(item->data), target);
    }
    g_list_free(children);
    return result;
}

GtkWidget *findAccessibleWidget(GtkWidget *widget, GType type,
                                std::string_view name) {
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) {
        auto *accessible = gtk_widget_get_accessible(widget);
        const auto *accessibleName = atk_object_get_name(accessible);
        if (accessibleName != nullptr && name == accessibleName) {
            return widget;
        }
    }
    if (!GTK_IS_CONTAINER(widget)) {
        return nullptr;
    }
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *item = children; item != nullptr && result == nullptr;
         item = item->next) {
        result = findAccessibleWidget(GTK_WIDGET(item->data), type, name);
    }
    g_list_free(children);
    return result;
}

template <typename Predicate>
bool waitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        drainEvents();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drainEvents();
    return predicate();
}

std::size_t lineCount(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++count;
    }
    return count;
}

bool hasFocusWithin(GtkWidget *window, GtkWidget *widget) {
    auto *focused = gtk_window_get_focus(GTK_WINDOW(window));
    return focused == widget ||
           (focused != nullptr && gtk_widget_is_ancestor(focused, widget));
}

struct DictionaryDialogProbe final {
    bool ran = false;
    std::size_t attempts = 0;
};

gboolean inspectDictionaryDialog(gpointer data) {
    auto *probe = static_cast<DictionaryDialogProbe *>(data);
    auto *windows = gtk_window_list_toplevels();
    GtkWidget *dialog = nullptr;
    for (auto *item = windows; item != nullptr; item = item->next) {
        auto *candidate = GTK_WIDGET(item->data);
        const auto *title = GTK_IS_WINDOW(candidate)
                                ? gtk_window_get_title(GTK_WINDOW(candidate))
                                : nullptr;
        if (GTK_IS_DIALOG(candidate) && gtk_widget_get_visible(candidate) &&
            gtk_widget_get_mapped(candidate) &&
            title != nullptr && std::string_view(title) == "添加用户词条") {
            dialog = candidate;
            break;
        }
    }
    g_list_free(windows);
    if (dialog == nullptr) {
        ++probe->attempts;
        assert(probe->attempts < 200);
        return G_SOURCE_CONTINUE;
    }

    auto *cancel = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                      GTK_RESPONSE_CANCEL);
    auto *save = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                    GTK_RESPONSE_ACCEPT);
    assert(cancel != nullptr && save != nullptr);
    assert(std::string_view(atk_object_get_description(
               gtk_widget_get_accessible(cancel))) ==
           "关闭对话框且不保存词条修改");
    assert(std::string_view(atk_object_get_description(
               gtk_widget_get_accessible(save))) ==
           "校验通过后保存当前词条");

    auto *pinyin = findAccessibleWidget(dialog, GTK_TYPE_ENTRY, "拼音");
    auto *phrase = findAccessibleWidget(dialog, GTK_TYPE_ENTRY, "词条");
    auto *weight =
        findAccessibleWidget(dialog, GTK_TYPE_SPIN_BUTTON, "权重");
    assert(pinyin != nullptr && phrase != nullptr && weight != nullptr);
    gtk_entry_set_text(GTK_ENTRY(pinyin), "ni");
    gtk_entry_set_text(GTK_ENTRY(phrase), "你");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), 1.0);
    assert(gtk_widget_get_sensitive(save));

    gtk_window_set_focus(GTK_WINDOW(dialog), pinyin);
    assert(hasFocusWithin(dialog, pinyin));
    assert(gtk_widget_child_focus(dialog, GTK_DIR_TAB_FORWARD));
    assert(hasFocusWithin(dialog, phrase));
    assert(gtk_widget_child_focus(dialog, GTK_DIR_TAB_FORWARD));
    assert(hasFocusWithin(dialog, weight));
    assert(gtk_widget_child_focus(dialog, GTK_DIR_TAB_FORWARD));
    assert(hasFocusWithin(dialog, cancel));
    auto *actionArea = gtk_widget_get_parent(cancel);
    assert(actionArea != nullptr &&
           actionArea == gtk_widget_get_parent(save));
    GList *actionChain = nullptr;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    assert(gtk_container_get_focus_chain(GTK_CONTAINER(actionArea),
                                         &actionChain));
    G_GNUC_END_IGNORE_DEPRECATIONS
    assert(g_list_length(actionChain) == 2);
    assert(actionChain->data == cancel);
    assert(actionChain->next->data == save);
    g_list_free(actionChain);
    gtk_window_set_focus(GTK_WINDOW(dialog), save);
    assert(hasFocusWithin(dialog, save));
    gtk_window_set_focus(GTK_WINDOW(dialog), cancel);
    assert(hasFocusWithin(dialog, cancel));

    probe->ran = true;
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    return G_SOURCE_REMOVE;
}

} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) {
        return 77;
    }

    const auto root = std::filesystem::temp_directory_path() /
                      ("modernime-settings-shell-gtk-" +
                       std::to_string(static_cast<long long>(getpid())));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "bin", error);
    assert(!error);
    const auto fcitxLog = root / "fcitx5.log";
    const auto remote = root / "bin/fcitx5-remote";
    const auto fcitx = root / "bin/fcitx5";
    {
        std::ofstream script(remote);
        script << "#!/bin/sh\n"
                  "case \"$1\" in\n"
                  "  -n) printf 'modernime\\n';;\n"
                  "  -m) printf 'modernime\\n';;\n"
                  "  *) printf '2\\n';;\n"
                  "esac\n";
    }
    {
        std::ofstream script(fcitx);
        script << "#!/bin/sh\n"
                  "printf 'attempt\\n' >> \""
               << fcitxLog.string()
               << "\"\n"
                  "exit 7\n";
    }
    std::filesystem::permissions(
        remote, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, error);
    assert(!error);
    std::filesystem::permissions(
        fcitx, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, error);
    assert(!error);
    const auto *oldPath = std::getenv("PATH");
    const auto path = (root / "bin").string() + ":" +
                      (oldPath == nullptr ? "" : oldPath);
    assert(g_setenv("PATH", path.c_str(), TRUE));

    auto *application = gtk_application_new(
        nullptr, static_cast<GApplicationFlags>(G_APPLICATION_NON_UNIQUE));
    GError *registerError = nullptr;
    assert(g_application_register(G_APPLICATION(application), nullptr,
                                  &registerError));
    assert(registerError == nullptr);

    modernime::core::SettingsPaths paths{
        root / "config/settings.conf", root / "data/user.dict",
        root / "data/learning.db", root / "data/clipboard.history"};
    {
        modernime::settings::SettingsShell shell(application, paths);
        auto *window = shell.window();
        assert(window != nullptr && !gtk_widget_get_mapped(window));
        int width = 0;
        int height = 0;
        gtk_window_get_default_size(GTK_WINDOW(window), &width, &height);
        assert(width == 860 && height == 620);
        gtk_window_resize(GTK_WINDOW(window), 720, 520);
        shell.present();
        drainEvents();
        assert(gtk_widget_get_mapped(window));
        gtk_window_get_size(GTK_WINDOW(window), &width, &height);
        assert(width == 720 && height == 520);

        auto *search = GTK_WIDGET(g_object_get_data(
            G_OBJECT(window), "modernime-settings-search-entry"));
        auto *popover = GTK_WIDGET(g_object_get_data(
            G_OBJECT(window), "modernime-settings-search-popover"));
        auto *apply = GTK_WIDGET(g_object_get_data(
            G_OBJECT(window), "modernime-settings-apply-button"));
        assert(search != nullptr && popover != nullptr && apply != nullptr);

        shell.show(modernime::settings::SettingsPageId::Input,
                   "input-enabled");
        drainEvents();
        auto *inputEnabled = findTarget(window, "input-enabled");
        auto *defaultMode = findTarget(window, "default-mode");
        auto *more = findTarget(window, "input-more");
        assert(inputEnabled != nullptr && gtk_widget_has_focus(inputEnabled));
        assert(defaultMode != nullptr && more != nullptr);
        traverseFocus(window, true);
        assert(gtk_widget_has_focus(more));
        traverseFocus(window, false);
        assert(gtk_widget_has_focus(inputEnabled));
        traverseFocus(window, false);
        assert(hasFocusWithin(window, defaultMode));

        const bool initialInputEnabled = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(inputEnabled));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inputEnabled), FALSE);
        shell.show(modernime::settings::SettingsPageId::Input,
                   "default-mode");
        drainEvents();
        auto *defaultModeFallback = gtk_widget_get_parent(defaultMode);
        assert(defaultModeFallback != nullptr);
        assert(gtk_widget_has_focus(defaultModeFallback));
        assert(gtk_widget_get_can_focus(defaultModeFallback));
        assert(gtk_style_context_has_class(
            gtk_widget_get_style_context(defaultModeFallback),
            std::string(modernime::settings::kSettingsFocusFallbackClass)
                .c_str()));
        traverseFocus(window, false);
        assert(!gtk_widget_get_can_focus(defaultModeFallback));
        assert(!gtk_style_context_has_class(
            gtk_widget_get_style_context(defaultModeFallback),
            std::string(modernime::settings::kSettingsFocusFallbackClass)
                .c_str()));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inputEnabled),
                                     initialInputEnabled);

        dispatchKey(window, GDK_KEY_f, GDK_CONTROL_MASK);
        assert(gtk_widget_has_focus(search));
        gtk_entry_set_text(GTK_ENTRY(search), "a");
        assert(waitUntil(
            [popover] { return gtk_widget_get_visible(popover); }));
        dispatchKey(window, GDK_KEY_Escape, GDK_CONTROL_MASK);
        assert(gtk_widget_get_visible(popover));
        dispatchKey(window, GDK_KEY_Escape, GDK_MOD1_MASK);
        assert(gtk_widget_get_visible(popover));
        dispatchKey(window, GDK_KEY_Escape, GdkModifierType{});
        assert(waitUntil(
            [popover] { return !gtk_widget_get_visible(popover); }));

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inputEnabled),
                                     !initialInputEnabled);
        drainEvents();
        assert(gtk_widget_get_sensitive(apply));
        dispatchKey(window, GDK_KEY_Return, GDK_CONTROL_MASK);
        assert(std::filesystem::exists(paths.settingsFile));
        assert(!gtk_widget_get_sensitive(apply));

        shell.show(modernime::settings::SettingsPageId::Dictionary);
        drainEvents();
        auto *add = findTarget(window, "dictionary-add");
        assert(add != nullptr);
        DictionaryDialogProbe dialogProbe;
        g_timeout_add(10, inspectDictionaryDialog, &dialogProbe);
        gtk_button_clicked(GTK_BUTTON(add));
        assert(dialogProbe.ran);

        shell.show(modernime::settings::SettingsPageId::Diagnostics);
        auto *refresh = findTarget(window, "diagnostics-refresh");
        auto *reload = findTarget(window, "diagnostics-reload");
        assert(refresh != nullptr && reload != nullptr);
        assert(waitUntil([reload] { return gtk_widget_get_sensitive(reload); }));
        for (std::size_t attempt = 1; attempt <= 2; ++attempt) {
            gtk_button_clicked(GTK_BUTTON(reload));
            assert(!gtk_widget_get_sensitive(refresh));
            assert(!gtk_widget_get_sensitive(reload));
            assert(waitUntil(
                [reload] { return gtk_widget_get_sensitive(reload); }));
            assert(lineCount(fcitxLog) == attempt);
        }
    }
    g_object_unref(application);
    std::filesystem::remove_all(root, error);
    return 0;
}
