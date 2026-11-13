#include "modernime/settings/backend_client.h"
#include "modernime/settings/dictionary_pack_manager.h"

#include <gtk/gtk.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace modernime::settings;
namespace {

template <class Predicate> void wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(predicate());
}

GtkWidget *find(GtkWidget *widget, const char *target) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
    if (id && std::string_view(id) == target) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *child = children; child && !result; child = child->next)
        result = find(GTK_WIDGET(child->data), target);
    g_list_free(children);
    return result;
}

void click(GtkWidget *window, const char *target) {
    auto *control = find(window, target);
    assert(control && gtk_widget_is_sensitive(control));
    gtk_button_clicked(GTK_BUTTON(control));
}

GtkWidget *dialog() {
    auto *windows = gtk_window_list_toplevels();
    GtkWidget *result = nullptr;
    for (auto *item = windows; item; item = item->next)
        if (GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data)))
            result = GTK_WIDGET(item->data);
    g_list_free(windows);
    return result;
}

int rows(GtkWidget *window) {
    return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(find(window, "pack-list"))), nullptr);
}

void select(GtkWidget *window, int row) {
    auto *path = gtk_tree_path_new_from_indices(row, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(find(window, "pack-list"))), path);
    gtk_tree_path_free(path);
}

struct Service final {
    bool enabled = true, present = true, imported = false;
    bool failList = false, deferList = false;
    GDBusMethodInvocation *pending = nullptr;
    std::vector<std::string> calls;
    std::vector<std::string> lastArgs;

    unsigned count(std::string_view action) const {
        return static_cast<unsigned>(std::count(calls.begin(), calls.end(), action));
    }
    std::string packs() const {
        auto json = std::string(R"({"ok":true,"packs":[{"id":"built-in","name":"内置示例","source":"合成夹具","license":"CC0","version":"1","entryCount":3,"enabled":false,"bundled":true})");
        if (present) json += std::string(R"(,{"id":"user-pack","name":"用户示例","source":"合成夹具","license":"CC0","version":"2","entryCount":8,"enabled":)") +
            (enabled ? "true" : "false") + R"(,"bundled":false})";
        if (imported) json += R"(,{"id":"import-pack","name":"待导入示例","source":"合成目录","license":"CC0","version":"3","entryCount":5,"enabled":true,"bundled":false})";
        return json + "]}";
    }
    static void reply(GDBusMethodInvocation *invocation, const std::string &json) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", json.c_str()));
    }
    void releasePending() {
        assert(pending);
        reply(pending, packs());
        g_object_unref(std::exchange(pending, nullptr));
    }
    static void execute(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                        const gchar *, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
        auto &service = *static_cast<Service *>(data);
        const char *rawAction = nullptr;
        gchar **rawArgs = nullptr;
        g_variant_get(parameters, "(&s^as)", &rawAction, &rawArgs);
        const std::string action(rawAction);
        service.calls.push_back(action);
        service.lastArgs.clear();
        for (auto **arg = rawArgs; arg && *arg; ++arg) service.lastArgs.emplace_back(*arg);
        g_strfreev(rawArgs);
        if (action == "capabilities") {
            reply(invocation, R"({"ok":true,"version":1,"actions":["pack.list","pack.inspect","pack.import","pack.update","pack.enable","pack.disable","pack.remove"]})");
        } else if (action == "pack.list") {
            if (std::exchange(service.deferList, false)) {
                assert(!service.pending);
                service.pending = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
            } else if (std::exchange(service.failList, false)) {
                reply(invocation, R"({"ok":false,"error":"operation_failed","message":"测试：词库包读取失败"})");
            } else reply(invocation, service.packs());
        } else if (action == "pack.inspect") {
            assert(service.lastArgs.size() == 1 && std::filesystem::path(service.lastArgs[0]).is_absolute());
            reply(invocation, R"({"ok":true,"pack":{"id":"import-pack","name":"待导入示例","source":"合成目录","license":"CC0","version":"3","entryCount":5,"enabled":true,"bundled":false}})");
        } else {
            if (action == "pack.disable") service.enabled = false;
            else if (action == "pack.enable") service.enabled = true;
            else if (action == "pack.remove") service.present = false;
            else if (action == "pack.import" || action == "pack.update") service.imported = true;
            else assert(false);
            reply(invocation, R"({"ok":true})");
        }
    }
};

GtkWidget *open(DictionaryPackManager &manager) {
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 680, 480);
    gtk_container_add(GTK_CONTAINER(window), manager.widget());
    gtk_widget_show_all(window);
    manager.refresh();
    return window;
}

void ready(GtkWidget *window) {
    wait([&] { return gtk_widget_is_sensitive(find(window, "pack-import")); });
}

void chooseDirectory(GtkWidget *window, const std::filesystem::path &directory) {
    click(window, "pack-import");
    auto *chooser = dialog();
    assert(chooser && GTK_IS_FILE_CHOOSER(chooser));
    assert(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), directory.c_str()));
    wait([&] {
        auto *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        const bool selected = name && std::filesystem::path(name) == directory;
        g_free(name);
        return selected;
    });
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
    wait([&] { return dialog() && !GTK_IS_FILE_CHOOSER(dialog()); });
}

} // namespace

