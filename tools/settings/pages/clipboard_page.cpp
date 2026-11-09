#include "modernime/settings/pages/clipboard_page.h"

#include "modernime/settings/clipboard_history_model.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include "modernime/core/clipboard_history.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    struct DisplayItem {
        std::string text;
        ClipboardItemMeta meta;
    };

    struct ReloadTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        uint64_t generation = 0;
        bool shouldNotify = false;
        std::vector<DisplayItem> items;
        std::vector<std::string> rawEntries;
        bool success = false;
        std::string error;
    };

    struct BatchRenderContext {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::size_t nextIndex = 0;
        bool shouldNotify = false;
    };

    struct ClearTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        uint64_t generation = 0;
        bool success = false;
        std::string error;
    };

    struct DeleteTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        uint64_t generation = 0;
        std::size_t index = 0;
        std::string itemNumber;
        std::string expectedText;
        std::vector<std::string> remainingEntries;
        bool success = false;
        std::string error;
    };

    Impl(SettingsWindowModel &settingsModel, std::filesystem::path historyPathArg,
         std::function<void()> settingsChangedCallback,
         std::function<void(std::string)> notifyCallback)
        : model(settingsModel), historyPath(std::move(historyPathArg)),
          history(historyPath),
          settingsChanged(std::move(settingsChangedCallback)),
          notify(std::move(notifyCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "剪贴板", "找回最近复制的文字、链接与代码。历史仅保存在本机。"));
        page = pageGuard.get();
        buildPage();
        refreshSettings();
        pageGuard.release();
    }

    ~Impl() {
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        if (lifetime) {
            lifetime->deactivate();
        }
    }

    void buildPage() {
        auto *settingsDisclosure = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(page), settingsDisclosure, FALSE, FALSE, 0);
        settingsToggle = gtk_toggle_button_new_with_label("剪贴板设置");
        addStyleClass(settingsToggle, "modernime-details-button");
        gtk_widget_set_halign(settingsToggle, GTK_ALIGN_START);
        gtk_widget_set_focus_on_click(settingsToggle, FALSE);
        gtk_button_set_image(GTK_BUTTON(settingsToggle),
                             gtk_image_new_from_icon_name("pan-down-symbolic", GTK_ICON_SIZE_MENU));
        gtk_button_set_image_position(GTK_BUTTON(settingsToggle), GTK_POS_RIGHT);
        gtk_button_set_always_show_image(GTK_BUTTON(settingsToggle), TRUE);
        setAccessibleWidgetText(settingsToggle, "剪贴板设置",
                                "展开或收起剪贴板开关与触发键设置");
        gtk_box_pack_start(GTK_BOX(settingsDisclosure), settingsToggle, FALSE, FALSE, 0);
        settingsRevealer = gtk_revealer_new();
        auto *settingsSection = createSectionCard("", "");
        gtk_container_add(GTK_CONTAINER(settingsRevealer), settingsSection);
        gtk_box_pack_start(GTK_BOX(settingsDisclosure), settingsRevealer, FALSE, FALSE, 0);
        bindSettingsDisclosure(settingsToggle, settingsRevealer,
            gtk_button_get_image(GTK_BUTTON(settingsToggle)), true);

        clipboardEnabled = gtk_switch_new();
        setAccessibleWidgetText(clipboardEnabled, "启用剪贴板",
                                "启用或关闭本地剪贴板历史入口");
        setTarget(clipboardEnabled, "clipboard-enabled");
        gtk_box_pack_start(GTK_BOX(settingsSection),
                           createSettingRow("启用剪贴板",
                                            "在中文输入时快速取用最近复制的内容。",
                                            clipboardEnabled),
                           FALSE, FALSE, 0);

        clipboardTrigger = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(clipboardTrigger), 3);
        gtk_entry_set_width_chars(GTK_ENTRY(clipboardTrigger), 10);
        gtk_entry_set_max_width_chars(GTK_ENTRY(clipboardTrigger), 10);
        gtk_entry_set_placeholder_text(GTK_ENTRY(clipboardTrigger), "例如 V+2");
        setTarget(clipboardTrigger, "clipboard-trigger");
        clipboardTriggerFallback = createSettingRow(
            "触发键",
            "先输入字母，再紧跟数字。例如 V+2。",
            clipboardTrigger);
        setAccessibleWidgetText(clipboardTrigger, "剪贴板触发键",
            "设置在中文输入状态打开本地剪贴板历史的按键");
        setAccessibleWidgetText(
            clipboardTriggerFallback, "剪贴板触发键",
            "剪贴板功能关闭时仍可定位并阅读触发键设置");
        gtk_box_pack_start(GTK_BOX(settingsSection), clipboardTriggerFallback,
                           FALSE, FALSE, 0);
        auto *historySection = createSectionCard(
            "剪贴板历史", "最近内容在前，可双击复制或展开长文本。历史操作完成后无需再保存设置。");
        gtk_box_pack_start(GTK_BOX(page), historySection, TRUE, TRUE, 0);
        auto *historySummary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        historyCount = gtk_label_new("正在读取…");
        addStyleClass(historyCount, "modernime-description");
        gtk_widget_set_halign(historyCount, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(historySummary), historyCount, TRUE, TRUE, 0);
        auto *refreshButton = gtk_button_new_with_label("刷新历史");
        addStyleClass(refreshButton, "modernime-card-btn");
        gtk_widget_set_tooltip_text(refreshButton, "重新读取磁盘上的剪贴板历史");
        setAccessibleWidgetText(refreshButton, "刷新历史",
                                "重新读取磁盘上的剪贴板历史");
        gtk_box_pack_end(GTK_BOX(historySummary), refreshButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(historySection), historySummary, FALSE, FALSE, 0);

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
        gtk_widget_set_size_request(historyScrolled, -1, 160);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(historyScrolled),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(historyScrolled), historyListBox);
        gtk_box_pack_start(GTK_BOX(historySection), historyScrolled, TRUE, TRUE,
                           0);

        historyState = gtk_label_new("正在读取…");
        addStyleClass(historyState, "modernime-description");
        gtk_widget_set_halign(historyState, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(historyState), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(historyState), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(historyState), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_margin_top(historyState, 6);
        gtk_box_pack_start(GTK_BOX(historySection), historyState, FALSE, FALSE,
                           0);

        auto *historyActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        addStyleClass(historyActions, "modernime-clipboard-toolbar");
        const auto actionLabels = settingsClipboardActionLabels();
        copyButton = gtk_button_new_with_label(actionLabels[0].data());
        deleteButton = gtk_button_new_with_label(actionLabels[1].data());
        clearButton = gtk_button_new_with_label(actionLabels[2].data());
        addStyleClass(clearButton, "destructive-action");
        gtk_widget_set_tooltip_text(copyButton, "复制选中的历史内容到系统剪贴板");
        gtk_widget_set_tooltip_text(deleteButton, "删除选中的历史内容");
        gtk_widget_set_tooltip_text(clearButton, "清空全部历史内容，需要确认");
        setAccessibleWidgetText(copyButton, actionLabels[0],
                                "复制选中的历史内容到系统剪贴板");
        setAccessibleWidgetText(deleteButton, actionLabels[1],
                                "删除选中的历史内容");
        setAccessibleWidgetText(clearButton, actionLabels[2],
                                "确认后清空全部本地剪贴板历史");
        for (auto *button : {copyButton, deleteButton}) {
            gtk_box_pack_start(GTK_BOX(historyActions), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_end(GTK_BOX(historyActions), clearButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(historySection), historyActions, FALSE, FALSE,
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
            page, {settingsToggle, clipboardEnabled, clipboardTrigger, historyListBox,
                   copyButton, deleteButton, clearButton, refreshButton});
    }

    void clearHistoryListBox() {
        if (historyListBox == nullptr) {
            return;
        }
        auto *children =
            gtk_container_get_children(GTK_CONTAINER(historyListBox));
        for (auto *c = children; c != nullptr; c = c->next) {
            gtk_widget_destroy(GTK_WIDGET(c->data));
        }
        g_list_free(children);
    }

    void refresh(bool shouldNotify) {
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        ++historyGeneration;
        const auto gen = historyGeneration;
        isLoading = true;
        updateHistoryActionState();
        gtk_label_set_text(GTK_LABEL(historyCount), "正在读取…");

        auto *data = new ReloadTaskData{
            lifetime, historyPath, gen, shouldNotify, {}, {}, false, {}
        };

        auto *task = g_task_new(nullptr, nullptr, onReloadFinished, nullptr);
        g_task_set_task_data(task, data, nullptr);
        g_task_run_in_thread(task, onReloadWorker);
        g_object_unref(task);
    }

    static void onReloadWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
        auto *td = static_cast<ReloadTaskData *>(taskData);
        core::ClipboardHistory rawHistory;
        if (!rawHistory.load(td->path, &td->error)) {
            td->success = false;
        } else {
            td->success = true;
            td->rawEntries = rawHistory.entries();
            td->items.reserve(td->rawEntries.size());
            for (const auto &entry : td->rawEntries) {
                td->items.push_back({entry, analyzeClipboardContent(entry)});
            }
        }
        g_task_return_pointer(task, td, nullptr);
    }

    static void onReloadFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *td = static_cast<ReloadTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (td != nullptr) {
            td->lifetime->withOwner([td](Impl &owner) {
                if (td->generation != owner.historyGeneration) {
                    return;
                }
                owner.isLoading = false;
                if (!td->success) {
                    owner.displayItems.clear();
                    gtk_label_set_text(GTK_LABEL(owner.historyCount), "历史读取失败");
                    gtk_label_set_text(
                        GTK_LABEL(owner.historyState),
                        td->error.empty() ? "无法读取剪贴板历史"
                                          : ("无法读取剪贴板历史：" + td->error).c_str());
                    gtk_widget_set_visible(owner.historyState, TRUE);
                    owner.clearHistoryListBox();
                    owner.updateHistoryActionState();
                    if (td->shouldNotify) {
                        owner.notifyMessage(td->error);
                    }
                    return;
                }
                owner.history = ClipboardHistoryModel(owner.historyPath, std::move(td->rawEntries));
                owner.displayItems = std::move(td->items);
                owner.startBatchRender(td->generation, td->shouldNotify);
            });
            delete td;
        }
    }

    void startBatchRender(uint64_t gen, bool shouldNotify) {
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        clearHistoryListBox();

        if (displayItems.empty()) {
            gtk_label_set_text(GTK_LABEL(historyState),
                               "还没有剪贴板历史\n复制一段文字后，可在这里查看和再次使用。若尚未出现，请刷新历史。");
            gtk_widget_set_visible(historyState, TRUE);
            gtk_label_set_text(
                GTK_LABEL(historyCount),
                ("0 / " + std::to_string(core::ClipboardHistory::kMaxEntries) + " 条记录")
                    .c_str());
            updateHistoryActionState();
            if (shouldNotify) {
                notifyMessage("剪贴板历史已刷新");
            }
            return;
        }

        gtk_label_set_text(GTK_LABEL(historyState), "");
        gtk_widget_set_visible(historyState, FALSE);

        auto *ctx = new BatchRenderContext{lifetime, gen, 0, shouldNotify};
        idleBatchSourceId = g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            onBatchIdle,
            ctx,
            [](gpointer d) { delete static_cast<BatchRenderContext *>(d); });
    }

    static gboolean onBatchIdle(gpointer data) {
        auto *ctx = static_cast<BatchRenderContext *>(data);
        bool keepGoing = false;
        const bool alive = ctx->lifetime->withOwner([ctx, &keepGoing](Impl &owner) {
            if (ctx->generation != owner.historyGeneration) {
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            while (ctx->nextIndex < owner.displayItems.size()) {
                const auto idx = ctx->nextIndex;
                auto *row = owner.buildCardRow(idx, owner.displayItems[idx]);
                gtk_container_add(GTK_CONTAINER(owner.historyListBox), row);
                ++ctx->nextIndex;

                if (ctx->nextIndex % 10 == 0 ||
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start) >= std::chrono::milliseconds(4)) {
                    break;
                }
            }
            if (ctx->nextIndex < owner.displayItems.size()) {
                keepGoing = true;
            } else {
                owner.idleBatchSourceId = 0;
                gtk_widget_show_all(owner.historyListBox);
                gtk_label_set_text(
                    GTK_LABEL(owner.historyCount),
                    (std::to_string(owner.displayItems.size()) + " / " +
                     std::to_string(core::ClipboardHistory::kMaxEntries) + " 条记录")
                        .c_str());
                auto *firstRow = gtk_list_box_get_row_at_index(
                    GTK_LIST_BOX(owner.historyListBox), 0);
                if (firstRow != nullptr) {
                    gtk_list_box_select_row(GTK_LIST_BOX(owner.historyListBox), firstRow);
                }
                owner.updateHistoryActionState();
                if (ctx->shouldNotify) {
                    owner.notifyMessage("剪贴板历史已刷新");
                }
            }
        });
        return (alive && keepGoing) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
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
                if (control == clipboardEnabled || control == clipboardTrigger) {
                    setSettingsDisclosureExpanded(settingsToggle, true, false);
                }
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
        if (impl->isLoading || impl->isOperating) {
            return;
        }
        const auto selected = impl->selectedHistoryIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要删除的剪贴板历史");
            return;
        }
        impl->deleteIndex(*selected);
    }

    static void onClear(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->isLoading || impl->isOperating) {
            return;
        }
        if (impl->displayItems.empty()) {
            impl->notifyMessage("当前没有可清空的剪贴板历史");
            return;
        }
        const auto count = std::to_string(impl->displayItems.size());
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "确定清空全部 %s 条剪贴板历史吗？此操作不可撤销。", count.c_str());
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "清空",
            "清空全部本地剪贴板历史且无法撤销");
        addStyleClass(gtk_dialog_get_widget_for_response(
                          GTK_DIALOG(dialog), GTK_RESPONSE_YES),
                      "destructive-action");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留全部剪贴板历史");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        impl->isOperating = true;
        impl->updateHistoryActionState();
        auto *taskData = new ClearTaskData{
            impl->lifetime, impl->historyPath, impl->historyGeneration, false, {}
        };
        auto *task = g_task_new(nullptr, nullptr, onClearFinished, nullptr);
        g_task_set_task_data(task, taskData, nullptr);
        g_task_run_in_thread(task, onClearWorker);
        g_object_unref(task);
    }

    static void onClearWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
        auto *td = static_cast<ClearTaskData *>(taskData);
        ClipboardHistoryModel rawModel(td->path);
        td->success = rawModel.clear(&td->error);
        g_task_return_pointer(task, td, nullptr);
    }

    static void onClearFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *td = static_cast<ClearTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (td != nullptr) {
            td->lifetime->withOwner([td](Impl &owner) {
                owner.isOperating = false;
                if (td->generation != owner.historyGeneration) {
                    return;
                }
                if (td->success) {
                    owner.refresh(false);
                    owner.notifyMessage("剪贴板历史已清空");
                } else {
                    owner.updateHistoryActionState();
                    owner.notifyMessage(td->error.empty() ? "剪贴板历史清空失败" : td->error);
                }
            });
            delete td;
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
        if (index >= displayItems.size()) {
            notifyMessage("请先选择要复制的剪贴板历史");
            return;
        }
        const auto &entry = displayItems[index].text;
        auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (clipboard == nullptr) {
            notifyMessage("系统剪贴板不可用");
            return;
        }
        gtk_clipboard_set_text(clipboard, entry.c_str(), -1);
        notifyMessage("已复制第 " + std::to_string(index + 1) + " 条剪贴板历史");
    }

    void deleteIndex(std::size_t index) {
        if (isLoading || isOperating) {
            return;
        }
        if (index >= displayItems.size()) {
            notifyMessage("请先选择要删除的剪贴板历史");
            return;
        }
        const auto expectedText = displayItems[index].text;
        const auto initialGeneration = historyGeneration;
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

        std::size_t targetIndex = index;
        if (historyGeneration != initialGeneration) {
            auto it = std::find_if(displayItems.begin(), displayItems.end(),
                                   [&](const DisplayItem &item) { return item.text == expectedText; });
            if (it == displayItems.end()) {
                notifyMessage("选中的剪贴板历史已不存在");
                return;
            }
            targetIndex = static_cast<std::size_t>(std::distance(displayItems.begin(), it));
        }

        isOperating = true;
        updateHistoryActionState();
        auto *taskData = new DeleteTaskData{
            lifetime, historyPath, historyGeneration, targetIndex, number, expectedText, {}, false, {}
        };
        auto *task = g_task_new(nullptr, nullptr, onDeleteFinished, nullptr);
        g_task_set_task_data(task, taskData, nullptr);
        g_task_run_in_thread(task, onDeleteWorker);
        g_object_unref(task);
    }

    static void onDeleteWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
        auto *td = static_cast<DeleteTaskData *>(taskData);
        ClipboardHistoryModel rawModel(td->path);
        td->success = rawModel.remove(td->index, td->expectedText, &td->error);
        if (td->success) {
            td->remainingEntries = rawModel.entries();
        }
        g_task_return_pointer(task, td, nullptr);
    }

    static void onDeleteFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *td = static_cast<DeleteTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (td != nullptr) {
            td->lifetime->withOwner([td](Impl &owner) {
                owner.isOperating = false;
                if (td->generation != owner.historyGeneration) {
                    return;
                }
                if (td->success) {
                    owner.history = ClipboardHistoryModel(owner.historyPath, std::move(td->remainingEntries));
                    owner.refresh(false);
                    owner.notifyMessage("已删除第 " + td->itemNumber + " 条剪贴板历史");
                } else {
                    owner.updateHistoryActionState();
                    owner.notifyMessage(td->error.empty() ? "剪贴板历史删除失败" : td->error);
                }
            });
            delete td;
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
        if (index < 0 || static_cast<std::size_t>(index) >= displayItems.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(index);
    }

    void updateHistoryActionState() {
        const auto selected = selectedHistoryIndex();
        const bool hasSelected = selected.has_value();
        const bool hasEntries = !displayItems.empty();
        const bool busy = isLoading || isOperating;
        gtk_widget_set_sensitive(copyButton, !busy && hasSelected);
        gtk_widget_set_sensitive(deleteButton, !busy && hasSelected);
        gtk_widget_set_sensitive(clearButton, !busy && hasEntries);
    }

    GtkWidget *buildCardRow(std::size_t index, const DisplayItem &item) {
        auto *row = gtk_list_box_row_new();
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), TRUE);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);

        auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addStyleClass(card, "modernime-clipboard-card");

        const auto &meta = item.meta;

        // Secondary metadata stays on one line; the content has its own reading area.
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(header, GTK_ALIGN_CENTER);

        const char *typeName = meta.badgeClass == "modernime-badge-code" ? "代码" :
                               meta.badgeClass == "modernime-badge-url" ? "链接" :
                               meta.badgeClass == "modernime-badge-multiline" ? "多行文本" : "文本";
        const char *typeIcon = meta.badgeClass == "modernime-badge-code" ? "text-x-script-symbolic" :
                               meta.badgeClass == "modernime-badge-url" ? "insert-link-symbolic" :
                               "text-x-generic-symbolic";
        auto *typeBadge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        addStyleClass(typeBadge, "modernime-clipboard-type");
        addStyleClass(typeBadge, meta.badgeClass.c_str());
        gtk_widget_set_valign(typeBadge, GTK_ALIGN_CENTER);
        auto *typeImage = gtk_image_new_from_icon_name(typeIcon, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(typeImage), 14);
        addStyleClass(typeImage, "modernime-clipboard-type-icon");
        gtk_box_pack_start(GTK_BOX(typeBadge), typeImage, FALSE, FALSE, 0);
        auto *badgeLabel = gtk_label_new(typeName);
        gtk_box_pack_start(GTK_BOX(typeBadge), badgeLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(header), typeBadge, FALSE, FALSE, 0);

        const std::string metaStr =
            (meta.lineCount > 1 ? std::to_string(meta.lineCount) + " 行 · " : "") +
            std::to_string(meta.charCount) + " 字";
        auto *metaLabel = gtk_label_new(metaStr.c_str());
        addStyleClass(metaLabel, "modernime-card-meta");
        gtk_box_pack_start(GTK_BOX(header), metaLabel, FALSE, FALSE, 0);

        auto *spacer = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);

        auto *previewLabel = gtk_label_new(meta.preview.c_str());
        addStyleClass(previewLabel, "modernime-card-preview");
        gtk_label_set_xalign(GTK_LABEL(previewLabel), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(previewLabel), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(previewLabel), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_ellipsize(GTK_LABEL(previewLabel), PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(GTK_LABEL(previewLabel), 2);
        gtk_widget_set_hexpand(previewLabel, TRUE);
        if (meta.badgeClass == "modernime-badge-code") {
            addStyleClass(previewLabel, "modernime-card-code");
        } else if (meta.badgeClass == "modernime-badge-url") {
            addStyleClass(previewLabel, "modernime-card-url");
        }

        auto *previewRevealer = gtk_revealer_new();
        setSettingsRevealed(previewRevealer, true, false);
        gtk_container_add(GTK_CONTAINER(previewRevealer), previewLabel);

        GtkWidget *expandedBox = nullptr;
        GtkWidget *expandedRevealer = nullptr;
        if (meta.isLong || meta.lineCount > 1 || meta.charCount > 40) {
            auto *toggleBtn = gtk_button_new_with_label("展开");
            addStyleClass(toggleBtn, "modernime-toggle-btn");
            gtk_widget_set_tooltip_text(toggleBtn, "展开查看完整内容");
            gtk_widget_set_no_show_all(toggleBtn, TRUE);
            gtk_widget_set_visible(toggleBtn, meta.isLong);
            g_object_set_data(G_OBJECT(toggleBtn), "clipboard-preview-truncated",
                               GINT_TO_POINTER(meta.isLong));
            g_signal_connect_object(
                previewLabel, "size-allocate",
                G_CALLBACK(+[](GtkWidget *label, GtkAllocation *, gpointer button) {
                    auto *preview = gtk_widget_get_parent(label);
                    if (gtk_widget_get_visible(label) &&
                        gtk_revealer_get_reveal_child(GTK_REVEALER(preview))) {
                        const bool truncated = g_object_get_data(
                            G_OBJECT(button), "clipboard-preview-truncated") != nullptr;
                        gtk_widget_set_visible(GTK_WIDGET(button), truncated ||
                            pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(label))));
                    }
                }), toggleBtn, GConnectFlags(0));
            gtk_box_pack_start(GTK_BOX(header), toggleBtn, FALSE, FALSE, 0);

            expandedBox = gtk_scrolled_window_new(nullptr, nullptr);
            addStyleClass(expandedBox, "modernime-card-expanded-box");
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(expandedBox),
                                           GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
            gtk_widget_set_size_request(expandedBox, -1, 180);
            expandedRevealer = gtk_revealer_new();
            gtk_container_add(GTK_CONTAINER(expandedRevealer), expandedBox);

            struct ToggleData {
                Impl *impl;
                GtkWidget *preview;
                GtkWidget *expanded;
                GtkWidget *revealer;
                std::size_t index;
                bool isCode;
            };
            auto *tdata = new ToggleData{this, previewRevealer, expandedBox,
                                         expandedRevealer, index,
                                         meta.badgeClass == "modernime-badge-code"};
            g_signal_connect_data(
                toggleBtn, "clicked",
                G_CALLBACK(+[](GtkButton *b, gpointer d) {
                    auto *td = static_cast<ToggleData *>(d);
                    auto *rowWidget = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
                    if (rowWidget != nullptr && td->impl->historyListBox != nullptr) {
                        gtk_list_box_select_row(GTK_LIST_BOX(td->impl->historyListBox), GTK_LIST_BOX_ROW(rowWidget));
                    }
                    const bool isExpanded =
                        !gtk_revealer_get_reveal_child(GTK_REVEALER(td->revealer));
                    if (isExpanded) {
                        // Allocate the full text only when requested, not for all 30 records.
                        if (gtk_bin_get_child(GTK_BIN(td->expanded)) == nullptr) {
                            auto *reader = gtk_text_view_new();
                            gtk_text_view_set_editable(GTK_TEXT_VIEW(reader), FALSE);
                            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(reader), FALSE);
                            gtk_text_view_set_monospace(GTK_TEXT_VIEW(reader), td->isCode);
                            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(reader), GTK_WRAP_WORD_CHAR);
                            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(reader), 12);
                            gtk_text_view_set_right_margin(GTK_TEXT_VIEW(reader), 12);
                            gtk_text_view_set_top_margin(GTK_TEXT_VIEW(reader), 10);
                            gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(reader), 10);
                            addStyleClass(reader, "modernime-clipboard-reader");
                            if (td->isCode) {
                                addStyleClass(reader, "modernime-card-code");
                            }
                            if (td->index < td->impl->displayItems.size()) {
                                const auto &text = td->impl->displayItems[td->index].text;
                                gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(reader)),
                                                         text.c_str(), static_cast<gint>(text.size()));
                            }
                            setAccessibleWidgetText(reader, "剪贴板完整内容", "可选择文字，内容不会被编辑");
                            gtk_container_add(GTK_CONTAINER(td->expanded), reader);
                            gtk_widget_show(reader);
                        }
                        gtk_button_set_label(b, "收起");
                        gtk_widget_set_tooltip_text(GTK_WIDGET(b), "收起完整内容预览");
                    } else {
                        auto *window = gtk_widget_get_toplevel(GTK_WIDGET(b));
                        auto *focus = GTK_IS_WINDOW(window)
                            ? gtk_window_get_focus(GTK_WINDOW(window)) : nullptr;
                        if (focus != nullptr && gtk_widget_is_ancestor(focus, td->expanded)) {
                            gtk_widget_grab_focus(GTK_WIDGET(b));
                        }
                        gtk_button_set_label(b, "展开");
                        gtk_widget_set_tooltip_text(GTK_WIDGET(b), "展开查看完整内容");
                    }
                    setSettingsRevealed(td->preview, !isExpanded);
                    setSettingsRevealed(td->revealer, isExpanded);
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

        gtk_box_pack_start(GTK_BOX(card), header, FALSE, FALSE, 0);
        auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(content), previewRevealer, FALSE, FALSE, 0);
        if (expandedRevealer != nullptr) {
            gtk_box_pack_start(GTK_BOX(content), expandedRevealer, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(card), content, FALSE, FALSE, 0);

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
    std::filesystem::path historyPath;
    ClipboardHistoryModel history;
    std::function<void()> settingsChanged;
    std::function<void(std::string)> notify;
    GtkWidget *page = nullptr;
    GtkWidget *settingsToggle = nullptr;
    GtkWidget *settingsRevealer = nullptr;
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

    std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime =
        std::make_shared<detail::DiagnosticsLifetime<Impl>>(this);
    uint64_t historyGeneration = 0;
    guint idleBatchSourceId = 0;
    bool isLoading = false;
    bool isOperating = false;
    std::vector<DisplayItem> displayItems;
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
