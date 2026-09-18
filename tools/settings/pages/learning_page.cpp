#include "modernime/settings/pages/learning_page.h"

#include "modernime/settings/data_controller.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <array>
#include <chrono>
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
        const auto pathText = "学习数据库：" + path.string();
        learningPath = gtk_label_new(pathText.c_str());
        addStyleClass(learningPath, "modernime-path");
        gtk_widget_set_halign(learningPath, GTK_ALIGN_START);
        gtk_label_set_selectable(GTK_LABEL(learningPath), TRUE);
        setAccessibleWidgetText(learningPath, pathText,
                                "可复制的本地学习数据库路径");
        gtk_label_set_line_wrap(GTK_LABEL(learningPath), TRUE);
        auto *dataSection = createSectionCard(
            "学习数据", "学习记录保存在本地；清空前会自动创建可恢复的备份。");
        gtk_box_pack_start(GTK_BOX(page), dataSection, FALSE, FALSE, 0);
        learningDataFallback = dataSection;
        learningCount = gtk_label_new("正在读取学习记录…");
        addStyleClass(learningCount, "modernime-description");
        gtk_widget_set_halign(learningCount, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(dataSection), learningCount, FALSE, FALSE,
                           0);
        gtk_box_pack_start(GTK_BOX(dataSection), learningPath, FALSE, FALSE,
                           0);

        clearLearningButton = gtk_button_new_with_label("清空学习记录");
        setTarget(clearLearningButton, "learning-data");
        gtk_widget_set_halign(clearLearningButton, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(clearLearningButton,
                                    "备份后清空 ModernIME 的学习排序");
        setAccessibleWidgetText(clearLearningButton, "清空学习记录",
                                "先创建并验证备份，再清空本地学习排序");
        gtk_box_pack_start(GTK_BOX(dataSection), clearLearningButton, FALSE,
                           FALSE, 0);
        g_signal_connect(learningEnabled, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(contextLearning, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(clearLearningButton, "clicked", G_CALLBACK(onClear),
                         this);
        setSettingsFocusChain(
            page, {learningEnabled, contextLearning, learningPath,
                   clearLearningButton});
    }

    void refresh(bool shouldNotify) {
        std::string error;
        const auto count = DataController::learningEntryCount(path, &error);
        if (!error.empty()) {
            gtk_label_set_text(GTK_LABEL(learningCount),
                               "当前学习记录：读取失败");
            gtk_widget_set_sensitive(clearLearningButton, FALSE);
            if (shouldNotify) {
                notifyMessage(error);
            }
            return;
        }
        gtk_label_set_text(
            GTK_LABEL(learningCount),
            ("当前学习记录：" + std::to_string(count) + " 条").c_str());
        gtk_widget_set_sensitive(clearLearningButton, count != 0);
        if (shouldNotify) {
            notifyMessage("学习记录统计已刷新");
        }
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

    static void onClear(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "清空后将无法恢复当前学习排序，是否先备份并清空？");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "备份并清空",
            "创建并验证备份后清空本地学习排序");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留当前学习记录");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        gtk_widget_set_sensitive(impl->clearLearningButton, FALSE);
        const auto backup = learningBackupPath(impl->path);
        std::string error;
        if (DataController::backupAndClearLearning(impl->path, backup, &error)) {
            impl->notifyMessage("学习记录已清空，备份位于 " + backup.string());
        } else {
            impl->notifyMessage(error);
        }
        impl->refresh(false);
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
    GtkWidget *learningPath = nullptr;
    GtkWidget *learningCount = nullptr;
    GtkWidget *clearLearningButton = nullptr;
    GtkWidget *learningDataFallback = nullptr;
    bool refreshing = false;
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
