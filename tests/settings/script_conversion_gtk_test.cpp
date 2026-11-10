#include "modernime/settings/script_conversion_control.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *root, std::string_view id) {
    const auto *name = static_cast<const char *>(g_object_get_data(G_OBJECT(root), "modernime-settings-target"));
    if (name && name == id) return root;
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), id);
    g_list_free(children); return result;
}
template<class F> void wait(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    assert(condition());
}
struct FakeConfig {
    bool traditional = false, failRead = false, failWrite = false, hold = false;
    unsigned writes = 0;
    GDBusMethodInvocation *pending = nullptr;
    GVariant *value() const {
        GVariantBuilder config, enabled, hotkey;
        g_variant_builder_init(&config, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_init(&enabled, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&enabled, "{sv}", "0", g_variant_new_string("other-input"));
        if (traditional) g_variant_builder_add(&enabled, "{sv}", "1", g_variant_new_string("modernime"));
        g_variant_builder_init(&hotkey, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&hotkey, "{sv}", "0", g_variant_new_string("Control+F9"));
        g_variant_builder_add(&config, "{sv}", "EnabledIM", g_variant_builder_end(&enabled));
        g_variant_builder_add(&config, "{sv}", "Hotkey", g_variant_builder_end(&hotkey));
        g_variant_builder_add(&config, "{sv}", "Engine", g_variant_new_string("OpenCC"));
        g_variant_builder_add(&config, "{sv}", "OpenCCS2TProfile", g_variant_new_string("synthetic-profile"));
        g_variant_builder_add(&config, "{sv}", "Unrelated", g_variant_new_string("keep-me"));
        return g_variant_builder_end(&config);
    }
    void reply(GDBusMethodInvocation *call) const {
        GVariantBuilder descriptions; g_variant_builder_init(&descriptions, G_VARIANT_TYPE("a(sa(sssva{sv}))"));
        g_dbus_method_invocation_return_value(call, g_variant_new("(v@a(sa(sssva{sv})))", value(), g_variant_builder_end(&descriptions)));
    }
    static void method(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                       GVariant *parameters, GDBusMethodInvocation *call, gpointer data) {
        auto &self = *static_cast<FakeConfig *>(data);
        const gchar *requestedUri = nullptr;
        g_variant_get_child(parameters, 0, "&s", &requestedUri);
        assert(std::string_view(requestedUri) == "fcitx://config/addon/chttrans");
        if (std::string_view(method) == "GetConfig") {
            if (self.failRead) g_dbus_method_invocation_return_dbus_error(call, "org.fcitx.Test.Unavailable", "synthetic read failure");
            else if (self.hold) self.pending = G_DBUS_METHOD_INVOCATION(g_object_ref(call));
            else self.reply(call);
            return;
        }
        assert(std::string_view(method) == "SetConfig"); ++self.writes;
        if (self.failWrite) {
            g_dbus_method_invocation_return_dbus_error(call, "org.fcitx.Test.Unavailable", "synthetic write failure"); return;
        }
        auto *boxed = g_variant_get_child_value(parameters, 1); auto *config = g_variant_get_variant(boxed);
        const gchar *value = nullptr;
        for (const auto *key : {"Engine", "OpenCCS2TProfile", "Unrelated"}) {
            assert(g_variant_lookup(config, key, "&s", &value));
            assert(std::string_view(value) == (std::string_view(key) == "Engine" ? "OpenCC" : std::string_view(key) == "Unrelated" ? "keep-me" : "synthetic-profile"));
        }
        auto *enabled = g_variant_lookup_value(config, "EnabledIM", G_VARIANT_TYPE_VARDICT);
        assert(enabled && g_variant_lookup(enabled, "0", "&s", &value) && std::string_view(value) == "other-input");
        self.traditional = g_variant_lookup(enabled, "1", "&s", &value) && std::string_view(value) == "modernime";
        auto *hotkey = g_variant_lookup_value(config, "Hotkey", G_VARIANT_TYPE_VARDICT);
        assert(hotkey && g_variant_lookup(hotkey, "0", "&s", &value) && std::string_view(value) == "Control+F9");
        g_variant_unref(hotkey); g_variant_unref(enabled); g_variant_unref(config); g_variant_unref(boxed);
        g_dbus_method_invocation_return_value(call, g_variant_new("()"));
    }
};
} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    installSettingsStyles();
    GError *error = nullptr; auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error); assert(bus && !error);
    // Run under an isolated test bus: never claim the real desktop's Fcitx service.
    auto *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.fcitx.Fcitx5"), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    gboolean occupied = TRUE; assert(reply && !error); g_variant_get(reply, "(b)", &occupied); g_variant_unref(reply); assert(!occupied);
    auto *info = g_dbus_node_info_new_for_xml("<node><interface name='org.fcitx.Fcitx.Controller1'><method name='GetConfig'><arg type='s' direction='in'/><arg type='v' direction='out'/><arg type='a(sa(sssva{sv}))' direction='out'/></method><method name='SetConfig'><arg type='s' direction='in'/><arg type='v' direction='in'/></method></interface></node>", &error); assert(info && !error);
    FakeConfig fake; static const GDBusInterfaceVTable vtable{FakeConfig::method, nullptr, nullptr, {nullptr}};
    const auto registration = g_dbus_connection_register_object(bus, "/controller", info->interfaces[0], &vtable, &fake, nullptr, &error); assert(registration && !error);
    bool owned = false;
    const auto service = g_bus_own_name_on_connection(bus, "org.fcitx.Fcitx5", G_BUS_NAME_OWNER_FLAGS_NONE,
        +[](GDBusConnection *, const gchar *, gpointer p) { *static_cast<bool *>(p) = true; }, nullptr, &owned, nullptr);
    wait([&] { return owned; });
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto control = std::make_unique<ScriptConversionControl>(); gtk_container_add(GTK_CONTAINER(window), control->widget());
    gtk_widget_show_all(window);
    auto *toggle = find(window, "script-traditional"); auto *status = find(window, "script-status");
    wait([&] { return gtk_widget_is_sensitive(toggle); }); assert(!gtk_switch_get_active(GTK_SWITCH(toggle)));
    gtk_switch_set_active(GTK_SWITCH(toggle), TRUE);
    wait([&] { return fake.writes == 1 && gtk_widget_is_sensitive(toggle); });
    assert(fake.traditional && gtk_switch_get_active(GTK_SWITCH(toggle)));
    // A native shortcut changes the sole Fcitx state; refresh must never write it back.
    fake.traditional = false; control->refresh();
    wait([&] { return gtk_widget_is_sensitive(toggle); }); assert(!gtk_switch_get_active(GTK_SWITCH(toggle)) && fake.writes == 1);
    fake.failRead = true; control->refresh();
    wait([&] { return std::string_view(gtk_label_get_text(GTK_LABEL(status))).find("暂不可用") != std::string_view::npos; });
    assert(!gtk_widget_is_sensitive(toggle) && fake.writes == 1);
    fake.failRead = false; control->refresh(); wait([&] { return gtk_widget_is_sensitive(toggle); });
    fake.failWrite = true; gtk_switch_set_active(GTK_SWITCH(toggle), TRUE);
    wait([&] { return std::string_view(gtk_label_get_text(GTK_LABEL(status))).find("切换失败") != std::string_view::npos; });
    assert(!fake.traditional && !gtk_switch_get_active(GTK_SWITCH(toggle)) && !gtk_widget_is_sensitive(toggle));
    fake.hold = true; control->refresh(); wait([&] { return fake.pending != nullptr; });
    gtk_widget_destroy(window); control.reset();
    fake.reply(fake.pending); g_object_unref(fake.pending);
    bool drained = false; g_timeout_add(100, +[](gpointer p) -> gboolean { *static_cast<bool *>(p) = true; return G_SOURCE_REMOVE; }, &drained); wait([&] { return drained; });
    g_bus_unown_name(service); g_dbus_connection_unregister_object(bus, registration); g_dbus_node_info_unref(info); g_object_unref(bus);
}
