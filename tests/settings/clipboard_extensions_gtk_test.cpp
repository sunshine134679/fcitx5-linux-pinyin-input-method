#include "modernime/core/clipboard_history.h"
#include "modernime/core/clipboard_queries.h"
#include "modernime/settings/pages/clipboard_page.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *root, std::string_view target) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(root), "modernime-settings-target"));
    if (id && target == id) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        auto *menu = gtk_menu_button_get_popover(GTK_MENU_BUTTON(root));
        if (menu) if (auto *result = find(GTK_WIDGET(menu), target)) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = nullptr;
    for (auto *child = children; child && !result; child = child->next)
        result = find(GTK_WIDGET(child->data), target);
    g_list_free(children);
    return result;
}
void drain() { while (g_main_context_iteration(nullptr, FALSE)) {} }
template<class Predicate> void wait(Predicate predicate,
    std::source_location location = std::source_location::current()) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    do {
        drain();
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    if (!predicate()) {
        g_printerr("clipboard wait timed out at %s:%u (%s)\n",
                   location.file_name(), location.line(), location.function_name());
        auto *windows = gtk_window_list_toplevels();
        for (auto *item = windows; item; item = item->next) {
            auto *window = GTK_WINDOW(item->data);
            g_printerr("clipboard window type=%s active=%d visible=%d mapped=%d title=%s\n",
                       G_OBJECT_TYPE_NAME(window), gtk_window_is_active(window),
                       gtk_widget_get_visible(GTK_WIDGET(window)),
                       gtk_widget_get_mapped(GTK_WIDGET(window)),
                       gtk_window_get_title(window) ? gtk_window_get_title(window) : "");
            auto *list = find(GTK_WIDGET(window), "clipboard-history");
            if (list) {
                auto *children = gtk_container_get_children(GTK_CONTAINER(list));
                g_printerr("clipboard list rows=%u\n", g_list_length(children));
                g_list_free(children);
            }
            for (const auto *target : {"clipboard-search", "clipboard-favorite", "clipboard-delete", "clipboard-clear-history", "clipboard-clear-favorites"}) {
                auto *control = find(GTK_WIDGET(window), target);
                if (control) g_printerr("clipboard control=%s sensitive=%d visible=%d\n",
                                       target, gtk_widget_is_sensitive(control), gtk_widget_get_visible(control));
            }
        }
        g_list_free(windows);
    }
    assert(predicate());
}
int rows(GtkWidget *list) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(list));
    const auto count = g_list_length(children);
    g_list_free(children);
    return static_cast<int>(count);
}
void click(GtkWidget *button) {
    assert(button && gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button));
    drain();
}
void confirm(GtkWidget *button, GtkWidget *requery = nullptr) {
    struct Confirmation {
        GtkWidget *parent;
        GtkWidget *requery;
        bool responded = false;
    } context{gtk_widget_get_toplevel(button), requery};
    const auto source = g_timeout_add(5, +[](gpointer data) -> gboolean {
        auto *confirmation = static_cast<Confirmation *>(data);
        auto *windows = gtk_window_list_toplevels();
        for (auto *item = windows; item && !confirmation->responded; item = item->next) {
            if (GTK_IS_MESSAGE_DIALOG(item->data) &&
                gtk_widget_get_visible(GTK_WIDGET(item->data)) &&
                gtk_window_get_transient_for(GTK_WINDOW(item->data)) == GTK_WINDOW(confirmation->parent)) {
                // GTK may deliver a delayed same-value search while a modal
                // confirmation runs its nested main loop. A YES must still write.
                if (confirmation->requery) g_signal_emit_by_name(confirmation->requery, "search-changed");
                gtk_dialog_response(GTK_DIALOG(item->data), GTK_RESPONSE_YES);
                confirmation->responded = true;
            }
        }
        g_list_free(windows);
        return confirmation->responded ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    }, &context);
    click(button);
    if (!context.responded) g_source_remove(source);
    assert(context.responded);
}
void search(GtkWidget *entry, const char *query) {
    gtk_entry_set_text(GTK_ENTRY(entry), query);
    g_signal_emit_by_name(entry, "search-changed");
    drain();
}
std::string bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
}

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    std::string pattern = "/tmp/modernime-clipboard-extensions-XXXXXX";
    assert(mkdtemp(pattern.data()));
    const std::filesystem::path root(pattern);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
    const auto historyPath = root / "clipboard-history.bin";
    const auto favoritesPath = root / "clipboard-favorites.bin";
    modernime::core::ClipboardHistory history;
    assert(history.observe("synthetic keep oldest"));
    assert(history.observe("中文 Needle synthetic match"));
    assert(history.observe("synthetic keep newest"));
    assert(history.save(historyPath));
    modernime::core::ClipboardFavorites favorites(favoritesPath);
    assert(favorites.add("synthetic favorite only"));
    SettingsWindowModel model(root / "settings.conf");
    installSettingsStyles();
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 960, 780);
    auto page = std::make_unique<ClipboardPage>(model, historyPath, [] {}, [](std::string) {}, favoritesPath);
    gtk_container_add(GTK_CONTAINER(window), page->widget());
    gtk_widget_show_all(window);
    guint32 activationTime = GDK_CURRENT_TIME;
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gtk_widget_get_display(window)))
        activationTime = gdk_x11_get_server_time(gtk_widget_get_window(window));
