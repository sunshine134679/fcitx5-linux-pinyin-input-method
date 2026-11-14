#include "modernime/settings/pages/learning_page.h"

#include "modernime/settings/data_controller.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/learning_manager.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <array>
#include <chrono>
#include <string>
#include <utility>

namespace modernime::settings {
namespace {

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

std::filesystem::path learningBackupPath(const std::filesystem::path &path) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    auto filename = path.filename().string();
    if (filename.empty()) {
        filename = "learning.sqlite3";
    }
    filename += ".backup-" + std::to_string(now) + ".sqlite3";
    return path.parent_path() / filename;
}

} // namespace

class LearningPage::Impl final {
public:
    Impl(SettingsWindowModel &settingsModel, std::filesystem::path learningStore,
         std::function<void()> settingsChangedCallback,
         std::function<void(std::string)> notifyCallback)
        : model(settingsModel), path(std::move(learningStore)),
          settingsChanged(std::move(settingsChangedCallback)),
          notify(std::move(notifyCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "学习记录", ""));
        page = pageGuard.get();
        buildPage();
        refreshSettings();
        refresh(false);
        pageGuard.release();
    }

    ~Impl() {
        if (lifetime) {
            lifetime->deactivate();
        }
    }

    void buildPage() {
        learningSection = createSectionCard(
            "学习行为", "");
        gtk_box_pack_start(GTK_BOX(page), learningSection, FALSE, FALSE, 0);

        learningEnabled = gtk_switch_new();
        contextLearning = gtk_switch_new();
        setTarget(learningEnabled, "learning-enabled");
        setTarget(contextLearning, "context-learning");
        gtk_widget_set_tooltip_text(learningEnabled, "记录你主动选择的候选词");
        gtk_widget_set_tooltip_text(contextLearning,
                                    "根据输入前后的文字调整候选顺序");
        setAccessibleWidgetText(learningEnabled, "记忆用户候选选择",
                                "启用或暂停本地记录用户候选选择");
        setAccessibleWidgetText(contextLearning,
                                "根据光标前后文调整候选排序",
                                "启用或关闭本地上下文候选排序");
        gtk_box_pack_start(GTK_BOX(learningSection),
                           createSettingRow("记忆用户候选选择",
                                            "",
                                            learningEnabled),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(learningSection),
                           createSettingRow("根据前后文调整候选排序",
                                            "",
                                            contextLearning),
                           FALSE, FALSE, 0);
        auto *dataSection = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(dataSection), "modernime-learning-storage-state");
        gtk_box_pack_start(GTK_BOX(page), dataSection, FALSE, FALSE, 0);
        learningDataFallback = dataSection;

        const auto dataLabel = [dataSection](const char *target) {
            auto *label = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
            gtk_widget_set_hexpand(label, TRUE);
            setTarget(label, target);
            gtk_box_pack_start(GTK_BOX(dataSection), label, FALSE, FALSE, 0);
            return label;
        };
        dataStatus = dataLabel("learning-status");
        gtk_style_context_add_class(gtk_widget_get_style_context(dataStatus), "modernime-data-summary");
        operationResult = dataLabel("learning-operation-result");
        gtk_label_set_selectable(GTK_LABEL(operationResult), TRUE);
        gtk_widget_set_no_show_all(operationResult, TRUE);

        retryButton = gtk_button_new_with_label("重试");
        setTarget(retryButton, "learning-retry");
        setAccessibleWidgetText(retryButton, "重新读取学习记录", "重新读取学习数据统计，不修改记录");
        gtk_widget_set_halign(retryButton, GTK_ALIGN_START);
        gtk_widget_set_no_show_all(retryButton, TRUE);
        gtk_box_pack_start(GTK_BOX(dataSection), retryButton, FALSE, FALSE, 0);
        g_signal_connect(retryButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            static_cast<Impl *>(data)->refresh(false);
        }), this);

        clearLearningButton = gtk_button_new_with_label("清空学习记录");
        gtk_style_context_add_class(gtk_widget_get_style_context(clearLearningButton), "destructive-action");
        gtk_widget_set_sensitive(clearLearningButton, FALSE);
        setTarget(clearLearningButton, "learning-clear");
        gtk_widget_set_margin_top(clearLearningButton, 0);
        gtk_widget_set_halign(clearLearningButton, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(clearLearningButton,
                                    "备份后清空本地已记录的学习排序数据");
        setAccessibleWidgetText(clearLearningButton, "清空学习记录",
                                "先创建并验证备份，清空本地学习排序");
        g_signal_connect(learningEnabled, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(contextLearning, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(clearLearningButton, "clicked", G_CALLBACK(onClear),
                         this);
        records = std::make_unique<LearningManager>([life = lifetime](std::string message) {
            life->withOwner([&](Impl &owner) {
                owner.refresh(false, false);
                owner.notifyMessage(std::move(message));
            });
        });
        gtk_box_pack_start(GTK_BOX(page), records->widget(), TRUE, TRUE, 0);
        gtk_box_reorder_child(GTK_BOX(page), records->widget(), 2);
        auto *footer = GTK_WIDGET(g_object_get_data(G_OBJECT(records->widget()), "modernime-learning-footer"));
        gtk_box_pack_end(GTK_BOX(footer), clearLearningButton, FALSE, FALSE, 0);
        gtk_container_unset_focus_chain(GTK_CONTAINER(footer));
        learningDataFallback = records->widget();
        setSettingsFocusChain(
            page, {learningEnabled, contextLearning, records->widget(), retryButton});
    }

    struct RefreshTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        uint64_t generation = 0;
        bool shouldNotify = false;
        std::size_t count = 0;
        LearningBackendInfo backendInfo;
        std::string error;
    };

    static void onRefreshFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *taskData = static_cast<RefreshTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (taskData != nullptr) {
            taskData->lifetime->withOwner([taskData](Impl &owner) {
                if (taskData->generation != owner.refreshGeneration_ || owner.isClearing_) {
                    return;
                }
                if (!taskData->error.empty()) {
                    g_warning("[ModernIME Learning] Storage=%s, path=%s, error=%s",
                              taskData->backendInfo.name.c_str(), taskData->path.c_str(),
                              taskData->error.c_str());
                    gtk_widget_set_sensitive(owner.clearLearningButton, FALSE);
                    const auto message = "读取失败：" + taskData->error;
                    owner.setDataStatus(message, true);
                    gtk_widget_show(owner.retryButton);
                    if (taskData->shouldNotify) {
                        owner.notifyMessage(taskData->error);
                    }
                    return;
                }
                g_message("[ModernIME Learning] Storage=%s (%s), entries=%zu, path=%s",
                          taskData->backendInfo.name.c_str(),
                          taskData->backendInfo.description.c_str(),
                          taskData->count, taskData->path.c_str());
                gtk_widget_set_sensitive(owner.clearLearningButton,
                                         !owner.isClearing_ && taskData->count != 0);
                const auto message = taskData->count == 0
                    ? std::string("暂无学习记录")
                    : "已保存 " + std::to_string(taskData->count) + " 条学习记录";
                owner.setDataStatus(message);
                if (taskData->shouldNotify) {
                    owner.notifyMessage("学习记录统计已刷新");
                }
            });
            delete taskData;
        }
    }

    void refresh(bool shouldNotify, bool includeRecords = true) {
        if (isClearing_) {
            return;
        }
        if (includeRecords && records) {
            gtk_widget_set_sensitive(records->widget(), TRUE);
            records->refresh();
        }
        setDataStatus("正在读取学习记录…");
        gtk_widget_hide(retryButton);
        ++refreshGeneration_;
        auto *taskData = new RefreshTaskData{lifetime, path, refreshGeneration_, shouldNotify, 0, {}, {}};
        auto *task = g_task_new(nullptr, nullptr, onRefreshFinished, nullptr);
        g_task_set_task_data(task, taskData, nullptr);
        g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer data, GCancellable *) {
            auto *td = static_cast<RefreshTaskData *>(data);
            td->count = DataController::learningEntryCount(td->path, &td->error);
            td->backendInfo = DataController::detectActiveLearningBackend(td->path);
            g_task_return_pointer(t, td, nullptr);
        });
        g_object_unref(task);
    }

    void refreshSettings() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_switch_set_active(GTK_SWITCH(learningEnabled),
                              settings.learningEnabled);
        gtk_switch_set_active(GTK_SWITCH(contextLearning),
                              settings.contextLearningEnabled);
        refreshing = false;
    }

    bool focusTarget(std::string_view target) {
        if (records && records->focusTarget(target)) return true;
        if (target == "learning-clear" || target == "learning-data") {
            return focusWidgetOrFallback(clearLearningButton, learningDataFallback);
        }
        const std::array controls{
            std::pair{learningEnabled, static_cast<GtkWidget *>(nullptr)},
            std::pair{contextLearning, static_cast<GtkWidget *>(nullptr)},
            std::pair{clearLearningButton, learningDataFallback},
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

    GtkWidget *takeBehaviorWidget() {
        if (behaviorTaken || !learningSection || !gtk_widget_get_parent(learningSection)) return nullptr;
        behaviorTaken = true;
        g_object_ref(learningSection);
        gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(learningSection)), learningSection);
        setSettingsFocusChain(page, {records->widget(), retryButton});
        return learningSection;
    }
    void setDataStatus(const std::string &message, bool attention = false) {
        storageAttention = attention;
        gtk_label_set_text(GTK_LABEL(dataStatus), message.c_str());
        gtk_widget_set_visible(dataStatus, !isEmbedded || attention);
    }
    void setEmbedded(bool embedded) {
        isEmbedded = embedded;
        gtk_widget_set_margin_start(page, embedded ? 0 : 24);
        gtk_widget_set_margin_end(page, embedded ? 0 : 24);
        gtk_widget_set_margin_top(page, embedded ? 0 : 20);
        gtk_widget_set_margin_bottom(page, embedded ? 0 : 18);
        gtk_box_set_spacing(GTK_BOX(page), embedded ? 0 : 12);
        gtk_widget_set_no_show_all(dataStatus, embedded);
        gtk_widget_set_visible(dataStatus, !embedded || storageAttention);
        auto *header = GTK_WIDGET(g_object_get_data(G_OBJECT(page), "modernime-page-header"));
        if (header) {
            gtk_widget_set_no_show_all(header, embedded);
            gtk_widget_set_visible(header, !embedded);
        }
    }
    GtkWidget *widget() const { return page; }

    bool hasPendingOperations() const { return isClearing_ || (records && g_object_get_data(G_OBJECT(records->widget()), "modernime-learning-mutation-busy")); }

