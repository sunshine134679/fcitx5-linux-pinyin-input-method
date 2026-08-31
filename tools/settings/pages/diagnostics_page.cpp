#include "modernime/settings/pages/diagnostics_page.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <array>
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

void setLabel(GtkWidget *label, const char *text) {
    if (label != nullptr) {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
}

struct RuntimeTask final {
    bool reload = false;
    std::uint64_t reloadRevision = 0;
    std::filesystem::path fcitxExecutable;
    std::filesystem::path remoteExecutable;
    Environment environment;
};

void runtimeTaskFunction(GTask *task, gpointer, gpointer data,
                         GCancellable *) {
    const auto *request = static_cast<const RuntimeTask *>(data);
    if (request->reload) {
        g_task_return_pointer(
            task,
            new RuntimeResult(RuntimeController::reload(
                request->fcitxExecutable, request->remoteExecutable,
                request->environment)),
            [](gpointer value) { delete static_cast<RuntimeResult *>(value); });
        return;
    }
    g_task_return_pointer(
        task,
        new RuntimeStatus(RuntimeController::probe(request->remoteExecutable,
                                                   request->environment)),
        [](gpointer value) { delete static_cast<RuntimeStatus *>(value); });
}

} // namespace

class DiagnosticsPage::Impl final {
public:
    Impl(std::filesystem::path fcitxPath,
         std::filesystem::path remotePath,
         Environment currentEnvironment,
         std::function<void(std::string)> notifyCallback,
         std::function<std::uint64_t()> reloadRevisionCallback,
         std::function<void(std::uint64_t)> reloadSucceededCallback)
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
            "系统与诊断", "检查 Fcitx5、ModernIME 插件和当前激活状态"));
        page = pageGuard.get();
        buildPage();
        refresh();
        pageGuard.release();
    }

    ~Impl() { state->deactivate(); }

    GtkWidget *widget() const { return page; }

    void refresh() { startTask(false); }

    void reload() { startTask(true); }

    bool focusTarget(std::string_view target) {
        for (auto *control : {refreshButton, reloadButton}) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                if (!gtk_widget_get_sensitive(control)) {
                    pendingFocusTarget = target;
                    return true;
                }
                return focusWidgetOrFallback(control);
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

    static void taskFinished(GObject *source, GAsyncResult *result,
                             gpointer) {
        auto *task = G_TASK(result);
        const auto *request = static_cast<const RuntimeTask *>(
            g_task_get_task_data(task));
        auto *state = static_cast<SharedLifetime *>(
            g_object_get_data(source, kStateKey));
        GError *error = nullptr;

        if (request->reload) {
            auto *reloadResult = static_cast<RuntimeResult *>(
                g_task_propagate_pointer(task, &error));
            if (state != nullptr) {
                const auto revision = request->reloadRevision;
                (*state)->withOwner([reloadResult, error, revision](Impl &owner) {
                    owner.finishReload(reloadResult, error, revision);
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
        auto *section = createSectionCard(
            "运行状态", "如果状态异常，可以在这里重新加载 ModernIME。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        statusMessage = gtk_label_new("正在读取 Fcitx5 状态…");
        addStyleClass(statusMessage, "modernime-status");
        gtk_widget_set_halign(statusMessage, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(statusMessage), TRUE);
        gtk_box_pack_start(GTK_BOX(section), statusMessage, FALSE, FALSE, 0);

        auto *statusGrid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(statusGrid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(statusGrid), 16);
        const auto labels = settingsRuntimeStatusLabels();
        const auto addStatusRow = [statusGrid](int row,
                                               std::string_view title,
                                               GtkWidget **value) {
            auto *label = gtk_label_new(std::string(title).c_str());
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(statusGrid), label, 0, row, 1, 1);
            *value = gtk_label_new("检测中…");
            addStyleClass(*value, "modernime-status");
            gtk_widget_set_halign(*value, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(statusGrid), *value, 1, row, 1, 1);
        };
        addStatusRow(0, labels[0], &availability);
        addStatusRow(1, labels[1], &service);
        addStatusRow(2, labels[2], &inputMethod);
        addStatusRow(3, labels[3], &modernime);
        gtk_box_pack_start(GTK_BOX(section), statusGrid, FALSE, FALSE, 0);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        refreshButton = gtk_button_new_with_label("刷新状态");
        reloadButton = gtk_button_new_with_label("重新加载 ModernIME");
        setTarget(refreshButton, "diagnostics-refresh");
        setTarget(reloadButton, "diagnostics-reload");
        gtk_widget_set_tooltip_text(refreshButton,
                                    "重新查询 Fcitx5 和 ModernIME 状态");
        gtk_widget_set_tooltip_text(reloadButton,
                                    "保存配置后重新加载 ModernIME");
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
        setSettingsFocusChain(page, {refreshButton, reloadButton});
    }

    void startTask(bool reloadRequest) {
        if (busy) {
            return;
        }
        busy = true;
        gtk_widget_set_sensitive(refreshButton, FALSE);
        gtk_widget_set_sensitive(reloadButton, FALSE);
        if (reloadRequest) {
            notifyMessage("正在请求重载 ModernIME…");
        } else {
            setLabel(statusMessage, "正在检测 Fcitx5 状态…");
        }

        auto *task =
            g_task_new(lifetime.get(), nullptr, taskFinished, nullptr);
        auto *request = new RuntimeTask{
            reloadRequest,
            reloadRequest && reloadRevision ? reloadRevision() : 0,
            fcitx, remote, environment};
        g_task_set_task_data(task, request, [](gpointer value) {
            delete static_cast<RuntimeTask *>(value);
        });
        g_task_run_in_thread(task, runtimeTaskFunction);
        g_object_unref(task);
    }

    void finishRefresh(const RuntimeStatus *runtimeStatus,
                       const GError *error) {
        if (runtimeStatus != nullptr) {
            updateStatus(*runtimeStatus);
        } else {
            updateFailure(error == nullptr ? nullptr : error->message);
        }
        setBusy(false);
    }

    void finishReload(const RuntimeResult *result, const GError *error,
                      std::uint64_t revision) {
        const bool success = result != nullptr && result->success;
        setBusy(false, !success);
        if (result == nullptr) {
            notifyMessage(error == nullptr ? "ModernIME 重载失败"
                                           : error->message);
            return;
        }
        notifyMessage(result->success ? "ModernIME 已重新加载并激活"
                                      : result->message);
        if (result->success) {
            if (reloadSucceeded) {
                reloadSucceeded(revision);
            }
            refresh();
        }
    }

    void updateStatus(const RuntimeStatus &runtimeStatus) {
        setLabel(availability, runtimeStatus.available ? "可用" : "不可用");
        setLabel(service, runtimeStatus.running ? "正在运行" : "未运行");
        setLabel(inputMethod,
                 runtimeStatus.currentInputMethod.empty()
                     ? (runtimeStatus.inputContextAvailable ? "未获取"
                                                            : "暂无输入上下文")
                     : runtimeStatus.currentInputMethod.c_str());
        setLabel(
            modernime,
            runtimeStatus.modernimeActive
                ? "已激活"
                : (!runtimeStatus.modernimeAvailable
                       ? "未加载"
                       : (!runtimeStatus.inputContextAvailable &&
                                  runtimeStatus.running
                              ? "已就绪"
                              : (runtimeStatus.running ? "未激活"
                                                       : "未运行"))));
        setLabel(statusMessage,
                 runtimeStatus.message.empty()
                     ? "无法读取 Fcitx5 状态"
                     : runtimeStatus.message.c_str());
    }

    void updateFailure(const char *message) {
        setLabel(availability, "检测失败");
        setLabel(service, "未知");
        setLabel(inputMethod, "未获取");
        setLabel(modernime, "未知");
        setLabel(statusMessage,
                 message == nullptr || *message == '\0'
                     ? "无法读取 Fcitx5 状态"
                     : message);
    }

    void setBusy(bool value, bool resolvePendingFocus = true) {
        busy = value;
        gtk_widget_set_sensitive(refreshButton, !busy);
        gtk_widget_set_sensitive(reloadButton, !busy);
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
    std::string pendingFocusTarget;
    GtkWidget *page = nullptr;
    GtkWidget *statusMessage = nullptr;
    GtkWidget *availability = nullptr;
    GtkWidget *service = nullptr;
    GtkWidget *inputMethod = nullptr;
    GtkWidget *modernime = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *reloadButton = nullptr;
};

DiagnosticsPage::DiagnosticsPage(std::filesystem::path fcitx,
                                 std::filesystem::path remote,
                                 Environment environment,
                                 std::function<void(std::string)> notify,
                                 std::function<std::uint64_t()> reloadRevision,
                                 std::function<void(std::uint64_t)> reloadSucceeded)
    : impl_(std::make_unique<Impl>(std::move(fcitx), std::move(remote),
                                   std::move(environment),
                                   std::move(notify),
                                   std::move(reloadRevision),
                                   std::move(reloadSucceeded))) {}

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
