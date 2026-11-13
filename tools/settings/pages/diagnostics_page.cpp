#include "modernime/settings/pages/diagnostics_page.h"
#include "modernime/settings/application_diagnostics.h"
#include "modernime/settings/data_bundle_manager.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/runtime_controller.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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

void setLabel(GtkWidget *label, const char *text) {
    gtk_label_set_text(GTK_LABEL(label), text);
}

enum class RuntimeAction { Probe, Reload, EnablePanel };

GtkWidget *wrappedLabel(const char *text) {
    auto *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    return label;
}

struct RuntimeTask final {
    RuntimeAction action = RuntimeAction::Probe;
    std::filesystem::path fcitxExecutable;
    std::filesystem::path remoteExecutable;
    Environment environment;
    std::uint64_t reloadRevision = 0;
};

void runtimeTaskFunction(GTask *task, gpointer, gpointer data,
                         GCancellable *) {
    const auto *request = static_cast<const RuntimeTask *>(data);
    if (request->action != RuntimeAction::Probe) {
        g_task_return_pointer(
            task,
            new RuntimeResult(request->action == RuntimeAction::EnablePanel
                ? RuntimeController::enableDesktopPanel(request->environment)
                : RuntimeController::reload(request->fcitxExecutable, request->remoteExecutable, request->environment)),
            [](gpointer value) { delete static_cast<RuntimeResult *>(value); });
        return;
    }
    auto *status = new RuntimeStatus(RuntimeController::probe(request->remoteExecutable, request->environment));
    status->desktopPanel = RuntimeController::probeDesktopPanel(request->environment);
    g_task_return_pointer(
        task, status,
        [](gpointer value) { delete static_cast<RuntimeStatus *>(value); });
}

void applyStatusBadge(GtkWidget *widget, std::string_view text) {
    if (widget == nullptr) {
        return;
    }
    auto *context = gtk_widget_get_style_context(widget);
    for (const auto *cls : {"modernime-badge-green", "modernime-badge-neutral",
                            "modernime-badge-amber", "modernime-badge-red"}) {
        gtk_style_context_remove_class(context, cls);
    }
    if (text == "可用" || text == "正在运行" || text == "已激活") {
        gtk_style_context_add_class(context, "modernime-badge-green");
    } else if (text == "未知") {
        gtk_style_context_add_class(context, "modernime-badge-amber");
    } else if (text == "不可用" || text == "未运行" || text == "未加载" ||
               text == "检测失败") {
        gtk_style_context_add_class(context, "modernime-badge-red");
    } else {
        gtk_style_context_add_class(context, "modernime-badge-neutral");
    }
}

} // namespace