private:
    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        onChanged(nullptr, data);
    }

    static void onChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        auto settings = impl->model.settings();
        settings.learningEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->learningEnabled));
        settings.contextLearningEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->contextLearning));
        impl->model.setSettings(std::move(settings));
        impl->settingsChanged();
    }

    struct ClearTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        std::filesystem::path backup;
        bool success = false;
        std::string error;
    };

    static void onClearFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *taskData = static_cast<ClearTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (taskData != nullptr) {
            taskData->lifetime->withOwner([taskData](Impl &owner) {
                owner.isClearing_ = false;
                if (taskData->success) {
                    g_message("[ModernIME Learning] Cleared learning data, backup created at: %s",
                              taskData->backup.c_str());
                    const auto message = "学习记录已清空，备份位于 " + taskData->backup.string();
                    gtk_label_set_text(GTK_LABEL(owner.operationResult), message.c_str());
                    owner.notifyMessage(message);
                } else {
                    g_warning("[ModernIME Learning] Failed to clear learning data: %s",
                              taskData->error.c_str());
                    owner.notifyMessage(taskData->error);
                    const auto message = "清空失败：" + taskData->error;
                    gtk_label_set_text(GTK_LABEL(owner.operationResult), message.c_str());
                }
                gtk_widget_show(owner.operationResult);
                owner.refresh(false);
            });
            delete taskData;
        }
    }

    static void onClear(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->isClearing_) {
            return;
        }
        if (impl->records && g_object_get_data(G_OBJECT(impl->records->widget()), "modernime-learning-mutation-busy")) {
            impl->notifyMessage("请等待当前学习记录操作完成。");
            return;
        }
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "清空学习记录？");
        setAccessibleWidgetText(dialog, "清空学习记录", "确认是否备份并清空本地学习记录");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "将重置已记录的输入习惯。清空前会自动创建备份文件。");
        auto *messageArea = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dialog));
        gtk_box_set_spacing(GTK_BOX(messageArea), 8);
        auto *labels = gtk_container_get_children(GTK_CONTAINER(messageArea));
        for (auto *item = labels; item; item = item->next) {
            if (!GTK_IS_LABEL(item->data)) continue;
            auto *label = GTK_LABEL(item->data);
            gtk_widget_set_halign(GTK_WIDGET(label), GTK_ALIGN_FILL);
            gtk_label_set_xalign(label, 0.0F);
            gtk_label_set_line_wrap_mode(label, PANGO_WRAP_WORD_CHAR);
            gtk_label_set_max_width_chars(label, 30);
        }
        g_list_free(labels);
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "备份并清空",
            "创建备份并清空本地学习记录");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留当前学习记录");
        gtk_style_context_add_class(gtk_widget_get_style_context(
            gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES)), "destructive-action");
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_NO);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        if (impl->isClearing_) {
            return;
        }

        if (impl->records && g_object_get_data(G_OBJECT(impl->records->widget()), "modernime-learning-mutation-busy")) {
            impl->notifyMessage("请等待当前学习记录操作完成。");
            return;
        }

        impl->isClearing_ = true;
        if (impl->records) gtk_widget_set_sensitive(impl->records->widget(), FALSE);
        ++impl->refreshGeneration_;
        gtk_widget_set_sensitive(impl->clearLearningButton, FALSE);
        impl->setDataStatus("正在备份并清空学习记录…", true);
        gtk_widget_hide(impl->operationResult);
        gtk_widget_hide(impl->retryButton);

        const auto backup = learningBackupPath(impl->path);
        auto *taskData = new ClearTaskData{impl->lifetime, impl->path, backup, false, {}};
        auto *task = g_task_new(nullptr, nullptr, onClearFinished, nullptr);
        g_task_set_task_data(task, taskData, nullptr);
        g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer task_data, GCancellable *) {
            auto *td = static_cast<ClearTaskData *>(task_data);
            td->success = DataController::backupAndClearLearning(td->path, td->backup, &td->error);
            g_task_return_pointer(t, td, nullptr);
        });
        g_object_unref(task);
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
    std::filesystem::path path;
    std::function<void()> settingsChanged;
    std::function<void(std::string)> notify;
    GtkWidget *page = nullptr;
    GtkWidget *learningSection = nullptr;
    GtkWidget *learningEnabled = nullptr;
    GtkWidget *contextLearning = nullptr;
    GtkWidget *clearLearningButton = nullptr;
    GtkWidget *learningDataFallback = nullptr;
    GtkWidget *dataStatus = nullptr;
    GtkWidget *operationResult = nullptr;
    GtkWidget *retryButton = nullptr;
    std::unique_ptr<LearningManager> records;
    bool isEmbedded = false;
    bool storageAttention = false;
    bool behaviorTaken = false;
    bool refreshing = false;
    bool isClearing_ = false;
    uint64_t refreshGeneration_ = 0;
    std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime =
        std::make_shared<detail::DiagnosticsLifetime<Impl>>(this);
};

LearningPage::LearningPage(SettingsWindowModel &settings,
                           std::filesystem::path learningPath,
                           std::function<void()> settingsChanged,
                           std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(settings, std::move(learningPath),
                                   std::move(settingsChanged),
                                   std::move(notify))) {}

LearningPage::~LearningPage() = default;

GtkWidget *LearningPage::widget() const {
    return impl_->widget();
}

void LearningPage::refresh(bool notify) {
    impl_->refresh(notify);
}

GtkWidget *LearningPage::takeBehaviorWidget() { return impl_->takeBehaviorWidget(); }
void LearningPage::setEmbedded(bool embedded) { impl_->setEmbedded(embedded); }

void LearningPage::refreshSettings() {
    impl_->refreshSettings();
}

bool LearningPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

bool LearningPage::hasPendingOperations() const { return impl_->hasPendingOperations(); }

} // namespace modernime::settings
