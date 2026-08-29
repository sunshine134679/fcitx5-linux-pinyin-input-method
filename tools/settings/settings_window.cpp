#include "modernime/settings/settings_window.h"
#include "modernime/settings/clipboard_history_model.h"
#include "modernime/settings/data_controller.h"
#include "modernime/settings/runtime_controller.h"
#include "modernime/settings/settings_ui_contract.h"

#include "modernime/core/clipboard_history.h"
#include "modernime/pinyin/user_dictionary.h"

#include <gtk/gtk.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modernime::settings {

class SettingsWindow::Impl final {
public:
    Impl(void *application, core::SettingsPaths paths)
        : application_(application), paths_(std::move(paths)),
          model(paths_.settingsFile),
          clipboardHistory(paths_.clipboardHistory) {}

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
    GtkWidget *inputEnabled = nullptr;
    GtkWidget *defaultMode = nullptr;
    GtkWidget *toggleKey = nullptr;
    GtkWidget *punctuationFullWidth = nullptr;
    GtkWidget *candidateNumber = nullptr;
    GtkWidget *candidateArrow = nullptr;
    GtkWidget *candidatePage = nullptr;
    GtkWidget *clipboardEnabled = nullptr;
    GtkWidget *clipboardTrigger = nullptr;
    GtkListStore *clipboardHistoryStore = nullptr;
    GtkWidget *clipboardHistoryCount = nullptr;
    GtkWidget *clipboardHistoryState = nullptr;
    GtkWidget *clipboardHistoryView = nullptr;
    GtkWidget *clipboardCopyButton = nullptr;
    GtkWidget *clipboardDeleteButton = nullptr;
    GtkWidget *clipboardClearButton = nullptr;
    ClipboardHistoryModel clipboardHistory;
    GtkWidget *learningEnabled = nullptr;
    GtkWidget *contextLearning = nullptr;
    GtkWidget *learningPath = nullptr;
    GtkWidget *learningCount = nullptr;
    GtkWidget *clearLearningButton = nullptr;
    GtkListStore *dictionaryStore = nullptr;
    GtkWidget *dictionaryView = nullptr;
    GtkWidget *dictionarySearch = nullptr;
    GtkWidget *dictionaryCount = nullptr;
    GtkWidget *dictionaryState = nullptr;
    GtkWidget *dictionaryEditButton = nullptr;
    GtkWidget *dictionaryDeleteButton = nullptr;
    std::vector<pinyin::UserDictionaryEntry> dictionaryEntries;
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

void addStyleClass(GtkWidget *widget, const char *className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                className);
}

void installStyles() {
    auto *screen = gdk_screen_get_default();
    if (screen == nullptr) {
        return;
    }
    auto *provider = gtk_css_provider_new();
    constexpr const char *css = R"css(
        .modernime-sidebar {
            border-right: 1px solid @borders;
            background-color: @theme_bg_color;
            padding: 10px 6px;
        }
        .modernime-page {
            background-color: @theme_bg_color;
        }
        .modernime-page-scroller,
        .modernime-page-viewport,
        .modernime-page-scroller > viewport,
        .modernime-page-stack {
            background-color: @theme_bg_color;
            border: none;
        }
        .modernime-page-surface {
            background-color: @theme_bg_color;
            border: none;
        }
        .modernime-page-title {
            font-size: 20px;
            font-weight: 600;
        }
        .modernime-page-subtitle,
        .modernime-description,
        .modernime-path {
            color: @insensitive_fg_color;
        }
        .modernime-section {
            border: 1px solid @borders;
            border-radius: 10px;
            background-color: @theme_base_color;
            padding: 14px;
        }
        .modernime-section-title {
            font-weight: 600;
        }
        .modernime-status {
            padding: 4px 8px;
        }
        .modernime-status-dirty {
            color: #b35a00;
            font-weight: 600;
        }
        .modernime-status-error,
        entry.error {
            color: #b3261e;
        }
        entry.error {
            border-color: #b3261e;
        }
    )css";
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        screen, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

GtkWidget *makePageShell(const char *title, const char *subtitle) {
    auto *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    addStyleClass(page, "modernime-page");
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);
    gtk_widget_set_margin_top(page, 28);
    gtk_widget_set_margin_bottom(page, 28);

    auto *heading = gtk_label_new(title);
    addStyleClass(heading, "modernime-page-title");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    auto *description = gtk_label_new(subtitle);
    addStyleClass(description, "modernime-page-subtitle");
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);
    return page;
}

GtkWidget *makeSectionCard(const char *title, const char *description) {
    auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    addStyleClass(card, "modernime-section");
    if (title != nullptr && *title != '\0') {
        auto *heading = gtk_label_new(title);
        addStyleClass(heading, "modernime-section-title");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
    }
    if (description != nullptr && *description != '\0') {
        auto *help = gtk_label_new(description);
        addStyleClass(help, "modernime-description");
        gtk_widget_set_halign(help, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(help), TRUE);
        gtk_box_pack_start(GTK_BOX(card), help, FALSE, FALSE, 0);
    }
    return card;
}

GtkWidget *makeScrollablePage(GtkWidget *page) {
    auto *surface = gtk_event_box_new();
    addStyleClass(surface, "modernime-page-surface");
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(surface), TRUE);
    gtk_widget_set_hexpand(surface, TRUE);
    gtk_widget_set_vexpand(surface, TRUE);

    auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    addStyleClass(scrolled, "modernime-page-scroller");
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled),
                                        GTK_SHADOW_NONE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_hexpand(page, TRUE);
    gtk_widget_set_vexpand(page, TRUE);
    gtk_widget_set_halign(page, GTK_ALIGN_FILL);
    gtk_widget_set_valign(page, GTK_ALIGN_FILL);
    gtk_container_add(GTK_CONTAINER(surface), page);
    gtk_container_add(GTK_CONTAINER(scrolled), surface);
    auto *viewport = gtk_bin_get_child(GTK_BIN(scrolled));
    if (viewport != nullptr) {
        addStyleClass(viewport, "modernime-page-viewport");
    }
    return scrolled;
}