#endif
    gtk_window_present_with_time(GTK_WINDOW(window), activationTime);
    wait([&] { return gtk_window_is_active(GTK_WINDOW(window)); });
    page->refresh(false);
    auto *list = find(window, "clipboard-history");
    auto *entry = find(window, "clipboard-search");
    auto *favorite = find(window, "clipboard-favorite");
    auto *remove = find(window, "clipboard-delete");
    auto *clearHistory = find(window, "clipboard-clear-history");
    auto *clearFavorites = find(window, "clipboard-clear-favorites");
    auto *favoritesOnly = find(window, "clipboard-favorites");
    wait([&] { return rows(list) == 4 && gtk_widget_is_sensitive(favorite); });
    for (const auto *target : {"clipboard-search", "clipboard-favorites", "clipboard-favorite", "clipboard-clear-favorites"}) {
        const bool focused = page->focusTarget(target);
        if (!focused) {
            auto *control = find(window, target);
            auto *actual = gtk_window_get_focus(GTK_WINDOW(window));
            g_printerr("clipboard focus target=%s active=%d visible=%d mapped=%d sensitive=%d can-focus=%d has-focus=%d actual=%s\n",
                       target, gtk_window_is_active(GTK_WINDOW(window)),
                       gtk_widget_get_visible(control), gtk_widget_get_mapped(control),
                       gtk_widget_is_sensitive(control), gtk_widget_get_can_focus(control),
                       gtk_widget_has_focus(control), actual ? G_OBJECT_TYPE_NAME(actual) : "none");
        }
        assert(focused);
    }

    // Latest query wins; a filtered display index must retain the original identity.
    search(entry, "no synthetic match exists");
    search(entry, "nEeDlE");
    wait([&] { return rows(list) == 1 && gtk_widget_is_sensitive(favorite); });
    assert(std::string_view(gtk_button_get_label(GTK_BUTTON(favorite))) == "收藏");
    click(favorite);
    wait([&] { return gtk_widget_is_sensitive(favorite) && std::string_view(gtk_button_get_label(GTK_BUTTON(favorite))) == "取消收藏"; });
    assert(favorites.load() && favorites.entries().size() == 2);
    confirm(remove, entry);
    wait([&] { return rows(list) == 1 && gtk_widget_is_sensitive(favorite) && !gtk_widget_is_sensitive(remove); });
    assert(history.load(historyPath) && history.entries().size() == 2);
    assert(history.entries()[0] == "synthetic keep newest" && history.entries()[1] == "synthetic keep oldest");
    assert(favorites.load() && favorites.entries().size() == 2);

    // Clearing history uses the complete history, even while the view is filtered.
    confirm(clearHistory, entry);
    wait([&] { return gtk_widget_is_sensitive(favorite) && !gtk_widget_is_sensitive(clearHistory); });
    assert(history.load(historyPath) && history.entries().empty());
    assert(favorites.load() && favorites.entries().size() == 2);
    click(favorite);
    wait([&] { return rows(list) == 0 && gtk_widget_is_sensitive(clearFavorites); });
    assert(favorites.load() && favorites.entries().size() == 1);
    search(entry, "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(favoritesOnly), TRUE);
    wait([&] { return rows(list) == 1 && gtk_widget_is_sensitive(clearFavorites); });
    assert(history.observe("synthetic history survives favorite clear") && history.save(historyPath));
    page->refresh(false);
    wait([&] { return rows(list) == 1 && gtk_widget_is_sensitive(clearHistory) && gtk_widget_is_sensitive(clearFavorites); });
    confirm(clearFavorites, entry);
    wait([&] { return rows(list) == 0 && !gtk_widget_is_sensitive(clearFavorites) && gtk_widget_is_sensitive(clearHistory); });
    assert(favorites.load() && favorites.entries().empty());
    assert(history.load(historyPath) && history.entries() == std::vector<std::string>{"synthetic history survives favorite clear"});
    assert(!model.dirty()); // History and favorite actions do not enter SettingsStore edits.

    // Failed reads retain old snapshots and disable writes to that file.
    history.clear();
    assert(history.observe("synthetic retained history") && history.save(historyPath));
    assert(favorites.add("synthetic retained favorite"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(favoritesOnly), FALSE);
    page->refresh(false);
    wait([&] { return rows(list) == 2 && gtk_widget_is_sensitive(clearHistory) && gtk_widget_is_sensitive(clearFavorites); });
    { std::ofstream output(favoritesPath, std::ios::binary | std::ios::trunc); output << "synthetic corrupt favorites"; }
    page->refresh(false);
    wait([&] { return rows(list) == 2 && gtk_widget_is_sensitive(clearHistory) && !gtk_widget_is_sensitive(clearFavorites) && !gtk_widget_is_sensitive(favorite); });
    assert(bytes(favoritesPath) == "synthetic corrupt favorites");
    assert(gtk_widget_get_visible(find(window, "clipboard-error-details")));
    { std::ofstream output(historyPath, std::ios::binary | std::ios::trunc); output << "synthetic corrupt history"; }
    page->refresh(false);
    wait([&] { return rows(list) == 2 && !gtk_widget_is_sensitive(clearHistory) && !gtk_widget_is_sensitive(clearFavorites) && gtk_widget_is_sensitive(find(window, "clipboard-copy")); });
    assert(bytes(historyPath) == "synthetic corrupt history");
    assert(bytes(favoritesPath) == "synthetic corrupt favorites");

    // Completing an in-flight read after owner destruction must not touch GTK.
    page->refresh(false);
    gtk_widget_destroy(window);
    page.reset();
    drain();
    std::filesystem::remove_all(root);
}
