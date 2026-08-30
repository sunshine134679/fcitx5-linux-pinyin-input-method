#include "modernime/settings/settings_window.h"
#include "modernime/settings/pages/clipboard_page.h"
#include "modernime/settings/pages/diagnostics_page.h"
#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/pages/input_page.h"
#include "modernime/settings/pages/learning_page.h"
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
    std::unique_ptr<DiagnosticsPage> diagnosticsPage;
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
    impl_->diagnosticsPage = std::make_unique<DiagnosticsPage>(
        fcitxPath(), fcitxRemotePath(), currentEnvironment(),
        [this](std::string message) {
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
                         createScrollablePage(impl_->diagnosticsPage->widget()),
                         pages[5].name.data(), "系统与诊断");

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
