#include "modernime/settings/pages/clipboard_page.h"

#include "modernime/settings/clipboard_history_model.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include "modernime/core/clipboard_history.h"

#include <gtk/gtk.h>

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

void setWidgetError(GtkWidget *widget, bool invalid, std::string_view message) {
    auto *context = gtk_widget_get_style_context(widget);
    if (invalid) {
        gtk_style_context_add_class(context, "error");
        gtk_widget_set_tooltip_text(widget, std::string(message).c_str());
    } else {
        gtk_style_context_remove_class(context, "error");
        gtk_widget_set_tooltip_text(widget, nullptr);
    }
}

} // namespace

class ClipboardPage::Impl final {
public:
    Impl(SettingsWindowModel &settingsModel, std::filesystem::path historyPath,
         std::function<void()> settingsChangedCallback,
         std::function<void(std::string)> notifyCallback)
        : model(settingsModel), history(std::move(historyPath)),
          settingsChanged(std::move(settingsChangedCallback)),
          notify(std::move(notifyCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "剪贴板", "配置 V+2 功能并查看保存在本地的剪贴板历史"));
        page = pageGuard.get();
        buildPage();
        refreshSettings();
        pageGuard.release();
    }

    void buildPage() {
        auto *settingsSection = createSectionCard(
            "触发方式", "仅在中文输入状态且当前没有正在输入拼音时触发。");
        gtk_box_pack_start(GTK_BOX(page), settingsSection, FALSE, FALSE, 0);

        clipboardEnabled = gtk_check_button_new_with_label("启用 V+2 剪贴板");
        setAccessibleWidgetText(clipboardEnabled, "启用 V+2 剪贴板",
                                "启用或关闭本地剪贴板历史入口");
        setTarget(clipboardEnabled, "clipboard-enabled");
        gtk_box_pack_start(GTK_BOX(settingsSection), clipboardEnabled, FALSE,
                           FALSE, 0);

        auto *grid = gtk_grid_new();
        clipboardTriggerFallback = grid;
        setAccessibleWidgetText(
            clipboardTriggerFallback, "剪贴板触发键",
            "剪贴板功能关闭时仍可定位并阅读触发键设置");
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        auto *triggerLabel = gtk_label_new("剪贴板触发键");
        gtk_widget_set_halign(triggerLabel, GTK_ALIGN_START);
        clipboardTrigger = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(clipboardTrigger), 3);
        gtk_entry_set_width_chars(GTK_ENTRY(clipboardTrigger), 6);
        gtk_entry_set_placeholder_text(GTK_ENTRY(clipboardTrigger), "例如 V+2");
        gtk_widget_set_hexpand(clipboardTrigger, TRUE);
        setTarget(clipboardTrigger, "clipboard-trigger");
        setAccessibleWidgetText(
            clipboardTrigger, "剪贴板触发键",
            "设置在中文输入状态打开本地剪贴板历史的按键");
        gtk_grid_attach(GTK_GRID(grid), triggerLabel, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), clipboardTrigger, 1, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(settingsSection), grid, FALSE, FALSE, 0);

        auto *description = gtk_label_new(
            "仅在中文输入状态且当前没有正在输入的拼音时触发。按第一个字母后，"
            "可继续选择对应功能或直接按回车输出字母。");
        addStyleClass(description, "modernime-description");
        gtk_widget_set_halign(description, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
        gtk_box_pack_start(GTK_BOX(settingsSection), description, FALSE, FALSE,
                           0);
        auto *historySection = createSectionCard(
            "剪贴板历史", "按最近使用顺序显示，最多保留 30 条内容。");
        gtk_box_pack_start(GTK_BOX(page), historySection, TRUE, TRUE, 0);
        historyCount = gtk_label_new("正在读取…");
        addStyleClass(historyCount, "modernime-description");
        gtk_widget_set_halign(historyCount, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(historySection), historyCount, FALSE, FALSE,
                           0);

        auto historyStoreOwner =
            detail::GObjectHandle<GtkListStore>::adopt(
                gtk_list_store_new(2, G_TYPE_UINT, G_TYPE_STRING));
        historyStore = historyStoreOwner.get();
        historyView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(historyStore));
        historyStoreOwner.reset();
        setTarget(historyView, "clipboard-history");
        setAccessibleWidgetText(
            historyView, "剪贴板历史",
            "选择一条本地历史后可以复制或删除");
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(historyView), TRUE);
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(historyView), TRUE);
        gtk_widget_set_tooltip_text(
            historyView, "选择一条历史后可以复制、删除；也可以使用键盘上下键移动");
        auto *numberRenderer = gtk_cell_renderer_text_new();
        auto *numberColumn = gtk_tree_view_column_new_with_attributes(
            "序号", numberRenderer, "text", 0, nullptr);
        gtk_tree_view_column_set_resizable(numberColumn, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(historyView), numberColumn);
        auto *textRenderer = gtk_cell_renderer_text_new();
        auto *textColumn = gtk_tree_view_column_new_with_attributes(
            "内容", textRenderer, "text", 1, nullptr);
        gtk_tree_view_column_set_expand(textColumn, TRUE);
        gtk_tree_view_column_set_resizable(textColumn, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(historyView), textColumn);
        auto *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(historyView));
        gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
        g_signal_connect(selection, "changed", G_CALLBACK(onSelectionChanged),
                         this);
        auto *historyScrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_vexpand(historyScrolled, TRUE);
        gtk_widget_set_hexpand(historyScrolled, TRUE);
        gtk_widget_set_size_request(historyScrolled, -1, 160);
        gtk_container_add(GTK_CONTAINER(historyScrolled), historyView);
        gtk_box_pack_start(GTK_BOX(historySection), historyScrolled, TRUE, TRUE,
                           0);

        historyState = gtk_label_new("正在读取…");
        addStyleClass(historyState, "modernime-description");
        gtk_widget_set_halign(historyState, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(historyState), TRUE);
        gtk_box_pack_start(GTK_BOX(historySection), historyState, FALSE, FALSE,
                           0);

        auto *historyActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        const auto actionLabels = settingsClipboardActionLabels();
        copyButton = gtk_button_new_with_label(actionLabels[0].data());
        deleteButton = gtk_button_new_with_label(actionLabels[1].data());
        clearButton = gtk_button_new_with_label(actionLabels[2].data());
        gtk_widget_set_tooltip_text(copyButton, "复制选中的历史内容到系统剪贴板");
        gtk_widget_set_tooltip_text(deleteButton, "删除选中的历史内容");
        gtk_widget_set_tooltip_text(clearButton, "清空全部历史内容，需要确认");
        setAccessibleWidgetText(copyButton, actionLabels[0],
                                "复制选中的历史内容到系统剪贴板");
        setAccessibleWidgetText(deleteButton, actionLabels[1],
                                "删除选中的历史内容");
        setAccessibleWidgetText(clearButton, actionLabels[2],
                                "确认后清空全部本地剪贴板历史");
        for (auto *button : {copyButton, deleteButton, clearButton}) {
            gtk_box_pack_start(GTK_BOX(historyActions), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(historySection), historyActions, FALSE, FALSE,
                           0);

        auto *refreshButton = gtk_button_new_with_label("刷新历史");
        gtk_widget_set_halign(refreshButton, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(refreshButton, "重新读取磁盘上的剪贴板历史");
        setAccessibleWidgetText(refreshButton, "刷新历史",
                                "重新读取磁盘上的剪贴板历史");
        gtk_box_pack_start(GTK_BOX(historySection), refreshButton, FALSE, FALSE,
                           0);
        g_signal_connect(refreshButton, "clicked", G_CALLBACK(onRefresh), this);
        g_signal_connect(copyButton, "clicked", G_CALLBACK(onCopy), this);
        g_signal_connect(deleteButton, "clicked", G_CALLBACK(onDelete), this);
        g_signal_connect(clearButton, "clicked", G_CALLBACK(onClear), this);
        g_signal_connect(clipboardEnabled, "toggled", G_CALLBACK(onChanged),
                         this);
        g_signal_connect(clipboardTrigger, "changed", G_CALLBACK(onChanged),
                         this);
        setSettingsFocusChain(
            page, {clipboardEnabled, clipboardTrigger, historyView,
                   copyButton, deleteButton, clearButton, refreshButton});
    }

    void refresh(bool shouldNotify) {
        gtk_list_store_clear(historyStore);
        std::string error;
        if (!history.reload(&error)) {
            gtk_label_set_text(GTK_LABEL(historyCount), "历史读取失败");
            gtk_label_set_text(GTK_LABEL(historyState),
                               error.empty() ? "无法读取剪贴板历史"
                                             : ("无法读取剪贴板历史：" + error)
                                                   .c_str());
            gtk_widget_set_visible(historyState, TRUE);
            updateHistoryActionState();
            if (shouldNotify) {
                notifyMessage(error);
            }
            return;
        }

        const auto &entries = history.entries();
        gtk_label_set_text(GTK_LABEL(historyState),
                           entries.empty()
                               ? "暂无剪贴板历史；复制内容后会自动记录"
                               : "");
        gtk_widget_set_visible(historyState, entries.empty());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            GtkTreeIter iter;
            gtk_list_store_append(historyStore, &iter);
            gtk_list_store_set(historyStore, &iter, 0,
                               static_cast<guint>(index + 1), 1,
                               entries[index].c_str(), -1);
        }
        gtk_label_set_text(
            GTK_LABEL(historyCount),
            ("当前 " + std::to_string(entries.size()) + " 条，最多保存 " +
             std::to_string(core::ClipboardHistory::kMaxEntries) + " 条")
                .c_str());
        updateHistoryActionState();
        if (shouldNotify) {
            notifyMessage("剪贴板历史已刷新");
        }
    }

    void refreshSettings() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(clipboardEnabled),
                                     settings.clipboardEnabled);
        gtk_entry_set_text(GTK_ENTRY(clipboardTrigger),
                           settings.clipboardTrigger.c_str());
        refreshing = false;
        updateSettingsState();
    }

    bool focusTarget(std::string_view target) {
        const std::array controls{
            std::pair{clipboardEnabled, static_cast<GtkWidget *>(nullptr)},
            std::pair{clipboardTrigger, clipboardTriggerFallback},
            std::pair{historyView, static_cast<GtkWidget *>(nullptr)},
        };
        for (const auto &[control, fallback] : controls) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                return focusWidgetOrFallback(control, fallback);
            }
        }
        return false;
    }

    GtkWidget *widget() const { return page; }

