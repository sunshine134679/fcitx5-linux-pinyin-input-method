#include "modernime/settings/settings_shell.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    static guint32 lastEventTime = 0;
    assert(GTK_IS_WINDOW(window));
    auto *focus = gtk_window_get_focus(GTK_WINDOW(window));
    assert(focus != nullptr);
    auto *eventWindow = gtk_widget_get_window(window);
    assert(eventWindow != nullptr);
    auto *display = gtk_widget_get_display(window);
    auto *keymap = gdk_keymap_get_for_display(display);
    auto *seat = gdk_display_get_default_seat(display);
    assert(seat != nullptr);
    auto *keyboard = gdk_seat_get_keyboard(seat);
    assert(keyboard != nullptr);
    const auto sendEvent = [&](GdkEventType type, guint eventKeyval,
                               GdkModifierType eventState,
                               bool isModifier) {
        GdkKeymapKey *keys = nullptr;
        gint keyCount = 0;
        assert(gdk_keymap_get_entries_for_keyval(keymap, eventKeyval, &keys,
                                                 &keyCount));
        assert(keys != nullptr && keyCount > 0);
        assert(keys[0].keycode <= G_MAXUINT16);
        assert(keys[0].group >= 0 && keys[0].group <= G_MAXUINT8);
        auto *event = gdk_event_new(type);
        event->key.window = GDK_WINDOW(g_object_ref(eventWindow));
        event->key.send_event = TRUE;
        auto eventTime =
            static_cast<guint32>(g_get_monotonic_time() / 1000);
        if (eventTime <= lastEventTime) {
            eventTime = lastEventTime + 1;
        }
        event->key.time = eventTime;
        lastEventTime = eventTime;
        event->key.state = eventState;
        event->key.keyval = eventKeyval;
        event->key.hardware_keycode = static_cast<guint16>(keys[0].keycode);
        event->key.group = static_cast<guint8>(keys[0].group);
        event->key.is_modifier = isModifier;
        event->key.length = 0;
        event->key.string = nullptr;
        gdk_event_set_device(event, keyboard);
        gdk_event_set_source_device(event, keyboard);
        gtk_main_do_event(event);
        gdk_event_free(event);
        g_free(keys);
    };
    struct ModifierKey final {
        GdkModifierType mask;
        guint keyval;
    };
    constexpr ModifierKey modifierKeys[] = {
        {GDK_SHIFT_MASK, GDK_KEY_Shift_L},
        {GDK_CONTROL_MASK, GDK_KEY_Control_L},
        {GDK_MOD1_MASK, GDK_KEY_Alt_L},
    };
    for (const auto &modifier : modifierKeys) {
        if ((state & modifier.mask) != 0) {
            sendEvent(GDK_KEY_PRESS, modifier.keyval, GdkModifierType{}, true);
        }
    }
    sendEvent(GDK_KEY_PRESS, keyval, state, false);
    sendEvent(GDK_KEY_RELEASE, keyval, state, false);
    for (auto item = std::rbegin(modifierKeys);
         item != std::rend(modifierKeys); ++item) {
        if ((state & item->mask) != 0) {
            sendEvent(GDK_KEY_RELEASE, item->keyval, state, true);
        }
    }
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

struct WidgetTypeSearch final {
    GType type;
    GtkWidget *result = nullptr;
};

GtkWidget *findWidgetByType(GtkWidget *widget, GType type);

void findWidgetByTypeChild(GtkWidget *child, gpointer data) {
    auto *search = static_cast<WidgetTypeSearch *>(data);
    if (search->result == nullptr) {
        search->result = findWidgetByType(child, search->type);
    }
}

