#include "modernime/settings/settings_window.h"

#include <gtk/gtk.h>

#include <string>
#include <utility>

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

void updateBasicPageFromModel(SettingsWindow::Impl *impl) {
    const auto &settings = impl->model.settings();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->inputEnabled),
                                 settings.inputEnabled);
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(impl->defaultMode),
        settings.defaultMode == core::InputMode::English ? 1 : 0);
    gtk_entry_set_text(GTK_ENTRY(impl->toggleKey), settings.toggleKey.c_str());
}

void onBasicChanged(GtkWidget *, gpointer data) {
    updateModelFromBasicPage(static_cast<SettingsWindow::Impl *>(data));
}

void onSave(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromBasicPage(impl);
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
                         makeInfoPage("候选设置", "候选键盘操作将在此处配置。"),
                         "candidate", "候选设置");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeInfoPage("智能学习", "学习开关和学习数据管理将在此处配置。"),
                         "learning", "智能学习");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeInfoPage("用户词典", "专业词条管理将在此处配置。"),
                         "dictionary", "用户词典");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeInfoPage("输入法状态", "Fcitx5 运行状态将在此处显示。"),
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
