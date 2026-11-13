#include "modernime/settings/application_diagnostics.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <cassert>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace modernime::settings;

namespace {
GtkWidget *find(GtkWidget *root, std::string_view target) {
    const auto *id = static_cast<const char *>(g_object_get_data(
        G_OBJECT(root), "modernime-settings-target"));
    if (id && target == id) return root;
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next)
        result = find(GTK_WIDGET(item->data), target);
    g_list_free(children);
    return result;
}

template <class Predicate> void wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(predicate());
}

std::string text(GtkWidget *root, std::string_view target) {
    auto *label = find(root, target);
    assert(GTK_IS_LABEL(label));
    return gtk_label_get_text(GTK_LABEL(label));
}

struct FakeBackend {
    bool observed = false;
    bool fail = false;
    bool hold = false;
    GDBusMethodInvocation *pending = nullptr;
    std::string lastApplication;

    static void reply(GDBusMethodInvocation *call, const std::string &json) {
        g_dbus_method_invocation_return_value(call, g_variant_new("(s)", json.c_str()));
    }

    static std::string report(std::string_view application) {
        return "{\"ok\":true,\"observed\":true,\"application\":\"" + std::string(application) +
            "\",\"frontend\":\"dbus\",\"display\":\"wayland:synthetic\",\"transport\":\"wayland\","
            "\"toolkit\":\"unknown\",\"sandbox\":\"unknown\",\"focused\":true,\"sensitive\":false,"
            "\"surroundingText\":true,\"clientPreedit\":true,\"cursorRectangle\":true,"
            "\"modernimeSelected\":true,\"observedAtMs\":42,\"handledKeys\":7,"
            "\"maximumDispatchUs\":125,\"suggestions\":[]}";
    }

    void release() {
        auto *call = std::exchange(pending, nullptr);
        assert(call);
        reply(call, report("org.example.Editor"));
        g_object_unref(call);
    }