void setWidgetError(GtkWidget *widget, bool invalid, const char *message) {
    auto *context = gtk_widget_get_style_context(widget);
    if (invalid) {
        gtk_style_context_add_class(context, "error");
        gtk_widget_set_tooltip_text(widget, message);
    } else {
        gtk_style_context_remove_class(context, "error");
        gtk_widget_set_tooltip_text(widget, nullptr);
    }
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
    settings.punctuationEnabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(impl->punctuationFullWidth));
    impl->model.setSettings(std::move(settings));
}

void updateModelFromCandidatePage(SettingsWindow::Impl *impl) {
    impl->model.setCandidateOptions(
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->candidateNumber)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->candidateArrow)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(impl->candidatePage)));
}

void updateModelFromLearningPage(SettingsWindow::Impl *impl) {
    auto settings = impl->model.settings();
    settings.learningEnabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(impl->learningEnabled));
    settings.contextLearningEnabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(impl->contextLearning));
    impl->model.setSettings(std::move(settings));
}

void updateModelFromClipboardPage(SettingsWindow::Impl *impl) {
    impl->model.setClipboardOptions(
        gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->clipboardEnabled)),
        gtk_entry_get_text(GTK_ENTRY(impl->clipboardTrigger)));
}

void updateBasicPageFromModel(SettingsWindow::Impl *impl) {
    const auto &settings = impl->model.settings();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->inputEnabled),
                                 settings.inputEnabled);
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(impl->defaultMode),
        settings.defaultMode == core::InputMode::English ? 1 : 0);
    gtk_entry_set_text(GTK_ENTRY(impl->toggleKey), settings.toggleKey.c_str());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->punctuationFullWidth),
                                 settings.punctuationEnabled);
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

void updateLearningPageFromModel(SettingsWindow::Impl *impl) {
    const auto &settings = impl->model.settings();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->learningEnabled),
                                 settings.learningEnabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->contextLearning),
                                 settings.contextLearningEnabled);
}

void updateClipboardPageFromModel(SettingsWindow::Impl *impl) {
    const auto &settings = impl->model.settings();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(impl->clipboardEnabled),
                                 settings.clipboardEnabled);
    gtk_entry_set_text(GTK_ENTRY(impl->clipboardTrigger),
                       settings.clipboardTrigger.c_str());
}

void updateDependentSensitivity(SettingsWindow::Impl *impl) {
    if (impl->inputEnabled != nullptr) {
        const auto enabled = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->inputEnabled));
        if (impl->defaultMode != nullptr) {
            gtk_widget_set_sensitive(impl->defaultMode, enabled);
        }
        if (impl->toggleKey != nullptr) {
            gtk_widget_set_sensitive(impl->toggleKey, enabled);
        }
        if (impl->punctuationFullWidth != nullptr) {
            gtk_widget_set_sensitive(impl->punctuationFullWidth, enabled);
        }
    }
    if (impl->clipboardEnabled == nullptr ||
        impl->clipboardTrigger == nullptr) {
        return;
    }
    gtk_widget_set_sensitive(
        impl->clipboardTrigger,
        gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->clipboardEnabled)));
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

    const auto issueFor = [&validation](std::string_view key) {
        for (const auto &issue : validation.issues) {
            if (issue.key == key) {
                return issue.message.c_str();
            }
        }
        return static_cast<const char *>(nullptr);
    };
    if (impl->toggleKey != nullptr) {
        const auto *message = issueFor("input.toggle_key");
        setWidgetError(impl->toggleKey, message != nullptr,
                       message == nullptr ? "" : message);
    }
    if (impl->clipboardTrigger != nullptr) {
        const auto *message = issueFor("clipboard.trigger");
        setWidgetError(impl->clipboardTrigger, message != nullptr,
                       message == nullptr ? "" : message);
    }
}

void onBasicChanged(GtkWidget *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromBasicPage(impl);
    updateDependentSensitivity(impl);
    updateActionState(impl);
}

void onCandidateChanged(GtkWidget *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromCandidatePage(impl);
    updateActionState(impl);
}

void onLearningChanged(GtkWidget *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromLearningPage(impl);
    updateActionState(impl);
}

void onClipboardChanged(GtkWidget *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromClipboardPage(impl);
    updateDependentSensitivity(impl);
    updateActionState(impl);
}

std::optional<std::size_t> selectedClipboardIndex(
    SettingsWindow::Impl *impl) {
    if (impl->clipboardHistoryView == nullptr) {
        return std::nullopt;
    }
    auto *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(impl->clipboardHistoryView));
    GtkTreeModel *model = nullptr;
    GtkTreeIter iterator;
    if (!gtk_tree_selection_get_selected(selection, &model, &iterator)) {
        return std::nullopt;
    }
    auto *path = gtk_tree_model_get_path(model, &iterator);
    if (path == nullptr) {
        return std::nullopt;
    }
    const auto *indices = gtk_tree_path_get_indices(path);
    const auto index = indices == nullptr ? -1 : indices[0];
    gtk_tree_path_free(path);
    if (index < 0 || static_cast<std::size_t>(index) >=
                         impl->clipboardHistory.entries().size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(index);
}

void updateClipboardHistoryActionState(SettingsWindow::Impl *impl) {
    const auto selected = selectedClipboardIndex(impl);
    const bool hasSelected = selected.has_value();
    const bool hasEntries = !impl->clipboardHistory.entries().empty();
    if (impl->clipboardCopyButton != nullptr) {
        gtk_widget_set_sensitive(impl->clipboardCopyButton, hasSelected);
    }
    if (impl->clipboardDeleteButton != nullptr) {
        gtk_widget_set_sensitive(impl->clipboardDeleteButton, hasSelected);
    }
    if (impl->clipboardClearButton != nullptr) {
        gtk_widget_set_sensitive(impl->clipboardClearButton, hasEntries);
    }
}

