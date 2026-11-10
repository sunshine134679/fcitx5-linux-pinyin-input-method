#include "modernime/settings/pages/input_page.h"
#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_shell_state.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *widget, std::string_view name) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
    if (id && name == id) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), name);
    g_list_free(children);
    return result;
}
void drain() { while (g_main_context_iteration(nullptr, FALSE)) {} }
template<class Predicate> void wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        drain(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(predicate());
}
void capture(GtkWidget *window, const std::filesystem::path &path) {
    bool drawn = false;
    const auto handler = g_signal_connect_after(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer p) -> gboolean {
        *static_cast<bool *>(p) = true; return FALSE;
    }), &drawn);
    gtk_widget_queue_draw(window); wait([&] { return drawn; });
    g_signal_handler_disconnect(window, handler);
    auto *pixbuf = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0,
        gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
    assert(pixbuf && gdk_pixbuf_save(pixbuf, path.c_str(), "png", nullptr, nullptr));
    g_object_unref(pixbuf);
}
void click(GtkWidget *root, const char *name) {
    auto *control = find(root, name);
    assert(control && gtk_widget_is_sensitive(control));
    gtk_button_clicked(GTK_BUTTON(control));
    drain();
}
GtkWidget *dialog() {
    auto *windows = gtk_window_list_toplevels();
    GtkWidget *result = nullptr;
    for (auto *item = windows; item; item = item->next)
        if (GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data)) &&
            (!result || gtk_window_get_transient_for(GTK_WINDOW(item->data)) == GTK_WINDOW(result)))
            result = GTK_WIDGET(item->data);
    g_list_free(windows);
    assert(result);
    return result;
}
void rule(GtkWidget *manager, const char *key, const char *value, bool application) {
    click(manager, "input-rule-add");
    auto *editor = dialog();
    gtk_entry_set_text(GTK_ENTRY(find(editor, "input-rule-key")), key);
    if (application) setSettingsChoiceActive(find(editor, "input-rule-value"), std::string_view(value) == "english" ? 1 : 0);
    else gtk_entry_set_text(GTK_ENTRY(find(editor, "input-rule-value")), value);
    assert(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT)));
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT);
    drain();
}
void selectFirst(GtkWidget *manager) {
    auto *view = find(manager, "input-rule-list");
    auto *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path);
    gtk_tree_path_free(path);
}
}

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    std::string pattern = "/tmp/modernime-input-editor-XXXXXX";
    assert(mkdtemp(pattern.data()));
    const std::filesystem::path root(pattern);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    const auto file = root / "settings.conf";
    auto initial = modernime::core::defaultSettings();
    initial.punctuationOverrides['/'] = "/";
    initial.appInputModes["org.example.Editor"] = modernime::core::InputMode::Chinese;
    assert(modernime::core::SettingsStore::save(file, initial));
    SettingsWindowModel model(file);
    installSettingsStyles();
    int changes = 0;
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "modernime-settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 880, 720);
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(window), scroll);
    InputPage page(model, [&] { ++changes; });
    gtk_container_add(GTK_CONTAINER(scroll), page.widget());
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
    drain();
    for (const auto *name : {"punctuation.auto_pair", "punctuation.smart", "input.undo_commit"}) {
        auto *control = find(page.widget(), name);
        assert(control && gtk_switch_get_active(GTK_SWITCH(control)));
        gtk_switch_set_active(GTK_SWITCH(control), false);
    }
    gtk_switch_set_active(GTK_SWITCH(find(page.widget(), "input.remember_per_app")), true);
    setSettingsChoiceActive(find(page.widget(), "punctuation.underscore"), 1);
    assert(!model.settings().autoPairPunctuation && !model.settings().smartPunctuation);
    assert(model.settings().rememberModePerApp && !model.settings().undoCommitEnabled);
    assert(model.settings().underscoreStyle == modernime::core::UnderscoreStyle::Ascii);
    assert(changes > 0 && model.dirty());
    model.resetEdits(); page.refresh();
    assert(gtk_switch_get_active(GTK_SWITCH(find(page.widget(), "punctuation.auto_pair"))));
    assert(!gtk_switch_get_active(GTK_SWITCH(find(page.widget(), "input.remember_per_app"))));

    click(page.widget(), "input.app");
    auto *manager = dialog();
    rule(manager, "code", "english", true);
    gtk_dialog_response(GTK_DIALOG(manager), GTK_RESPONSE_CANCEL); drain();
    assert(!model.settings().appInputModes.contains("code") && !model.dirty());
    click(page.widget(), "input.app"); manager = dialog();
    rule(manager, "code", "english", true);
    auto *search = find(manager, "input-rule-search");
    gtk_entry_set_text(GTK_ENTRY(search), "CODE");
    g_signal_emit_by_name(search, "search-changed"); drain();
    auto *view = find(manager, "input-rule-list");
    assert(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(view)), nullptr) == 1);
    selectFirst(manager); click(manager, "input-rule-edit");
    auto *editor = dialog();
    setSettingsChoiceActive(find(editor, "input-rule-value"), 0);
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT); drain();
    gtk_dialog_response(GTK_DIALOG(manager), GTK_RESPONSE_ACCEPT); drain();
    assert(model.settings().appInputModes.at("code") == modernime::core::InputMode::Chinese);
    assert(model.settings().appInputModes.contains("org.example.Editor"));

    click(page.widget(), "punctuation.map"); manager = dialog();
    click(manager, "input-rule-add"); editor = dialog();
    gtk_entry_set_text(GTK_ENTRY(find(editor, "input-rule-key")), "_");
    gtk_entry_set_text(GTK_ENTRY(find(editor, "input-rule-value")), " bad ");
    assert(!gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT)));
    gtk_entry_set_text(GTK_ENTRY(find(editor, "input-rule-value")), "_");
    assert(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT)));
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT); drain();
    auto *mappingSearch = find(manager, "input-rule-search");
    gtk_entry_set_text(GTK_ENTRY(mappingSearch), "/");
    g_signal_emit_by_name(mappingSearch, "search-changed"); drain();
    selectFirst(manager); click(manager, "input-rule-delete");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT); drain();
    gtk_dialog_response(GTK_DIALOG(manager), GTK_RESPONSE_ACCEPT); drain();
    assert(!model.settings().punctuationOverrides.contains('/'));
    assert(model.settings().punctuationOverrides.at('_') == "_");
    assert(modernime::core::SettingsStore::load(file).settings == initial);
    for (const auto *name : {"punctuation.auto_pair", "punctuation.smart", "input.undo_commit"})
        gtk_switch_set_active(GTK_SWITCH(find(page.widget(), name)), false);
    gtk_switch_set_active(GTK_SWITCH(find(page.widget(), "input.remember_per_app")), true);
    setSettingsChoiceActive(find(page.widget(), "punctuation.underscore"), 2);
    assert(model.save());
    const auto saved = modernime::core::SettingsStore::load(file);
    assert(saved.settings == model.settings());
    assert(!saved.settings.punctuationOverrides.contains('/'));
    assert(!saved.settings.autoPairPunctuation && !saved.settings.smartPunctuation && !saved.settings.undoCommitEnabled);
    assert(saved.settings.rememberModePerApp && saved.settings.underscoreStyle == modernime::core::UnderscoreStyle::Chinese);

    gtk_switch_set_active(GTK_SWITCH(find(page.widget(), "punctuation")), false);
    assert(!gtk_widget_is_sensitive(find(page.widget(), "punctuation.auto_pair")));
    assert(!gtk_widget_is_sensitive(find(page.widget(), "punctuation.map")));
    assert(model.settings().autoPairPunctuation == saved.settings.autoPairPunctuation);
    model.resetEdits(); page.refresh();
    gtk_window_present(GTK_WINDOW(window));
    wait([&] { return gtk_window_is_active(GTK_WINDOW(window)); });
    for (const auto *name : {"punctuation.auto_pair", "punctuation.smart", "punctuation.underscore", "punctuation.map", "input.app", "input.remember_per_app", "input.undo_commit"}) {
        bool found = false;
        for (const auto &entry : settingsSearchEntries()) if (entry.target == name) found = true;
        assert(found);
        assert(page.focusTarget(name));
    }
    assert(settingsFocusRouteForIssue("input.app.code")->target == "input.app");
    assert(settingsFocusRouteForIssue("punctuation.map.95")->target == "punctuation.map");
    if (const auto *captures = std::getenv("MODERNIME_INPUT_SCREENSHOT_DIR")) {
        std::filesystem::create_directories(captures);
        const auto directory = std::filesystem::path(captures);
        auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        gtk_window_set_focus(GTK_WINDOW(window), nullptr);
        capture(window, directory / "折叠布局.png");
        for (const auto &theme : modernime::core::kClientThemes) {
            applySettingsTheme(theme.key);
            drain();
            gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
            capture(window, directory / (std::string(theme.key) + "-标点.png"));
        }
        applySettingsTheme("blue"); gtk_adjustment_set_value(adjustment, 0);
        capture(window, directory / "应用模式.png");
        click(page.widget(), "input.app"); capture(dialog(), directory / "应用规则.png");
        click(dialog(), "input-rule-add"); capture(dialog(), directory / "添加规则.png");
        gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL); drain();
        gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL); drain();
    }
    // Parent destruction also closes managers and their nested edit dialogs.
    click(page.widget(), "input.app"); click(dialog(), "input-rule-add");
    gtk_widget_destroy(window); drain();
    std::filesystem::remove_all(root);
}
