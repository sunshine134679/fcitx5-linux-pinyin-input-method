#include "modernime/settings/settings_window.h"
#include "modernime/settings/pages/clipboard_page.h"
#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/pages/input_page.h"
#include "modernime/settings/pages/learning_page.h"
#include "modernime/settings/runtime_controller.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modernime::settings {

class SettingsWindow::Impl final {
public:
    Impl(void *application, core::SettingsPaths paths)
        : application_(application), paths_(std::move(paths)),
          model(paths_.settingsFile) {}

    void *application_;
    core::SettingsPaths paths_;
    SettingsWindowModel model;
    GtkWidget *window = nullptr;
    GtkWidget *stack = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *editState = nullptr;
    GtkWidget *saveButton = nullptr;
    GtkWidget *applyButton = nullptr;
    GtkWidget *resetButton = nullptr;
    GtkWidget *defaultsButton = nullptr;
    std::unique_ptr<InputPage> inputPage;
    std::unique_ptr<ClipboardPage> clipboardPage;
    std::unique_ptr<LearningPage> learningPage;
    std::unique_ptr<DictionaryPage> dictionaryPage;
    GtkWidget *runtimeStatus = nullptr;
    GtkWidget *runtimeAvailability = nullptr;
    GtkWidget *runtimeService = nullptr;
    GtkWidget *runtimeInputMethod = nullptr;
    GtkWidget *runtimeModernime = nullptr;
    GtkWidget *refreshStatusButton = nullptr;
    GtkWidget *reloadButton = nullptr;
};

namespace {

void setStatus(SettingsWindow::Impl *impl, const char *message) {
    if (impl->status != nullptr) {
        gtk_label_set_text(GTK_LABEL(impl->status), message);
    }
}

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void updateActionState(SettingsWindow::Impl *impl) {
    if (impl->editState == nullptr) {
        return;
    }
    const auto validation = impl->model.validation();
    const bool canSave = impl->model.dirty() && validation.valid;
    for (auto *button : {impl->saveButton, impl->applyButton}) {
        if (button != nullptr) {
            gtk_widget_set_sensitive(button, canSave);
        }
    }
    if (impl->resetButton != nullptr) {
        gtk_widget_set_sensitive(impl->resetButton, impl->model.dirty());
    }

    auto *context = gtk_widget_get_style_context(impl->editState);
    gtk_style_context_remove_class(context, "modernime-status-dirty");
    gtk_style_context_remove_class(context, "modernime-status-error");
    if (!validation.valid) {
        addStyleClass(impl->editState, "modernime-status-error");
        const auto message = validation.issues.empty()
                                  ? std::string("当前设置无法保存")
                                  : validation.issues.front().message;
        gtk_label_set_text(GTK_LABEL(impl->editState), message.c_str());
    } else if (impl->model.dirty()) {
        addStyleClass(impl->editState, "modernime-status-dirty");
        gtk_label_set_text(GTK_LABEL(impl->editState), "有未保存修改");
    } else {
        gtk_label_set_text(GTK_LABEL(impl->editState), "所有设置已保存");
    }

}

void onStackVisibleChildChanged(GObject *object, GParamSpec *, gpointer data) {
    const auto *name = gtk_stack_get_visible_child_name(GTK_STACK(object));
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    if (name != nullptr && std::string_view(name) == "clipboard" &&
        impl->clipboardPage != nullptr) {
        impl->clipboardPage->refresh(false);
    }
}

bool saveEditedSettings(SettingsWindow::Impl *impl, bool closeAfterSave) {
    const auto validation = impl->model.validation();
    if (!validation.valid) {
        const auto message = validation.issues.empty()
                                  ? std::string("当前设置无法保存")
                                  : validation.issues.front().message;
        setStatus(impl, message.c_str());
        updateActionState(impl);
        return false;
    }
    if (!impl->model.dirty()) {
        setStatus(impl, "没有需要保存的修改");
        if (closeAfterSave) {
            gtk_widget_hide(impl->window);
        }
        return true;
    }
    std::string error;
    if (impl->model.save(&error)) {
        setStatus(impl, closeAfterSave
                           ? "设置已保存；重新加载输入法后生效"
                           : "设置已应用；重新加载输入法后生效");
        updateActionState(impl);
        if (closeAfterSave) {
            gtk_widget_hide(impl->window);
        }
        return true;
    } else {
        setStatus(impl, error.c_str());
        updateActionState(impl);
        return false;
    }
}

void onSave(GtkButton *, gpointer data) {
    saveEditedSettings(static_cast<SettingsWindow::Impl *>(data), true);
}

void onApply(GtkButton *, gpointer data) {
    saveEditedSettings(static_cast<SettingsWindow::Impl *>(data), false);
}

void onResetEdits(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    impl->model.resetEdits();
    impl->inputPage->refresh();
    impl->clipboardPage->refreshSettings();
    impl->learningPage->refreshSettings();
    updateActionState(impl);
    setStatus(impl, "已恢复未保存的修改");
}

void onResetDefaults(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "这只会恢复 ModernIME 的设置默认值，不会删除学习记录或用户词典。继续吗？");
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_YES) {
        return;
    }
    impl->model.editDefaults();
    impl->inputPage->refresh();
    impl->clipboardPage->refreshSettings();
    impl->learningPage->refreshSettings();
    updateActionState(impl);
    setStatus(impl, "ModernIME 设置已恢复默认值，请保存后生效");
}