void refreshClipboardHistory(SettingsWindow::Impl *impl, bool notify) {
    if (impl->clipboardHistoryStore == nullptr) {
        return;
    }

    gtk_list_store_clear(impl->clipboardHistoryStore);
    std::string error;
    if (!impl->clipboardHistory.reload(&error)) {
        gtk_label_set_text(GTK_LABEL(impl->clipboardHistoryCount),
                           "历史读取失败");
        if (impl->clipboardHistoryState != nullptr) {
            gtk_label_set_text(GTK_LABEL(impl->clipboardHistoryState),
                               error.empty() ? "无法读取剪贴板历史"
                                             : ("无法读取剪贴板历史：" + error)
                                                   .c_str());
            gtk_widget_set_visible(impl->clipboardHistoryState, TRUE);
        }
        updateClipboardHistoryActionState(impl);
        if (notify) {
            setStatus(impl, error.c_str());
        }
        return;
    }

    const auto &entries = impl->clipboardHistory.entries();
    if (impl->clipboardHistoryState != nullptr) {
        gtk_label_set_text(
            GTK_LABEL(impl->clipboardHistoryState),
            entries.empty() ? "暂无剪贴板历史；复制内容后会自动记录" : "");
        gtk_widget_set_visible(impl->clipboardHistoryState, entries.empty());
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        GtkTreeIter iter;
        gtk_list_store_append(impl->clipboardHistoryStore, &iter);
        gtk_list_store_set(impl->clipboardHistoryStore, &iter, 0,
                           static_cast<guint>(index + 1), 1,
                           entries[index].c_str(), -1);
    }
    const auto count = std::to_string(entries.size());
    gtk_label_set_text(
        GTK_LABEL(impl->clipboardHistoryCount),
        ("当前 " + count + " 条，最多保存 " +
         std::to_string(core::ClipboardHistory::kMaxEntries) + " 条")
            .c_str());
    updateClipboardHistoryActionState(impl);
    if (notify) {
        setStatus(impl, "剪贴板历史已刷新");
    }
}

void onClipboardSelectionChanged(GtkTreeSelection *, gpointer data) {
    updateClipboardHistoryActionState(
        static_cast<SettingsWindow::Impl *>(data));
}

void onClipboardRefresh(GtkButton *, gpointer data) {
    refreshClipboardHistory(static_cast<SettingsWindow::Impl *>(data), true);
}

void onClipboardCopy(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    const auto selected = selectedClipboardIndex(impl);
    if (!selected.has_value()) {
        setStatus(impl, "请先选择要复制的剪贴板历史");
        return;
    }
    const auto &entry = impl->clipboardHistory.entries()[*selected];
    auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (clipboard == nullptr) {
        setStatus(impl, "系统剪贴板不可用");
        return;
    }
    gtk_clipboard_set_text(clipboard, entry.c_str(), -1);
    setStatus(impl, "已复制选中的剪贴板历史");
}

void onClipboardDelete(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    const auto selected = selectedClipboardIndex(impl);
    if (!selected.has_value()) {
        setStatus(impl, "请先选择要删除的剪贴板历史");
        return;
    }
    const auto number = std::to_string(*selected + 1);
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "确定删除第 %s 条剪贴板历史吗？", number.c_str());
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_YES) {
        return;
    }

    std::string error;
    if (impl->clipboardHistory.remove(*selected, &error)) {
        refreshClipboardHistory(impl, false);
        setStatus(impl, "已删除选中的剪贴板历史");
    } else {
        setStatus(impl, error.empty() ? "剪贴板历史删除失败" : error.c_str());
    }
}

void onClipboardClear(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    if (impl->clipboardHistory.entries().empty()) {
        setStatus(impl, "当前没有可清空的剪贴板历史");
        return;
    }
    const auto count = std::to_string(impl->clipboardHistory.entries().size());
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "确定清空全部 %s 条剪贴板历史吗？此操作不可撤销。",
        count.c_str());
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_YES) {
        return;
    }

    std::string error;
    if (impl->clipboardHistory.clear(&error)) {
        refreshClipboardHistory(impl, false);
        setStatus(impl, "剪贴板历史已清空");
    } else {
        setStatus(impl, error.empty() ? "剪贴板历史清空失败" : error.c_str());
    }
}

void onStackVisibleChildChanged(GObject *object, GParamSpec *, gpointer data) {
    const auto *name = gtk_stack_get_visible_child_name(GTK_STACK(object));
    if (name != nullptr && std::string_view(name) == "clipboard") {
        refreshClipboardHistory(static_cast<SettingsWindow::Impl *>(data),
                                false);
    }
}

bool saveEditedSettings(SettingsWindow::Impl *impl, bool closeAfterSave) {
    updateModelFromBasicPage(impl);
    updateModelFromCandidatePage(impl);
    updateModelFromClipboardPage(impl);
    updateModelFromLearningPage(impl);
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
    updateBasicPageFromModel(impl);
    updateCandidatePageFromModel(impl);
    updateClipboardPageFromModel(impl);
    updateLearningPageFromModel(impl);
    updateDependentSensitivity(impl);
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
    updateBasicPageFromModel(impl);
    updateCandidatePageFromModel(impl);
    updateClipboardPageFromModel(impl);
    updateLearningPageFromModel(impl);
    updateDependentSensitivity(impl);
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

GtkWidget *makeBasicPage(SettingsWindow::Impl *impl) {
    auto *page = makePageShell(
        "基本设置", "配置 ModernIME 的启用状态、默认输入状态和切换快捷键");
    auto *section = makeSectionCard(
        "输入状态", "这些设置决定 ModernIME 何时接收键盘输入。");
    auto *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    impl->inputEnabled = gtk_check_button_new_with_label("启用 ModernIME");
    gtk_grid_attach(GTK_GRID(grid), impl->inputEnabled, 0, 0, 2, 1);

    auto *modeLabel = gtk_label_new("默认输入状态");
    gtk_widget_set_halign(modeLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), modeLabel, 0, 1, 1, 1);
    impl->defaultMode = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(impl->defaultMode),
                                   "中文");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(impl->defaultMode),
                                   "英文");
    gtk_widget_set_hexpand(impl->defaultMode, TRUE);
    gtk_grid_attach(GTK_GRID(grid), impl->defaultMode, 1, 1, 1, 1);

    auto *toggleLabel = gtk_label_new("中英文切换快捷键");
    gtk_widget_set_halign(toggleLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), toggleLabel, 0, 2, 1, 1);
    impl->toggleKey = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(impl->toggleKey),
                                   "例如 Ctrl+Space");
    gtk_widget_set_hexpand(impl->toggleKey, TRUE);
    gtk_grid_attach(GTK_GRID(grid), impl->toggleKey, 1, 2, 1, 1);

    impl->punctuationFullWidth =
        gtk_check_button_new_with_label("中文标点使用全角（，。？！等）");
    gtk_grid_attach(GTK_GRID(grid), impl->punctuationFullWidth, 0, 3, 2, 1);
    gtk_box_pack_start(GTK_BOX(section), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

    updateBasicPageFromModel(impl);
    g_signal_connect(impl->inputEnabled, "toggled", G_CALLBACK(onBasicChanged),
                     impl);
    g_signal_connect(impl->defaultMode, "changed", G_CALLBACK(onBasicChanged),
                     impl);
    g_signal_connect(impl->toggleKey, "changed", G_CALLBACK(onBasicChanged),
                     impl);
    g_signal_connect(impl->punctuationFullWidth, "toggled",
                     G_CALLBACK(onBasicChanged), impl);
    return page;
}

