#include "modernime/settings/pages/updates_page.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/settings/update_controller.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

#ifndef MODERNIME_VERSION
#define MODERNIME_VERSION "0.1.22"
#endif

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

enum class UpdateActionKind {
    CheckOnly,
    RefreshCache,
};

struct UpdateCheckTask final {
    UpdateActionKind action = UpdateActionKind::CheckOnly;
    Environment environment;
    UpdateStatus currentStatus;
};

struct UpdateRefreshResult final {
    RuntimeResult cacheResult;
    UpdateStatus updatedStatus;
};

void updateTaskFunction(GTask *task, gpointer, gpointer data, GCancellable *) {
    const auto *request = static_cast<const UpdateCheckTask *>(data);
    if (request->action == UpdateActionKind::RefreshCache) {
        auto res = std::make_unique<UpdateRefreshResult>();
        res->cacheResult = UpdateController::refreshPackageCache(
            request->currentStatus, request->environment);
        if (res->cacheResult.success) {
            res->updatedStatus =
                UpdateController::checkForUpdates(request->environment);
            if (!res->updatedStatus.offlineOrUnreachable &&
                !res->updatedStatus.updateAvailable) {
                res->updatedStatus.cacheMayBeStale = false;
                res->updatedStatus.statusMessage =
                    "已同步最新远端软件源元数据，当前已是最新版本（" +
                    res->updatedStatus.currentVersion + "）。";
            }
        }
        g_task_return_pointer(
            task, res.release(),
            [](gpointer value) {
                delete static_cast<UpdateRefreshResult *>(value);
            });
        return;
    }
    g_task_return_pointer(
        task,
        new UpdateStatus(UpdateController::checkForUpdates(request->environment)),
        [](gpointer value) { delete static_cast<UpdateStatus *>(value); });
}

std::string resolveSourceUrl(std::string_view source) {
    auto pos = source.find("http://");
    if (pos == std::string_view::npos) {
        pos = source.find("https://");
    }
    if (pos != std::string_view::npos) {
        auto end = source.find_first_of(" \t\r\n", pos);
        std::string rawUrl = (end == std::string_view::npos)
                                 ? std::string(source.substr(pos))
                                 : std::string(source.substr(pos, end - pos));
        if (rawUrl.find("sunshine134679.github.io/fcitx5-linux-pinyin-input-method") != std::string::npos) {
            return "https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/";
        }
        return rawUrl;
    }
    return "https://sunshine134679.github.io/fcitx5-linux-pinyin-input-method/";
}

} // namespace

class UpdatesPage::Impl final {
public:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;
    using SharedLifetime = std::shared_ptr<Lifetime>;

    static constexpr const char *kStateKey = "modernime-updates-page-lifetime";

    Impl(Environment currentEnvironment,
         std::function<void(std::string)> notifyCallback)
        : environment(std::move(currentEnvironment)),
          notify(std::move(notifyCallback)) {
        lastStatus.currentVersion = MODERNIME_VERSION;
        lastStatus.refreshCacheCommand = "sudo apt update";
        lastStatus.upgradeCommand =
            "sudo apt update && sudo apt install --only-upgrade modernime";

        lifetime = detail::GObjectHandle<GObject>::adopt(
            G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr)));
        state = std::make_shared<Lifetime>(this);
        g_object_set_data_full(
            lifetime.get(), kStateKey, new SharedLifetime(state),
            [](gpointer value) {
                delete static_cast<SharedLifetime *>(value);
            });

        detail::GtkWidgetGuard pageGuard(createPageShell(
            "更新与安装", "检查版本更新、查看安装渠道与终端安全升级引导。"));
        page = pageGuard.get();
        buildUi();
        pageGuard.release();
    }

    ~Impl() {
        if (state) {
            state->deactivate();
        }
    }

    GtkWidget *widget() const { return page; }

    void refresh() {
        startCheck();
    }

    bool focusTarget(std::string_view target) {
        if (target == "updates-check" || target == "diagnostics-check-update" ||
            target == "updates-refresh-cache") {
            return focusWidgetOrFallback(checkButton, checkButton);
        }
        if (target == "updates-copy-command") {
            return focusWidgetOrFallback(copyCommandButton, copyCommandButton);
        }
        if (target == "updates-source" || target == "updates-source-jump") {
            return focusWidgetOrFallback(sourceJumpButton, sourceJumpButton);
        }
        return false;
    }