private:
    static void onChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        impl->model.setClipboardOptions(
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->clipboardEnabled)),
            gtk_entry_get_text(GTK_ENTRY(impl->clipboardTrigger)));
        impl->updateSettingsState();
        impl->settingsChanged();
    }

    static void onSelectionChanged(GtkTreeSelection *, gpointer data) {
        static_cast<Impl *>(data)->updateHistoryActionState();
    }

    static void onRefresh(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->refresh(true);
    }

    static void onCopy(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要复制的剪贴板历史");
            return;
        }
        const auto &entry = impl->history.entries()[*selected];
        auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (clipboard == nullptr) {
            impl->notifyMessage("系统剪贴板不可用");
            return;
        }
        gtk_clipboard_set_text(clipboard, entry.c_str(), -1);
        impl->notifyMessage("已复制选中的剪贴板历史");
    }

    static void onDelete(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要删除的剪贴板历史");
            return;
        }
        const auto number = std::to_string(*selected + 1);
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO, "确定删除第 %s 条剪贴板历史吗？", number.c_str());
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "删除",
            "删除选中的本地剪贴板历史");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留剪贴板历史并关闭对话框");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        std::string error;
        if (impl->history.remove(*selected, &error)) {
            impl->refresh(false);
            impl->notifyMessage("已删除选中的剪贴板历史");
        } else {
            impl->notifyMessage(error.empty() ? "剪贴板历史删除失败" : error);
        }
    }

    static void onClear(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->history.entries().empty()) {
            impl->notifyMessage("当前没有可清空的剪贴板历史");
            return;
        }
        const auto count = std::to_string(impl->history.entries().size());
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "确定清空全部 %s 条剪贴板历史吗？此操作不可撤销。", count.c_str());
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "清空",
            "清空全部本地剪贴板历史且无法撤销");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留全部剪贴板历史");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        std::string error;
        if (impl->history.clear(&error)) {
            impl->refresh(false);
            impl->notifyMessage("剪贴板历史已清空");
        } else {
            impl->notifyMessage(error.empty() ? "剪贴板历史清空失败" : error);
        }
    }

    std::optional<std::size_t> selectedHistoryIndex() const {
        auto *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(historyView));
        GtkTreeModel *treeModel = nullptr;
        GtkTreeIter iterator;
        if (!gtk_tree_selection_get_selected(selection, &treeModel, &iterator)) {
            return std::nullopt;
        }
        auto *path = gtk_tree_model_get_path(treeModel, &iterator);
        if (path == nullptr) {
            return std::nullopt;
        }
        const auto *indices = gtk_tree_path_get_indices(path);
        const auto index = indices == nullptr ? -1 : indices[0];
        gtk_tree_path_free(path);
        if (index < 0 || static_cast<std::size_t>(index) >= history.entries().size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(index);
    }

    void updateHistoryActionState() {
        const bool hasSelected = selectedHistoryIndex().has_value();
        const bool hasEntries = !history.entries().empty();
        gtk_widget_set_sensitive(copyButton, hasSelected);
        gtk_widget_set_sensitive(deleteButton, hasSelected);
        gtk_widget_set_sensitive(clearButton, hasEntries);
    }

    void updateSettingsState() {
        gtk_widget_set_sensitive(
            clipboardTrigger,
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(clipboardEnabled)));
        const auto validation = model.validation();
        for (const auto &issue : validation.issues) {
            if (issue.key == "clipboard.trigger") {
                setWidgetError(clipboardTrigger, true, issue.message);
                return;
            }
        }
        setWidgetError(clipboardTrigger, false, "");
    }

    GtkWindow *parentWindow() const {
        auto *topLevel = gtk_widget_get_toplevel(page);
        return GTK_IS_WINDOW(topLevel) ? GTK_WINDOW(topLevel) : nullptr;
    }

    void notifyMessage(std::string message) const {
        if (notify) {
            notify(std::move(message));
        }
    }

    SettingsWindowModel &model;
    ClipboardHistoryModel history;
    std::function<void()> settingsChanged;
    std::function<void(std::string)> notify;
    GtkWidget *page = nullptr;
    GtkWidget *clipboardEnabled = nullptr;
    GtkWidget *clipboardTrigger = nullptr;
    GtkWidget *clipboardTriggerFallback = nullptr;
    GtkListStore *historyStore = nullptr;
    GtkWidget *historyCount = nullptr;
    GtkWidget *historyState = nullptr;
    GtkWidget *historyView = nullptr;
    GtkWidget *copyButton = nullptr;
    GtkWidget *deleteButton = nullptr;
    GtkWidget *clearButton = nullptr;
    bool refreshing = false;
};

ClipboardPage::ClipboardPage(SettingsWindowModel &settings,
                             std::filesystem::path historyPath,
                             std::function<void()> settingsChanged,
                             std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(settings, std::move(historyPath),
                                   std::move(settingsChanged),
                                   std::move(notify))) {}

ClipboardPage::~ClipboardPage() = default;

GtkWidget *ClipboardPage::widget() const {
    return impl_->widget();
}

void ClipboardPage::refresh(bool notify) {
    impl_->refresh(notify);
}

void ClipboardPage::refreshSettings() {
    impl_->refreshSettings();
}

bool ClipboardPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