GtkWidget *makeCandidatePage(SettingsWindow::Impl *impl) {
    auto *page = makePageShell(
        "候选设置", "配置候选选择、方向键导航和翻页方式，不改变候选栏外观尺寸");
    auto *section = makeSectionCard(
        "候选操作", "关闭某项后，对应按键会交给其他输入行为处理。");

    impl->candidateNumber = gtk_check_button_new_with_label("数字键选择候选");
    impl->candidateArrow = gtk_check_button_new_with_label(
        "左右方向键切换候选");
    impl->candidatePage = gtk_check_button_new_with_label(
        "上下方向键和 + / = 翻页");
    gtk_widget_set_tooltip_text(impl->candidateNumber,
                                "使用数字键选择当前候选项");
    gtk_widget_set_tooltip_text(impl->candidateArrow,
                                "使用左右方向键切换候选项");
    gtk_widget_set_tooltip_text(impl->candidatePage,
                                "使用上下方向键或 + / = 翻页");
    gtk_box_pack_start(GTK_BOX(section), impl->candidateNumber, FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(section), impl->candidateArrow, FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(section), impl->candidatePage, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
    updateCandidatePageFromModel(impl);
    g_signal_connect(impl->candidateNumber, "toggled",
                     G_CALLBACK(onCandidateChanged), impl);
    g_signal_connect(impl->candidateArrow, "toggled",
                     G_CALLBACK(onCandidateChanged), impl);
    g_signal_connect(impl->candidatePage, "toggled",
                     G_CALLBACK(onCandidateChanged), impl);
    return page;
}

GtkWidget *makeClipboardPage(SettingsWindow::Impl *impl) {
    auto *page = makePageShell(
        "剪贴板", "配置 V+2 功能并查看保存在本地的剪贴板历史");
    auto *settingsSection = makeSectionCard(
        "触发方式", "仅在中文输入状态且当前没有正在输入拼音时触发。");

    impl->clipboardEnabled =
        gtk_check_button_new_with_label("启用 V+2 剪贴板");
    gtk_box_pack_start(GTK_BOX(settingsSection), impl->clipboardEnabled, FALSE,
                       FALSE, 0);

    auto *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    auto *triggerLabel = gtk_label_new("剪贴板触发键");
    gtk_widget_set_halign(triggerLabel, GTK_ALIGN_START);
    impl->clipboardTrigger = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(impl->clipboardTrigger), 3);
    gtk_entry_set_width_chars(GTK_ENTRY(impl->clipboardTrigger), 6);
    gtk_entry_set_placeholder_text(GTK_ENTRY(impl->clipboardTrigger),
                                   "例如 V+2");
    gtk_widget_set_hexpand(impl->clipboardTrigger, TRUE);
    gtk_grid_attach(GTK_GRID(grid), triggerLabel, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), impl->clipboardTrigger, 1, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(settingsSection), grid, FALSE, FALSE, 0);

    auto *description = gtk_label_new(
        "仅在中文输入状态且当前没有正在输入的拼音时触发。按第一个字母后，"
        "可继续选择对应功能或直接按回车输出字母。");
    addStyleClass(description, "modernime-description");
    gtk_widget_set_halign(description, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
    gtk_box_pack_start(GTK_BOX(settingsSection), description, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), settingsSection, FALSE, FALSE, 0);

    auto *historySection = makeSectionCard(
        "剪贴板历史", "按最近使用顺序显示，最多保留 30 条内容。");

    impl->clipboardHistoryCount = gtk_label_new("正在读取…");
    addStyleClass(impl->clipboardHistoryCount, "modernime-description");
    gtk_widget_set_halign(impl->clipboardHistoryCount, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(historySection), impl->clipboardHistoryCount,
                       FALSE, FALSE, 0);

    impl->clipboardHistoryStore =
        gtk_list_store_new(2, G_TYPE_UINT, G_TYPE_STRING);
    impl->clipboardHistoryView = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(impl->clipboardHistoryStore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(impl->clipboardHistoryView),
                                      TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(impl->clipboardHistoryView),
                                    TRUE);
    gtk_widget_set_tooltip_text(
        impl->clipboardHistoryView,
        "选择一条历史后可以复制、删除；也可以使用键盘上下键移动");
    auto *numberRenderer = gtk_cell_renderer_text_new();
    auto *numberColumn = gtk_tree_view_column_new_with_attributes(
        "序号", numberRenderer, "text", 0, nullptr);
    gtk_tree_view_column_set_resizable(numberColumn, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(impl->clipboardHistoryView),
                                numberColumn);
    auto *textRenderer = gtk_cell_renderer_text_new();
    auto *textColumn = gtk_tree_view_column_new_with_attributes(
        "内容", textRenderer, "text", 1, nullptr);
    gtk_tree_view_column_set_expand(textColumn, TRUE);
    gtk_tree_view_column_set_resizable(textColumn, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(impl->clipboardHistoryView),
                                textColumn);
    auto *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(impl->clipboardHistoryView));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed",
                     G_CALLBACK(onClipboardSelectionChanged), impl);
    auto *historyScrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(historyScrolled, TRUE);
    gtk_widget_set_hexpand(historyScrolled, TRUE);
    gtk_widget_set_size_request(historyScrolled, -1, 160);
    gtk_container_add(GTK_CONTAINER(historyScrolled),
                      impl->clipboardHistoryView);
    gtk_box_pack_start(GTK_BOX(historySection), historyScrolled, TRUE, TRUE, 0);

    impl->clipboardHistoryState = gtk_label_new("正在读取…");
    addStyleClass(impl->clipboardHistoryState, "modernime-description");
    gtk_widget_set_halign(impl->clipboardHistoryState, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(impl->clipboardHistoryState), TRUE);
    gtk_box_pack_start(GTK_BOX(historySection), impl->clipboardHistoryState,
                       FALSE, FALSE, 0);

    auto *historyActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const auto clipboardActionLabels = settingsClipboardActionLabels();
    impl->clipboardCopyButton = gtk_button_new_with_label(
        clipboardActionLabels[0].data());
    impl->clipboardDeleteButton = gtk_button_new_with_label(
        clipboardActionLabels[1].data());
    impl->clipboardClearButton = gtk_button_new_with_label(
        clipboardActionLabels[2].data());
    gtk_widget_set_tooltip_text(impl->clipboardCopyButton,
                                "复制选中的历史内容到系统剪贴板");
    gtk_widget_set_tooltip_text(impl->clipboardDeleteButton,
                                "删除选中的历史内容");
    gtk_widget_set_tooltip_text(impl->clipboardClearButton,
                                "清空全部历史内容，需要确认");
    for (auto *button : {impl->clipboardCopyButton,
                         impl->clipboardDeleteButton,
                         impl->clipboardClearButton}) {
        gtk_box_pack_start(GTK_BOX(historyActions), button, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(historySection), historyActions, FALSE, FALSE,
                       0);

    auto *refreshButton = gtk_button_new_with_label("刷新历史");
    gtk_widget_set_halign(refreshButton, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(refreshButton, "重新读取磁盘上的剪贴板历史");
    gtk_box_pack_start(GTK_BOX(historySection), refreshButton, FALSE, FALSE, 0);
    g_signal_connect(refreshButton, "clicked", G_CALLBACK(onClipboardRefresh),
                     impl);
    g_signal_connect(impl->clipboardCopyButton, "clicked",
                     G_CALLBACK(onClipboardCopy), impl);
    g_signal_connect(impl->clipboardDeleteButton, "clicked",
                     G_CALLBACK(onClipboardDelete), impl);
    g_signal_connect(impl->clipboardClearButton, "clicked",
                     G_CALLBACK(onClipboardClear), impl);
    gtk_box_pack_start(GTK_BOX(page), historySection, TRUE, TRUE, 0);

    updateClipboardPageFromModel(impl);
    updateDependentSensitivity(impl);
    g_signal_connect(impl->clipboardEnabled, "toggled",
                     G_CALLBACK(onClipboardChanged), impl);
    g_signal_connect(impl->clipboardTrigger, "changed",
                     G_CALLBACK(onClipboardChanged), impl);
    return page;
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

void refreshLearningData(SettingsWindow::Impl *impl, bool notify);

void onClearLearning(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "清空后将无法恢复当前学习排序，是否先备份并清空？");
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_YES) {
        return;
    }

    gtk_widget_set_sensitive(impl->clearLearningButton, FALSE);
    const auto backup = learningBackupPath(impl->paths_.learningStore);
    std::string error;
    if (DataController::backupAndClearLearning(impl->paths_.learningStore,
                                                backup, &error)) {
        refreshLearningData(impl, false);
        setStatus(impl, ("学习记录已清空，备份位于 " + backup.string()).c_str());
    } else {
        setStatus(impl, error.c_str());
    }
    gtk_widget_set_sensitive(impl->clearLearningButton, TRUE);
}

