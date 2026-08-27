#include "modernime/settings/settings_window.h"
#include "modernime/settings/runtime_controller.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace modernime::settings {

class SettingsWindow::Impl final {
public:
    Impl(void *application, std::filesystem::path settingsPath)
        : application_(application), model(std::move(settingsPath)) {}

    void *application_;
    SettingsWindowModel model;
    GtkWidget *window = nullptr;
    GtkWidget *stack = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *inputEnabled = nullptr;
    GtkWidget *defaultMode = nullptr;
    GtkWidget *toggleKey = nullptr;
    GtkWidget *candidateNumber = nullptr;
    GtkWidget *candidateArrow = nullptr;
    GtkWidget *candidatePage = nullptr;
    GtkWidget *runtimeStatus = nullptr;
    GtkWidget *reloadButton = nullptr;
};

namespace {

void setStatus(SettingsWindow::Impl *impl, const char *message) {
    gtk_label_set_text(GTK_LABEL(impl->status), message);
}

void updateModelFromBasicPage(SettingsWindow::Impl *impl) {
    auto settings = impl->model.settings();
    settings.inputEnabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(impl->inputEnabled));
    const auto mode = gtk_combo_box_get_active(
        GTK_COMBO_BOX(impl->defaultMode));
    settings.defaultMode = mode == 1 ? core::InputMode::English
                                     : core::InputMode::Chinese;
    settings.toggleKey = gtk_entry_get_text(GTK_ENTRY(impl->toggleKey));
    impl->model.setSettings(std::move(settings));
}

void updateModelFromCandidatePage(SettingsWindow::Impl *impl) {
    impl->model.setCandidateOptions(
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->candidateNumber)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->candidateArrow)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->candidatePage)));
}

void updateBasicPageFromModel(SettingsWindow::Impl *impl) {
    const auto &settings = impl->model.settings();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->inputEnabled),
                                 settings.inputEnabled);
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(impl->defaultMode),
        settings.defaultMode == core::InputMode::English ? 1 : 0);
    gtk_entry_set_text(GTK_ENTRY(impl->toggleKey), settings.toggleKey.c_str());
}

void updateCandidatePageFromModel(SettingsWindow::Impl *impl) {
    const auto &settings = impl->model.settings();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->candidateNumber),
                                 settings.numberSelection);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->candidateArrow),
                                 settings.arrowNavigation);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->candidatePage),
                                 settings.pageNavigation);
}

void onBasicChanged(GtkWidget *, gpointer data) {
    updateModelFromBasicPage(static_cast<SettingsWindow::Impl *>(data));
}

void onCandidateChanged(GtkWidget *, gpointer data) {
    updateModelFromCandidatePage(static_cast<SettingsWindow::Impl *>(data));
}

void onSave(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromBasicPage(impl);
    updateModelFromCandidatePage(impl);
    std::string error;
    if (impl->model.save(&error)) {
        setStatus(impl, "设置已保存；重新加载输入法后生效");
    } else {
        setStatus(impl, error.c_str());
    }
}

void onResetEdits(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    impl->model.resetEdits();
    updateBasicPageFromModel(impl);
    updateCandidatePageFromModel(impl);
    setStatus(impl, "已恢复未保存的修改");
}

