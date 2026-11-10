#include "modernime/settings/symbol_browser.h"
#include "modernime/settings/settings_model.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/core/symbol_catalog.h"

#include <gtk/gtk.h>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *root, std::string_view name) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(root), "modernime-settings-target"));
    if (id && name == id) return root;
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), name);
    g_list_free(children);
    return result;
}
void drain() { while (g_main_context_iteration(nullptr, FALSE)) {} }
int rows(GtkWidget *dialog) {
    return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(find(dialog, "symbol-results"))), nullptr);
}
void query(GtkWidget *dialog, const char *text) {
    auto *entry = find(dialog, "symbol-query");
    gtk_entry_set_text(GTK_ENTRY(entry), text);
    drain();
}
void select(GtkWidget *dialog, std::string_view id) {
    auto *view = GTK_TREE_VIEW(find(dialog, "symbol-results"));
    auto *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    bool found = false;
    for (auto valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *value = nullptr;
        gtk_tree_model_get(model, &iter, 0, &value, -1);
        found = id == value;
        g_free(value);
        if (found) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(view), &iter);
            break;
        }
    }
    assert(found);
    drain();
}
void favorite(GtkWidget *dialog) {
    auto *button = find(dialog, "symbol-favorite");
    assert(button && gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button));
    drain();
}
void close(GtkWidget *dialog, int response) {
    gtk_dialog_response(GTK_DIALOG(dialog), response);
    drain();
}
}

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    std::string pattern = "/tmp/modernime-symbol-browser-XXXXXX";
    assert(mkdtemp(pattern.data()));
    const std::filesystem::path root(pattern);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
    auto initial = modernime::core::defaultSettings();
    initial.symbolSearchEnabled = false;
    initial.symbolFavorites = {"→"};
    const auto file = root / "settings.conf";
    assert(modernime::core::SettingsStore::save(file, initial));
    SettingsWindowModel model(file);
    installSettingsStyles();
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_show(window);
    int changes = 0;
    const auto open = [&] { return openSymbolBrowser(GTK_WINDOW(window), model, [&] { ++changes; }); };
    auto *dialog = open();
    drain();

    // Browsing stays available when the engine's symbol switch is off.
    assert(rows(dialog) == 170);
    assert(!model.settings().symbolSearchEnabled && !model.dirty());
    auto *category = find(dialog, "symbol-category");
    const char *keys[] = {"", "punctuation", "math", "units", "arrows", "emoji", "other"};
    for (int i = 0; i < 7; ++i) {
        setSettingsChoiceActive(category, i);
        drain();
        assert(rows(dialog) == static_cast<int>(modernime::core::SymbolCatalog::search("", keys[i], {}, false, 200).size()));
    }
    setSettingsChoiceActive(category, 0);
    for (const auto *text : {"箭头", "JIANTOU", "ARROW", "→"}) {
        query(dialog, text);
        assert(rows(dialog) > 0);
        select(dialog, "→");
        assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(dialog, "symbol-preview")))) == "→");
        // Check copy availability without touching the user's clipboard.
        assert(gtk_widget_is_sensitive(find(dialog, "symbol-copy")));
    }
    query(dialog, "no_such_symbol_12345");
    assert(rows(dialog) == 0);
    assert(!gtk_widget_is_sensitive(find(dialog, "symbol-copy")));
    assert(!gtk_widget_is_sensitive(find(dialog, "symbol-favorite")));

    query(dialog, "");
    auto *only = find(dialog, "symbol-favorites-only");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(only), TRUE);
    assert(rows(dialog) == 1);
    select(dialog, "→");
    favorite(dialog);
    assert(rows(dialog) == 0);
    assert(model.settings().symbolFavorites == initial.symbolFavorites && changes == 0);
    close(dialog, GTK_RESPONSE_CANCEL);
    assert(!model.dirty() && changes == 0);

    // Preserve exact paired text and variation selectors in a private draft.
    dialog = open();
    for (const auto *id : {"《》", "❤️", "❗️"}) {
        query(dialog, id);
        select(dialog, id);
        favorite(dialog);
    }
    assert(model.settings().symbolFavorites == initial.symbolFavorites);
    // Acceptance merges only the favorite field into the latest model draft.
    auto edited = model.settings();
    edited.calculatorEnabled = false;
    model.setSettings(edited);
    close(dialog, GTK_RESPONSE_ACCEPT);
    const std::set<std::string> expected = {"→", "《》", "❤️", "❗️"};
    assert(model.settings().symbolFavorites == expected && changes == 1);
    assert(!model.settings().calculatorEnabled && !model.settings().symbolSearchEnabled);
    assert(modernime::core::SettingsStore::load(file).settings == initial);
    model.resetEdits();
    assert(model.settings() == initial && !model.dirty());

    dialog = open();
    query(dialog, "❤️");
    select(dialog, "❤️");
    favorite(dialog);
    close(dialog, GTK_RESPONSE_ACCEPT);
    assert(model.dirty() && changes == 2);
    assert(model.save());
    const auto saved = modernime::core::SettingsStore::load(file);
    assert(saved.settings.symbolFavorites == std::set<std::string>({"→", "❤️"}));
    assert(!saved.settings.symbolFavorites.contains("❤"));
    dialog = open();
    query(dialog, "❤️");
    select(dialog, "❤️");
    favorite(dialog);
    close(dialog, GTK_RESPONSE_ACCEPT);
    assert(model.save());
    assert(modernime::core::SettingsStore::load(file).settings.symbolFavorites == initial.symbolFavorites);

    // Destroying the parent drops private edits without notifying the model.
    dialog = open();
    select(dialog, "→");
    favorite(dialog);
    const auto before = changes;
    gtk_widget_destroy(window);
    drain();
    assert(changes == before && !model.dirty());
    std::filesystem::remove_all(root);
}