void refreshLearningData(SettingsWindow::Impl *impl, bool notify) {
    if (impl->learningCount == nullptr) {
        return;
    }
    std::string error;
    const auto count = DataController::learningEntryCount(
        impl->paths_.learningStore, &error);
    if (!error.empty()) {
        gtk_label_set_text(GTK_LABEL(impl->learningCount),
                           "当前学习记录：读取失败");
        gtk_widget_set_sensitive(impl->clearLearningButton, FALSE);
        if (notify) {
            setStatus(impl, error.c_str());
        }
        return;
    }
    gtk_label_set_text(
        GTK_LABEL(impl->learningCount),
        ("当前学习记录：" + std::to_string(count) + " 条").c_str());
    gtk_widget_set_sensitive(impl->clearLearningButton, count != 0);
    if (notify) {
        setStatus(impl, "学习记录统计已刷新");
    }
}

GtkWidget *makeLearningPage(SettingsWindow::Impl *impl) {
    auto *page = makePageShell(
        "智能学习", "让 ModernIME 记住你的候选选择，并结合上下文优化排序");
    auto *learningSection = makeSectionCard(
        "学习行为", "关闭后不会删除已经保存的学习数据。");

    impl->learningEnabled = gtk_check_button_new_with_label("记忆用户候选选择");
    impl->contextLearning = gtk_check_button_new_with_label(
        "根据光标前后文调整候选排序");
    gtk_widget_set_tooltip_text(impl->learningEnabled,
                                "记录你主动选择的候选词");
    gtk_widget_set_tooltip_text(impl->contextLearning,
                                "根据输入前后的文字调整候选顺序");
    gtk_box_pack_start(GTK_BOX(learningSection), impl->learningEnabled, FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(learningSection), impl->contextLearning, FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), learningSection, FALSE, FALSE, 0);
    updateLearningPageFromModel(impl);
    g_signal_connect(impl->learningEnabled, "toggled",
                     G_CALLBACK(onLearningChanged), impl);
    g_signal_connect(impl->contextLearning, "toggled",
                     G_CALLBACK(onLearningChanged), impl);

    const auto pathText = "学习数据库：" + impl->paths_.learningStore.string();
    impl->learningPath = gtk_label_new(pathText.c_str());
    addStyleClass(impl->learningPath, "modernime-path");
    gtk_widget_set_halign(impl->learningPath, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(impl->learningPath), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(impl->learningPath), TRUE);
    auto *dataSection = makeSectionCard(
        "学习数据", "学习记录保存在本地；清空前会自动创建可恢复的备份。");
    impl->learningCount = gtk_label_new("正在读取学习记录…");
    addStyleClass(impl->learningCount, "modernime-description");
    gtk_widget_set_halign(impl->learningCount, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(dataSection), impl->learningCount, FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(dataSection), impl->learningPath, FALSE, FALSE,
                       0);

    impl->clearLearningButton = gtk_button_new_with_label("清空学习记录");
    gtk_widget_set_halign(impl->clearLearningButton, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(impl->clearLearningButton,
                                "备份后清空 ModernIME 的学习排序");
    gtk_box_pack_start(GTK_BOX(dataSection), impl->clearLearningButton, FALSE,
                       FALSE, 0);
    g_signal_connect(impl->clearLearningButton, "clicked",
                     G_CALLBACK(onClearLearning), impl);
    gtk_box_pack_start(GTK_BOX(page), dataSection, FALSE, FALSE, 0);
    refreshLearningData(impl, false);
    return page;
}

