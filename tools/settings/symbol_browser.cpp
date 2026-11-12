#include "modernime/settings/symbol_browser.h"
#include "modernime/settings/settings_model.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/core/symbol_catalog.h"

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {
constexpr std::size_t kFavoriteLimit = 128;
struct Category { const char *key; const char *name; };
constexpr std::array<Category, 7> kCategories = {{{"", "全部"},
    {"punctuation", "常用标点"}, {"math", "数学"}, {"units", "单位"},
    {"arrows", "箭头"}, {"emoji", "表情"}, {"other", "其他"}}};

const char *categoryName(std::string_view key) {
    for (const auto &category : kCategories) if (key == category.key) return category.name;
    return "其他";
}
void target(GtkWidget *widget, const char *name) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(name), g_free);
}
GtkWidget *description(const char *text) {
    auto *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 68);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "modernime-description");
    return label;
}

struct Browser {
    SettingsWindowModel &model;
    std::function<void()> changed;
    std::set<std::string> favorites;
    GtkWidget *query = nullptr, *category = nullptr, *only = nullptr;
    GtkWidget *results = nullptr, *count = nullptr, *preview = nullptr, *details = nullptr;
    GtkWidget *favorite = nullptr, *copy = nullptr, *status = nullptr;

    const core::SymbolEntry *selected() const {
        GtkTreeIter iter;
        GtkTreeModel *store = nullptr;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(results)), &store, &iter)) return nullptr;
        gchar *id = nullptr;
        gtk_tree_model_get(store, &iter, 0, &id, -1);
        const auto *entry = core::SymbolCatalog::find(id);
        g_free(id);
        return entry;
    }

    void selectionChanged() {
        const auto *entry = selected();
        const bool isFavorite = entry && favorites.contains(entry->id);
        gtk_label_set_text(GTK_LABEL(preview), entry ? entry->text.c_str() : "未选择符号");
        const auto detail = entry ? entry->name + " · " + categoryName(entry->category) : "选择结果后可收藏或复制。";
        gtk_label_set_text(GTK_LABEL(details), detail.c_str());
        gtk_button_set_label(GTK_BUTTON(favorite), isFavorite ? "取消收藏" : "收藏符号");
        gtk_widget_set_sensitive(copy, entry != nullptr);
        gtk_widget_set_sensitive(favorite, entry && (isFavorite || favorites.size() < kFavoriteLimit));
        gtk_label_set_text(GTK_LABEL(status), entry && !isFavorite && favorites.size() >= kFavoriteLimit
            ? "已收藏 128 个符号，取消一个收藏后可添加。" : "");
    }

    void render() {
        const auto *previous = selected();
        const auto index = settingsChoiceActive(category);
        const auto categoryKey = index >= 0 && index < static_cast<int>(kCategories.size()) ? kCategories[index].key : "";
        const std::vector<std::string> favoriteIds(favorites.begin(), favorites.end());
        const auto matches = core::SymbolCatalog::search(gtk_entry_get_text(GTK_ENTRY(query)), categoryKey,
            favoriteIds, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(only)), 200);
        auto *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(results)));
        gtk_list_store_clear(store);
        GtkTreeIter selectedIter;
        bool restore = false;
        for (const auto *entry : matches) {
            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, entry->id.c_str(), 1, entry->text.c_str(),
                2, entry->name.c_str(), 3, categoryName(entry->category),
                4, favorites.contains(entry->id) ? "已收藏" : "", -1);
            if (entry == previous) { selectedIter = iter; restore = true; }
        }
        if (restore || gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &selectedIter))
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(results)), &selectedIter);
        const auto message = matches.empty()
            ? (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(only)) && favorites.empty()
                ? std::string("还没有收藏。取消“仅收藏”后选择符号添加。")
                : std::string("没有匹配的符号，试试其他关键词或分类。"))
            : "显示 " + std::to_string(matches.size()) + " 个符号，已收藏 " + std::to_string(favorites.size()) + " / 128 个";
        gtk_label_set_text(GTK_LABEL(count), message.c_str());
        selectionChanged();
    }

    void toggleFavorite() {
        const auto *entry = selected();
        if (!entry) return;
        if (!favorites.erase(entry->id)) {
            if (favorites.size() >= kFavoriteLimit) return;
            favorites.insert(entry->id);
        }
        render();
    }

    void copySelected() {
        const auto *entry = selected();
        if (!entry) return;
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), entry->text.c_str(), -1);
        gtk_label_set_text(GTK_LABEL(status), "已复制符号。");
    }
};
} // namespace

