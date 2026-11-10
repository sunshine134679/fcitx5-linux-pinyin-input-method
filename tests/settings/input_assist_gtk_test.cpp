#include "modernime/settings/pages/input_page.h"
#include "modernime/settings/pages/candidates_page.h"
#include "modernime/settings/candidate_pin_dialog.h"
#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_widgets.h"
#include <gtk/gtk.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *root, std::string_view name) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(root), "modernime-settings-target"));
    if (id && name == id) return root;
    if (!GTK_IS_CONTAINER(root)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), name);
    g_list_free(children); return result;
}
void drain() { while (g_main_context_iteration(nullptr, FALSE)) {} }
template<class Predicate> void wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!predicate() && std::chrono::steady_clock::now() < end) {
        drain(); std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    assert(predicate());
}
GtkWidget *dialog() {
    auto *windows = gtk_window_list_toplevels(); GtkWidget *result = nullptr;
    for (auto *item = windows; item; item = item->next)
        if (GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data))) result = GTK_WIDGET(item->data);
    g_list_free(windows); assert(result); return result;
}
void click(GtkWidget *root, const char *id) {
    auto *control = find(root, id); assert(control && gtk_widget_is_sensitive(control));
    gtk_button_clicked(GTK_BUTTON(control)); drain();
}
void entry(GtkWidget *root, const char *id, const char *value) {
    gtk_entry_set_text(GTK_ENTRY(find(root, id)), value); drain();
}
int rows(GtkWidget *view) { return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(view)), nullptr); }
void select(GtkWidget *view, int index = 0) {
    auto *path = gtk_tree_path_new_from_indices(index, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path); gtk_tree_path_free(path); drain();
}
void capture(GtkWidget *window, const std::filesystem::path &path) {
    bool drawn = false;
    auto handler = g_signal_connect_after(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer p) -> gboolean { *static_cast<bool *>(p) = true; return FALSE; }), &drawn);
    gtk_widget_queue_draw(window); wait([&] { return drawn; }); g_signal_handler_disconnect(window, handler);
    auto *pixbuf = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0, gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
    assert(pixbuf && gdk_pixbuf_save(pixbuf, path.c_str(), "png", nullptr, nullptr)); g_object_unref(pixbuf);
}
}
int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    std::string pattern = "/tmp/modernime-input-assist-XXXXXX"; assert(mkdtemp(pattern.data()));
    const std::filesystem::path root(pattern);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1); setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, nullptr);
    auto initial = modernime::core::defaultSettings();
    initial.candidatePins = {{modernime::core::PinyinScheme::Full, "shi", "试", "shi"}, {modernime::core::PinyinScheme::Xiaohe, "ui", "市", "shi"}};
    const auto file = root / "settings.conf"; assert(modernime::core::SettingsStore::save(file, initial));
    SettingsWindowModel model(file); installSettingsStyles();
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(window), 930, 740);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "modernime-settings");
    auto *stack = gtk_stack_new(); gtk_container_add(GTK_CONTAINER(window), stack);
    InputPage input(model, [] {}); CandidatesPage candidates(model, [] {});
    gtk_stack_add_named(GTK_STACK(stack), createScrollablePage(input.widget()), "input");
    gtk_stack_add_named(GTK_STACK(stack), createScrollablePage(candidates.widget()), "candidates");
    gtk_widget_show_all(window); gtk_window_present(GTK_WINDOW(window)); drain();
    wait([&] { return gtk_window_is_active(GTK_WINDOW(window)); });
    const auto switchHeight = gtk_widget_get_allocated_height(find(window, "input.temporary_english"));
    for (const auto *name : {"input.component_lookup", "input.calculator", "symbols.enabled"})
        assert(gtk_widget_get_allocated_height(find(window, name)) == switchHeight);
    for (const auto *name : {"input.temporary_english", "input.component_lookup", "input.calculator", "symbols.enabled"}) {
        auto *control = find(window, name); assert(control && gtk_switch_get_active(GTK_SWITCH(control)));
        assert(input.focusTarget(name)); gtk_switch_set_active(GTK_SWITCH(control), false);
    }
    assert(model.dirty() && !model.settings().temporaryEnglishEnabled && !model.settings().componentLookupEnabled && !model.settings().calculatorEnabled && !model.settings().symbolSearchEnabled);
    assert(model.save());
    auto saved = modernime::core::SettingsStore::load(file); assert(saved.settings == model.settings());
    gtk_switch_set_active(GTK_SWITCH(find(window, "input.calculator")), true); model.resetEdits(); input.refresh();
    assert(!gtk_switch_get_active(GTK_SWITCH(find(window, "input.calculator"))));
    click(window, "calculator-preview"); auto *popup = dialog(); entry(popup, "assist-preview-input", "(2+3)*4");
    assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(popup, "assist-preview-results")))) == "20");
    assert(gtk_widget_is_sensitive(find(popup, "assist-preview-copy")));
    entry(popup, "assist-preview-input", "1/0");
    assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(popup, "assist-preview-results")))).empty());
    assert(!gtk_widget_is_sensitive(find(popup, "assist-preview-copy")));
    gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_CLOSE); drain();
    click(window, "component-preview"); popup = dialog(); entry(popup, "assist-preview-input", "mumu");
    auto *view = find(popup, "assist-preview-results"); wait([&] { return rows(view) > 0; });
    select(view); assert(gtk_widget_is_sensitive(find(popup, "assist-preview-copy")));
    if (const auto *captures = std::getenv("MODERNIME_ASSIST_SCREENSHOTS")) {
        std::filesystem::create_directories(captures); capture(popup, std::filesystem::path(captures) / "拆字试用.png");
    }
    gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_CLOSE); drain();
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "candidates"); drain();
    click(window, "candidate.pin"); popup = dialog();
    entry(popup, "pin-search", "UI"); g_signal_emit_by_name(find(popup, "pin-search"), "search-changed");
    view = find(popup, "pin-rules"); assert(rows(view) == 1); select(view); click(popup, "pin-delete");
    gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_CANCEL); drain(); assert(model.settings().candidatePins.size() == 2 && !model.dirty());
    click(window, "candidate.pin"); popup = dialog();
    entry(popup, "pin-search", "UI"); g_signal_emit_by_name(find(popup, "pin-search"), "search-changed");
    select(find(popup, "pin-rules")); click(popup, "pin-delete");
    gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT); drain();
    assert(model.settings().candidatePins.size() == 1 && model.settings().candidatePins.front().scheme == modernime::core::PinyinScheme::Full);
    click(window, "candidate.pin"); popup = dialog(); click(popup, "pin-add");
    entry(popup, "pin-input", "nihao"); click(popup, "pin-query");
    wait([&] { return gtk_widget_is_sensitive(find(popup, "pin-query")); });
    view = find(popup, "pin-candidates"); assert(rows(view) > 0); select(view);
    if (const auto *captures = std::getenv("MODERNIME_ASSIST_SCREENSHOTS")) capture(popup, std::filesystem::path(captures) / "选择置顶候选.png");
    click(popup, "pin-choose"); gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT); drain();
    assert(model.settings().candidatePins.size() == 2);
    const auto pin = model.settings().candidatePins.back(); assert(pin.input == "nihao" && !pin.fullPinyin.empty() && !pin.text.empty());
    assert(modernime::core::SettingsStore::load(file).settings.candidatePins == initial.candidatePins);
    assert(model.save()); saved = modernime::core::SettingsStore::load(file);
    assert(saved.settings == model.settings() && saved.settings.candidatePins.back() == pin);
    for (const auto &rule : saved.settings.candidatePins) assert(rule.scheme != modernime::core::PinyinScheme::Xiaohe);
    click(window, "candidate.pin"); popup = dialog(); click(popup, "pin-add");
    entry(popup, "pin-input", "shi"); click(popup, "pin-query");
    entry(popup, "pin-input", "nihao");
    wait([&] { return gtk_widget_is_sensitive(find(popup, "pin-query")); });
    assert(rows(find(popup, "pin-candidates")) == 0 && !gtk_widget_is_sensitive(find(popup, "pin-choose")));
    gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_CANCEL); drain();
    if (const auto *captures = std::getenv("MODERNIME_ASSIST_SCREENSHOTS")) {
        click(window, "candidate.pin"); popup = dialog();
        for (const auto &theme : modernime::core::kClientThemes) {
            applySettingsTheme(theme.key); capture(popup, std::filesystem::path(captures) / (std::string(theme.key) + "-置顶管理.png"));
        }
        gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_CANCEL); drain();
    }
    // Worker completion after window destruction must not access GTK.
    click(window, "candidate.pin"); popup = dialog(); click(popup, "pin-add"); entry(popup, "pin-input", "nihao"); click(popup, "pin-query");
    gtk_widget_destroy(window); drain();
    // All query paths point to fresh synthetic storage, which read-only queries must not create.
    const auto paths = modernime::core::SettingsPaths::fromEnvironment((root / "config").string(), (root / "data").string(), "");
    auto actual = queryPinCandidates(paths, initial, modernime::core::PinyinScheme::Full, "nihaoma", 32);
    assert(actual.error.empty() && !actual.candidates.empty());
    for (const auto &candidate : actual.candidates) assert(candidate.text != "你好" && candidate.input == "nihaoma");
    assert(!std::filesystem::exists(paths.learningStore) && !std::filesystem::exists(paths.userDictionary));
    drain(); std::filesystem::remove_all(root);
}