bool confirmDiscardChanges(SettingsWindow::Impl *impl) {
    if (!impl->model.dirty()) {
        return true;
    }
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE, "当前有未保存的修改，关闭后将丢失。仍要关闭吗？");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "继续编辑", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "放弃修改并关闭", GTK_RESPONSE_YES);
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_YES;
}

gboolean onWindowDelete(GtkWidget *, GdkEvent *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    if (!confirmDiscardChanges(impl)) {
        return TRUE;
    }
    gtk_widget_hide(impl->window);
    return TRUE;
}


std::string modernimeAddonDirectories() {
    std::vector<std::string> directories;
#ifdef MODERNIME_INSTALL_PREFIX
    directories.emplace_back(
        (std::filesystem::path(MODERNIME_INSTALL_PREFIX) / "lib/fcitx5")
            .string());
#endif
#ifdef MODERNIME_SYSTEM_FCITX5_ADDON_DIR
    if (std::string_view(MODERNIME_SYSTEM_FCITX5_ADDON_DIR).size() != 0) {
        directories.emplace_back(MODERNIME_SYSTEM_FCITX5_ADDON_DIR);
    }
#endif
    if (const auto *existing = std::getenv("FCITX_ADDON_DIRS");
        existing != nullptr && *existing != '\0') {
        directories.emplace_back(existing);
    }

    std::ostringstream result;
    for (std::size_t index = 0; index < directories.size(); ++index) {
        if (index != 0) {
            result << ':';
        }
        result << directories[index];
    }
    return result.str();
}

Environment currentEnvironment() {
    Environment environment;
    for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY",
                             "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"}) {
        const auto *value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            environment.emplace_back(name, value);
        }
    }
    const auto addonDirectories = modernimeAddonDirectories();
    if (!addonDirectories.empty()) {
        environment.emplace_back("FCITX_ADDON_DIRS", addonDirectories);
    }
    return environment;
}

std::filesystem::path fcitxPath() {
    auto *path = g_find_program_in_path("fcitx5");
    if (path == nullptr) {
        return "fcitx5";
    }
    const std::filesystem::path result(path);
    g_free(path);
    return result;
}

std::filesystem::path fcitxRemotePath() {
    auto *path = g_find_program_in_path("fcitx5-remote");
    if (path == nullptr) {
        return "fcitx5-remote";
    }
    const std::filesystem::path result(path);
    g_free(path);
    return result;
}

struct RuntimeTask final {
    SettingsWindow::Impl *impl;
    bool reload;
    std::filesystem::path fcitxExecutable;
    std::filesystem::path executable;
    Environment environment;
};

void startRuntimeTask(SettingsWindow::Impl *impl, bool reload);

void runtimeTaskFunction(GTask *task, gpointer, gpointer data,
                         GCancellable *) {
    const auto *request = static_cast<const RuntimeTask *>(data);
    if (request->reload) {
        g_task_return_pointer(
            task,
            new RuntimeResult(RuntimeController::reload(
                request->fcitxExecutable, request->executable,
                request->environment)),
            [](gpointer value) { delete static_cast<RuntimeResult *>(value); });
    } else {
        g_task_return_pointer(
            task,
            new RuntimeStatus(RuntimeController::probe(request->executable,
                                                       request->environment)),
            [](gpointer value) { delete static_cast<RuntimeStatus *>(value); });
    }
}

