#include "modernime/settings/pages/updates_page.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/settings/update_controller.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <cstdlib>
#include <memory>
#include <optional>
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

void revealDetails(GtkToggleButton *button, GtkRevealer *revealer) {
    gboolean animations = TRUE;
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(button)),
                 "gtk-enable-animations", &animations, nullptr);
    gtk_revealer_set_transition_duration(revealer, animations ? 220 : 0);
    gtk_revealer_set_reveal_child(revealer,
                                  gtk_toggle_button_get_active(button));
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
    bool refreshCache = false;
};

struct UpdateCheckResult final {
    UpdateStatus status;
    std::optional<RuntimeResult> cacheRefresh;
};

void updateTaskFunction(GTask *task, gpointer, gpointer data, GCancellable *) {
    const auto *request = static_cast<const UpdateCheckTask *>(data);
    auto result = std::make_unique<UpdateCheckResult>();
    if (request->refreshCache) {
        UpdateStatus channel;
        channel.installChannel = UpdateController::detectInstallChannel(request->environment);
        if (channel.installChannel == "rpm") channel.manager = PackageManagerKind::Dnf;
        if (channel.installChannel == "deb") channel.manager = PackageManagerKind::Apt;
        result->cacheRefresh = UpdateController::refreshPackageCache(
            channel, request->environment, /* waitForCompletion = */ true);
    } else {
        result->status = UpdateController::checkForUpdates(request->environment);
    }
    g_task_return_pointer(
        task, result.release(),
        [](gpointer value) { delete static_cast<UpdateCheckResult *>(value); });
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
        focusCopyWhenReady = false;
        startCheck();
    }

    bool focusTarget(std::string_view target) {
        focusCopyWhenReady = false;
        if (target == "updates-check" || target == "diagnostics-check-update" ||
            target == "updates-refresh-cache") {
            return focusWidgetOrFallback(checkButton, checkButton);
        }
        if (target == "updates-copy-command") {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(upgradeDetails), TRUE);
            const bool focused = focusWidgetOrFallback(copyCommandButton, upgradeDetails);
            focusCopyWhenReady = focused && busy;
            return focused;
        }
        if (target == "updates-source" || target == "updates-source-jump") {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(versionDetails), TRUE);
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
            owner.startCheck(true);
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
        gtk_box_pack_start(GTK_BOX(page), details, FALSE, FALSE, 0);
        buildVersionSection(details);
        buildReleaseNotesSection(details);
        buildUpgradeGuideSection(details);

        setSettingsFocusChain(
            page, {checkButton, versionDetails, sourceJumpButton, releaseDetails,
                   upgradeDetails, copyCommandButton});
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
        gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
        gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 220);
        g_signal_connect_object(
            *expander, "toggled",
            G_CALLBACK(+[](GtkToggleButton *button, gpointer child) {
                revealDetails(button, GTK_REVEALER(child));
            }), revealer, static_cast<GConnectFlags>(0));
        g_signal_connect_object(
            *expander, "toggled",
            G_CALLBACK(+[](GtkToggleButton *button, gpointer image) {
                gtk_image_set_from_icon_name(
                    GTK_IMAGE(image), gtk_toggle_button_get_active(button)
                        ? "pan-down-symbolic" : "go-next-symbolic",
                    GTK_ICON_SIZE_MENU);
            }), arrow, static_cast<GConnectFlags>(0));
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
        gtk_grid_set_row_spacing(GTK_GRID(grid), 16);
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
        addRow(1, "安装渠道", "尚未检查", &channelLabel);

        auto *sourceTitle = gtk_label_new("软件源");
        addStyleClass(sourceTitle, "modernime-description");
        gtk_widget_set_halign(sourceTitle, GTK_ALIGN_START);
        gtk_widget_set_valign(sourceTitle, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), sourceTitle, 0, 2, 1, 1);

        sourceJumpButton = gtk_button_new();
        addStyleClass(sourceJumpButton, "modernime-source-link");
        auto *linkContent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_pack_start(GTK_BOX(linkContent), gtk_label_new("访问软件源"), FALSE, FALSE, 0);
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
        setAccessibleWidgetText(sourceJumpButton, "访问软件源", "在默认浏览器中查看软件源与安装说明");
        gtk_widget_set_tooltip_text(sourceJumpButton, "在默认浏览器中打开官方软件源页面");
        gtk_widget_set_halign(sourceJumpButton, GTK_ALIGN_START);
        gtk_widget_set_valign(sourceJumpButton, GTK_ALIGN_CENTER);

        g_signal_connect_data(sourceJumpButton, "clicked", G_CALLBACK(onJumpSourceClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));

        gtk_grid_attach(GTK_GRID(grid), sourceJumpButton, 1, 2, 1, 1);

        gtk_box_pack_start(GTK_BOX(section), grid, FALSE, FALSE, 0);
        checkDetailsLabel = wrappedLabel("检查后可在这里查看完整结果。", true);
        addStyleClass(checkDetailsLabel, "modernime-check-details-message");
        setTarget(checkDetailsLabel, "updates-check-details-message");
        gtk_box_pack_start(GTK_BOX(section), checkDetailsLabel, FALSE, FALSE, 0);
        statusHintLabel = wrappedLabel("");
        addStyleClass(statusHintLabel, "modernime-status-hint");
        setTarget(statusHintLabel, "updates-status-hint");
        gtk_box_pack_start(GTK_BOX(section), statusHintLabel, FALSE, FALSE, 0);
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
        setAccessibleWidgetText(checkButton, "检查更新", "刷新软件源后自动检查 ModernIME 新版本；需要时请求系统授权");

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


        g_signal_connect_data(checkButton, "clicked", G_CALLBACK(onCheckClicked),
                             new SharedLifetime(state), destroySignalState,
                             static_cast<GConnectFlags>(0));

    }

    void buildReleaseNotesSection(GtkWidget *parent) {
        auto *section = detailsSection(parent, "更新说明",
                                       "updates-release-details", &releaseDetails);
        changelogLabel = wrappedLabel("暂无", true);
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
        gtk_widget_set_halign(guideDescLabel, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(guideDescLabel), TRUE);
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

    void startCheck(bool refreshCache = false) {
        if (busy) {
            return;
        }
        busy = true;
        cacheRefreshed = false;
        gtk_widget_set_sensitive(checkButton, FALSE);
        gtk_widget_set_sensitive(copyCommandButton, FALSE);
        gtk_button_set_label(GTK_BUTTON(checkButton), "检查中…");
        setSummary(refreshCache ? "正在刷新软件源…" : "正在读取版本信息…",
                   refreshCache ? "需要时请完成系统授权，刷新后会自动检查版本。"
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

        launchTask(refreshCache);
    }

    void launchTask(bool refreshCache) {
        auto *task = g_task_new(lifetime.get(), nullptr, updateTaskFinished, nullptr);
        auto request = std::make_unique<UpdateCheckTask>();
        request->environment = environment;
        request->refreshCache = refreshCache;
        g_task_set_task_data(
            task, request.release(),
            [](gpointer value) {
                delete static_cast<UpdateCheckTask *>(value);
            });
        g_task_run_in_thread(task, updateTaskFunction);
        g_object_unref(task);
    }

    void finishCheck(const UpdateCheckResult *result, const GError *error) {
        if (error == nullptr && result != nullptr && result->cacheRefresh &&
            result->cacheRefresh->success) {
            cacheRefreshed = true;
            setSummary("正在检查新版本…", "软件源已刷新，正在读取可用版本。", "neutral");
            launchTask(false);
            return;
        }
        busy = false;
        gtk_spinner_stop(GTK_SPINNER(progressSpinner));
        gtk_widget_hide(progressSpinner);
        gtk_widget_set_sensitive(checkButton, TRUE);
        gtk_button_set_label(GTK_BUTTON(checkButton), "重新检查");
        if (error != nullptr || result == nullptr || result->cacheRefresh) {
            const char *errMsg = error != nullptr && error->message != nullptr
                                     ? error->message
                                     : result && result->cacheRefresh
                                     ? result->cacheRefresh->message.c_str()
                                     : "未知错误";
            g_warning("[ModernIME Updates] Check error: %s", errMsg);
            setSummary(result && result->cacheRefresh ? "软件源刷新未完成" : "未能完成检查",
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
            channelText = "deb (Debian/Ubuntu 软件源)";
        } else if (channelText == "rpm") {
            channelText = "rpm (Fedora/openEuler 软件源)";
        } else if (channelText == "script") {
            channelText = "script (独立用户态脚本安装)";
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
            gtk_label_set_text(GTK_LABEL(checkDetailsLabel), cacheRefreshed
                ? "已刷新已配置的软件源，并完成版本检查；未发现比当前安装版本更高的版本。"
                : "当前显示的是本地软件源缓存。点击“检查更新”会刷新软件源，并自动查询可用版本。");
        }

        gtk_label_set_text(
            GTK_LABEL(changelogLabel),
            status.changelogSummary.empty() ? "暂无更新说明" : status.changelogSummary.c_str());

        if (status.packageManagerLocked) {
            setSummary("软件包管理器正忙", "等待当前安装或更新结束后，再重新检查。",
                       "warning");
        } else if (status.updateAvailable) {
            setSummary("可更新至 " + status.candidateVersion,
                       cacheRefreshed ? "已刷新软件源，展开升级引导查看安装方式。"
                                      : "本地软件源中有可用版本，点击检查更新可刷新版本信息。",
                       "success");
        } else if (status.offlineOrUnreachable) {
            setSummary("暂时无法确认更新", "请重新检查；若仍失败，可展开检查详情查看原因。",
                       "warning");
        } else if (status.cacheMayBeStale) {
            setSummary(cacheRefreshed ? "未发现新版本" : "本地软件源缓存未发现更新",
                       cacheRefreshed ? "已刷新软件源，当前没有比已安装版本更新的版本。"
                                      : "点击检查更新，会自动刷新软件源并查询新版本。",
                       "neutral");
        } else if (status.checked) {
            setSummary("检查已结束", "暂无可用版本信息，可展开检查详情查看原因。",
                       "neutral");
        } else {
            setSummary("尚未检查更新", "检查已配置的软件源，查看是否有可用版本。",
                       "neutral");
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
    UpdateStatus lastStatus;
    bool busy = false;
    bool focusCopyWhenReady = false;
    bool cacheRefreshed = false;

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