int main(int argc, char **argv) {
    // Start before GTK/BackendClient create connections; no request reaches the user's session bus.
    const std::string display = g_getenv("DISPLAY") ? g_getenv("DISPLAY") : "";
    auto *testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(testBus);
    // GTestDBus clears DISPLAY to prevent automatic session-bus startup.
    if (!display.empty()) g_setenv("DISPLAY", display.c_str(), TRUE);
    if (!gtk_init_check(&argc, &argv)) {
        g_test_dbus_down(testBus);
        g_object_unref(testBus);
        return 77;
    }
    GError *error = nullptr;
    auto *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    assert(connection && !error);
    auto *claimed = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.modernime.Backend1", 4U),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    assert(claimed && !error);
    guint claim = 0;
    g_variant_get(claimed, "(u)", &claim);
    assert(claim == 1);
    g_variant_unref(claimed);
    auto *info = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.modernime.Backend1'><method name='Execute'>"
        "<arg type='s' direction='in'/><arg type='as' direction='in'/>"
        "<arg type='s' direction='out'/></method></interface></node>", &error);
    assert(info && !error);
    Service service;
    const GDBusInterfaceVTable table{Service::execute, nullptr, nullptr, {}};
    const auto registration = g_dbus_connection_register_object(connection, "/org/modernime/Backend1",
        info->interfaces[0], &table, &service, nullptr, &error);
    assert(registration && !error);

    std::vector<std::string> notices;
    auto manager = std::make_unique<DictionaryPackManager>([&](std::string text) { notices.push_back(std::move(text)); });
    auto *window = open(*manager);
    ready(window);
    assert(rows(window) == 2);
    gtk_window_present(GTK_WINDOW(window));
    wait([&] { return gtk_window_has_toplevel_focus(GTK_WINDOW(window)); });
    assert(manager->focusTarget("pack-list"));
    select(window, 0);
    assert(!gtk_widget_is_sensitive(find(window, "pack-remove")));
    select(window, 1);
    assert(std::string_view(gtk_button_get_label(GTK_BUTTON(find(window, "pack-toggle")))) == "停用");
    click(window, "pack-toggle");
    assert(dialog());
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL);
    assert(service.count("pack.disable") == 0);
    click(window, "pack-toggle");
    // Programmatic duplicate activation must respect the logical busy guard too.
    gtk_button_clicked(GTK_BUTTON(find(window, "pack-toggle")));
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    ready(window);
    assert(service.count("pack.disable") == 1 && !service.enabled);
    assert(service.lastArgs.empty()); // Final request is the state-refresh query.
    assert(notices.size() == 1 && notices.back().find("后台加载") != std::string::npos);
    assert(std::string_view(gtk_button_get_label(GTK_BUTTON(find(window, "pack-toggle")))) == "启用");
    click(window, "pack-toggle");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    ready(window);
    assert(service.count("pack.enable") == 1 && service.enabled);
    click(window, "pack-remove");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL);
    assert(service.count("pack.remove") == 0);
    click(window, "pack-remove");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    ready(window);
    assert(service.count("pack.remove") == 1 && rows(window) == 1);

    service.failList = true;
    click(window, "pack-refresh");
    wait([&] { return gtk_widget_is_sensitive(find(window, "pack-refresh")); });
    assert(!gtk_widget_is_sensitive(find(window, "pack-import")));
    assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(window, "pack-state")))) == "测试：词库包读取失败");
    click(window, "pack-refresh");
    ready(window);
    assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(window, "pack-state")))).empty());

    std::string pattern = "/tmp/modernime-pack-manager-XXXXXX";
    assert(mkdtemp(pattern.data()));
    const std::filesystem::path directory(pattern);
    std::ofstream(directory / "manifest.ini") << "id=import-pack\nname=待导入示例\n";
    std::ofstream(directory / "entries.tsv") << "agu\tA股\t0\n";
    chooseDirectory(window, directory);
    assert(std::string_view(gtk_window_get_title(GTK_WINDOW(dialog()))) == "导入词库包");
    assert(service.count("pack.inspect") == 1 && service.count("pack.import") == 0);
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL);
    chooseDirectory(window, directory);
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    ready(window);
    assert(service.count("pack.import") == 1 && rows(window) == 2);
    chooseDirectory(window, directory);
    assert(std::string_view(gtk_window_get_title(GTK_WINDOW(dialog()))) == "更新词库包");
    assert(service.count("pack.update") == 0);
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    ready(window);
    assert(service.count("pack.update") == 1);
    std::filesystem::remove_all(directory);

    // Widget destruction and owner destruction independently invalidate in-flight callbacks.
    service.deferList = true;
    manager->refresh();
    wait([&] { return service.pending != nullptr; });
    gtk_widget_destroy(window);
    service.releasePending();
    bool barrier = false;
    BackendClient client;
    client.execute("pack.list", {}, [&](BackendReply reply) { assert(reply.ok); barrier = true; });
    wait([&] { return barrier; });
    manager.reset();
    auto closing = std::make_unique<DictionaryPackManager>([](std::string) { assert(false); });
    service.deferList = true;
    auto *closingWindow = open(*closing);
    wait([&] { return service.pending != nullptr; });
    closing.reset();
    service.releasePending();
    gtk_widget_destroy(closingWindow);
    barrier = false;
    client.execute("pack.list", {}, [&](BackendReply reply) { assert(reply.ok); barrier = true; });
    wait([&] { return barrier; });

    g_dbus_connection_unregister_object(connection, registration);
    g_dbus_node_info_unref(info);
    g_dbus_connection_close_sync(connection, nullptr, nullptr);
    g_object_unref(connection);
    g_test_dbus_down(testBus);
    g_object_unref(testBus);
    std::cout << "[PASS] 词库包列表、启停/移除确认、导入更新预览与在途销毁\n";
}