void setRuntimeLabel(GtkWidget *label, const char *text) {
    if (label != nullptr) {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
}

void updateRuntimeStatusWidgets(SettingsWindow::Impl *impl,
                                const RuntimeStatus &status) {
    setRuntimeLabel(impl->runtimeAvailability,
                    status.available ? "可用" : "不可用");
    setRuntimeLabel(impl->runtimeService,
                    status.running ? "正在运行" : "未运行");
    setRuntimeLabel(impl->runtimeInputMethod,
                    status.currentInputMethod.empty()
                        ? (status.inputContextAvailable ? "未获取"
                                                        : "暂无输入上下文")
                        : status.currentInputMethod.c_str());
    setRuntimeLabel(impl->runtimeModernime,
                    status.modernimeActive
                        ? "已激活"
                        : (!status.modernimeAvailable
                               ? "未加载"
                               : (!status.inputContextAvailable && status.running
                                      ? "已就绪"
                                      : (status.running ? "未激活" : "未运行"))));
    setRuntimeLabel(impl->runtimeStatus,
                    status.message.empty() ? "无法读取 Fcitx5 状态"
                                           : status.message.c_str());
}

void updateRuntimeFailureWidgets(SettingsWindow::Impl *impl,
                                 const char *message) {
    setRuntimeLabel(impl->runtimeAvailability, "检测失败");
    setRuntimeLabel(impl->runtimeService, "未知");
    setRuntimeLabel(impl->runtimeInputMethod, "未获取");
    setRuntimeLabel(impl->runtimeModernime, "未知");
    setRuntimeLabel(impl->runtimeStatus,
                    message == nullptr || *message == '\0'
                        ? "无法读取 Fcitx5 状态"
                        : message);
}

void runtimeTaskFinished(GObject *, GAsyncResult *result, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    auto *task = G_TASK(result);
    const auto *request = static_cast<const RuntimeTask *>(
        g_task_get_task_data(task));
    GError *error = nullptr;
    if (request->reload) {
        auto *reload = static_cast<RuntimeResult *>(
            g_task_propagate_pointer(task, &error));
        if (reload != nullptr) {
            setStatus(impl, reload->success ? "ModernIME 已重新加载并激活"
                                             : reload->message.c_str());
            gtk_widget_set_sensitive(impl->reloadButton, TRUE);
            gtk_widget_set_sensitive(impl->refreshStatusButton, TRUE);
            if (reload->success) {
                gtk_widget_set_sensitive(impl->reloadButton, FALSE);
                gtk_widget_set_sensitive(impl->refreshStatusButton, FALSE);
                startRuntimeTask(impl, false);
            }
        } else {
            setStatus(impl, error == nullptr ? "ModernIME 重载失败"
                                             : error->message);
            gtk_widget_set_sensitive(impl->reloadButton, TRUE);
            gtk_widget_set_sensitive(impl->refreshStatusButton, TRUE);
        }
        delete reload;
        g_clear_error(&error);
    } else {
        auto *status = static_cast<RuntimeStatus *>(
            g_task_propagate_pointer(task, &error));
        if (status != nullptr) {
            updateRuntimeStatusWidgets(impl, *status);
        } else {
            updateRuntimeFailureWidgets(impl,
                                        error == nullptr ? nullptr
                                                          : error->message);
        }
        gtk_widget_set_sensitive(impl->refreshStatusButton, TRUE);
        gtk_widget_set_sensitive(impl->reloadButton, TRUE);
        delete status;
        g_clear_error(&error);
    }
}

void startRuntimeTask(SettingsWindow::Impl *impl, bool reload) {
    auto *task = g_task_new(G_OBJECT(impl->window), nullptr,
                            runtimeTaskFinished, impl);
    auto *request = new RuntimeTask{impl, reload, fcitxPath(),
                                    fcitxRemotePath(), currentEnvironment()};
    g_task_set_task_data(task, request, [](gpointer value) {
        delete static_cast<RuntimeTask *>(value);
    });
    g_task_run_in_thread(task, runtimeTaskFunction);
    g_object_unref(task);
}

void onReload(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    gtk_widget_set_sensitive(impl->reloadButton, FALSE);
    gtk_widget_set_sensitive(impl->refreshStatusButton, FALSE);
    setStatus(impl, "正在请求重载 ModernIME…");
    startRuntimeTask(impl, true);
}

void onRefreshStatus(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    gtk_widget_set_sensitive(impl->refreshStatusButton, FALSE);
    gtk_widget_set_sensitive(impl->reloadButton, FALSE);
    setRuntimeLabel(impl->runtimeStatus, "正在检测 Fcitx5 状态…");
    startRuntimeTask(impl, false);
}

GtkWidget *makeStatusPage(SettingsWindow::Impl *impl) {
    auto *page = createPageShell(
        "输入法状态", "检查 Fcitx5、ModernIME 插件和当前激活状态");
    auto *section = createSectionCard(
        "运行状态", "如果状态异常，可以在这里重新加载 ModernIME。");
    impl->runtimeStatus = gtk_label_new("正在读取 Fcitx5 状态…");
    addStyleClass(impl->runtimeStatus, "modernime-status");
    gtk_widget_set_halign(impl->runtimeStatus, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(impl->runtimeStatus), TRUE);
    gtk_box_pack_start(GTK_BOX(section), impl->runtimeStatus, FALSE, FALSE, 0);

    auto *statusGrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(statusGrid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(statusGrid), 16);
    const auto statusLabels = settingsRuntimeStatusLabels();
    const auto addStatusRow = [statusGrid](int row, std::string_view title,
                                           GtkWidget **value,
                                           const char *initial) {
        auto *label = gtk_label_new(title.data());
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(statusGrid), label, 0, row, 1, 1);
        *value = gtk_label_new(initial);
        addStyleClass(*value, "modernime-status");
        gtk_widget_set_halign(*value, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(statusGrid), *value, 1, row, 1, 1);
    };
    addStatusRow(0, statusLabels[0], &impl->runtimeAvailability, "检测中…");
    addStatusRow(1, statusLabels[1], &impl->runtimeService, "检测中…");
    addStatusRow(2, statusLabels[2], &impl->runtimeInputMethod, "检测中…");
    addStatusRow(3, statusLabels[3], &impl->runtimeModernime, "检测中…");
    gtk_box_pack_start(GTK_BOX(section), statusGrid, FALSE, FALSE, 0);

    auto *statusActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    impl->refreshStatusButton = gtk_button_new_with_label("刷新状态");
    gtk_widget_set_tooltip_text(impl->refreshStatusButton,
                                "重新查询 Fcitx5 和 ModernIME 状态");
    impl->reloadButton = gtk_button_new_with_label("重新加载 ModernIME");
    gtk_widget_set_tooltip_text(impl->reloadButton,
                                "保存配置后重新加载 ModernIME");
    gtk_box_pack_start(GTK_BOX(statusActions), impl->refreshStatusButton,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(statusActions), impl->reloadButton, FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(section), statusActions, FALSE, FALSE, 0);
    g_signal_connect(impl->refreshStatusButton, "clicked",
                     G_CALLBACK(onRefreshStatus), impl);
    g_signal_connect(impl->reloadButton, "clicked", G_CALLBACK(onReload),
                     impl);
    gtk_widget_set_sensitive(impl->refreshStatusButton, FALSE);
    gtk_widget_set_sensitive(impl->reloadButton, FALSE);
    startRuntimeTask(impl, false);
    gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
    return page;
}

} // namespace