GtkWidget *makeBasicPage(SettingsWindow::Impl *impl) {
    auto *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 24);
    gtk_widget_set_margin_end(grid, 24);
    gtk_widget_set_margin_top(grid, 24);
    gtk_widget_set_margin_bottom(grid, 24);

    auto *title = gtk_label_new("基本设置");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "title-3");
    gtk_grid_attach(GTK_GRID(grid), title, 0, 0, 2, 1);

    impl->inputEnabled = gtk_check_button_new_with_label("启用 ModernIME");
    gtk_grid_attach(GTK_GRID(grid), impl->inputEnabled, 0, 1, 2, 1);

    auto *modeLabel = gtk_label_new("默认输入状态");
    gtk_widget_set_halign(modeLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), modeLabel, 0, 2, 1, 1);
    impl->defaultMode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(impl->defaultMode),
                                   "中文");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(impl->defaultMode),
                                   "英文");
    gtk_grid_attach(GTK_GRID(grid), impl->defaultMode, 1, 2, 1, 1);

    auto *toggleLabel = gtk_label_new("中英文切换快捷键");
    gtk_widget_set_halign(toggleLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), toggleLabel, 0, 3, 1, 1);
    impl->toggleKey = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(impl->toggleKey),
                                   "例如 Ctrl+Space");
    gtk_grid_attach(GTK_GRID(grid), impl->toggleKey, 1, 3, 1, 1);

    updateBasicPageFromModel(impl);
    g_signal_connect(impl->inputEnabled, "toggled", G_CALLBACK(onBasicChanged),
                     impl);
    g_signal_connect(impl->defaultMode, "changed", G_CALLBACK(onBasicChanged),
                     impl);
    g_signal_connect(impl->toggleKey, "changed", G_CALLBACK(onBasicChanged),
                     impl);
    return grid;
}

GtkWidget *makeCandidatePage(SettingsWindow::Impl *impl) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    auto *title = gtk_label_new("候选设置");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "title-3");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    impl->candidateNumber = gtk_check_button_new_with_label("数字键选择候选");
    impl->candidateArrow = gtk_check_button_new_with_label(
        "左右方向键切换候选");
    impl->candidatePage = gtk_check_button_new_with_label(
        "上下方向键和 + / = 翻页");
    gtk_box_pack_start(GTK_BOX(box), impl->candidateNumber, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), impl->candidateArrow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), impl->candidatePage, FALSE, FALSE, 0);
    updateCandidatePageFromModel(impl);
    g_signal_connect(impl->candidateNumber, "toggled",
                     G_CALLBACK(onCandidateChanged), impl);
    g_signal_connect(impl->candidateArrow, "toggled",
                     G_CALLBACK(onCandidateChanged), impl);
    g_signal_connect(impl->candidatePage, "toggled",
                     G_CALLBACK(onCandidateChanged), impl);
    return box;
}

GtkWidget *makeInfoPage(const char *title, const char *message) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    auto *heading = gtk_label_new(title);
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),
                                "title-3");
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);
    auto *body = gtk_label_new(message);
    gtk_widget_set_halign(body, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
    gtk_box_pack_start(GTK_BOX(box), body, FALSE, FALSE, 0);
    return box;
}

Environment currentEnvironment() {
    Environment environment;
    for (const char *name : {"DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                             "XDG_RUNTIME_DIR"}) {
        const auto *value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            environment.emplace_back(name, value);
        }
    }
    return environment;
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
            new RuntimeResult(RuntimeController::reload(request->executable,
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
            setStatus(impl, reload->success ? "ModernIME 重载请求已发送"
                                             : reload->message.c_str());
            gtk_widget_set_sensitive(impl->reloadButton, TRUE);
            if (reload->success) {
                startRuntimeTask(impl, false);
            }
        }
        delete reload;
        g_clear_error(&error);
    } else {
        auto *status = static_cast<RuntimeStatus *>(
            g_task_propagate_pointer(task, &error));
        if (status != nullptr) {
            const auto message = status->message.empty()
                                     ? "无法读取 Fcitx5 状态"
                                     : status->message;
            gtk_label_set_text(GTK_LABEL(impl->runtimeStatus), message.c_str());
        }
        delete status;
        g_clear_error(&error);
    }
}

void startRuntimeTask(SettingsWindow::Impl *impl, bool reload) {
    auto *task = g_task_new(G_OBJECT(impl->window), nullptr,
                            runtimeTaskFinished, impl);
    auto *request = new RuntimeTask{impl, reload, fcitxRemotePath(),
                                    currentEnvironment()};
    g_task_set_task_data(task, request, [](gpointer value) {
        delete static_cast<RuntimeTask *>(value);
    });
    g_task_run_in_thread(task, runtimeTaskFunction);
    g_object_unref(task);
}