private:
    static void destroySignalState(gpointer data, GClosure *) {
        delete static_cast<SharedLifetime *>(data);
    }

    static void onCheckClicked(GtkButton *, gpointer data) {
        auto *shared = static_cast<SharedLifetime *>(data);
        (*shared)->withOwner([](Impl &owner) {
            owner.startCheck();
        });
    }

    static void onJumpSourceClicked(GtkButton *, gpointer data) {
        auto *shared = static_cast<SharedLifetime *>(data);
        (*shared)->withOwner([](Impl &owner) {
            owner.jumpToSource();
        });
    }

    static void onCopyCommandClicked(GtkButton *, gpointer data) {
        auto *shared = static_cast<SharedLifetime *>(data);
        (*shared)->withOwner([](Impl &owner) {
            owner.copyCommand();
        });
    }

    static void updateTaskFinished(GObject *source, GAsyncResult *result, gpointer) {
        GError *error = nullptr;
        auto *statePtr = static_cast<SharedLifetime *>(
            g_object_get_data(source, kStateKey));
        const auto *request = static_cast<const UpdateCheckTask *>(
            g_task_get_task_data(G_TASK(result)));
        const auto action = request->action;

        if (action == UpdateActionKind::RefreshCache) {
            auto *res = static_cast<UpdateRefreshResult *>(
                g_task_propagate_pointer(G_TASK(result), &error));
            if (statePtr != nullptr) {
                (*statePtr)->withOwner([res, error](Impl &owner) {
                    owner.finishRefreshCache(res, error);
                });
            }
            delete res;
        } else {
            auto *status = static_cast<UpdateStatus *>(
                g_task_propagate_pointer(G_TASK(result), &error));
            if (statePtr != nullptr) {
                (*statePtr)->withOwner([status, error](Impl &owner) {
                    owner.finishCheck(status, error);
                });
            }
            delete status;
        }
        g_clear_error(&error);
    }

    void buildUi() {
        buildVersionSection();
        buildCheckSection();
        buildUpgradeGuideSection();

        setSettingsFocusChain(
            page, {sourceJumpButton, checkButton, copyCommandButton});
        applyStatus(lastStatus);
    }

    static GtkWidget *wrappedLabel(const char *text, bool selectable = false) {
        auto *label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_label_set_yalign(GTK_LABEL(label), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
        gtk_label_set_selectable(GTK_LABEL(label), selectable);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_halign(label, GTK_ALIGN_FILL);
        return label;
    }

    void buildVersionSection() {
        auto *section = createSectionCard(
            "当前版本与安装渠道", "当前运行的 ModernIME 版本、软件包分发方式与软件源。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        auto *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 24);

        const auto addRow = [grid](int row, std::string_view title, const char *initial, GtkWidget **valLabel) {
            auto *label = gtk_label_new(std::string(title).c_str());
            addStyleClass(label, "modernime-description");
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            gtk_widget_set_valign(label, GTK_ALIGN_START);
            *valLabel = wrappedLabel(initial);
            gtk_grid_attach(GTK_GRID(grid), *valLabel, 1, row, 1, 1);
        };

        addRow(0, "当前版本", MODERNIME_VERSION, &currentVersionLabel);
        addRow(1, "安装渠道", "检测中…", &channelLabel);

        auto *sourceTitle = gtk_label_new("软件源");
        addStyleClass(sourceTitle, "modernime-description");
        gtk_widget_set_halign(sourceTitle, GTK_ALIGN_START);
        gtk_widget_set_valign(sourceTitle, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(grid), sourceTitle, 0, 2, 1, 1);

        sourceJumpButton = gtk_button_new_with_label("一键跳转 ↗");
        setTarget(sourceJumpButton, "updates-source-jump");
        setAccessibleWidgetText(sourceJumpButton, "软件源一键跳转", "在默认浏览器中打开官方软件源页面");
        gtk_widget_set_tooltip_text(sourceJumpButton, "在默认浏览器中打开官方软件源页面");
        gtk_widget_set_halign(sourceJumpButton, GTK_ALIGN_START);
        gtk_widget_set_valign(sourceJumpButton, GTK_ALIGN_CENTER);

        g_signal_connect_data(sourceJumpButton, "clicked", G_CALLBACK(onJumpSourceClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));

        gtk_grid_attach(GTK_GRID(grid), sourceJumpButton, 1, 2, 1, 1);

        gtk_box_pack_start(GTK_BOX(section), grid, FALSE, FALSE, 0);
    }

    void buildCheckSection() {
        auto *section = createSectionCard(
            "新版本检查", "通过只读查询检查软件源是否有可用的新版本。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        auto *notice = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        addStyleClass(notice, "modernime-update-notice");
        gtk_widget_set_margin_bottom(notice, 8);
        auto *icon = gtk_image_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_valign(icon, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(notice), icon, FALSE, FALSE, 0);
        statusMessageLabel = wrappedLabel("点击“检查更新”获取最新版本信息。");
        setTarget(statusMessageLabel, "updates-status");
        gtk_box_pack_start(GTK_BOX(notice), statusMessageLabel, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(section), notice, FALSE, FALSE, 0);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_bottom(actions, 12);
        checkButton = gtk_button_new_with_label("检查更新");
        addStyleClass(checkButton, kSettingsPrimaryButtonClass);
        setTarget(checkButton, "updates-check");
        setAccessibleWidgetText(checkButton, "检查更新", "查询软件源中是否有 ModernIME 新版本");

        gtk_box_pack_start(GTK_BOX(actions), checkButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), actions, FALSE, FALSE, 0);

        g_signal_connect_data(checkButton, "clicked", G_CALLBACK(onCheckClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));

        auto *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 24);

        const auto addRow = [grid](int row, std::string_view title, const char *initial, GtkWidget **valLabel) {
            auto *label = gtk_label_new(std::string(title).c_str());
            addStyleClass(label, "modernime-description");
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            gtk_widget_set_valign(label, GTK_ALIGN_START);
            *valLabel = wrappedLabel(initial);
            gtk_grid_attach(GTK_GRID(grid), *valLabel, 1, row, 1, 1);
        };

        addRow(0, "最新版本", "未检查", &candidateVersionLabel);
        gtk_box_pack_start(GTK_BOX(section), grid, FALSE, FALSE, 0);
        auto *summaryTitle = wrappedLabel("更新说明");
        addStyleClass(summaryTitle, "modernime-description");
        gtk_widget_set_margin_top(summaryTitle, 10);
        gtk_box_pack_start(GTK_BOX(section), summaryTitle, FALSE, FALSE, 0);
        changelogLabel = wrappedLabel("暂无", true);
        gtk_widget_set_margin_top(changelogLabel, 4);
        gtk_box_pack_start(GTK_BOX(section), changelogLabel, FALSE, FALSE, 0);
    }

    void buildUpgradeGuideSection() {
        auto *section = createSectionCard(
            "安全升级引导", "复制升级命令到终端执行，由系统包管理器完成更新。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        guideDescLabel = gtk_label_new(
            "检测到可更新版本时，请复制并在终端中运行以下升级命令：");
        addStyleClass(guideDescLabel, "modernime-setting-desc");
        gtk_widget_set_halign(guideDescLabel, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(guideDescLabel), TRUE);
        gtk_box_pack_start(GTK_BOX(section), guideDescLabel, FALSE, FALSE, 0);

        commandLabel = wrappedLabel("sudo apt update && sudo apt install --only-upgrade modernime", true);
        addStyleClass(commandLabel, "modernime-code-block");
        gtk_widget_set_margin_top(commandLabel, 8);
        gtk_widget_set_margin_bottom(commandLabel, 10);
        gtk_box_pack_start(GTK_BOX(section), commandLabel, FALSE, FALSE, 0);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        copyCommandButton = gtk_button_new_with_label("复制升级命令");
        setTarget(copyCommandButton, "updates-copy-command");
        setAccessibleWidgetText(copyCommandButton, "复制升级命令", "将升级命令复制到剪贴板，以便在终端中粘贴执行");
        gtk_box_pack_start(GTK_BOX(actions), copyCommandButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), actions, FALSE, FALSE, 0);

        g_signal_connect_data(copyCommandButton, "clicked", G_CALLBACK(onCopyCommandClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));
    }

    void jumpToSource() {
        const std::string url = resolveSourceUrl(lastStatus.packageSource);
        g_message("[ModernIME Updates] Opening software source page: %s", url.c_str());

        auto *toplevel = gtk_widget_get_toplevel(page);
        GtkWindow *parentWindow = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr;
        GError *error = nullptr;
        bool launched = gtk_show_uri_on_window(parentWindow, url.c_str(), GDK_CURRENT_TIME, &error);
        if (!launched) {
            if (error != nullptr) {
                g_warning("[ModernIME Updates] gtk_show_uri error: %s, falling back to xdg-open", error->message);
                g_clear_error(&error);
            }
            gchar *quoted = g_shell_quote(url.c_str());
            std::string cmd = std::string("xdg-open ") + quoted + " >/dev/null 2>&1 &";
            g_free(quoted);
            std::system(cmd.c_str());
        }
        if (notify) {
            notify("已在浏览器中打开软件源页面");
        }
    }

    void startCheck() {
        if (busy) {
            return;
        }
        busy = true;
        gtk_widget_set_sensitive(checkButton, FALSE);
        gtk_label_set_text(GTK_LABEL(statusMessageLabel), "正在检查更新…");

        auto *task = g_task_new(lifetime.get(), nullptr, updateTaskFinished, nullptr);
        auto request = std::make_unique<UpdateCheckTask>();
        request->action = UpdateActionKind::CheckOnly;
        request->environment = environment;
        request->currentStatus = lastStatus;
        g_task_set_task_data(
            task, request.release(),
            [](gpointer value) {
                delete static_cast<UpdateCheckTask *>(value);
            });
        g_task_run_in_thread(task, updateTaskFunction);
        g_object_unref(task);
    }

    void finishCheck(const UpdateStatus *status, const GError *error) {
        busy = false;
        gtk_widget_set_sensitive(checkButton, TRUE);
        if (error != nullptr || status == nullptr) {
            const char *errMsg = error != nullptr && error->message != nullptr
                                     ? error->message
                                     : "未知错误";
            g_warning("[ModernIME Updates] Check error: %s", errMsg);
            gtk_label_set_text(GTK_LABEL(statusMessageLabel),
                               "未能获取最新版本信息，请检查网络连接。");
            return;
        }
        lastStatus = *status;
        applyStatus(lastStatus);
    }

    void finishRefreshCache(const UpdateRefreshResult *result, const GError *error) {
        busy = false;
        gtk_widget_set_sensitive(checkButton, TRUE);
        if (error != nullptr || result == nullptr) {
            g_warning("[ModernIME Updates] Refresh cache failed");
            gtk_label_set_text(GTK_LABEL(statusMessageLabel), "刷新未完成，请检查网络连接。");
            return;
        }
        if (!result->cacheResult.success) {
            g_warning("[ModernIME Updates] Refresh cache detail: %s",
                      result->cacheResult.message.c_str());
            gtk_label_set_text(GTK_LABEL(statusMessageLabel),
                               "已取消授权或网络不可达。");
            return;
        }
        lastStatus = result->updatedStatus;
        applyStatus(lastStatus);
    }

    void applyStatus(const UpdateStatus &status) {
        g_message("[ModernIME Updates] Status: current=%s, candidate=%s, source=%s, locked=%d, available=%d, msg=%s",
                  status.currentVersion.c_str(),
                  status.candidateVersion.c_str(),
                  status.packageSource.c_str(),
                  status.packageManagerLocked,
                  status.updateAvailable,
                  status.statusMessage.c_str());

        gtk_label_set_text(GTK_LABEL(currentVersionLabel),
                           status.currentVersion.empty() ? MODERNIME_VERSION : status.currentVersion.c_str());

        std::string channelText = status.installChannel;
        if (channelText.empty() || channelText == "unknown") {
            channelText = (status.manager == PackageManagerKind::Dnf) ? "rpm (系统包)" : "deb (系统包)";
        } else if (channelText == "deb") {
            channelText = "deb (Debian/Ubuntu 软件源)";
        } else if (channelText == "rpm") {
            channelText = "rpm (Fedora/openEuler 软件源)";
        } else if (channelText == "script") {
            channelText = "script (独立用户态脚本安装)";
        }
        gtk_label_set_text(GTK_LABEL(channelLabel), channelText.c_str());

        std::string candDisplay;
        if (status.updateAvailable) {
            candDisplay = status.candidateVersion;
        } else if (status.checked && !status.offlineOrUnreachable) {
            candDisplay = (status.currentVersion.empty() ? MODERNIME_VERSION : status.currentVersion) + " (已是最新)";
        } else if (status.offlineOrUnreachable) {
            candDisplay = "暂不可用";
        } else {
            candDisplay = "未检查";
        }
        gtk_label_set_text(GTK_LABEL(candidateVersionLabel), candDisplay.c_str());

        gtk_label_set_text(
            GTK_LABEL(changelogLabel),
            status.changelogSummary.empty() ? "暂无更新说明" : status.changelogSummary.c_str());

        std::string userMsg;
        if (status.packageManagerLocked) {
            userMsg = "系统包管理器正在运行中，请稍后重试。";
        } else if (status.updateAvailable) {
            userMsg = "发现新版本 " + status.candidateVersion + "！可复制下方升级命令在终端中安全更新。";
        } else if (status.offlineOrUnreachable) {
            userMsg = "未能获取最新版本信息，请检查网络连接。";
        } else if (status.checked) {
            userMsg = "当前已是最新版本（" + (status.currentVersion.empty() ? MODERNIME_VERSION : status.currentVersion) + "）。";
        } else {
            userMsg = "点击“检查更新”获取最新版本信息。";
        }
        gtk_label_set_text(GTK_LABEL(statusMessageLabel), userMsg.c_str());

        std::string cmd = status.upgradeCommand;
        if (cmd.empty()) {
            cmd = (status.manager == PackageManagerKind::Dnf)
                      ? "sudo dnf upgrade modernime"
                      : "sudo apt update && sudo apt install --only-upgrade modernime";
        }
        gtk_label_set_text(GTK_LABEL(commandLabel), cmd.c_str());
    }

    void copyCommand() {
        const auto *cmd = gtk_label_get_text(GTK_LABEL(commandLabel));
        if (cmd != nullptr && *cmd != '\0') {
            auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(clipboard, cmd, -1);
            if (notify) {
                notify("已复制升级命令到剪贴板，请在终端执行");
            }
        }
    }

    Environment environment;
    std::function<void(std::string)> notify;
    UpdateStatus lastStatus;
    bool busy = false;

    GtkWidget *page = nullptr;
    GtkWidget *currentVersionLabel = nullptr;
    GtkWidget *channelLabel = nullptr;
    GtkWidget *sourceJumpButton = nullptr;
    GtkWidget *candidateVersionLabel = nullptr;
    GtkWidget *changelogLabel = nullptr;
    GtkWidget *statusMessageLabel = nullptr;
    GtkWidget *guideDescLabel = nullptr;
    GtkWidget *commandLabel = nullptr;
    GtkWidget *checkButton = nullptr;
    GtkWidget *copyCommandButton = nullptr;

    detail::GObjectHandle<GObject> lifetime;
    SharedLifetime state;
};

UpdatesPage::UpdatesPage(Environment environment,
                         std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(std::move(environment), std::move(notify))) {}

UpdatesPage::~UpdatesPage() = default;

GtkWidget *UpdatesPage::widget() const {
    return impl_->widget();
}

void UpdatesPage::refresh() {
    impl_->refresh();
}

bool UpdatesPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