SettingsWindow::SettingsWindow(void *application, core::SettingsPaths paths)
    : impl_(std::make_unique<Impl>(application, std::move(paths))) {
    impl_->window = gtk_application_window_new(
        GTK_APPLICATION(impl_->application_));
    gtk_window_set_title(GTK_WINDOW(impl_->window), "ModernIME 设置");
    gtk_window_set_default_size(GTK_WINDOW(impl_->window), 860, 620);
    addStyleClass(impl_->window, kSettingsWindowClass);
    installSettingsStyles();
    g_signal_connect(impl_->window, "delete-event", G_CALLBACK(onWindowDelete),
                     impl_.get());

    auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    impl_->stack = gtk_stack_new();
    addStyleClass(impl_->stack, "modernime-page-stack");
    gtk_stack_set_transition_type(GTK_STACK(impl_->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    g_signal_connect(impl_->stack, "notify::visible-child-name",
                     G_CALLBACK(onStackVisibleChildChanged), impl_.get());
    auto *sidebar = gtk_stack_sidebar_new();
    addStyleClass(sidebar, kSettingsSidebarClass);
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar),
                                GTK_STACK(impl_->stack));
    gtk_widget_set_size_request(sidebar, 200, -1);
    gtk_box_pack_start(GTK_BOX(body), sidebar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), impl_->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    const auto pages = legacySettingsPageDefinitions();
    impl_->inputPage = std::make_unique<InputPage>(
        impl_->model, [this] { updateActionState(impl_.get()); });
    impl_->clipboardPage = std::make_unique<ClipboardPage>(
        impl_->model, impl_->paths_.clipboardHistory,
        [this] { updateActionState(impl_.get()); },
        [this](std::string message) {
            setStatus(impl_.get(), message.c_str());
        });
    impl_->learningPage = std::make_unique<LearningPage>(
        impl_->model, impl_->paths_.learningStore,
        [this] { updateActionState(impl_.get()); },
        [this](std::string message) {
            setStatus(impl_.get(), message.c_str());
        });
    impl_->dictionaryPage = std::make_unique<DictionaryPage>(
        impl_->paths_.userDictionary, [this](std::string message) {
            setStatus(impl_.get(), message.c_str());
        });
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         createScrollablePage(impl_->inputPage->widget()),
                         "input", "输入体验");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         createScrollablePage(impl_->clipboardPage->widget()),
                         pages[2].name.data(), pages[2].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         createScrollablePage(impl_->learningPage->widget()),
                         pages[3].name.data(), pages[3].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         createScrollablePage(impl_->dictionaryPage->widget()),
                         pages[4].name.data(), pages[4].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         createScrollablePage(makeStatusPage(impl_.get())),
                         pages[5].name.data(), pages[5].title.data());

    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(actions, 12);
    gtk_widget_set_margin_end(actions, 12);
    gtk_widget_set_margin_top(actions, 8);
    gtk_widget_set_margin_bottom(actions, 8);
    const auto actionLabels = settingsActionLabels();
    auto *reset = gtk_button_new_with_label(actionLabels[2].data());
    auto *defaults = gtk_button_new_with_label(actionLabels[3].data());
    auto *save = gtk_button_new_with_label(actionLabels[1].data());
    auto *apply = gtk_button_new_with_label(actionLabels[0].data());
    impl_->saveButton = save;
    impl_->applyButton = apply;
    impl_->resetButton = reset;
    impl_->defaultsButton = defaults;
    gtk_box_pack_end(GTK_BOX(actions), apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), save, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), defaults, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), reset, FALSE, FALSE, 0);
    impl_->editState = gtk_label_new("所有设置已保存");
    addStyleClass(impl_->editState, "modernime-status");
    gtk_widget_set_halign(impl_->editState, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(actions), impl_->editState, FALSE, FALSE, 0);
    impl_->status = gtk_label_new("");
    addStyleClass(impl_->status, "modernime-status");
    gtk_widget_set_halign(impl_->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(actions), impl_->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    g_signal_connect(save, "clicked", G_CALLBACK(onSave), impl_.get());
    g_signal_connect(apply, "clicked", G_CALLBACK(onApply), impl_.get());
    g_signal_connect(reset, "clicked", G_CALLBACK(onResetEdits), impl_.get());
    g_signal_connect(defaults, "clicked", G_CALLBACK(onResetDefaults),
                     impl_.get());
    gtk_container_add(GTK_CONTAINER(impl_->window), root);
    updateActionState(impl_.get());
    if (!impl_->model.loadDiagnostics().empty()) {
        std::ostringstream warning;
        warning << "配置读取警告：";
        for (std::size_t index = 0;
             index < impl_->model.loadDiagnostics().size(); ++index) {
            if (index != 0) {
                warning << "；";
            }
            warning << impl_->model.loadDiagnostics()[index];
        }
        setStatus(impl_.get(), warning.str().c_str());
    }
}

SettingsWindow::~SettingsWindow() {
    if (impl_ != nullptr && impl_->window != nullptr) {
        gtk_widget_destroy(impl_->window);
    }
}

void SettingsWindow::present() {
    gtk_widget_show_all(impl_->window);
    gtk_window_present(GTK_WINDOW(impl_->window));
}

void SettingsWindow::showBasicPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "input");
}

void SettingsWindow::showCandidatePage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "input");
}

void SettingsWindow::showClipboardPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "clipboard");
    impl_->clipboardPage->refresh(false);
}

void SettingsWindow::showLearningPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "learning");
    impl_->learningPage->refresh(false);
}

void SettingsWindow::showDictionaryPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "dictionary");
    impl_->dictionaryPage->refresh();
}

void SettingsWindow::showStatusPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "status");
}

void SettingsWindow::presentError(std::string_view message) {
    setStatus(impl_.get(), std::string(message).c_str());
}

} // namespace modernime::settings
