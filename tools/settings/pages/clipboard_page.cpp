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
            "触发方式",
            "中文输入状态下输入触发字母后紧跟触发数字即可打开剪贴板。");
        gtk_box_pack_start(GTK_BOX(page), settingsSection, FALSE, FALSE, 0);

        clipboardEnabled = gtk_switch_new();
        setAccessibleWidgetText(clipboardEnabled, "启用 V+2 剪贴板",
                                "启用或关闭本地剪贴板历史入口");
        setTarget(clipboardEnabled, "clipboard-enabled");
        gtk_box_pack_start(GTK_BOX(settingsSection),
                           createSettingRow("启用 V+2 剪贴板",
                                            "中文输入状态下输入触发字母后紧跟数字即可打开剪贴板。",
                                            clipboardEnabled),
                           FALSE, FALSE, 0);

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
            "输入触发字母（默认 v）后立即按下触发数字（默认 2）即可打开剪贴板。"
            "触发字母本身按正常拼音输入处理，不会被吞掉或改写。");
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

        historyListBox = gtk_list_box_new();
        addStyleClass(historyListBox, "modernime-clipboard-list");
        setTarget(historyListBox, "clipboard-history");
        setAccessibleWidgetText(
            historyListBox, "剪贴板历史",
            "查看、展开、复制或删除本地剪贴板历史记录");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(historyListBox),
                                        GTK_SELECTION_SINGLE);
        g_signal_connect(historyListBox, "row-selected",
                         G_CALLBACK(onRowSelected), this);
        g_signal_connect(historyListBox, "row-activated",
                         G_CALLBACK(onRowActivated), this);

        auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        historyScrolled = scrolled;
        gtk_widget_set_vexpand(historyScrolled, TRUE);
        gtk_widget_set_hexpand(historyScrolled, TRUE);
        gtk_widget_set_size_request(historyScrolled, -1, 320);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(historyScrolled),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(historyScrolled), historyListBox);
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
        g_signal_connect(clipboardEnabled, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(clipboardTrigger, "changed", G_CALLBACK(onChanged),
                         this);
        setSettingsFocusChain(
            page, {clipboardEnabled, clipboardTrigger, historyListBox,
                   copyButton, deleteButton, clearButton, refreshButton});
    }

    void refresh(bool shouldNotify) {
        auto *children =
            gtk_container_get_children(GTK_CONTAINER(historyListBox));
        for (auto *c = children; c != nullptr; c = c->next) {
            gtk_widget_destroy(GTK_WIDGET(c->data));
        }
        g_list_free(children);

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
            auto *row = buildCardRow(index, entries[index]);
            gtk_container_add(GTK_CONTAINER(historyListBox), row);
        }
        gtk_widget_show_all(historyListBox);

        gtk_label_set_text(
            GTK_LABEL(historyCount),
            ("当前 " + std::to_string(entries.size()) + " 条，最多保存 " +
             std::to_string(core::ClipboardHistory::kMaxEntries) + " 条")
                .c_str());

        if (!entries.empty()) {
            auto *firstRow =
                gtk_list_box_get_row_at_index(GTK_LIST_BOX(historyListBox), 0);
            if (firstRow != nullptr) {
                gtk_list_box_select_row(GTK_LIST_BOX(historyListBox), firstRow);
            }
        }

        updateHistoryActionState();
        if (shouldNotify) {
            notifyMessage("剪贴板历史已刷新");
        }
    }

    void refreshSettings() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_switch_set_active(GTK_SWITCH(clipboardEnabled),
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
            std::pair{historyListBox, static_cast<GtkWidget *>(nullptr)},
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
    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        onChanged(nullptr, data);
    }

    static void onChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        impl->model.setClipboardOptions(
            gtk_switch_get_active(GTK_SWITCH(impl->clipboardEnabled)),
            gtk_entry_get_text(GTK_ENTRY(impl->clipboardTrigger)));
        impl->updateSettingsState();
        impl->settingsChanged();
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
        impl->copyIndex(*selected);
    }

    static void onDelete(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要删除的剪贴板历史");
            return;
        }
        impl->deleteIndex(*selected);
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

    static void onRowSelected(GtkListBox *, GtkListBoxRow *, gpointer data) {
        static_cast<Impl *>(data)->updateHistoryActionState();
    }

    static void onRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (row == nullptr) {
            return;
        }
        const auto index = gtk_list_box_row_get_index(row);
        if (index >= 0) {
            impl->copyIndex(static_cast<std::size_t>(index));
        }
    }

    void copyIndex(std::size_t index) {
        if (index >= history.entries().size()) {
            notifyMessage("请先选择要复制的剪贴板历史");
            return;
        }
        const auto &entry = history.entries()[index];
        auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (clipboard == nullptr) {
            notifyMessage("系统剪贴板不可用");
            return;
        }
        gtk_clipboard_set_text(clipboard, entry.c_str(), -1);
        notifyMessage("已复制第 " + std::to_string(index + 1) + " 条剪贴板历史");
    }

    void deleteIndex(std::size_t index) {
        if (index >= history.entries().size()) {
            notifyMessage("请先选择要删除的剪贴板历史");
            return;
        }
        const auto number = std::to_string(index + 1);
        auto *dialog = gtk_message_dialog_new(
            parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
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
        if (history.remove(index, &error)) {
            refresh(false);
            notifyMessage("已删除第 " + number + " 条剪贴板历史");
        } else {
            notifyMessage(error.empty() ? "剪贴板历史删除失败" : error);
        }
    }

    std::optional<std::size_t> selectedHistoryIndex() const {
        if (historyListBox == nullptr) {
            return std::nullopt;
        }
        auto *selectedRow =
            gtk_list_box_get_selected_row(GTK_LIST_BOX(historyListBox));
        if (selectedRow == nullptr) {
            return std::nullopt;
        }
        const auto index = gtk_list_box_row_get_index(selectedRow);
        if (index < 0 || static_cast<std::size_t>(index) >= history.entries().size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(index);
    }

    void updateHistoryActionState() {
        const auto selected = selectedHistoryIndex();
        const bool hasSelected = selected.has_value();
        const bool hasEntries = !history.entries().empty();
        gtk_widget_set_sensitive(copyButton, hasSelected);
        gtk_widget_set_sensitive(deleteButton, hasSelected);
        gtk_widget_set_sensitive(clearButton, hasEntries);
    }

    GtkWidget *buildCardRow(std::size_t index, const std::string &entry) {
        auto *row = gtk_list_box_row_new();
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), TRUE);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);

        auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addStyleClass(card, "modernime-clipboard-card");

        const auto meta = analyzeClipboardContent(entry);

        // Header: [ #1 ] [ badge ] [ meta ]   ...   [ 展开 ] [ 复制 ] [ 删除 ]
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(header, GTK_ALIGN_CENTER);

        auto *indexLabel = gtk_label_new(("#" + std::to_string(index + 1)).c_str());
        addStyleClass(indexLabel, "modernime-card-index");
        gtk_box_pack_start(GTK_BOX(header), indexLabel, FALSE, FALSE, 0);

        auto *badgeLabel = gtk_label_new(meta.typeBadge.c_str());
        addStyleClass(badgeLabel, meta.badgeClass.c_str());
        gtk_box_pack_start(GTK_BOX(header), badgeLabel, FALSE, FALSE, 0);

        const std::string metaStr = std::to_string(meta.lineCount) + " 行 · " +
                                    std::to_string(meta.charCount) + " 字符";
        auto *metaLabel = gtk_label_new(metaStr.c_str());
        addStyleClass(metaLabel, "modernime-card-meta");
        gtk_box_pack_start(GTK_BOX(header), metaLabel, FALSE, FALSE, 0);

        auto *spacer = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);

        const auto previewText = makeCollapsedPreview(entry);
        auto *previewLabel = gtk_label_new(previewText.c_str());
        addStyleClass(previewLabel, "modernime-card-preview");
        gtk_label_set_xalign(GTK_LABEL(previewLabel), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(previewLabel), TRUE);

        GtkWidget *expandedBox = nullptr;
        if (meta.isLong) {
            auto *toggleBtn = gtk_button_new_with_label("展开");
            addStyleClass(toggleBtn, "modernime-toggle-btn");
            gtk_widget_set_tooltip_text(toggleBtn, "展开查看完整内容");
            gtk_box_pack_start(GTK_BOX(header), toggleBtn, FALSE, FALSE, 0);

            expandedBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            addStyleClass(expandedBox, "modernime-card-expanded-box");
            gtk_widget_set_no_show_all(expandedBox, TRUE);
            gtk_widget_hide(expandedBox);

            auto *fullLabel = gtk_label_new(entry.c_str());
            addStyleClass(fullLabel, "modernime-card-code");
            gtk_label_set_xalign(GTK_LABEL(fullLabel), 0.0f);
            gtk_label_set_line_wrap(GTK_LABEL(fullLabel), TRUE);
            gtk_label_set_selectable(GTK_LABEL(fullLabel), TRUE);
            gtk_widget_show(fullLabel);
            gtk_box_pack_start(GTK_BOX(expandedBox), fullLabel, TRUE, TRUE, 0);

            struct ToggleData {
                Impl *impl;
                GtkWidget *btn;
                GtkWidget *preview;
                GtkWidget *expanded;
                bool isExpanded = false;
            };
            auto *tdata = new ToggleData{this, toggleBtn, previewLabel, expandedBox, false};
            g_signal_connect_data(
                toggleBtn, "clicked",
                G_CALLBACK(+[](GtkButton *b, gpointer d) {
                    auto *td = static_cast<ToggleData *>(d);
                    auto *rowWidget = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
                    if (rowWidget != nullptr && td->impl->historyListBox != nullptr) {
                        gtk_list_box_select_row(GTK_LIST_BOX(td->impl->historyListBox), GTK_LIST_BOX_ROW(rowWidget));
                    }
                    td->isExpanded = !td->isExpanded;
                    if (td->isExpanded) {
                        gtk_button_set_label(b, "收起");
                        gtk_widget_set_tooltip_text(GTK_WIDGET(b), "收起完整内容预览");
                        gtk_widget_hide(td->preview);
                        gtk_widget_show(td->expanded);
                    } else {
                        gtk_button_set_label(b, "展开");
                        gtk_widget_set_tooltip_text(GTK_WIDGET(b), "展开查看完整内容");
                        gtk_widget_hide(td->expanded);
                        gtk_widget_show(td->preview);
                    }
                }),
                tdata,
                [](gpointer d, GClosure *) { delete static_cast<ToggleData *>(d); },
                GConnectFlags(0));
        }

        auto *copyCardBtn = gtk_button_new_with_label("复制");
        addStyleClass(copyCardBtn, "modernime-card-btn");
        gtk_widget_set_tooltip_text(copyCardBtn, "复制此条内容到系统剪贴板");
        gtk_box_pack_start(GTK_BOX(header), copyCardBtn, FALSE, FALSE, 0);

        struct CardActionData {
            Impl *impl;
            std::size_t index;
        };
        auto *cdata = new CardActionData{this, index};
        g_signal_connect_data(
            copyCardBtn, "clicked",
            G_CALLBACK(+[](GtkButton *b, gpointer d) {
                auto *cd = static_cast<CardActionData *>(d);
                auto *rowWidget = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
                if (rowWidget != nullptr && cd->impl->historyListBox != nullptr) {
                    gtk_list_box_select_row(GTK_LIST_BOX(cd->impl->historyListBox), GTK_LIST_BOX_ROW(rowWidget));
                }
                cd->impl->copyIndex(cd->index);
            }),
            cdata,
            [](gpointer d, GClosure *) { delete static_cast<CardActionData *>(d); },
            GConnectFlags(0));

        auto *delCardBtn = gtk_button_new_with_label("删除");
        addStyleClass(delCardBtn, "modernime-card-btn");
        gtk_widget_set_tooltip_text(delCardBtn, "删除此条剪贴板记录");
        gtk_box_pack_start(GTK_BOX(header), delCardBtn, FALSE, FALSE, 0);

        auto *ddata = new CardActionData{this, index};
        g_signal_connect_data(
            delCardBtn, "clicked",
            G_CALLBACK(+[](GtkButton *b, gpointer d) {
                auto *cd = static_cast<CardActionData *>(d);
                auto *rowWidget = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
                if (rowWidget != nullptr && cd->impl->historyListBox != nullptr) {
                    gtk_list_box_select_row(GTK_LIST_BOX(cd->impl->historyListBox), GTK_LIST_BOX_ROW(rowWidget));
                }
                cd->impl->deleteIndex(cd->index);
            }),
            ddata,
            [](gpointer d, GClosure *) { delete static_cast<CardActionData *>(d); },
            GConnectFlags(0));

        gtk_box_pack_start(GTK_BOX(card), header, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card), previewLabel, FALSE, FALSE, 0);
        if (expandedBox != nullptr) {
            gtk_box_pack_start(GTK_BOX(card), expandedBox, FALSE, FALSE, 0);
        }

        gtk_container_add(GTK_CONTAINER(row), card);
        return row;
    }

    void updateSettingsState() {
        gtk_widget_set_sensitive(
            clipboardTrigger,
            gtk_switch_get_active(GTK_SWITCH(clipboardEnabled)));
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
    GtkWidget *historyCount = nullptr;
    GtkWidget *historyState = nullptr;
    GtkWidget *historyListBox = nullptr;
    GtkWidget *historyScrolled = nullptr;
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