void onReload(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    gtk_widget_set_sensitive(impl->reloadButton, FALSE);
    setStatus(impl, "正在请求重载 ModernIME…");
    startRuntimeTask(impl, true);
}

GtkWidget *makeStatusPage(SettingsWindow::Impl *impl) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    auto *title = gtk_label_new("输入法状态");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(title),
                                "title-3");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    impl->runtimeStatus = gtk_label_new("正在读取 Fcitx5 状态…");
    gtk_widget_set_halign(impl->runtimeStatus, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(impl->runtimeStatus), TRUE);
    gtk_box_pack_start(GTK_BOX(box), impl->runtimeStatus, FALSE, FALSE, 0);
    impl->reloadButton = gtk_button_new_with_label("重新加载 ModernIME");
    gtk_widget_set_halign(impl->reloadButton, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), impl->reloadButton, FALSE, FALSE, 0);
    g_signal_connect(impl->reloadButton, "clicked", G_CALLBACK(onReload),
                     impl);
    startRuntimeTask(impl, false);
    return box;
}

} // namespace

SettingsWindow::SettingsWindow(void *application,
                               std::filesystem::path settingsPath)
    : impl_(std::make_unique<Impl>(application, std::move(settingsPath))) {
    impl_->window = gtk_application_window_new(
        GTK_APPLICATION(impl_->application_));
    gtk_window_set_title(GTK_WINDOW(impl_->window), "ModernIME 设置");
    gtk_window_set_default_size(GTK_WINDOW(impl_->window), 720, 520);

    auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    impl_->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(impl_->stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    auto *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar),
                                GTK_STACK(impl_->stack));
    gtk_widget_set_size_request(sidebar, 170, -1);
    gtk_box_pack_start(GTK_BOX(body), sidebar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), impl_->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeBasicPage(impl_.get()), "basic", "基本设置");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeCandidatePage(impl_.get()),
                         "candidate", "候选设置");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeInfoPage("智能学习", "学习开关和学习数据管理将在此处配置。"),
                         "learning", "智能学习");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeInfoPage("用户词典", "专业词条管理将在此处配置。"),
                         "dictionary", "用户词典");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeStatusPage(impl_.get()),
                         "status", "输入法状态");

    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(actions, 12);
    gtk_widget_set_margin_end(actions, 12);
    gtk_widget_set_margin_top(actions, 8);
    gtk_widget_set_margin_bottom(actions, 8);
    auto *reset = gtk_button_new_with_label("恢复修改");
    auto *save = gtk_button_new_with_label("保存");
    auto *apply = gtk_button_new_with_label("应用");
    gtk_box_pack_end(GTK_BOX(actions), apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), save, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(actions), reset, FALSE, FALSE, 0);
    impl_->status = gtk_label_new("");
    gtk_widget_set_halign(impl_->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(actions), impl_->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 0);
    g_signal_connect(save, "clicked", G_CALLBACK(onSave), impl_.get());
    g_signal_connect(apply, "clicked", G_CALLBACK(onSave), impl_.get());
    g_signal_connect(reset, "clicked", G_CALLBACK(onResetEdits), impl_.get());
    gtk_container_add(GTK_CONTAINER(impl_->window), root);
}

SettingsWindow::~SettingsWindow() {
    if (impl_ != nullptr && impl_->window != nullptr) {
        gtk_widget_destroy(impl_->window);
    }
}

void SettingsWindow::present() {
    gtk_window_present(GTK_WINDOW(impl_->window));
}

void SettingsWindow::showBasicPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "basic");
}

void SettingsWindow::showCandidatePage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "candidate");
}

void SettingsWindow::showLearningPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "learning");
}

void SettingsWindow::showDictionaryPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "dictionary");
}

void SettingsWindow::showStatusPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "status");
}

void SettingsWindow::presentError(std::string_view message) {
    setStatus(impl_.get(), std::string(message).c_str());
}

} // namespace modernime::settings