std::string foldAscii(std::string_view value) {
    std::string result(value);
    for (auto &character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return result;
}

bool dictionaryMatches(const pinyin::UserDictionaryEntry &entry,
                       std::string_view query) {
    if (query.empty()) {
        return true;
    }
    const auto foldedQuery = foldAscii(query);
    const auto foldedPinyin = foldAscii(entry.pinyin);
    const auto foldedPhrase = foldAscii(entry.phrase);
    return foldedPinyin.find(foldedQuery) != std::string::npos ||
           foldedPhrase.find(foldedQuery) != std::string::npos;
}

void updateDictionaryActionState(SettingsWindow::Impl *impl);

void refreshDictionaryPage(SettingsWindow::Impl *impl) {
    impl->dictionaryEntries = DataController::loadDictionary(
        impl->paths_.userDictionary);
    gtk_list_store_clear(impl->dictionaryStore);
    const auto query = impl->dictionarySearch == nullptr
                           ? std::string()
                           : gtk_entry_get_text(GTK_ENTRY(impl->dictionarySearch));
    std::size_t visibleCount = 0;
    for (std::size_t index = 0; index < impl->dictionaryEntries.size(); ++index) {
        const auto &entry = impl->dictionaryEntries[index];
        if (!dictionaryMatches(entry, query)) {
            continue;
        }
        auto weight = std::ostringstream{};
        weight << std::setprecision(9) << entry.weight;
        GtkTreeIter iterator;
        gtk_list_store_append(impl->dictionaryStore, &iterator);
        gtk_list_store_set(impl->dictionaryStore, &iterator, 0,
                           entry.pinyin.c_str(), 1, entry.phrase.c_str(), 2,
                           weight.str().c_str(), 3,
                           static_cast<guint>(index), -1);
        ++visibleCount;
    }

    if (impl->dictionaryCount != nullptr) {
        const auto countText = query.empty()
                                   ? "共 " + std::to_string(visibleCount) + " 条"
                                   : "显示 " + std::to_string(visibleCount) +
                                         " 条，共 " +
                                         std::to_string(impl->dictionaryEntries.size()) +
                                         " 条";
        gtk_label_set_text(GTK_LABEL(impl->dictionaryCount), countText.c_str());
    }
    if (impl->dictionaryState != nullptr) {
        const char *state = nullptr;
        if (impl->dictionaryEntries.empty()) {
            state = "暂无用户词条；可以添加个人词条或导入专业词典";
        } else if (visibleCount == 0) {
            state = "没有匹配的词条；请更换拼音或词条关键词";
        }
        gtk_label_set_text(GTK_LABEL(impl->dictionaryState),
                           state == nullptr ? "" : state);
        gtk_widget_set_visible(impl->dictionaryState, state != nullptr);
    }
    updateDictionaryActionState(impl);
}

std::optional<std::size_t> selectedDictionaryIndex(
    SettingsWindow::Impl *impl) {
    auto *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(impl->dictionaryView));
    GtkTreeModel *model = nullptr;
    GtkTreeIter iterator;
    if (!gtk_tree_selection_get_selected(selection, &model, &iterator)) {
        return std::nullopt;
    }
    guint sourceIndex = 0;
    gtk_tree_model_get(model, &iterator, 3, &sourceIndex, -1);
    if (static_cast<std::size_t>(sourceIndex) >=
        impl->dictionaryEntries.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(sourceIndex);
}

void updateDictionaryActionState(SettingsWindow::Impl *impl) {
    const auto selected = selectedDictionaryIndex(impl);
    const bool hasSelected = selected.has_value();
    if (impl->dictionaryEditButton != nullptr) {
        gtk_widget_set_sensitive(impl->dictionaryEditButton, hasSelected);
    }
    if (impl->dictionaryDeleteButton != nullptr) {
        gtk_widget_set_sensitive(impl->dictionaryDeleteButton, hasSelected);
    }
}

void onDictionarySelectionChanged(GtkTreeSelection *, gpointer data) {
    updateDictionaryActionState(
        static_cast<SettingsWindow::Impl *>(data));
}

void onDictionarySearchChanged(GtkEditable *, gpointer data) {
    refreshDictionaryPage(static_cast<SettingsWindow::Impl *>(data));
}

bool saveDictionaryEntries(SettingsWindow::Impl *impl,
                           const std::vector<pinyin::UserDictionaryEntry> &entries) {
    std::string error;
    if (!DataController::saveDictionary(impl->paths_.userDictionary, entries,
                                        &error)) {
        setStatus(impl, error.c_str());
        return false;
    }
    refreshDictionaryPage(impl);
    setStatus(impl, "用户词典已保存；重新加载输入法后生效");
    return true;
}

struct DictionaryDialogState final {
    GtkWidget *pinyin = nullptr;
    GtkWidget *phrase = nullptr;
    GtkWidget *weight = nullptr;
    GtkWidget *validation = nullptr;
    GtkWidget *accept = nullptr;
};

void updateDictionaryDialogValidity(DictionaryDialogState *state) {
    pinyin::UserDictionary dictionary;
    const auto valid = dictionary.upsert(
        gtk_entry_get_text(GTK_ENTRY(state->pinyin)),
        gtk_entry_get_text(GTK_ENTRY(state->phrase)),
        static_cast<float>(gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(state->weight))));
    gtk_widget_set_sensitive(state->accept, valid);
    gtk_label_set_text(GTK_LABEL(state->validation),
                       valid ? "" : "拼音、词条或权重格式不合法");
    gtk_widget_set_visible(state->validation, !valid);
}

void onDictionaryDialogTextChanged(GtkEditable *, gpointer data) {
    updateDictionaryDialogValidity(
        static_cast<DictionaryDialogState *>(data));
}

void onDictionaryDialogWeightChanged(GtkSpinButton *, gpointer data) {
    updateDictionaryDialogValidity(
        static_cast<DictionaryDialogState *>(data));
}

