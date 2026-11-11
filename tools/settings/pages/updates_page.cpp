#include "modernime/settings/pages/updates_page.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/detail/release_notes.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/settings/update_controller.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

#ifndef MODERNIME_VERSION
#define MODERNIME_VERSION "0.1.23"
#endif

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

std::string_view upgradeCommand(const UpdateStatus &status) {
    if (!status.checked || (status.installChannel != "deb" &&
                            status.installChannel != "rpm" &&
                            status.installChannel != "script")) {
        return {};
    }
    return status.upgradeCommand;
}

struct UpdateCheckTask final {
    Environment environment;
    bool online = false;
};

struct UpdateCheckResult final {
    UpdateStatus status;
};

void updateTaskFunction(GTask *task, gpointer, gpointer data, GCancellable *) {
    const auto *request = static_cast<const UpdateCheckTask *>(data);
    auto result = std::make_unique<UpdateCheckResult>();
    result->status = request->online
        ? UpdateController::checkOnlineRelease(request->environment)
        : UpdateController::checkForUpdates(request->environment);
    g_task_return_pointer(
        task, result.release(),
        [](gpointer value) { delete static_cast<UpdateCheckResult *>(value); });
}

struct UpdateActionTask final {
    Environment environment;
    UpdateStatus status;
    DownloadedUpdate download;
    bool install = false;
    UpdateProgressCallback progress;
};

struct UpdateActionResult final {
    DownloadedUpdate download;
    RuntimeResult installation{false, {}};
};

void updateActionFunction(GTask *task, gpointer, gpointer data, GCancellable *) {
    const auto &request = *static_cast<UpdateActionTask *>(data);
    auto result = std::make_unique<UpdateActionResult>();
    if (request.install) {
        result->installation = UpdateController::installUpdate(request.download, request.environment, request.progress);
    } else {
        result->download = UpdateController::downloadUpdate(request.status, request.environment, request.progress);
    }
    g_task_return_pointer(task, result.release(),
        [](gpointer value) { delete static_cast<UpdateActionResult *>(value); });
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

std::string updateFailureSummary(std::string message) {
    if (message.find("invalid choice: 'unknown'") != std::string::npos)
        return "无法识别当前安装方式，请重新打开客户端后再试。";
    if (message.find("HTTP Error 404") != std::string::npos)
        return "暂未找到此版本的安装包，请重新检查更新。";
    if (message.find("HTTP Error 403") != std::string::npos || message.find("HTTP Error 429") != std::string::npos)
        return "下载服务暂时拒绝连接，请稍后重试。";
    if (message.find("CERTIFICATE_VERIFY_FAILED") != std::string::npos)
        return "无法验证下载连接，请检查系统时间和网络代理。";
    if (message.find("timed out") != std::string::npos || message.find("Timeout") != std::string::npos)
        return "下载连接超时，请检查网络后重试。";
    if (message.find("urlopen error") != std::string::npos)
        return "无法连接下载服务，请检查网络或代理后重试。";
    for (const std::string_view prefix : {"下载未完成：", "安装未完成：", "ModernIME 更新失败："}) {
        if (message.starts_with(prefix)) message.erase(0, prefix.size());
    }
    message = message.substr(0, message.find_first_of("\r\n"));
    if (g_utf8_validate(message.c_str(), -1, nullptr)) {
        const auto first = g_utf8_get_char(message.c_str());
        if (first >= 0x4e00 && first <= 0x9fff) {
            if (g_utf8_strlen(message.c_str(), -1) > 100)
                message.resize(static_cast<std::size_t>(g_utf8_offset_to_pointer(message.c_str(), 100) - message.c_str()));
            return message;
        }
    }
    return "更新助手未能完成操作。可以重试，或展开“检查详情”查看具体原因。";
}

} // namespace

class UpdatesPage::Impl final {
public:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;
    using SharedLifetime = std::shared_ptr<Lifetime>;

    static constexpr const char *kStateKey = "modernime-updates-page-lifetime";