GtkWidget *findWidgetByType(GtkWidget *widget, GType type) {
    if (g_type_is_a(G_OBJECT_TYPE(widget), type)) {
        return widget;
    }
    if (!GTK_IS_CONTAINER(widget)) {
        return nullptr;
    }
    WidgetTypeSearch search{type};
    gtk_container_forall(GTK_CONTAINER(widget), findWidgetByTypeChild,
                         &search);
    return search.result;
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

struct KeyPressObservation final {
    std::size_t count = 0;
    guint keyval = 0;
    GdkModifierType modifiers{};
};

gboolean observeKeyPress(GtkWidget *, GdkEventKey *event, gpointer data) {
    auto *observation = static_cast<KeyPressObservation *>(data);
    ++observation->count;
    observation->keyval = event->keyval;
    observation->modifiers = static_cast<GdkModifierType>(
        event->state & gtk_accelerator_get_default_mod_mask());
    return FALSE;
}

gboolean observeControllerKey(GtkEventControllerKey *, guint keyval, guint,
                              GdkModifierType state, gpointer data) {
    auto *observation = static_cast<KeyPressObservation *>(data);
    ++observation->count;
    observation->keyval = keyval;
    observation->modifiers = static_cast<GdkModifierType>(
        state & gtk_accelerator_get_default_mod_mask());
    return FALSE;
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
    dispatchKey(dialog, GDK_KEY_Tab, GdkModifierType{});
    assert(hasFocusWithin(dialog, phrase));
    dispatchKey(dialog, GDK_KEY_Tab, GdkModifierType{});
    assert(hasFocusWithin(dialog, weight));
    dispatchKey(dialog, GDK_KEY_Tab, GdkModifierType{});
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
    KeyPressObservation cancelKeyEvents;
    KeyPressObservation saveKeyEvents;
    g_signal_connect(cancel, "key-press-event", G_CALLBACK(observeKeyPress),
                     &cancelKeyEvents);
    g_signal_connect(save, "key-press-event", G_CALLBACK(observeKeyPress),
                     &saveKeyEvents);
    dispatchKey(dialog, GDK_KEY_Tab, GdkModifierType{});
    assert(cancelKeyEvents.count == 1);
    assert(hasFocusWithin(dialog, save));
    dispatchKey(dialog, GDK_KEY_ISO_Left_Tab, GDK_SHIFT_MASK);
    assert(saveKeyEvents.count > 0);
    assert(saveKeyEvents.keyval == GDK_KEY_ISO_Left_Tab);
    assert(saveKeyEvents.modifiers == GDK_SHIFT_MASK);
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

        auto *search =
            findAccessibleWidget(window, GTK_TYPE_SEARCH_ENTRY, "搜索设置");
        auto *apply = findAccessibleWidget(window, GTK_TYPE_BUTTON, "应用");
        assert(search != nullptr && apply != nullptr);

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
        auto *popover = findWidgetByType(window, GTK_TYPE_POPOVER);
        assert(popover != nullptr);
        assert(waitUntil(
            [popover] { return gtk_widget_get_visible(popover); }));
        KeyPressObservation escapeObservation;
        auto *escapeController = gtk_event_controller_key_new(window);
        gtk_event_controller_set_propagation_phase(escapeController,
                                                   GTK_PHASE_CAPTURE);
        g_signal_connect(escapeController, "key-pressed",
                         G_CALLBACK(observeControllerKey), &escapeObservation);
        dispatchKey(window, GDK_KEY_Escape, GDK_CONTROL_MASK);
        assert(escapeObservation.count > 0);
        assert(escapeObservation.keyval == GDK_KEY_Escape);
        assert(escapeObservation.modifiers == GDK_CONTROL_MASK);
        assert(gtk_widget_get_visible(popover));
        g_object_unref(escapeController);
        gtk_entry_set_text(GTK_ENTRY(search), "");
        assert(waitUntil([popover] {
            return !gtk_widget_get_visible(popover) &&
                   !gtk_widget_get_mapped(popover);
        }));
        gtk_entry_set_text(GTK_ENTRY(search), "a");
        assert(waitUntil(
            [popover] { return gtk_widget_get_visible(popover); }));
        escapeObservation = {};
        escapeController = gtk_event_controller_key_new(window);
        gtk_event_controller_set_propagation_phase(escapeController,
                                                   GTK_PHASE_CAPTURE);
        g_signal_connect(escapeController, "key-pressed",
                         G_CALLBACK(observeControllerKey), &escapeObservation);
        dispatchKey(window, GDK_KEY_Escape, GDK_MOD1_MASK);
        assert(escapeObservation.count > 0);
        assert(escapeObservation.keyval == GDK_KEY_Escape);
        assert(escapeObservation.modifiers == GDK_MOD1_MASK);
        assert(gtk_widget_get_visible(popover));
        g_object_unref(escapeController);
        dispatchKey(window, GDK_KEY_Escape, GdkModifierType{});
        assert(waitUntil(
            [popover] {
                return !gtk_widget_get_visible(popover) &&
                       !gtk_widget_get_mapped(popover);
            }));

        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inputEnabled),
                                     !initialInputEnabled);
        drainEvents();
        assert(gtk_widget_get_sensitive(apply));
        gtk_widget_grab_focus(inputEnabled);
        assert(gtk_widget_has_focus(inputEnabled));
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