bool editDictionaryEntry(SettingsWindow::Impl *impl,
                         std::optional<std::size_t> selected) {
    auto *dialog = gtk_dialog_new_with_buttons(
        selected.has_value() ? "编辑用户词条" : "添加用户词条",
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, "取消", GTK_RESPONSE_CANCEL,
        "保存", GTK_RESPONSE_ACCEPT, nullptr);
    auto *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    auto *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);
    gtk_container_add(GTK_CONTAINER(content), grid);

    auto *pinyinLabel = gtk_label_new("拼音");
    auto *phraseLabel = gtk_label_new("词条");
    auto *weightLabel = gtk_label_new("权重");
    gtk_widget_set_halign(pinyinLabel, GTK_ALIGN_START);
    gtk_widget_set_halign(phraseLabel, GTK_ALIGN_START);
    gtk_widget_set_halign(weightLabel, GTK_ALIGN_START);
    auto *pinyin = gtk_entry_new();
    auto *phrase = gtk_entry_new();
    auto *weight = gtk_spin_button_new_with_range(0.0, 1000000000.0, 1.0);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(weight), 3);
    gtk_grid_attach(GTK_GRID(grid), pinyinLabel, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pinyin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), phraseLabel, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), phrase, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), weightLabel, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), weight, 1, 2, 1, 1);
    auto *validation = gtk_label_new("");
    addStyleClass(validation, "modernime-status-error");
    gtk_widget_set_halign(validation, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), validation, 1, 3, 1, 1);
    if (selected.has_value()) {
        const auto &entry = impl->dictionaryEntries[*selected];
        gtk_entry_set_text(GTK_ENTRY(pinyin), entry.pinyin.c_str());
        gtk_entry_set_text(GTK_ENTRY(phrase), entry.phrase.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), entry.weight);
    }
    auto state = DictionaryDialogState{pinyin, phrase, weight, validation,
                                       gtk_dialog_get_widget_for_response(
                                           GTK_DIALOG(dialog),
                                           GTK_RESPONSE_ACCEPT)};
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_entry_set_activates_default(GTK_ENTRY(pinyin), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(phrase), TRUE);
    g_signal_connect(pinyin, "changed",
                     G_CALLBACK(onDictionaryDialogTextChanged), &state);
    g_signal_connect(phrase, "changed",
                     G_CALLBACK(onDictionaryDialogTextChanged), &state);
    g_signal_connect(weight, "value-changed",
                     G_CALLBACK(onDictionaryDialogWeightChanged), &state);
    updateDictionaryDialogValidity(&state);
    gtk_widget_show_all(dialog);
    gtk_widget_set_visible(validation, !gtk_widget_get_sensitive(state.accept));
    gtk_widget_grab_focus(pinyin);
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        return false;
    }

    const auto pinyinText = gtk_entry_get_text(GTK_ENTRY(pinyin));
    const auto phraseText = gtk_entry_get_text(GTK_ENTRY(phrase));
    const auto weightValue = static_cast<float>(
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(weight)));
    pinyin::UserDictionary dictionary;
    for (std::size_t index = 0; index < impl->dictionaryEntries.size();
         ++index) {
        if (!selected.has_value() || index != *selected) {
            const auto &entry = impl->dictionaryEntries[index];
            if (!dictionary.upsert(entry.pinyin, entry.phrase, entry.weight)) {
                gtk_widget_destroy(dialog);
                setStatus(impl, "现有用户词典包含非法词条");
                return false;
            }
        }
    }
    const bool valid = dictionary.upsert(pinyinText, phraseText, weightValue);
    gtk_widget_destroy(dialog);
    if (!valid) {
        setStatus(impl, "词条未保存：拼音、词条或权重格式不合法");
        return false;
    }
    return saveDictionaryEntries(impl, dictionary.entries());
}

void onDictionaryAdd(GtkButton *, gpointer data) {
    editDictionaryEntry(static_cast<SettingsWindow::Impl *>(data),
                         std::nullopt);
}

void onDictionaryEdit(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    const auto selected = selectedDictionaryIndex(impl);
    if (!selected.has_value()) {
        setStatus(impl, "请先选择要编辑的词条");
        return;
    }
    editDictionaryEntry(impl, selected);
}

void onDictionaryDelete(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    const auto selected = selectedDictionaryIndex(impl);
    if (!selected.has_value()) {
        setStatus(impl, "请先选择要删除的词条");
        return;
    }
    auto *dialog = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO, "确定删除选中的用户词条吗？");
    const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_YES) {
        return;
    }
    auto entries = impl->dictionaryEntries;
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*selected));
    saveDictionaryEntries(impl, entries);
}

