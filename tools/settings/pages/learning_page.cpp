#include "modernime/settings/pages/learning_page.h"

#include "modernime/settings/data_controller.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
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
            "智能学习", "让 ModernIME 记住你的候选选择，并结合上下文优化排序"));
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
        auto *learningSection = createSectionCard(
            "学习行为", "关闭后不会删除已经保存的学习数据。");
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
                                            "记录您主动选择的高频候选词，自适应学习并调频置顶。",
                                            learningEnabled),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(learningSection),
                           createSettingRow("根据前后文调整候选排序",
                                            "根据输入光标前后的上下文语境动态推荐候选词。",
                                            contextLearning),
                           FALSE, FALSE, 0);
        auto *dataSection = createSectionCard(
            "学习数据", "学习记录保存在本地；清空前会自动创建可恢复的备份。");
        gtk_box_pack_start(GTK_BOX(page), dataSection, FALSE, FALSE, 0);
        learningDataFallback = dataSection;

        clearLearningButton = gtk_button_new_with_label("清空学习记录");
        setTarget(clearLearningButton, "learning-clear");
        gtk_widget_set_margin_top(clearLearningButton, 8);
        gtk_widget_set_halign(clearLearningButton, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(clearLearningButton,
                                    "备份后清空本地已记录的学习排序数据");
        setAccessibleWidgetText(clearLearningButton, "清空学习记录",
                                "先创建并验证备份，清空本地学习排序");
        gtk_box_pack_start(GTK_BOX(dataSection), clearLearningButton, FALSE,
                           FALSE, 0);
        g_signal_connect(learningEnabled, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(contextLearning, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(clearLearningButton, "clicked", G_CALLBACK(onClear),
                         this);
        setSettingsFocusChain(
            page, {learningEnabled, contextLearning, clearLearningButton});
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
                if (taskData->generation != owner.refreshGeneration_) {
                    return;
                }
                if (!taskData->error.empty()) {
                    g_warning("[ModernIME Learning] Storage=%s, path=%s, error=%s",
                              taskData->backendInfo.name.c_str(), taskData->path.c_str(),
                              taskData->error.c_str());
                    gtk_widget_set_sensitive(owner.clearLearningButton, FALSE);
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
                if (taskData->shouldNotify) {
                    owner.notifyMessage("学习记录统计已刷新");
                }
            });
            delete taskData;
        }
    }

    void refresh(bool shouldNotify) {
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
                    owner.notifyMessage("学习记录已清空，备份位于 " + taskData->backup.string());
                } else {
                    g_warning("[ModernIME Learning] Failed to clear learning data: %s",
                              taskData->error.c_str());
                    owner.notifyMessage(taskData->error);
                }
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
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "清空前会自动创建可恢复的备份，是否确认清空所有学习记录？");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "备份并清空",
            "创建备份并清空本地学习记录");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留当前学习记录");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        if (impl->isClearing_) {
            return;
        }

        impl->isClearing_ = true;
        ++impl->refreshGeneration_;
        gtk_widget_set_sensitive(impl->clearLearningButton, FALSE);

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
    GtkWidget *learningEnabled = nullptr;
    GtkWidget *contextLearning = nullptr;
    GtkWidget *clearLearningButton = nullptr;
    GtkWidget *learningDataFallback = nullptr;
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

void LearningPage::refreshSettings() {
    impl_->refreshSettings();
}

bool LearningPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
