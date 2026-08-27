#include "modernime/settings/settings_window.h"
#include "modernime/settings/data_controller.h"
#include "modernime/settings/runtime_controller.h"

#include "modernime/pinyin/user_dictionary.h"

#include <gtk/gtk.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
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
    GtkWidget *inputEnabled = nullptr;
    GtkWidget *defaultMode = nullptr;
    GtkWidget *toggleKey = nullptr;
    GtkWidget *candidateNumber = nullptr;
    GtkWidget *candidateArrow = nullptr;
    GtkWidget *candidatePage = nullptr;
    GtkWidget *learningEnabled = nullptr;
    GtkWidget *contextLearning = nullptr;
    GtkWidget *learningPath = nullptr;
    GtkWidget *clearLearningButton = nullptr;
    GtkListStore *dictionaryStore = nullptr;
    GtkWidget *dictionaryView = nullptr;
    std::vector<pinyin::UserDictionaryEntry> dictionaryEntries;
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

void updateModelFromLearningPage(SettingsWindow::Impl *impl) {
    auto settings = impl->model.settings();
    settings.learningEnabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(impl->learningEnabled));
    settings.contextLearningEnabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(impl->contextLearning));
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

void onBasicChanged(GtkWidget *, gpointer data) {
    updateModelFromBasicPage(static_cast<SettingsWindow::Impl *>(data));
}

void onCandidateChanged(GtkWidget *, gpointer data) {
    updateModelFromCandidatePage(static_cast<SettingsWindow::Impl *>(data));
}

void onLearningChanged(GtkWidget *, gpointer data) {
    updateModelFromLearningPage(static_cast<SettingsWindow::Impl *>(data));
}

void onSave(GtkButton *, gpointer data) {
    auto *impl = static_cast<SettingsWindow::Impl *>(data);
    updateModelFromBasicPage(impl);
    updateModelFromCandidatePage(impl);
    updateModelFromLearningPage(impl);
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
    updateLearningPageFromModel(impl);
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
        setStatus(impl, ("学习记录已清空，备份位于 " + backup.string()).c_str());
    } else {
        setStatus(impl, error.c_str());
    }
    gtk_widget_set_sensitive(impl->clearLearningButton, TRUE);
}

GtkWidget *makeLearningPage(SettingsWindow::Impl *impl) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    auto *heading = gtk_label_new("智能学习");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),
                                "title-3");
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);

    impl->learningEnabled = gtk_check_button_new_with_label("记忆用户候选选择");
    impl->contextLearning = gtk_check_button_new_with_label(
        "根据光标前后文调整候选排序");
    gtk_box_pack_start(GTK_BOX(box), impl->learningEnabled, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), impl->contextLearning, FALSE, FALSE, 0);
    updateLearningPageFromModel(impl);
    g_signal_connect(impl->learningEnabled, "toggled",
                     G_CALLBACK(onLearningChanged), impl);
    g_signal_connect(impl->contextLearning, "toggled",
                     G_CALLBACK(onLearningChanged), impl);

    const auto pathText = "学习数据库：" + impl->paths_.learningStore.string();
    impl->learningPath = gtk_label_new(pathText.c_str());
    gtk_widget_set_halign(impl->learningPath, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(impl->learningPath), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(impl->learningPath), TRUE);
    gtk_box_pack_start(GTK_BOX(box), impl->learningPath, FALSE, FALSE, 0);

    impl->clearLearningButton = gtk_button_new_with_label("清空学习记录");
    gtk_widget_set_halign(impl->clearLearningButton, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), impl->clearLearningButton, FALSE, FALSE,
                       0);
    g_signal_connect(impl->clearLearningButton, "clicked",
                     G_CALLBACK(onClearLearning), impl);
    return box;
}

void refreshDictionaryPage(SettingsWindow::Impl *impl) {
    impl->dictionaryEntries = DataController::loadDictionary(
        impl->paths_.userDictionary);
    gtk_list_store_clear(impl->dictionaryStore);
    for (const auto &entry : impl->dictionaryEntries) {
        auto weight = std::ostringstream{};
        weight << std::setprecision(9) << entry.weight;
        GtkTreeIter iterator;
        gtk_list_store_append(impl->dictionaryStore, &iterator);
        gtk_list_store_set(impl->dictionaryStore, &iterator, 0,
                           entry.pinyin.c_str(), 1, entry.phrase.c_str(), 2,
                           weight.str().c_str(), -1);
    }
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
    auto *path = gtk_tree_model_get_path(model, &iterator);
    if (path == nullptr) {
        return std::nullopt;
    }
    const auto *indices = gtk_tree_path_get_indices(path);
    const auto index = indices == nullptr ? -1 : indices[0];
    gtk_tree_path_free(path);
    if (index < 0 || static_cast<std::size_t>(index) >=
                         impl->dictionaryEntries.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(index);
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
    if (selected.has_value()) {
        const auto &entry = impl->dictionaryEntries[*selected];
        gtk_entry_set_text(GTK_ENTRY(pinyin), entry.pinyin.c_str());
        gtk_entry_set_text(GTK_ENTRY(phrase), entry.phrase.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), entry.weight);
    }
    gtk_widget_show_all(dialog);
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
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const auto filename = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialog));
        const auto imported = DataController::loadDictionary(filename);
        g_free(filename);
        saveDictionaryEntries(impl, imported);
    }
    gtk_widget_destroy(dialog);
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
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    auto *heading = gtk_label_new("用户词典");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading),
                                "title-3");
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);
    impl->dictionaryStore = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING,
                                               G_TYPE_STRING);
    impl->dictionaryView = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(impl->dictionaryStore));
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
    gtk_container_add(GTK_CONTAINER(scrolled), impl->dictionaryView);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);

    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const auto addButton = gtk_button_new_with_label("添加");
    const auto editButton = gtk_button_new_with_label("编辑");
    const auto deleteButton = gtk_button_new_with_label("删除");
    const auto importButton = gtk_button_new_with_label("导入");
    const auto exportButton = gtk_button_new_with_label("导出");
    for (auto *button : {addButton, editButton, deleteButton, importButton,
                         exportButton}) {
        gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
    g_signal_connect(addButton, "clicked", G_CALLBACK(onDictionaryAdd), impl);
    g_signal_connect(editButton, "clicked", G_CALLBACK(onDictionaryEdit),
                     impl);
    g_signal_connect(deleteButton, "clicked", G_CALLBACK(onDictionaryDelete),
                     impl);
    g_signal_connect(importButton, "clicked", G_CALLBACK(onDictionaryImport),
                     impl);
    g_signal_connect(exportButton, "clicked", G_CALLBACK(onDictionaryExport),
                     impl);
    refreshDictionaryPage(impl);
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

SettingsWindow::SettingsWindow(void *application, core::SettingsPaths paths)
    : impl_(std::make_unique<Impl>(application, std::move(paths))) {
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
                         makeLearningPage(impl_.get()),
                         "learning", "智能学习");
    gtk_stack_add_titled(GTK_STACK(impl_->stack),
                         makeDictionaryPage(impl_.get()),
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
