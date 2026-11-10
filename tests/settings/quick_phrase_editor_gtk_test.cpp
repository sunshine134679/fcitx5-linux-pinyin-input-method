#include "modernime/settings/quick_phrase_editor.h"
#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/core/settings.h"

#include <gtk/gtk.h>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace modernime::settings;
namespace {
GtkWidget *find(GtkWidget *widget, const char *target) {
    const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
    if (id && std::string_view(id) == target) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *result = nullptr;
    for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), target);
    g_list_free(children);
    return result;
}
template<class Predicate> void wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate() && std::chrono::steady_clock::now() < end) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(predicate());
}
void click(GtkWidget *root, const char *target) {
    auto *button = find(root, target);
    if (!button || !gtk_widget_is_sensitive(button)) std::cerr << "操作尚未就绪：" << target << "；"
        << gtk_label_get_text(GTK_LABEL(find(root, "quick-phrase-state"))) << '\n';
    assert(button && gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button));
}
GtkWidget *dialog() {
    auto *windows = gtk_window_list_toplevels();
    GtkWidget *result = nullptr;
    for (auto *item = windows; item; item = item->next)
        if (GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data))) result = GTK_WIDGET(item->data);
    g_list_free(windows);
    assert(result);
    return result;
}
void edit(GtkWidget *root, const char *action, const char *abbreviation, const char *text) {
    click(root, action);
    auto *popup = dialog();
    gtk_entry_set_text(GTK_ENTRY(find(popup, "quick-phrase-abbreviation")), abbreviation);
    gtk_entry_set_text(GTK_ENTRY(find(popup, "quick-phrase-text")), text);
    assert(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT)));
    gtk_dialog_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT);
}
bool contains(const QuickPhraseSnapshot &snapshot, const QuickPhraseEntry &entry) {
    return std::find(snapshot.entries.begin(), snapshot.entries.end(), entry) != snapshot.entries.end();
}
void selectFirst(GtkWidget *root) {
    auto *view = find(root, "quick-phrase-list");
    auto *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path);
    gtk_tree_path_free(path);
}
void capture(GtkWidget *window, const std::filesystem::path &filename) {
    bool drawn = false;
    const auto handler = g_signal_connect_after(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer p) -> gboolean {
        *static_cast<bool *>(p) = true;
        return FALSE;
    }), &drawn);
    gtk_widget_queue_draw(window);
    wait([&] { return drawn; });
    g_signal_handler_disconnect(window, handler);
    gdk_display_sync(gtk_widget_get_display(window));
    auto *pixbuf = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0,
        gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
    assert(pixbuf);
    assert(gdk_pixbuf_save(pixbuf, filename.c_str(), "png", nullptr, nullptr));
    g_object_unref(pixbuf);
}
}

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    std::string pattern = "/tmp/modernime-quick-phrase-ui-XXXXXX";
    assert(mkdtemp(pattern.data()));
    const std::filesystem::path root(pattern);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    const auto path = quickPhrasePath();
    assert(saveQuickPhrases(path, {}, {{"dz", "测试地址"}, {"yx", "First@example.invalid"}, {"dz", "第二个地址"}}).saved());
    installSettingsStyles();
    std::atomic<int> reloads = 0;
    auto editor = std::make_unique<QuickPhraseEditor>(path, [](std::string) {}, [&] {
        const auto attempt = ++reloads;
        return QuickPhraseReloadResult{attempt > 1, attempt == 1 ? "测试：插件尚未启用" : ""};
    });
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 850, 650);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "modernime-settings");
    gtk_container_add(GTK_CONTAINER(window), editor->widget());
    gtk_widget_show_all(window);
    editor->refresh();
    wait([&] { return gtk_widget_is_sensitive(find(window, "quick-phrase-add")); });
    auto *search = find(window, "quick-phrase-search");
    gtk_entry_set_text(GTK_ENTRY(search), "FIRST");
    g_signal_emit_by_name(search, "search-changed");
    wait([&] { return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(find(window, "quick-phrase-list"))), nullptr) == 1; });
    selectFirst(window);
    edit(window, "quick-phrase-edit", "yx", "New@example.invalid");
    assert(contains(loadQuickPhrases(path), {"yx", "First@example.invalid"}));
    click(window, "quick-phrase-save");
    wait([&] { return reloads == 1 && gtk_widget_is_sensitive(find(window, "quick-phrase-apply")); });
    assert(contains(loadQuickPhrases(path), {"yx", "New@example.invalid"}));
    assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(window, "quick-phrase-state")))).find("已保存，暂未应用") != std::string_view::npos);
    const auto savedContent = loadQuickPhrases(path).content;
    click(window, "quick-phrase-apply");
    wait([&] { return reloads == 2 && gtk_widget_is_sensitive(find(window, "quick-phrase-add")) && !gtk_widget_is_sensitive(find(window, "quick-phrase-apply")); });
    assert(loadQuickPhrases(path).content == savedContent);

    gtk_entry_set_text(GTK_ENTRY(search), "第二");
    g_signal_emit_by_name(search, "search-changed");
    wait([&] { return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(find(window, "quick-phrase-list"))), nullptr) == 1; });
    selectFirst(window);
    click(window, "quick-phrase-delete");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    edit(window, "quick-phrase-add", "gs", "本地草稿");
    auto external = loadQuickPhrases(path);
    auto changed = external.entries;
    changed.push_back({"wb", "外部新增"});
    assert(saveQuickPhrases(path, external, changed).saved());
    click(window, "quick-phrase-save");
    wait([&] { return gtk_widget_is_sensitive(find(window, "quick-phrase-merge")); });
    assert(contains(loadQuickPhrases(path), {"dz", "第二个地址"}));
    click(window, "quick-phrase-merge");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    assert(!contains(loadQuickPhrases(path), {"gs", "本地草稿"}));
    click(window, "quick-phrase-save");
    wait([&] { return reloads == 3 && gtk_widget_is_sensitive(find(window, "quick-phrase-add")) && !gtk_widget_is_sensitive(find(window, "quick-phrase-save")); });
    const auto merged = loadQuickPhrases(path);
    assert(contains(merged, {"gs", "本地草稿"}) && contains(merged, {"wb", "外部新增"}));
    assert(!contains(merged, {"dz", "第二个地址"}) && contains(merged, {"dz", "测试地址"}));
    gtk_entry_set_text(GTK_ENTRY(search), "");
    g_signal_emit_by_name(search, "search-changed");
    edit(window, "quick-phrase-add", "ls", "放弃的修改");
    assert(editor->hasUnsavedChanges());
    editor->refresh();
    assert(editor->hasUnsavedChanges());
    g_idle_add(+[](gpointer) -> gboolean {
        gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL);
        return G_SOURCE_REMOVE;
    }, nullptr);
    assert(!editor->confirmLeave() && editor->hasUnsavedChanges());
    click(window, "quick-phrase-discard");
    wait([&] { return !editor->hasUnsavedChanges() && gtk_widget_is_sensitive(find(window, "quick-phrase-add")); });
    assert(!contains(loadQuickPhrases(path), {"ls", "放弃的修改"}));

    auto externalUpdate = loadQuickPhrases(path);
    auto updatedEntries = externalUpdate.entries;
    updatedEntries.push_back({"gx", "外部再次修改"});
    assert(saveQuickPhrases(path, externalUpdate, updatedEntries).saved());
    editor->refresh();
    wait([&] { return gtk_widget_is_sensitive(find(window, "quick-phrase-add")); });
    assert(std::string_view(gtk_label_get_text(GTK_LABEL(find(window, "quick-phrase-state")))).find("已保存，暂未应用") != std::string_view::npos);
    click(window, "quick-phrase-apply");
    wait([&] { return reloads == 4 && gtk_widget_is_sensitive(find(window, "quick-phrase-add")); });

    if (const auto *captures = std::getenv("MODERNIME_QUICK_PHRASE_CAPTURES")) {
        std::filesystem::create_directories(captures);
        auto *gtkSettings = gtk_widget_get_settings(window);
        gboolean animations = FALSE;
        g_object_get(gtkSettings, "gtk-enable-animations", &animations, nullptr);
        g_object_set(gtkSettings, "gtk-enable-animations", FALSE, nullptr);
        for (const auto &theme : modernime::core::kClientThemes) {
            applySettingsTheme(theme.key);
            const auto filename = std::filesystem::path(captures) / (std::string(theme.key) + ".png");
            capture(window, filename);
        }
        g_object_set(gtkSettings, "gtk-enable-animations", animations, nullptr);
    }
    gtk_widget_destroy(window);
    editor.reset();
    {
        std::ofstream(path) << "unsupported-format\n";
        QuickPhraseEditor unreadable(path, [](std::string) {});
        auto *errorWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_container_add(GTK_CONTAINER(errorWindow), unreadable.widget());
        gtk_widget_show_all(errorWindow);
        unreadable.refresh();
        wait([&] { return gtk_widget_is_sensitive(find(errorWindow, "quick-phrase-refresh")); });
        assert(!gtk_widget_is_sensitive(find(errorWindow, "quick-phrase-add")));
        gtk_widget_destroy(errorWindow);
    }
    std::filesystem::remove(path);
    {
        DictionaryPage dictionary(root / "user.dict", [](std::string) {});
        auto *dictionaryWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_default_size(GTK_WINDOW(dictionaryWindow), 780, 650);
        gtk_style_context_add_class(gtk_widget_get_style_context(dictionaryWindow), "modernime-settings");
        gtk_container_add(GTK_CONTAINER(dictionaryWindow), dictionary.widget());
        gtk_widget_show_all(dictionaryWindow);
        gtk_window_present(GTK_WINDOW(dictionaryWindow));
        wait([&] { return gtk_window_is_active(GTK_WINDOW(dictionaryWindow)); });
        assert(dictionary.focusTarget("quick-phrase-search"));
        assert(settingsPageDefinitions().size() == 8 && !searchSettings("快捷短语").empty());
        wait([&] { return gtk_widget_is_sensitive(find(dictionaryWindow, "quick-phrase-add")); });
        wait([&] { return !gtk_stack_get_transition_running(GTK_STACK(find(dictionaryWindow, "dictionary-views"))); });
        if (const auto *captures = std::getenv("MODERNIME_QUICK_PHRASE_CAPTURES"))
            capture(dictionaryWindow, std::filesystem::path(captures) / "个人词典内入口.png");
        gtk_widget_destroy(dictionaryWindow);
    }
    // A saved write can finish after its view has gone away; no GTK access is allowed then.
    std::atomic<bool> entered = false, finished = false;
    bool release = false;
    std::mutex mutex;
    std::condition_variable condition;
    auto closing = std::make_unique<QuickPhraseEditor>(path, [](std::string) {}, [&] {
        entered = true;
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
        finished = true;
        return QuickPhraseReloadResult{true, {}};
    });
    auto *closingWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(closingWindow), closing->widget());
    gtk_widget_show_all(closingWindow);
    closing->refresh();
    wait([&] { return gtk_widget_is_sensitive(find(closingWindow, "quick-phrase-add")); });
    edit(closingWindow, "quick-phrase-add", "cs", "关闭测试");
    click(closingWindow, "quick-phrase-save");
    wait([&] { return entered.load(); });
    gtk_widget_destroy(closingWindow);
    closing.reset();
    { std::lock_guard lock(mutex); release = true; }
    condition.notify_one();
    wait([&] { return finished.load(); });
    { std::lock_guard lock(mutex); }
    while (g_main_context_iteration(nullptr, FALSE)) {}
    std::filesystem::remove_all(root);
    std::cout << "[PASS] 快捷短语管理、过滤映射、冲突确认、独立应用与在途关闭\n";
}