    static void method(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                       const gchar *, GVariant *parameters,
                       GDBusMethodInvocation *call, gpointer data) {
        auto &self = *static_cast<FakeBackend *>(data);
        const gchar *action = nullptr;
        gchar **args = nullptr;
        g_variant_get(parameters, "(&s^as)", &action, &args);
        const std::string application = args[0] ? args[0] : "";
        g_strfreev(args);
        if (std::string_view(action) == "capabilities") {
            reply(call, "{\"ok\":true,\"version\":1,\"actions\":[\"applications\",\"diagnostics\"],\"selectionComponents\":[]}");
        } else if (std::string_view(action) == "applications") {
            reply(call, self.fail ? "{\"ok\":false,\"error\":\"diagnostics_unavailable\",\"message\":\"服务暂不可用\"}"
                : "{\"ok\":true,\"applications\":[\"org.example.Editor\",\"org.example.Terminal\"]}");
        } else {
            assert(std::string_view(action) == "diagnostics");
            self.lastApplication = application;
            if (self.hold && application == "org.example.Editor") {
                assert(!self.pending);
                self.pending = G_DBUS_METHOD_INVOCATION(g_object_ref(call));
            } else if (!self.observed) {
                reply(call, "{\"ok\":true,\"observed\":false,\"application\":\"\",\"suggestions\":[\"请在目标应用实际试输入\"]}");
            } else reply(call, report(application.empty() ? "org.example.Editor" : application));
        }
    }
};
} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    installSettingsStyles();
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
    GError *error = nullptr;
    auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    assert(bus && !error);
    auto *ownerReply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.modernime.Backend1"),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    gboolean occupied = TRUE;
    assert(ownerReply && !error);
    g_variant_get(ownerReply, "(b)", &occupied);
    g_variant_unref(ownerReply);
    assert(!occupied); // Only run on an isolated test bus.
    auto *info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.modernime.Backend1'><method name='Execute'>"
        "<arg type='s' direction='in'/><arg type='as' direction='in'/>"
        "<arg type='s' direction='out'/></method></interface></node>", &error);
    assert(info && !error);
    FakeBackend fake;
    static const GDBusInterfaceVTable vtable{FakeBackend::method, nullptr, nullptr, {nullptr}};
    const auto registration = g_dbus_connection_register_object(bus, "/org/modernime/Backend1",
        info->interfaces[0], &vtable, &fake, nullptr, &error);
    assert(registration && !error);
    bool owned = false;
    const auto service = g_bus_own_name_on_connection(bus, "org.modernime.Backend1", G_BUS_NAME_OWNER_FLAGS_NONE,
        +[](GDBusConnection *, const gchar *, gpointer data) { *static_cast<bool *>(data) = true; },
        nullptr, &owned, nullptr);
    wait([&] { return owned; });

    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto component = std::make_unique<ApplicationDiagnostics>();
    gtk_container_add(GTK_CONTAINER(window), component->widget());
    gtk_widget_show_all(window);
    auto *refresh = find(window, "application-refresh");
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    assert(text(window, "application-summary").find("还没有检测") != std::string::npos);
    assert(text(window, "application-transport") == "未知");

    fake.observed = true;
    setSettingsChoiceActive(find(window, "application-selector"), 1);
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    assert(fake.lastApplication == "org.example.Editor");
    assert(text(window, "application-summary") == "org.example.Editor");
    assert(text(window, "application-transport") == "Wayland");
    const auto details = text(window, "application-technical-details");
    assert(details.find("工具包：未知") != std::string::npos);
    assert(details.find("沙盒：未知") != std::string::npos);
    assert(details.find("42 ms") != std::string::npos && details.find("单调时钟") != std::string::npos);
    assert(details.find("125 μs") != std::string::npos && details.find("不含") != std::string::npos);

    fake.fail = true;
    component->refresh();
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    assert(text(window, "application-status").find("服务暂不可用") != std::string::npos);
    fake.fail = false;
    gtk_button_clicked(GTK_BUTTON(refresh));
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    assert(text(window, "application-summary") == "org.example.Editor");

    fake.hold = true;
    setSettingsChoiceActive(find(window, "application-selector"), 0);
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    setSettingsChoiceActive(find(window, "application-selector"), 1);
    wait([&] { return fake.pending != nullptr; });
    setSettingsChoiceActive(find(window, "application-selector"), 2);
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    assert(text(window, "application-summary") == "org.example.Terminal");
    fake.release();
    bool drained = false;
    g_timeout_add(50, +[](gpointer data) -> gboolean { *static_cast<bool *>(data) = true; return G_SOURCE_REMOVE; }, &drained);
    wait([&] { return drained; });
    assert(text(window, "application-summary") == "org.example.Terminal");

    setSettingsChoiceActive(find(window, "application-selector"), 1);
    wait([&] { return fake.pending != nullptr; });
    gtk_widget_destroy(window);
    component->refresh();
    assert(!component->focusTarget("application-selector"));
    fake.release();
    component.reset();

    // Destructor first: surviving widgets and their signal closures are also inert.
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    component = std::make_unique<ApplicationDiagnostics>();
    gtk_container_add(GTK_CONTAINER(window), component->widget());
    gtk_widget_show_all(window);
    refresh = find(window, "application-refresh");
    wait([&] { return gtk_widget_get_sensitive(refresh); });
    setSettingsChoiceActive(find(window, "application-selector"), 1);
    wait([&] { return fake.pending != nullptr; });
    component.reset();
    gtk_button_clicked(GTK_BUTTON(refresh));
    fake.release();
    drained = false;
    g_timeout_add(50, +[](gpointer data) -> gboolean { *static_cast<bool *>(data) = true; return G_SOURCE_REMOVE; }, &drained);
    wait([&] { return drained; });
    gtk_widget_destroy(window);

    g_bus_unown_name(service);
    g_dbus_connection_unregister_object(bus, registration);
    g_dbus_node_info_unref(info);
    g_object_unref(bus);
}