void onDictionaryImport(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    auto *dialog = gtk_file_chooser_dialog_new(
        "导入用户词典", GTK_WINDOW(impl->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "取消", GTK_RESPONSE_CANCEL, "导入", GTK_RESPONSE_ACCEPT, nullptr);
    gchar *filename = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    }
    gtk_widget_destroy(dialog);
    if (filename == nullptr) {
        return;
    }
    std::vector<pinyin::UserDictionaryEntry> imported;
    std::string error;
    if (!DataController::importDictionary(filename, imported, &error)) {
        setStatus(impl, error.c_str());
        g_free(filename);
        return;
    }
    g_free(filename);
    auto *confirm = gtk_message_dialog_new(
        GTK_WINDOW(impl->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "导入将替换现有全部 %zu 条词条（导入文件含 %zu 条），确定继续吗？",
        impl->dictionaryEntries.size(), imported.size());
    const auto confirmed = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (confirmed == GTK_RESPONSE_YES) {
        saveDictionaryEntries(impl, imported);
    } else {
        setStatus(impl, "已取消导入，现有词典未修改");
    }
}

void onDictionaryExport(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    auto *dialog = gtk_file_chooser_dialog_new(
        "导出用户词典", GTK_WINDOW(impl->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        "取消", GTK_RESPONSE_CANCEL, "导出", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog),
                                                   TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const auto filename = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialog));
        std::string error;
        if (DataController::saveDictionary(filename, impl->dictionaryEntries,
                                            &error)) {
            setStatus(impl, "用户词典已导出");
        } else {
            setStatus(impl, error.c_str());
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

GtkWidget *makeDictionaryPage(SettingsWindow::Impl *impl) {
    auto *page = makePageShell(
        "用户词典", "维护个人词条和专业名词，重载 ModernIME 后生效");
    auto *section = makeSectionCard(
        "词条列表", "拼音、词条和权重会在保存时统一校验；导入前会确认整体替换，失败不会覆盖原词典。");

    auto *searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    auto *searchLabel = gtk_label_new("搜索");
    gtk_widget_set_halign(searchLabel, GTK_ALIGN_START);
    impl->dictionarySearch = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(impl->dictionarySearch),
                                   "输入拼音或词条关键词");
    gtk_widget_set_hexpand(impl->dictionarySearch, TRUE);
    gtk_widget_set_tooltip_text(impl->dictionarySearch,
                                "实时筛选拼音和词条内容");
    gtk_box_pack_start(GTK_BOX(searchRow), searchLabel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(searchRow), impl->dictionarySearch, TRUE, TRUE,
                       0);
    gtk_box_pack_start(GTK_BOX(section), searchRow, FALSE, FALSE, 0);

    impl->dictionaryCount = gtk_label_new("正在读取用户词典…");
    addStyleClass(impl->dictionaryCount, "modernime-description");
    gtk_widget_set_halign(impl->dictionaryCount, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(section), impl->dictionaryCount, FALSE, FALSE,
                       0);

    impl->dictionaryStore = gtk_list_store_new(
        4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
    impl->dictionaryView = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(impl->dictionaryStore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(impl->dictionaryView),
                                      TRUE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(impl->dictionaryView), FALSE);
    gtk_widget_set_tooltip_text(impl->dictionaryView,
                                "选择词条后可以编辑或删除");
    for (const auto &column : {std::pair<const char *, int>{"拼音", 0},
                               {"词条", 1}, {"权重", 2}}) {
        auto *renderer = gtk_cell_renderer_text_new();
        auto *viewColumn = gtk_tree_view_column_new_with_attributes(
            column.first, renderer, "text", column.second, nullptr);
        gtk_tree_view_append_column(GTK_TREE_VIEW(impl->dictionaryView),
                                    viewColumn);
    }
    auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_size_request(scrolled, -1, 220);
    gtk_container_add(GTK_CONTAINER(scrolled), impl->dictionaryView);
    gtk_box_pack_start(GTK_BOX(section), scrolled, TRUE, TRUE, 0);
    impl->dictionaryState = gtk_label_new("");
    addStyleClass(impl->dictionaryState, "modernime-description");
    gtk_widget_set_halign(impl->dictionaryState, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(impl->dictionaryState), TRUE);
    gtk_box_pack_start(GTK_BOX(section), impl->dictionaryState, FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(page), section, TRUE, TRUE, 0);

    auto *selection = gtk_tree_view_get_selection(
        GTK_TREE_VIEW(impl->dictionaryView));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed",
                     G_CALLBACK(onDictionarySelectionChanged), impl);

    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const auto addButton = gtk_button_new_with_label("添加");
    const auto editButton = gtk_button_new_with_label("编辑");
    const auto deleteButton = gtk_button_new_with_label("删除");
    const auto importButton = gtk_button_new_with_label("导入");
    const auto exportButton = gtk_button_new_with_label("导出");
    impl->dictionaryEditButton = editButton;
    impl->dictionaryDeleteButton = deleteButton;
    gtk_widget_set_tooltip_text(addButton, "添加一条用户词典词条");
    gtk_widget_set_tooltip_text(editButton, "编辑选中的词条");
    gtk_widget_set_tooltip_text(deleteButton, "删除选中的词条");
    gtk_widget_set_tooltip_text(importButton, "从文本文件导入词条");
    gtk_widget_set_tooltip_text(exportButton, "将当前词典导出为文本文件");
    for (auto *button : {addButton, editButton, deleteButton, importButton,
                         exportButton}) {
        gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page), actions, FALSE, FALSE, 0);
    g_signal_connect(addButton, "clicked", G_CALLBACK(onDictionaryAdd), impl);
    g_signal_connect(editButton, "clicked", G_CALLBACK(onDictionaryEdit),
                     impl);
    g_signal_connect(deleteButton, "clicked", G_CALLBACK(onDictionaryDelete),
                     impl);
    g_signal_connect(importButton, "clicked", G_CALLBACK(onDictionaryImport),
                     impl);
    g_signal_connect(exportButton, "clicked", G_CALLBACK(onDictionaryExport),
                     impl);
    g_signal_connect(impl->dictionarySearch, "search-changed",
                     G_CALLBACK(onDictionarySearchChanged), impl);
    refreshDictionaryPage(impl);
    return page;
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
    auto *page = makePageShell(
        "输入法状态", "检查 Fcitx5、ModernIME 插件和当前激活状态");
    auto *section = makeSectionCard(
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
    addStyleClass(impl_->window, "modernime-settings");
    installStyles();
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
    addStyleClass(sidebar, "modernime-sidebar");
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar),
                                GTK_STACK(impl_->stack));
    gtk_widget_set_size_request(sidebar, 200, -1);
    gtk_box_pack_start(GTK_BOX(body), sidebar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), impl_->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);

    const auto pages = legacySettingsPageDefinitions();
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeScrollablePage(makeBasicPage(impl_.get())),
                         pages[0].name.data(), pages[0].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeScrollablePage(makeCandidatePage(impl_.get())),
                         pages[1].name.data(), pages[1].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeScrollablePage(makeClipboardPage(impl_.get())),
                         pages[2].name.data(), pages[2].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeScrollablePage(makeLearningPage(impl_.get())),
                         pages[3].name.data(), pages[3].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeScrollablePage(makeDictionaryPage(impl_.get())),
                         pages[4].name.data(), pages[4].title.data());
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeScrollablePage(makeStatusPage(impl_.get())),
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
    updateDependentSensitivity(impl_.get());
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
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "basic");
}

void SettingsWindow::showCandidatePage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "candidate");
}

void SettingsWindow::showClipboardPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "clipboard");
    refreshClipboardHistory(impl_.get(), false);
}

void SettingsWindow::showLearningPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "learning");
    refreshLearningData(impl_.get(), false);
}

void SettingsWindow::showDictionaryPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "dictionary");
    refreshDictionaryPage(impl_.get());
}

void SettingsWindow::showStatusPage() {
    gtk_stack_set_visible_child_name(GTK_STACK(impl_->stack), "status");
}

void SettingsWindow::presentError(std::string_view message) {
    setStatus(impl_.get(), std::string(message).c_str());
}

} // namespace modernime::settings