GtkWidget *openSymbolBrowser(GtkWindow *parent, SettingsWindowModel &model,
                             std::function<void()> changed) {
    auto *owner = new Browser{model, std::move(changed), model.settings().symbolFavorites};
    auto *dialog = gtk_dialog_new_with_buttons("符号与收藏", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "取消", GTK_RESPONSE_CANCEL, "使用这些收藏", GTK_RESPONSE_ACCEPT, nullptr);
    target(dialog, "symbol-browser");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "modernime-settings");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 690, 680);
    setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, "取消", "放弃弹窗内的收藏修改");
    setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "使用这些收藏", "将收藏写回设置草稿，仍需在主窗口保存设置");
    gtk_style_context_add_class(gtk_widget_get_style_context(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)), "suggested-action");
    auto *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 20);
    gtk_box_set_spacing(GTK_BOX(body), 10);
    gtk_box_pack_start(GTK_BOX(body), description("离线浏览常用符号。支持中文、拼音、英文或原符号搜索；输入时用 v; 打开符号搜索。"), FALSE, FALSE, 0);

    owner->query = gtk_search_entry_new();
    target(owner->query, "symbol-query");
    gtk_entry_set_placeholder_text(GTK_ENTRY(owner->query), "搜索符号，例如 箭头、jiantou、arrow、→");
    setAccessibleWidgetText(owner->query, "搜索符号", "支持中文名称、拼音、英文和原符号，忽略英文大小写与空白");
    gtk_box_pack_start(GTK_BOX(body), owner->query, FALSE, FALSE, 0);

    auto *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    owner->category = createSettingsChoice();
    for (const auto &category : kCategories) appendSettingsChoice(owner->category, category.name);
    setSettingsChoiceActive(owner->category, 0);
    target(owner->category, "symbol-category");
    gtk_box_pack_start(GTK_BOX(filters), createSettingRow("分类", "", owner->category), TRUE, TRUE, 0);
    owner->only = gtk_check_button_new_with_label("仅收藏");
    target(owner->only, "symbol-favorites-only");
    setAccessibleWidgetText(owner->only, "仅收藏", "仅显示当前收藏草稿中的符号");
    gtk_box_pack_start(GTK_BOX(filters), owner->only, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), filters, FALSE, FALSE, 0);

    auto *store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    owner->results = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    target(owner->results, "symbol-results");
    setAccessibleWidgetText(owner->results, "符号列表", "上下方向键选择符号，再使用收藏或复制按钮");
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(owner->results), FALSE);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(owner->results)), GTK_SELECTION_SINGLE);
    const char *titles[] = {"符号", "名称", "分类", "收藏"};
    for (int i = 0; i < 4; ++i) {
        auto *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 5, nullptr);
        auto *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i + 1, nullptr);
        gtk_tree_view_column_set_expand(column, i == 1);
        gtk_tree_view_column_set_min_width(column, i == 0 ? 60 : i == 1 ? 180 : i == 2 ? 104 : 64);
        gtk_tree_view_append_column(GTK_TREE_VIEW(owner->results), column);
    }
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), owner->results);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    owner->count = description("");
    target(owner->count, "symbol-count");
    gtk_box_pack_start(GTK_BOX(body), owner->count, FALSE, FALSE, 0);

    auto *selection = createSectionCard("符号预览", "当前选中的完整符号");
    auto *selectionRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_set_margin_top(selectionRow, 8);
    owner->preview = gtk_label_new("");
    target(owner->preview, "symbol-preview");
    gtk_label_set_selectable(GTK_LABEL(owner->preview), TRUE);
    gtk_label_set_xalign(GTK_LABEL(owner->preview), 0);
    gtk_label_set_width_chars(GTK_LABEL(owner->preview), 5);
    auto *attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_scale_new(1.5));
    gtk_label_set_attributes(GTK_LABEL(owner->preview), attributes);
    pango_attr_list_unref(attributes);
    setAccessibleWidgetText(owner->preview, {}, "选中符号的完整文本");
    gtk_box_pack_start(GTK_BOX(selectionRow), owner->preview, FALSE, FALSE, 0);
    owner->details = description("");
    gtk_box_pack_start(GTK_BOX(selectionRow), owner->details, TRUE, TRUE, 0);
    owner->favorite = gtk_button_new_with_label("收藏符号");
    owner->copy = gtk_button_new_with_label("复制符号");
    target(owner->favorite, "symbol-favorite");
    target(owner->copy, "symbol-copy");
    setAccessibleWidgetText(owner->favorite, {}, "在弹窗草稿中收藏或取消收藏当前符号");
    setAccessibleWidgetText(owner->copy, "复制符号", "将选中符号的完整文本复制到剪贴板");
    gtk_box_pack_start(GTK_BOX(selectionRow), owner->favorite, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selectionRow), owner->copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selection), selectionRow, FALSE, FALSE, 0);
    owner->status = description("");
    target(owner->status, "symbol-status");
    gtk_box_pack_start(GTK_BOX(selection), owner->status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), selection, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), description("确认后返回设置草稿，点击主窗口“保存设置”才会生效。"), FALSE, FALSE, 0);

    g_signal_connect(owner->query, "changed", G_CALLBACK(+[](GtkEditable *, gpointer p) { static_cast<Browser *>(p)->render(); }), owner);
    g_signal_connect(owner->category, "changed", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Browser *>(p)->render(); }), owner);
    g_signal_connect(owner->only, "toggled", G_CALLBACK(+[](GtkToggleButton *, gpointer p) { static_cast<Browser *>(p)->render(); }), owner);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(owner->results)), "changed", G_CALLBACK(+[](GtkTreeSelection *, gpointer p) { static_cast<Browser *>(p)->selectionChanged(); }), owner);
    g_signal_connect(owner->favorite, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<Browser *>(p)->toggleFavorite(); }), owner);
    g_signal_connect(owner->copy, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<Browser *>(p)->copySelected(); }), owner);
    g_signal_connect(owner->query, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer p) -> gboolean {
        if (event->keyval != GDK_KEY_Down) return GDK_EVENT_PROPAGATE;
        gtk_widget_grab_focus(static_cast<Browser *>(p)->results);
        return GDK_EVENT_STOP;
    }), owner);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *widget, gint response, gpointer p) {
        auto &o = *static_cast<Browser *>(p);
        std::function<void()> notify;
        if (response == GTK_RESPONSE_ACCEPT && o.favorites != o.model.settings().symbolFavorites) {
            auto settings = o.model.settings();
            settings.symbolFavorites = o.favorites;
            o.model.setSettings(std::move(settings));
            notify = o.changed;
        }
        gtk_widget_destroy(GTK_WIDGET(widget));
        if (notify) notify();
    }), owner);
    g_object_set_data_full(G_OBJECT(dialog), "modernime-symbol-browser", owner, +[](gpointer p) { delete static_cast<Browser *>(p); });
    setSettingsFocusChain(filters, {owner->category, owner->only});
    setSettingsFocusChain(selectionRow, {owner->preview, owner->favorite, owner->copy});
    setSettingsFocusChain(body, {owner->query, filters, scroll, selection});
    owner->render();
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(owner->query);
    return dialog;
}
} // namespace modernime::settings