    Impl(Environment currentEnvironment,
         std::function<void(std::string)> notifyCallback,
         std::function<bool()> beforeRestart,
         std::function<void()> closeCallback)
        : environment(std::move(currentEnvironment)),
          notify(std::move(notifyCallback)),
          prepareRestart(std::move(beforeRestart)),
          closeForRestart(std::move(closeCallback)) {
        lastStatus.currentVersion = MODERNIME_VERSION;

        lifetime = detail::GObjectHandle<GObject>::adopt(
            G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr)));
        state = std::make_shared<Lifetime>(this);
        g_object_set_data_full(
            lifetime.get(), kStateKey, new SharedLifetime(state),
            [](gpointer value) {
                delete static_cast<SharedLifetime *>(value);
            });

        detail::GtkWidgetGuard pageGuard(createPageShell(
            "更新与安装", ""));
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
        if (nativePhase != NativePhase::None) return;
        focusCopyWhenReady = false;
        startCheck();
    }

    bool focusTarget(std::string_view target) {
        focusCopyWhenReady = false;
        if (target == "updates-download") {
            return focusWidgetOrFallback(checkButton, checkButton);
        }
        if (target == "updates-check" || target == "diagnostics-check-update" ||
            target == "updates-refresh-cache") {
            if (lastStatus.updateAvailable) {
                setSettingsDisclosureExpanded(versionDetails, true, false);
                return focusWidgetOrFallback(recheckButton, versionDetails);
            }
            return focusWidgetOrFallback(checkButton, checkButton);
        }
        if (target == "updates-copy-command") {
            setSettingsDisclosureExpanded(upgradeDetails, true, false);
            const bool focused = focusWidgetOrFallback(copyCommandButton, upgradeDetails);
            focusCopyWhenReady = focused && busy;
            return focused;
        }
        if (target == "updates-source" || target == "updates-source-jump") {
            setSettingsDisclosureExpanded(versionDetails, true, false);
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
            if (owner.busy) return;
            if (owner.nativePhase == NativePhase::Ready) {
                owner.startUpdateAction(true);
            } else if (owner.nativePhase == NativePhase::Installed) {
                owner.restartClient();
            } else if (owner.lastStatus.updateAvailable &&
                (!owner.lastStatus.packageManagerLocked || owner.lastStatus.checkedOnline)) {
                owner.startUpdateAction(false);
            } else {
                owner.startCheck(true);
            }
        });
    }

    static void onRecheckClicked(GtkButton *, gpointer data) {
        auto *shared = static_cast<SharedLifetime *>(data);
        (*shared)->withOwner([](Impl &owner) { owner.startCheck(true); });
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

    static gboolean onUpgradeFocusOut(GtkWidget *, GdkEventFocus *, gpointer data) {
        auto *shared = static_cast<SharedLifetime *>(data);
        (*shared)->withOwner([](Impl &owner) { owner.focusCopyWhenReady = false; });
        return FALSE;
    }

    static void updateTaskFinished(GObject *source, GAsyncResult *result, gpointer) {
        GError *error = nullptr;
        auto *statePtr = static_cast<SharedLifetime *>(
            g_object_get_data(source, kStateKey));
        auto *status = static_cast<UpdateCheckResult *>(
            g_task_propagate_pointer(G_TASK(result), &error));
        if (statePtr != nullptr) {
            (*statePtr)->withOwner([status, error](Impl &owner) {
                owner.finishCheck(status, error);
            });
        }
        delete status;
        g_clear_error(&error);
    }

    void buildUi() {
        buildCheckSection();
        auto *details = createSectionCard("", "");
        addStyleClass(details, "modernime-update-details");
        gtk_box_set_spacing(GTK_BOX(details), 6);
        gtk_box_pack_start(GTK_BOX(page), details, FALSE, FALSE, 0);
        buildVersionSection(details);
        buildReleaseNotesSection(details);
        buildUpgradeGuideSection(details);

        setSettingsFocusChain(
            page, {checkButton, versionDetails, recheckButton, sourceJumpButton,
                   releaseDetails, changelogLabel, upgradeDetails, copyCommandButton});
        applyStatus(lastStatus);
    }

    static GtkWidget *detailsSection(GtkWidget *parent, const char *title,
                                     const char *target, GtkWidget **expander) {
        auto *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        addStyleClass(row, "modernime-details");
        *expander = gtk_toggle_button_new();
        gtk_widget_set_focus_on_click(*expander, FALSE);
        addStyleClass(*expander, "modernime-details-button");
        setTarget(*expander, target);
        setAccessibleWidgetText(*expander, title, "展开或收起详情");
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto *label = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
        auto *arrow = gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        gtk_box_pack_end(GTK_BOX(header), arrow, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(*expander), header);
        auto *revealer = gtk_revealer_new();
        bindSettingsDisclosure(*expander, revealer, arrow);
        auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        addStyleClass(content, "modernime-details-content");
        gtk_container_add(GTK_CONTAINER(revealer), content);
        gtk_box_pack_start(GTK_BOX(row), *expander, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), revealer, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(parent), row, FALSE, FALSE, 0);
        return content;
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

    void buildVersionSection(GtkWidget *parent) {
        auto *section = detailsSection(parent, "检查详情",
                                       "updates-version-details", &versionDetails);

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

        addRow(0, "可用版本", "未检查", &candidateVersionLabel);
        addRow(1, "安装方式", "尚未检查", &channelLabel);

        sourceJumpButton = gtk_button_new();
        addStyleClass(sourceJumpButton, "modernime-source-link");
        auto *linkContent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(linkContent), gtk_label_new("软件源与安装说明"), FALSE, FALSE, 0);
        auto *external = gtk_drawing_area_new();
        gtk_widget_set_size_request(external, 16, 16);
        gtk_widget_set_valign(external, GTK_ALIGN_CENTER);
        g_signal_connect(external, "draw", G_CALLBACK(+[](GtkWidget *widget, cairo_t *cr, gpointer) -> gboolean {
            GdkRGBA color{};
            gtk_style_context_get_color(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget), &color);
            gdk_cairo_set_source_rgba(cr, &color);
            cairo_set_line_width(cr, 1.5);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_move_to(cr, 6, 3); cairo_line_to(cr, 2, 3); cairo_line_to(cr, 2, 14);
            cairo_line_to(cr, 13, 14); cairo_line_to(cr, 13, 10);
            cairo_move_to(cr, 7, 9); cairo_line_to(cr, 14, 2);
            cairo_move_to(cr, 8, 2); cairo_line_to(cr, 14, 2); cairo_line_to(cr, 14, 8);
            cairo_stroke(cr);
            return FALSE;
        }), nullptr);
        gtk_box_pack_start(GTK_BOX(linkContent), external, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(sourceJumpButton), linkContent);
        setTarget(sourceJumpButton, "updates-source-jump");
        setAccessibleWidgetText(sourceJumpButton, "软件源与安装说明", "在默认浏览器中查看软件源与安装说明");
        gtk_widget_set_tooltip_text(sourceJumpButton, "在默认浏览器中打开官方软件源页面");
        gtk_widget_set_halign(sourceJumpButton, GTK_ALIGN_START);
        gtk_widget_set_valign(sourceJumpButton, GTK_ALIGN_CENTER);

        g_signal_connect_data(sourceJumpButton, "clicked", G_CALLBACK(onJumpSourceClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));

        gtk_box_pack_start(GTK_BOX(section), grid, FALSE, FALSE, 0);
        checkDetailsLabel = wrappedLabel("检查后可在这里查看完整结果。", true);
        addStyleClass(checkDetailsLabel, "modernime-check-details-message");
        setTarget(checkDetailsLabel, "updates-check-details-message");
        gtk_box_pack_start(GTK_BOX(section), checkDetailsLabel, FALSE, FALSE, 0);
        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        recheckButton = gtk_button_new_with_label("重新检查");
        gtk_widget_set_halign(recheckButton, GTK_ALIGN_START);
        setTarget(recheckButton, "updates-recheck");
        setAccessibleWidgetText(recheckButton, "重新检查", "在线查询官方最新版本，无需管理员授权");
        gtk_box_pack_start(GTK_BOX(actions), recheckButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), sourceJumpButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), actions, FALSE, FALSE, 0);
        g_signal_connect_data(recheckButton, "clicked", G_CALLBACK(onRecheckClicked),
                              new SharedLifetime(state), destroySignalState,
                              static_cast<GConnectFlags>(0));
    }

    void buildCheckSection() {
        auto *section = createSectionCard("", "");
        addStyleClass(section, "modernime-update-summary");
        summaryCard = section;
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        auto *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        statusArt = createSettingsStatusArt(SettingsStatusArt::Release);
        gtk_box_pack_start(GTK_BOX(summary), statusArt, FALSE, FALSE, 0);
        auto *version = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_valign(version, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(version, TRUE);
        auto *versionTitle = wrappedLabel("当前版本");
        addStyleClass(versionTitle, "modernime-version-meta");
        gtk_box_pack_start(GTK_BOX(version), versionTitle, FALSE, FALSE, 0);
        currentVersionLabel = wrappedLabel("");
        addStyleClass(currentVersionLabel, "modernime-release-version");
        gtk_box_pack_start(GTK_BOX(version), currentVersionLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(summary), version, TRUE, TRUE, 0);
        checkButton = gtk_button_new_with_label("检查更新");
        addStyleClass(checkButton, "modernime-check-button");
        addStyleClass(checkButton, kSettingsPrimaryButtonClass);
        gtk_button_set_image(GTK_BUTTON(checkButton),
                             gtk_image_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON));
        gtk_button_set_always_show_image(GTK_BUTTON(checkButton), TRUE);
        gtk_widget_set_valign(checkButton, GTK_ALIGN_CENTER);
        setTarget(checkButton, "updates-check");
        setAccessibleWidgetText(checkButton, "检查更新", "在线查询 ModernIME 官方最新版本，无需管理员授权");

        gtk_box_pack_end(GTK_BOX(summary), checkButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), summary, FALSE, FALSE, 0);
        statusMessageLabel = wrappedLabel("尚未检查更新");
        addStyleClass(statusMessageLabel, "modernime-release-status");
        setTarget(statusMessageLabel, "updates-status");
        auto *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        progressSpinner = gtk_spinner_new();
        gtk_widget_set_size_request(progressSpinner, 16, 16);
        gtk_widget_set_valign(progressSpinner, GTK_ALIGN_CENTER);
        gtk_widget_set_no_show_all(progressSpinner, TRUE);
        gtk_box_pack_start(GTK_BOX(progress), progressSpinner, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(progress), statusMessageLabel, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(version), progress, FALSE, FALSE, 0);
        statusHintLabel = wrappedLabel("");
        addStyleClass(statusHintLabel, "modernime-status-hint");
        setTarget(statusHintLabel, "updates-status-hint");
        gtk_widget_set_margin_top(statusHintLabel, 4);
        gtk_box_pack_start(GTK_BOX(version), statusHintLabel, FALSE, FALSE, 0);
        downloadProgress = gtk_progress_bar_new();
        setTarget(downloadProgress, "updates-download-progress");
        addStyleClass(downloadProgress, "modernime-update-progress");
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(downloadProgress), TRUE);
        gtk_widget_set_no_show_all(downloadProgress, TRUE);
        gtk_widget_set_margin_top(downloadProgress, 12);
        gtk_box_pack_start(GTK_BOX(section), downloadProgress, FALSE, FALSE, 0);
        errorSummaryLabel = wrappedLabel("");
        setTarget(errorSummaryLabel, "updates-error-summary");
        addStyleClass(errorSummaryLabel, "modernime-description");
        gtk_widget_set_no_show_all(errorSummaryLabel, TRUE);
        gtk_widget_set_margin_top(errorSummaryLabel, 8);
        gtk_box_pack_start(GTK_BOX(section), errorSummaryLabel, FALSE, FALSE, 0);

        g_signal_connect_data(checkButton, "clicked", G_CALLBACK(onCheckClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));

    }

    void buildReleaseNotesSection(GtkWidget *parent) {
        auto *section = detailsSection(parent, "更新说明",
                                       "updates-release-details", &releaseDetails);
        changelogLabel = wrappedLabel("暂无", true);
        setTarget(changelogLabel, "updates-release-notes");
        addStyleClass(changelogLabel, "modernime-release-notes");
        gtk_box_pack_start(GTK_BOX(section), changelogLabel, FALSE, FALSE, 0);
    }

    void buildUpgradeGuideSection(GtkWidget *parent) {
        auto *section = detailsSection(parent, "升级引导",
                                       "updates-upgrade-details", &upgradeDetails);
        g_signal_connect_data(upgradeDetails, "focus-out-event",
                              G_CALLBACK(onUpgradeFocusOut), new SharedLifetime(state),
                              destroySignalState, static_cast<GConnectFlags>(0));

        guideDescLabel = wrappedLabel("");
        addStyleClass(guideDescLabel, "modernime-setting-desc");
        gtk_box_pack_start(GTK_BOX(section), guideDescLabel, FALSE, FALSE, 0);

        commandLabel = wrappedLabel("", true);
        gtk_widget_set_no_show_all(commandLabel, TRUE);
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
            const auto rc = std::system(cmd.c_str());
            (void)rc;
        }
        if (notify) {
            notify("已在浏览器中打开软件源页面");
        }
    }

    enum class NativePhase { None, Downloading, Ready, Installing, Installed };

    struct ProgressEvent final {
        SharedLifetime state;
        std::uint64_t generation;
        UpdateProgress progress;
    };

    static void updateActionFinished(GObject *source, GAsyncResult *result, gpointer) {
        GError *error = nullptr;
        auto *value = static_cast<UpdateActionResult *>(g_task_propagate_pointer(G_TASK(result), &error));
        auto *shared = static_cast<SharedLifetime *>(g_object_get_data(source, kStateKey));
        if (shared) (*shared)->withOwner([value, error](Impl &owner) { owner.finishUpdateAction(value, error); });
        delete value;
        g_clear_error(&error);
    }

    void showProgress(const UpdateProgress &progress) {
        if (progress.total > 0) {
            const double fraction = std::min(1.0, static_cast<double>(progress.received) /
                                                  static_cast<double>(progress.total));
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(downloadProgress), fraction);
            const auto text = std::to_string(static_cast<int>(fraction * 100)) + "%";
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(downloadProgress), text.c_str());
        }
        if (progress.phase == "verify") {
            setSummary("正在校验安装包…", "核对下载内容的完整性。", "neutral");
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(downloadProgress), "正在校验");
        } else if (progress.phase == "install") {
            setSummary("正在安装更新…", "正在保护现有数据并安装，请等待完成。", "neutral");
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(downloadProgress), "正在安装");
        }
    }

    void startUpdateAction(bool install) {
        if (busy) return;
        if (install && prepareRestart && !prepareRestart()) return;
        busy = true;
        gtk_widget_hide(errorSummaryLabel);
        nativePhase = install ? NativePhase::Installing : NativePhase::Downloading;
        ++actionGeneration;
        gtk_widget_set_sensitive(checkButton, FALSE);
        gtk_widget_set_sensitive(recheckButton, FALSE);
        gtk_widget_set_sensitive(copyCommandButton, FALSE);
        gtk_widget_show(downloadProgress);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(downloadProgress), 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(downloadProgress), install ? "正在安装" : "正在连接下载服务");
        setPrimaryAction(install ? "正在更新…" : "下载中…", "folder-download-symbolic",
                         install ? "安装完成后自动重启" : "正在下载并校验更新");
        setSummary(install ? "正在安装更新…" : "正在下载安装包…",
                   install ? "安装完成后将自动重启输入法和设置窗口。"
                           : "下载完成后，可点击“重启并更新”完成安装。", "neutral");
        gboolean animations = TRUE;
        g_object_get(gtk_widget_get_settings(page), "gtk-enable-animations", &animations, nullptr);
        if (animations) {
            gtk_widget_show(progressSpinner);
            gtk_spinner_start(GTK_SPINNER(progressSpinner));
        }
        auto request = std::make_unique<UpdateActionTask>();
        request->environment = environment;
        request->status = lastStatus;
        request->download = downloaded;
        request->install = install;
        request->progress = [shared = state, generation = actionGeneration](const UpdateProgress &progress) {
            auto *event = new ProgressEvent{shared, generation, progress};
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                const auto &pending = *static_cast<ProgressEvent *>(data);
                pending.state->withOwner([&pending](Impl &owner) {
                    if (owner.busy && owner.actionGeneration == pending.generation) owner.showProgress(pending.progress);
                });
                return G_SOURCE_REMOVE;
            }, event, [](gpointer data) { delete static_cast<ProgressEvent *>(data); });
        };
        auto *task = g_task_new(lifetime.get(), nullptr, updateActionFinished, nullptr);
        g_task_set_task_data(task, request.release(), [](gpointer value) { delete static_cast<UpdateActionTask *>(value); });
        g_task_run_in_thread(task, updateActionFunction);
        g_object_unref(task);
    }

    void finishUpdateAction(const UpdateActionResult *result, const GError *error) {
        const bool wasInstalling = nativePhase == NativePhase::Installing;
        busy = false;
        gtk_spinner_stop(GTK_SPINNER(progressSpinner));
        gtk_widget_hide(progressSpinner);
        gtk_widget_set_sensitive(checkButton, TRUE);
        const RuntimeResult outcome = error || !result
            ? RuntimeResult{false, error ? error->message : "更新未完成。"}
            : wasInstalling ? result->installation : result->download.result;
        if (!outcome.success) {
            nativePhase = wasInstalling ? NativePhase::Ready : NativePhase::None;
            gtk_widget_hide(downloadProgress);
            setSummary(wasInstalling ? "安装未完成" : "下载未完成", "可重试，展开检查详情查看原因。", "warning");
            gtk_label_set_text(GTK_LABEL(errorSummaryLabel), updateFailureSummary(outcome.message).c_str());
            gtk_widget_show(errorSummaryLabel);
            gtk_label_set_text(GTK_LABEL(checkDetailsLabel), outcome.message.c_str());
            setPrimaryAction(wasInstalling ? "重试安装" : "重试下载", "view-refresh-symbolic",
                             wasInstalling ? "使用已下载的安装包重试安装" : "重新下载并校验安装包");
            gtk_widget_set_sensitive(recheckButton, !wasInstalling);
            if (notify) notify(wasInstalling ? "安装未完成，原因已显示在更新页面。" : "下载未完成，原因已显示在更新页面。");
            return;
        }
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(downloadProgress), 1);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(downloadProgress), wasInstalling ? "安装完成" : "下载与校验完成");
        if (wasInstalling) {
            nativePhase = NativePhase::Installed;
            setSummary("更新已安装", "重启输入法和设置窗口后生效。", "success");
            setPrimaryAction("立即重启", "view-refresh-symbolic", "重启输入法和设置窗口，完成更新");
            restartClient();
        } else {
            downloaded = result->download;
            nativePhase = NativePhase::Ready;
            setSummary(downloaded.version + " 已准备就绪", "点击“重启并更新”，自动安装并重新打开设置。", "success");
            setPrimaryAction("重启并更新", "view-refresh-symbolic", "安装已下载的更新，然后重启输入法和设置窗口");
            gtk_label_set_text(GTK_LABEL(checkDetailsLabel), "安装包已下载并通过 SHA256 完整性校验。");
        }
    }

    void restartClient() {
        if (prepareRestart && !prepareRestart()) return;
        if (!closeForRestart) {
            if (notify) notify("更新已安装，请重新启动输入法与设置窗口。");
            return;
        }
        const auto result = UpdateController::restartUpdatedClient(downloaded, environment);
        if (notify) notify(result.message);
        if (result.success) closeForRestart();
        else gtk_label_set_text(GTK_LABEL(checkDetailsLabel), result.message.c_str());
    }

    void setPrimaryAction(const char *label, const char *icon, const char *description) {
        gtk_button_set_label(GTK_BUTTON(checkButton), label);
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(GTK_BUTTON(checkButton))),
                                     icon, GTK_ICON_SIZE_BUTTON);
        setAccessibleWidgetText(checkButton, label, description);
        gtk_widget_set_tooltip_text(checkButton, description);
    }

    void startCheck(bool online = false) {
        if (busy || nativePhase != NativePhase::None) {
            return;
        }
        busy = true;
        gtk_widget_hide(errorSummaryLabel);
        gtk_widget_set_sensitive(recheckButton, FALSE);
        gtk_widget_set_sensitive(checkButton, FALSE);
        gtk_widget_set_sensitive(copyCommandButton, FALSE);
        setPrimaryAction("检查中…", "view-refresh-symbolic", "正在查询版本信息");
        setSummary(online ? "正在查询官方新版本…" : "正在读取版本信息…",
                   online ? "正在连接官方发布服务，无需管理员授权。"
                          : "正在读取已保存的软件源信息。", "neutral");
        gboolean animations = TRUE;
        g_object_get(gtk_widget_get_settings(page), "gtk-enable-animations", &animations, nullptr);
        if (animations) {
            gtk_widget_show(progressSpinner);
            gtk_spinner_start(GTK_SPINNER(progressSpinner));
        }
        gtk_label_set_text(GTK_LABEL(candidateVersionLabel), "检查中…");
        gtk_label_set_text(GTK_LABEL(channelLabel), "检测中…");
        gtk_label_set_text(GTK_LABEL(changelogLabel), "正在查询更新信息…");
        gtk_label_set_text(GTK_LABEL(commandLabel), "");
        gtk_widget_hide(commandLabel);
        gtk_label_set_text(GTK_LABEL(checkDetailsLabel), "正在读取完整检查结果…");
        gtk_label_set_text(GTK_LABEL(guideDescLabel), "正在检查安装渠道，请稍候。");

        launchTask(online);
    }

    void launchTask(bool online) {
        auto *task = g_task_new(lifetime.get(), nullptr, updateTaskFinished, nullptr);
        auto request = std::make_unique<UpdateCheckTask>();
        request->environment = environment;
        request->online = online;
        g_task_set_task_data(
            task, request.release(),
            [](gpointer value) {
                delete static_cast<UpdateCheckTask *>(value);
            });
        g_task_run_in_thread(task, updateTaskFunction);
        g_object_unref(task);
    }

    void finishCheck(const UpdateCheckResult *result, const GError *error) {
        busy = false;
        gtk_spinner_stop(GTK_SPINNER(progressSpinner));
        gtk_widget_hide(progressSpinner);
        gtk_widget_set_sensitive(checkButton, TRUE);
        gtk_widget_set_sensitive(recheckButton, TRUE);
        setPrimaryAction("重新检查", "view-refresh-symbolic", "在线查询官方最新版本，无需管理员授权");
        if (error != nullptr || result == nullptr) {
            lastStatus.updateAvailable = false;
            lastStatus.downloadUrl.clear();
            const char *errMsg = error != nullptr && error->message != nullptr
                                     ? error->message
                                     : "未知错误";
            g_warning("[ModernIME Updates] Check error: %s", errMsg);
            setSummary("未能完成检查",
                       "可重新检查，展开检查详情查看原因。",
                       "warning");
            gtk_label_set_text(GTK_LABEL(checkDetailsLabel), errMsg);
            gtk_label_set_text(GTK_LABEL(guideDescLabel), "本次检查未完成，请重新检查后获取升级命令。");
            gtk_label_set_text(GTK_LABEL(candidateVersionLabel), "暂不可用");
            gtk_label_set_text(GTK_LABEL(channelLabel), "未检测");
            gtk_label_set_text(GTK_LABEL(changelogLabel), "未能获取更新说明");
            focusCopyWhenReady = false;
            return;
        }
        lastStatus = result->status;
        applyStatus(lastStatus);
        // A search may arrive while copying is disabled. Complete that intent
        // only while the user still rests on its visible fallback control.
        const bool completeFocus = std::exchange(focusCopyWhenReady, false);
        if (completeFocus && gtk_widget_get_mapped(page) &&
            gtk_widget_has_focus(upgradeDetails)) {
            focusWidgetOrFallback(copyCommandButton, upgradeDetails);
        }
    }

    void setSummary(const std::string &message, const char *hint,
                    const char *stateClass) {
        gtk_label_set_text(GTK_LABEL(statusMessageLabel), message.c_str());
        gtk_label_set_text(GTK_LABEL(statusHintLabel), hint);
        auto *context = gtk_widget_get_style_context(summaryCard);
        for (const auto *name : {"neutral", "success", "warning"}) {
            gtk_style_context_remove_class(context, name);
        }
        gtk_style_context_add_class(context, stateClass);
        gtk_widget_queue_draw(statusArt);
    }

    void applyStatus(const UpdateStatus &status) {
        g_message("[ModernIME Updates] Status: current=%s, candidate=%s, source=%s, locked=%d, available=%d, msg=%s",
                  status.currentVersion.c_str(),
                  status.candidateVersion.c_str(),
                  status.packageSource.c_str(),
                  status.packageManagerLocked,
                  status.updateAvailable,
                  status.statusMessage.c_str());

        const std::string currentVersion = status.currentVersion.empty()
            ? MODERNIME_VERSION : status.currentVersion;
        gtk_label_set_text(GTK_LABEL(currentVersionLabel), currentVersion.c_str());

        std::string channelText = status.installChannel;
        if (channelText.empty() || channelText == "unknown") {
            channelText = status.checked ? "未识别" : "尚未检查";
        } else if (channelText == "deb") {
            channelText = "Debian / Ubuntu 软件包";
        } else if (channelText == "rpm") {
            channelText = "Fedora / openEuler 软件包";
        } else if (channelText == "script") {
            channelText = "用户目录安装（脚本）";
        }
        gtk_label_set_text(GTK_LABEL(channelLabel), channelText.c_str());

        const std::string candDisplay = !status.checked ? "未检查"
            : status.candidateVersion.empty() ? "暂不可用" : status.candidateVersion;
        gtk_label_set_text(GTK_LABEL(candidateVersionLabel), candDisplay.c_str());
        gtk_label_set_text(GTK_LABEL(checkDetailsLabel),
                           status.statusMessage.empty()
                               ? (status.checked ? "检查未提供额外说明。" : "检查后可在这里查看完整结果。")
                               : status.statusMessage.c_str());
        if (status.cacheMayBeStale && !status.packageManagerLocked) {
            gtk_label_set_text(GTK_LABEL(checkDetailsLabel),
                "当前显示的是本地软件源缓存。点击“检查更新”在线查询官方版本，无需刷新系统软件源。");
        }

        const auto notes = detail::releaseNotesMarkup(
            status.changelogSummary.empty() ? "暂无更新说明" : status.changelogSummary,
            status.candidateVersion);
        gtk_label_set_markup(GTK_LABEL(changelogLabel), notes.c_str());

        const bool downloadable = status.updateAvailable &&
            (!status.packageManagerLocked || status.checkedOnline);
        setPrimaryAction(downloadable ? "下载更新" : status.checked ? "重新检查" : "检查更新",
                         downloadable ? "folder-download-symbolic" : "view-refresh-symbolic",
                         downloadable ? "下载并校验适合当前安装渠道的更新包"
                                      : "在线查询官方最新版本，无需管理员授权");
        if (status.packageManagerLocked && !status.checkedOnline) {
            setSummary("软件包管理器正忙", "仍可在线检查官方版本；安装请等待系统包操作结束。", "warning");
        } else if (status.updateAvailable) {
            setSummary("可更新至 " + status.candidateVersion,
                       "自动选择当前安装渠道的更新包，下载后可重启完成安装。", "success");
        } else if (status.offlineOrUnreachable) {
            setSummary("暂时无法确认更新", "请重新检查；若仍失败，可展开检查详情查看原因。", "warning");
        } else if (status.checkedOnline) {
            setSummary("未发现新版本", "已查询官方最新正式版本。", "neutral");
        } else if (status.cacheMayBeStale) {
            setSummary("本地软件源缓存未发现更新", "点击检查更新，在线查询官方最新版本。", "neutral");
        } else if (status.checked) {
            setSummary("检查已结束", "暂无可用版本信息，可展开检查详情查看原因。", "neutral");
        } else {
            setSummary("尚未检查更新", "检查官方发布，查看是否有可用版本。", "neutral");
        }

        const auto cmd = upgradeCommand(status);
        gtk_label_set_text(GTK_LABEL(commandLabel), std::string(cmd).c_str());
        gtk_widget_set_visible(commandLabel, !cmd.empty());
        gtk_widget_set_sensitive(copyCommandButton, !busy && !cmd.empty());
        if (cmd.empty()) {
            gtk_label_set_text(GTK_LABEL(guideDescLabel), status.checked
                ? "暂未确认适用的升级命令。请按原安装渠道升级，或打开软件源页面查看说明。"
                : "先检查更新，确认安装渠道后会在这里显示对应命令。");
        } else if (status.installChannel == "script") {
            gtk_label_set_text(GTK_LABEL(guideDescLabel),
                "当前为脚本安装。以下命令会迁移到系统包渠道；确认需要迁移后，再复制到终端执行。");
        } else {
            gtk_label_set_text(GTK_LABEL(guideDescLabel),
                "复制以下命令到终端执行。系统包管理器会刷新软件源并确认升级内容。");
        }
    }

    void copyCommand() {
        if (busy || !gtk_widget_get_sensitive(copyCommandButton)) {
            return;
        }
        const auto cmd = upgradeCommand(lastStatus);
        if (!cmd.empty()) {
            auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
            gtk_clipboard_set_text(clipboard, cmd.data(), static_cast<gint>(cmd.size()));
            if (notify) {
                notify("已复制升级命令到剪贴板，请在终端执行");
            }
        } else if (notify) {
            notify(lastStatus.checked ? "尚未确认适用的升级命令，请按原安装渠道升级"
                                      : "请先检查更新以确认安装渠道");
        }
    }

    Environment environment;
    std::function<void(std::string)> notify;
    std::function<bool()> prepareRestart;
    std::function<void()> closeForRestart;
    DownloadedUpdate downloaded;
    NativePhase nativePhase = NativePhase::None;
    std::uint64_t actionGeneration = 0;
    UpdateStatus lastStatus;
    bool busy = false;
    bool focusCopyWhenReady = false;

    GtkWidget *page = nullptr;
    GtkWidget *summaryCard = nullptr;
    GtkWidget *statusArt = nullptr;
    GtkWidget *versionDetails = nullptr;
    GtkWidget *releaseDetails = nullptr;
    GtkWidget *upgradeDetails = nullptr;
    GtkWidget *currentVersionLabel = nullptr;
    GtkWidget *channelLabel = nullptr;
    GtkWidget *checkDetailsLabel = nullptr;
    GtkWidget *sourceJumpButton = nullptr;
    GtkWidget *candidateVersionLabel = nullptr;
    GtkWidget *changelogLabel = nullptr;
    GtkWidget *statusMessageLabel = nullptr;
    GtkWidget *statusHintLabel = nullptr;
    GtkWidget *progressSpinner = nullptr;
    GtkWidget *downloadProgress = nullptr;
    GtkWidget *errorSummaryLabel = nullptr;
    GtkWidget *guideDescLabel = nullptr;
    GtkWidget *commandLabel = nullptr;
    GtkWidget *checkButton = nullptr;
    GtkWidget *recheckButton = nullptr;
    GtkWidget *copyCommandButton = nullptr;

    detail::GObjectHandle<GObject> lifetime;
    SharedLifetime state;
};

UpdatesPage::UpdatesPage(Environment environment,
                         std::function<void(std::string)> notify,
                         std::function<bool()> prepareRestart,
                         std::function<void()> closeForRestart)
    : impl_(std::make_unique<Impl>(std::move(environment), std::move(notify),
                                  std::move(prepareRestart), std::move(closeForRestart))) {}

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