class DiagnosticsPage::Impl final {
public:
    Impl(std::filesystem::path fcitxPath,
         std::filesystem::path remotePath,
         Environment currentEnvironment,
         std::function<void(std::string)> notifyCallback,
         std::function<std::uint64_t()> reloadRevisionCallback,
         std::function<void(std::uint64_t)> reloadSucceededCallback,
         std::function<bool()> canRestore, std::function<void()> restored)
        : fcitx(std::move(fcitxPath)), remote(std::move(remotePath)),
          environment(std::move(currentEnvironment)),
          notify(std::move(notifyCallback)),
          reloadRevision(std::move(reloadRevisionCallback)),
          reloadSucceeded(std::move(reloadSucceededCallback)) {
        lifetime = detail::GObjectHandle<GObject>::adopt(
            G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr)));
        state = std::make_shared<Lifetime>(this);
        g_object_set_data_full(
            lifetime.get(), kStateKey, new SharedLifetime(state),
            [](gpointer value) {
                delete static_cast<SharedLifetime *>(value);
            });
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "系统与诊断", ""));
        page = pageGuard.get();
        auto *outerPage = page;
        auto *runtimeBody = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        page = runtimeBody;
        buildPage();
        page = outerPage;
        applications = std::make_unique<ApplicationDiagnostics>();
        bundles = std::make_unique<DataBundleManager>(std::move(canRestore), std::move(restored));
        views = gtk_stack_new();
        gtk_stack_set_homogeneous(GTK_STACK(views), FALSE);
        gtk_stack_set_interpolate_size(GTK_STACK(views), TRUE);
        gtk_stack_set_transition_type(GTK_STACK(views), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(views), 160);
        gtk_stack_add_titled(GTK_STACK(views), runtimeBody, "runtime", "运行状态");
        gtk_stack_add_titled(GTK_STACK(views), applications->widget(), "applications", "应用诊断");
        gtk_stack_add_titled(GTK_STACK(views), bundles->widget(), "bundles", "备份恢复");
        auto *switcher = gtk_stack_switcher_new();
        gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(views));
        gtk_widget_set_halign(switcher, GTK_ALIGN_START);
        addStyleClass(switcher, "modernime-dictionary-switcher");
        setAccessibleWidgetText(switcher, "诊断与数据工具", "切换运行状态、应用诊断和备份恢复");
        gtk_box_pack_start(GTK_BOX(page), switcher, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), views, FALSE, FALSE, 0);
        refresh();
        pageGuard.release();
    }

    ~Impl() { state->deactivate(); }

    GtkWidget *widget() const { return page; }

    void refresh() { startTask(RuntimeAction::Probe); if (applications) applications->refresh(); }

    void reload() { startTask(RuntimeAction::Reload); }

    bool focusTarget(std::string_view target) {
        if (applications && target.starts_with("application-")) {
            gtk_stack_set_visible_child_name(GTK_STACK(views), "applications");
            return applications->focusTarget(target);
        }
        if (bundles && (target.starts_with("bundle-") || target == "data-bundle")) {
            gtk_stack_set_visible_child_name(GTK_STACK(views), "bundles");
            return bundles->focusTarget(target);
        }
        for (auto *control : {refreshButton, reloadButton, panelEnableButton}) {
            if (control == nullptr) {
                continue;
            }
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                gtk_stack_set_visible_child_name(GTK_STACK(views), "runtime");
                if (!gtk_widget_get_sensitive(control) && busy) {
                    pendingFocusTarget = target;
                    return true;
                }
                return focusWidgetOrFallback(control, control == panelEnableButton ? panelSection : nullptr);
            }
        }
        return false;
    }

private:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;
    using SharedLifetime = std::shared_ptr<Lifetime>;

    static constexpr const char *kStateKey =
        "modernime-diagnostics-async-state";

    static void onRefresh(GtkButton *, gpointer data) {
        auto &state = *static_cast<SharedLifetime *>(data);
        state->withOwner([](Impl &owner) { owner.refresh(); });
    }

    static void onReload(GtkButton *, gpointer data) {
        auto &state = *static_cast<SharedLifetime *>(data);
        state->withOwner([](Impl &owner) { owner.reload(); });
    }

    static void destroySignalState(gpointer data, GClosure *) {
        delete static_cast<SharedLifetime *>(data);
    }

    static void runtimeTaskFinished(GObject *source, GAsyncResult *result,
                                    gpointer) {
        auto *task = G_TASK(result);
        const auto *request = static_cast<const RuntimeTask *>(
            g_task_get_task_data(task));
        auto *state = static_cast<SharedLifetime *>(
            g_object_get_data(source, kStateKey));
        GError *error = nullptr;

        if (request->action != RuntimeAction::Probe) {
            auto *reloadResult = static_cast<RuntimeResult *>(
                g_task_propagate_pointer(task, &error));
            if (state != nullptr) {
                const auto revision = request->reloadRevision;
                const auto action = request->action;
                (*state)->withOwner([reloadResult, error, revision, action](Impl &owner) {
                    owner.finishReload(reloadResult, error, revision, action);
                });
            }
            delete reloadResult;
        } else {
            auto *runtimeStatus = static_cast<RuntimeStatus *>(
                g_task_propagate_pointer(task, &error));
            if (state != nullptr) {
                (*state)->withOwner([runtimeStatus, error](Impl &owner) {
                    owner.finishRefresh(runtimeStatus, error);
                });
            }
            delete runtimeStatus;
        }
        g_clear_error(&error);
    }

    void buildPage() {
        auto *section = createSectionCard("", "");
        addStyleClass(section, "modernime-diagnostics-card");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        statusBanner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        addStyleClass(statusBanner, "modernime-runtime-summary");
        statusArt = createSettingsStatusArt(SettingsStatusArt::Input);
        gtk_box_pack_start(GTK_BOX(statusBanner), statusArt, FALSE, FALSE, 0);

        auto *summary = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        statusMessage = wrappedLabel("正在检查运行状态…");
        addStyleClass(statusMessage, "modernime-status-heading");
        setTarget(statusMessage, "diagnostics-summary");
        gtk_box_pack_start(GTK_BOX(summary), statusMessage, FALSE, FALSE, 0);
        nextStep = wrappedLabel("正在读取 Fcitx5 与 ModernIME 的状态。");
        addStyleClass(nextStep, "modernime-status-hint");
        setTarget(nextStep, "diagnostics-next-step");
        gtk_box_pack_start(GTK_BOX(summary), nextStep, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(statusBanner), summary, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(section), statusBanner, FALSE, FALSE, 0);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(actions, 18);
        refreshButton = gtk_button_new_with_label("刷新状态");
        addStyleClass(refreshButton, kSettingsPrimaryButtonClass);
        reloadButton = gtk_button_new_with_label("重新加载 ModernIME");
        setTarget(refreshButton, "diagnostics-refresh");
        setTarget(reloadButton, "diagnostics-reload");
        gtk_widget_set_tooltip_text(refreshButton,
                                    "重新查询 Fcitx5 和 ModernIME 状态");
        gtk_widget_set_tooltip_text(reloadButton,
                                    "请求 Fcitx5 重新加载并激活 ModernIME");
        setAccessibleWidgetText(refreshButton, "刷新状态",
                                "重新查询 Fcitx5 和 ModernIME 运行状态");
        setAccessibleWidgetText(reloadButton, "重新加载 ModernIME",
                                "请求 Fcitx5 重新加载并激活 ModernIME");
        gtk_box_pack_start(GTK_BOX(actions), refreshButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), reloadButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), actions, FALSE, FALSE, 0);
        g_signal_connect_data(
            refreshButton, "clicked", G_CALLBACK(onRefresh),
            new SharedLifetime(state), destroySignalState,
            static_cast<GConnectFlags>(0));
        g_signal_connect_data(
            reloadButton, "clicked", G_CALLBACK(onReload),
            new SharedLifetime(state), destroySignalState,
            static_cast<GConnectFlags>(0));

        panelSection = createSectionCard("GNOME 候选面板", "");
        gtk_box_pack_start(GTK_BOX(page), panelSection, FALSE, FALSE, 0);
        panelTitle = gtk_label_new("检测中…");
        setTarget(panelTitle, "diagnostics-panel-state");
        gtk_widget_set_halign(panelTitle, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(panelSection), panelTitle, FALSE, FALSE, 0);
        panelMessage = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(panelMessage), TRUE);
        gtk_label_set_xalign(GTK_LABEL(panelMessage), 0);
        gtk_box_pack_start(GTK_BOX(panelSection), panelMessage, FALSE, FALSE, 0);
        panelEnableButton = gtk_button_new_with_label("启用候选面板");
        setTarget(panelEnableButton, "diagnostics-panel-enable");
        gtk_widget_set_halign(panelEnableButton, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(panelSection), panelEnableButton, FALSE, FALSE, 0);
        g_signal_connect_data(panelEnableButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            static_cast<SharedLifetime *>(data)->get()->withOwner([](Impl &owner) {
                owner.startTask(RuntimeAction::EnablePanel);
            });
        }), new SharedLifetime(state), destroySignalState, static_cast<GConnectFlags>(0));
        gtk_widget_show_all(panelSection);
        gtk_widget_set_no_show_all(panelSection, TRUE);
        gtk_widget_hide(panelSection);

        auto *detailsCard = createSectionCard("", "");
        addStyleClass(detailsCard, "modernime-diagnostics-card");
        gtk_box_pack_start(GTK_BOX(page), detailsCard, FALSE, FALSE, 0);
        auto *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        addStyleClass(details, "modernime-details");
        detailsToggle = gtk_toggle_button_new();
        gtk_widget_set_focus_on_click(detailsToggle, FALSE);
        addStyleClass(detailsToggle, "modernime-details-button");
        setTarget(detailsToggle, "diagnostics-details");
        setAccessibleWidgetText(detailsToggle, "运行详情", "展开或收起底层检测结果");
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto *title = gtk_label_new("运行详情");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
        auto *arrow = gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        gtk_box_pack_end(GTK_BOX(header), arrow, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(detailsToggle), header);
        auto *revealer = gtk_revealer_new();
        bindSettingsDisclosure(detailsToggle, revealer, arrow);
        auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        addStyleClass(content, "modernime-details-content");
        auto *statusRows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        const auto labels = settingsRuntimeStatusLabels();
        const auto addStatusRow = [statusRows](std::string_view title,
                                               GtkWidget **value) {
            *value = gtk_label_new("检测中…");
            applyStatusBadge(*value, "检测中…");
            addStyleClass(*value, "modernime-diagnostics-value");
            gtk_widget_set_valign(*value, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(statusRows),
                               createSettingRow(title, "", *value), FALSE, FALSE, 0);
        };
        addStatusRow(labels[0], &availability);
        addStatusRow(labels[1], &service);
        addStatusRow(labels[2], &inputMethod);
        addStatusRow(labels[3], &modernime);
        gtk_box_pack_start(GTK_BOX(content), statusRows, FALSE, FALSE, 0);
        technicalMessage = wrappedLabel("");
        gtk_widget_set_no_show_all(technicalMessage, TRUE);
        gtk_label_set_selectable(GTK_LABEL(technicalMessage), TRUE);
        addStyleClass(technicalMessage, "modernime-check-details-message");
        gtk_box_pack_start(GTK_BOX(content), technicalMessage, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(revealer), content);
        gtk_box_pack_start(GTK_BOX(details), detailsToggle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(details), revealer, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(detailsCard), details, FALSE, FALSE, 0);

        setSettingsFocusChain(page, {refreshButton, reloadButton, panelEnableButton, detailsToggle});
    }

    void startTask(RuntimeAction action) {
        if (busy) {
            return;
        }
        setBusy(true);
        if (action == RuntimeAction::EnablePanel) {
            gtk_button_set_label(GTK_BUTTON(panelEnableButton), "正在启用…");
        } else {
            const bool reloading = action == RuntimeAction::Reload;
            gtk_button_set_label(GTK_BUTTON(reloading ? reloadButton : refreshButton),
                                 reloading ? "正在重新加载…" : "正在刷新…");
        }
        auto *task = g_task_new(lifetime.get(), nullptr, runtimeTaskFinished,
                                nullptr);
        const auto revision = action == RuntimeAction::Reload && reloadRevision ? reloadRevision() : 0;
        auto *request = new RuntimeTask{action, fcitx, remote, environment,
                                        revision};
        g_task_set_task_data(task, request, [](gpointer value) {
            delete static_cast<RuntimeTask *>(value);
        });
        g_task_run_in_thread(task, runtimeTaskFunction);
        g_object_unref(task);
    }

    void finishRefresh(const RuntimeStatus *runtimeStatus, const GError *error) {
        if (runtimeStatus != nullptr) {
            updateStatus(*runtimeStatus);
        } else {
            updateFailure(error == nullptr ? nullptr : error->message);
        }
        setBusy(false);
    }

    void finishReload(const RuntimeResult *result, const GError *error,
                      std::uint64_t revision, RuntimeAction action) {
        const bool success = result != nullptr && result->success;
        setBusy(false, !success);
        if (result == nullptr) {
            notifyMessage(error == nullptr ? "ModernIME 重载失败"
                                           : error->message);
            return;
        }
        notifyMessage(result->message);
        if (result->success) {
            if (action == RuntimeAction::Reload && reloadSucceeded) {
                reloadSucceeded(revision);
            }
            refresh();
        }
    }

    void updateStatus(const RuntimeStatus &runtimeStatus) {
        const auto &panel = runtimeStatus.desktopPanel;
        panelCanEnable = panel.state == DesktopPanelState::Disabled;
        if (panel.state != DesktopPanelState::NotApplicable) {
            gtk_widget_show_all(panelSection);
            gtk_widget_show(panelSection);
            setLabel(panelTitle, panel.title.c_str());
            setLabel(panelMessage, panel.message.c_str());
            gtk_widget_set_visible(panelEnableButton, panelCanEnable);
        } else gtk_widget_hide(panelSection);
        const auto availText = runtimeStatus.available ? "可用" : "不可用";
        setLabel(availability, availText);
        applyStatusBadge(availability, availText);

        const auto serviceText = runtimeStatus.running ? "正在运行" : "未运行";
        setLabel(service, serviceText);
        applyStatusBadge(service, serviceText);

        const std::string imText =
            runtimeStatus.currentInputMethod.empty()
                ? (runtimeStatus.inputContextAvailable ? "未获取"
                                                       : "暂无输入上下文")
                : runtimeStatus.currentInputMethod;
        setLabel(inputMethod, imText.c_str());
        applyStatusBadge(inputMethod, imText);

        const std::string modernimeText =
            runtimeStatus.modernimeActive
                ? "已激活"
                : (!runtimeStatus.modernimeAvailable
                       ? "未加载"
                       : (!runtimeStatus.inputContextAvailable &&
                                  runtimeStatus.running
                              ? "已就绪"
                              : (runtimeStatus.running ? "未激活"
                                                       : "未运行")));
        setLabel(modernime, modernimeText.c_str());
        applyStatusBadge(modernime, modernimeText);

        setLabel(technicalMessage,
                 runtimeStatus.message.empty()
                     ? "无法读取 Fcitx5 状态"
                     : runtimeStatus.message.c_str());
        gtk_widget_set_visible(technicalMessage, !runtimeStatus.available || !runtimeStatus.running);

        if (!runtimeStatus.available) {
            setSummary("Fcitx5 检查工具不可用",
                       "请确认 Fcitx5 已安装并启动，再刷新状态。",
                       "error");
        } else if (!runtimeStatus.running) {
            setSummary("暂时无法连接 Fcitx5",
                       "请确认 Fcitx5 已启动。具体原因可在运行详情中查看。",
                       "error");
        } else if (!runtimeStatus.modernimeAvailable) {
            setSummary("ModernIME 尚未加载",
                       "确认已安装 ModernIME 后，尝试重新加载；若仍不可用，请查看运行详情。",
                       "warning");
        } else if (runtimeStatus.modernimeActive) {
            setSummary("ModernIME 正在运行",
                       "输入法已激活，可以继续输入。无需重新加载。",
                       "success");
        } else if (!runtimeStatus.inputContextAvailable) {
            setSummary("ModernIME 已就绪",
                       "当前窗口没有输入焦点。点击应用中的输入框后，即可切换并使用 ModernIME。",
                       "neutral");
        } else {
            setSummary("ModernIME 等待切换",
                       "服务运行正常。请在输入框中切换到 ModernIME，即可开始输入。",
                       "neutral");
        }
    }

    void setSummary(const char *title, const char *hint,
                    const char *stateClass) {
        setLabel(statusMessage, title);
        setLabel(nextStep, hint);
        auto *context = gtk_widget_get_style_context(statusBanner);
        for (const auto *name : {"neutral", "success", "warning", "error"}) {
            gtk_style_context_remove_class(context, name);
        }
        gtk_style_context_add_class(context, stateClass);
        gtk_widget_queue_draw(statusArt);
    }

    void updateFailure(const char *message) {
        setLabel(availability, "检测失败");
        applyStatusBadge(availability, "检测失败");
        setLabel(service, "未知");
        applyStatusBadge(service, "未知");
        setLabel(inputMethod, "未获取");
        applyStatusBadge(inputMethod, "未获取");
        setLabel(modernime, "未知");
        applyStatusBadge(modernime, "未知");
        setLabel(technicalMessage,
                 message == nullptr || *message == '\0'
                     ? "无法读取 Fcitx5 状态"
                     : message);
        gtk_widget_show(technicalMessage);
        setSummary("未能读取运行状态", "请刷新重试，或展开运行详情查看原因。",
                   "warning");
    }

    void setBusy(bool value, bool resolvePendingFocus = true) {
        busy = value;
        gtk_widget_set_sensitive(refreshButton, !busy);
        gtk_widget_set_sensitive(reloadButton, !busy);
        gtk_widget_set_sensitive(panelEnableButton, !busy && panelCanEnable);
        if (!busy) {
            gtk_button_set_label(GTK_BUTTON(refreshButton), "刷新状态");
            gtk_button_set_label(GTK_BUTTON(reloadButton), "重新加载 ModernIME");
            gtk_button_set_label(GTK_BUTTON(panelEnableButton), "启用候选面板");
        }
        if (!busy && resolvePendingFocus && !pendingFocusTarget.empty()) {
            const auto target = std::move(pendingFocusTarget);
            pendingFocusTarget.clear();
            focusTarget(target);
        }
    }

    void notifyMessage(std::string message) const {
        if (notify) {
            notify(std::move(message));
        }
    }

    std::filesystem::path fcitx;
    std::filesystem::path remote;
    Environment environment;
    std::function<void(std::string)> notify;
    std::function<std::uint64_t()> reloadRevision;
    std::function<void(std::uint64_t)> reloadSucceeded;
    detail::GObjectHandle<GObject> lifetime;
    SharedLifetime state;
    bool busy = false;
    bool panelCanEnable = false;
    std::string pendingFocusTarget;
    GtkWidget *page = nullptr;
    GtkWidget *statusBanner = nullptr;
    GtkWidget *statusArt = nullptr;
    GtkWidget *statusMessage = nullptr;
    GtkWidget *nextStep = nullptr;
    GtkWidget *technicalMessage = nullptr;
    GtkWidget *detailsToggle = nullptr;
    GtkWidget *availability = nullptr;
    GtkWidget *service = nullptr;
    GtkWidget *inputMethod = nullptr;
    GtkWidget *modernime = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *reloadButton = nullptr;
    GtkWidget *views = nullptr;
    std::unique_ptr<ApplicationDiagnostics> applications;
    std::unique_ptr<DataBundleManager> bundles;
    GtkWidget *panelSection = nullptr;
    GtkWidget *panelTitle = nullptr;
    GtkWidget *panelMessage = nullptr;
    GtkWidget *panelEnableButton = nullptr;
};

DiagnosticsPage::DiagnosticsPage(std::filesystem::path fcitx,
                                 std::filesystem::path remote,
                                 Environment environment,
                                 std::function<void(std::string)> notify,
                                 std::function<std::uint64_t()> reloadRevision,
                                 std::function<void(std::uint64_t)> reloadSucceeded,
                                 std::function<bool()> canRestore, std::function<void()> restored)
    : impl_(std::make_unique<Impl>(std::move(fcitx), std::move(remote),
                                   std::move(environment),
                                   std::move(notify),
                                   std::move(reloadRevision),
                                   std::move(reloadSucceeded),
                                   std::move(canRestore), std::move(restored))) {}

DiagnosticsPage::~DiagnosticsPage() = default;

GtkWidget *DiagnosticsPage::widget() const {
    return impl_->widget();
}

void DiagnosticsPage::refresh() {
    impl_->refresh();
}

void DiagnosticsPage::reload() {
    impl_->reload();
}

bool DiagnosticsPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
