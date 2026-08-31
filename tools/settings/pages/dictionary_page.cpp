#include "modernime/settings/pages/dictionary_page.h"

#include "modernime/settings/data_controller.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include "modernime/pinyin/user_dictionary.h"

#include <gtk/gtk.h>

#include <array>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

} // namespace

class DictionaryPage::Impl final {
public:
    Impl(std::filesystem::path dictionaryPath,
         std::function<void(std::string)> notifyCallback)
        : path(std::move(dictionaryPath)), notify(std::move(notifyCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "个人词典", "维护个人词条和专业名词，重载 ModernIME 后生效"));
        page = pageGuard.get();
        buildPage();
        refresh();
        pageGuard.release();
    }

    void buildPage() {
        auto *section = createSectionCard(
            "词条列表", "拼音、词条和权重会在保存时统一校验；导入前会确认整体替换，失败不会覆盖原词典。");
        gtk_box_pack_start(GTK_BOX(page), section, TRUE, TRUE, 0);

        auto *searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto *searchLabel = gtk_label_new("搜索");
        gtk_widget_set_halign(searchLabel, GTK_ALIGN_START);
        dictionarySearch = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(dictionarySearch),
                                       "输入拼音或词条关键词");
        gtk_widget_set_hexpand(dictionarySearch, TRUE);
        gtk_widget_set_tooltip_text(dictionarySearch,
                                    "实时筛选拼音和词条内容");
        setTarget(dictionarySearch, "dictionary-search");
        setAccessibleWidgetText(dictionarySearch, "搜索",
                                "按拼音或词条关键词实时筛选个人词典");
        gtk_box_pack_start(GTK_BOX(searchRow), searchLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(searchRow), dictionarySearch, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(section), searchRow, FALSE, FALSE, 0);

        dictionaryCount = gtk_label_new("正在读取用户词典…");
        addStyleClass(dictionaryCount, "modernime-description");
        gtk_widget_set_halign(dictionaryCount, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(section), dictionaryCount, FALSE, FALSE, 0);

        auto dictionaryStoreOwner =
            detail::GObjectHandle<GtkListStore>::adopt(gtk_list_store_new(
                4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT));
        dictionaryStore = dictionaryStoreOwner.get();
        dictionaryView =
            gtk_tree_view_new_with_model(GTK_TREE_MODEL(dictionaryStore));
        dictionaryStoreOwner.reset();
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(dictionaryView), TRUE);
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(dictionaryView), FALSE);
        setAccessibleWidgetText(dictionaryView, "个人词典词条列表",
                                "选择词条后可以编辑或删除");
        gtk_widget_set_tooltip_text(dictionaryView,
                                    "选择词条后可以编辑或删除");
        for (const auto &column : {std::pair<const char *, int>{"拼音", 0},
                                   {"词条", 1}, {"权重", 2}}) {
            auto *renderer = gtk_cell_renderer_text_new();
            auto *viewColumn = gtk_tree_view_column_new_with_attributes(
                column.first, renderer, "text", column.second, nullptr);
            gtk_tree_view_append_column(GTK_TREE_VIEW(dictionaryView),
                                        viewColumn);
        }
        auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_vexpand(scrolled, TRUE);
        gtk_widget_set_size_request(scrolled, -1, 220);
        gtk_container_add(GTK_CONTAINER(scrolled), dictionaryView);
        gtk_box_pack_start(GTK_BOX(section), scrolled, TRUE, TRUE, 0);
        dictionaryState = gtk_label_new("");
        addStyleClass(dictionaryState, "modernime-description");
        gtk_widget_set_halign(dictionaryState, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(dictionaryState), TRUE);
        gtk_box_pack_start(GTK_BOX(section), dictionaryState, FALSE, FALSE, 0);
        auto *selection =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(dictionaryView));
        gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
        g_signal_connect(selection, "changed", G_CALLBACK(onSelectionChanged),
                         this);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        addButton = gtk_button_new_with_label("添加");
        editButton = gtk_button_new_with_label("编辑");
        deleteButton = gtk_button_new_with_label("删除");
        importButton = gtk_button_new_with_label("导入");
        auto *exportButton = gtk_button_new_with_label("导出");
        setTarget(addButton, "dictionary-add");
        setTarget(importButton, "dictionary-import-export");
        gtk_widget_set_tooltip_text(addButton, "添加一条用户词典词条");
        gtk_widget_set_tooltip_text(editButton, "编辑选中的词条");
        gtk_widget_set_tooltip_text(deleteButton, "删除选中的词条");
        gtk_widget_set_tooltip_text(importButton, "从文本文件导入词条");
        gtk_widget_set_tooltip_text(exportButton,
                                    "将当前词典导出为文本文件");
        setAccessibleWidgetText(addButton, "添加", "添加一条个人词典词条");
        setAccessibleWidgetText(editButton, "编辑", "编辑选中的词条");
        setAccessibleWidgetText(deleteButton, "删除", "删除选中的词条");
        setAccessibleWidgetText(importButton, "导入",
                                "从文本文件导入并整体替换个人词典");
        setAccessibleWidgetText(exportButton, "导出",
                                "将当前个人词典导出为文本文件");
        for (auto *button : {addButton, editButton, deleteButton, importButton,
                             exportButton}) {
            gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(page), actions, FALSE, FALSE, 0);
        g_signal_connect(addButton, "clicked", G_CALLBACK(onAdd), this);
        g_signal_connect(editButton, "clicked", G_CALLBACK(onEdit), this);
        g_signal_connect(deleteButton, "clicked", G_CALLBACK(onDelete), this);
        g_signal_connect(importButton, "clicked", G_CALLBACK(onImport), this);
        g_signal_connect(exportButton, "clicked", G_CALLBACK(onExport), this);
        g_signal_connect(dictionarySearch, "search-changed",
                         G_CALLBACK(onSearchChanged), this);
        setSettingsFocusChain(
            page, {dictionarySearch, dictionaryView, addButton, editButton,
                   deleteButton, importButton, exportButton});
    }

    void refresh() {
        dictionaryEntries = DataController::loadDictionary(path);
        gtk_list_store_clear(dictionaryStore);
        const auto query = dictionarySearch == nullptr
                               ? std::string()
                               : gtk_entry_get_text(GTK_ENTRY(dictionarySearch));
        std::size_t visibleCount = 0;
        for (std::size_t index = 0; index < dictionaryEntries.size(); ++index) {
            const auto &entry = dictionaryEntries[index];
            if (!dictionaryMatches(entry, query)) {
                continue;
            }
            auto weight = std::ostringstream{};
            weight << std::setprecision(9) << entry.weight;
            GtkTreeIter iterator;
            gtk_list_store_append(dictionaryStore, &iterator);
            gtk_list_store_set(dictionaryStore, &iterator, 0,
                               entry.pinyin.c_str(), 1, entry.phrase.c_str(), 2,
                               weight.str().c_str(), 3,
                               static_cast<guint>(index), -1);
            ++visibleCount;
        }

        if (dictionaryCount != nullptr) {
            const auto countText =
                query.empty()
                    ? "共 " + std::to_string(visibleCount) + " 条"
                    : "显示 " + std::to_string(visibleCount) + " 条，共 " +
                          std::to_string(dictionaryEntries.size()) + " 条";
            gtk_label_set_text(GTK_LABEL(dictionaryCount), countText.c_str());
        }
        if (dictionaryState != nullptr) {
            const char *state = nullptr;
            if (dictionaryEntries.empty()) {
                state = "暂无用户词条；可以添加个人词条或导入专业词典";
            } else if (visibleCount == 0) {
                state = "没有匹配的词条；请更换拼音或词条关键词";
            }
            gtk_label_set_text(GTK_LABEL(dictionaryState),
                               state == nullptr ? "" : state);
            gtk_widget_set_visible(dictionaryState, state != nullptr);
        }
        updateActionState();
    }

    bool focusTarget(std::string_view target) {
        const std::array controls{dictionarySearch, addButton, importButton};
        for (auto *control : controls) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                return focusWidgetOrFallback(control);
            }
        }
        return false;
    }

    GtkWidget *widget() const { return page; }

private:
    static void onSelectionChanged(GtkTreeSelection *, gpointer data) {
        static_cast<Impl *>(data)->updateActionState();
    }

    static void onSearchChanged(GtkEditable *, gpointer data) {
        static_cast<Impl *>(data)->refresh();
    }

    static void onAdd(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->editEntry(std::nullopt);
    }

    static void onEdit(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要编辑的词条");
            return;
        }
        impl->editEntry(selected);
    }

    static void onDelete(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要删除的词条");
            return;
        }
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO, "确定删除选中的用户词条吗？");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_YES,
                                       "删除", "删除选中的个人词典词条");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留词条并关闭对话框");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }
        auto entries = impl->dictionaryEntries;
        entries.erase(entries.begin() +
                      static_cast<std::ptrdiff_t>(*selected));
        impl->saveEntries(entries);
    }

    static void onImport(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        auto *dialog = gtk_file_chooser_dialog_new(
            "导入用户词典", impl->parentWindow(), GTK_FILE_CHOOSER_ACTION_OPEN,
            "取消", GTK_RESPONSE_CANCEL, "导入", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭文件选择器且不导入词典");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "导入",
            "读取选中的文本文件并进入整体替换确认");
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
            impl->notifyMessage(error);
            g_free(filename);
            return;
        }
        g_free(filename);
        auto *confirm = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "导入将替换现有全部 %zu 条词条（导入文件含 %zu 条），确定继续吗？",
            impl->dictionaryEntries.size(), imported.size());
        setDialogResponseAccessibility(
            GTK_DIALOG(confirm), GTK_RESPONSE_YES, "替换词典",
            "使用导入内容整体替换当前个人词典");
        setDialogResponseAccessibility(GTK_DIALOG(confirm), GTK_RESPONSE_NO,
                                       "取消", "保留当前个人词典");
        const auto confirmed = gtk_dialog_run(GTK_DIALOG(confirm));
        gtk_widget_destroy(confirm);
        if (confirmed == GTK_RESPONSE_YES) {
            impl->saveEntries(imported);
        } else {
            impl->notifyMessage("已取消导入，现有词典未修改");
        }
    }

    static void onExport(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        auto *dialog = gtk_file_chooser_dialog_new(
            "导出用户词典", impl->parentWindow(), GTK_FILE_CHOOSER_ACTION_SAVE,
            "取消", GTK_RESPONSE_CANCEL, "导出", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭文件选择器且不导出词典");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "导出",
            "将当前个人词典写入选择的文件");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog),
                                                       TRUE);
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            const auto filename =
                gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            std::string error;
            if (DataController::saveDictionary(filename,
                                               impl->dictionaryEntries,
                                               &error)) {
                impl->notifyMessage("用户词典已导出");
            } else {
                impl->notifyMessage(error);
            }
            g_free(filename);
        }
        gtk_widget_destroy(dialog);
    }

    std::optional<std::size_t> selectedIndex() const {
        auto *selection =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(dictionaryView));
        GtkTreeModel *model = nullptr;
        GtkTreeIter iterator;
        if (!gtk_tree_selection_get_selected(selection, &model, &iterator)) {
            return std::nullopt;
        }
        guint sourceIndex = 0;
        gtk_tree_model_get(model, &iterator, 3, &sourceIndex, -1);
        if (static_cast<std::size_t>(sourceIndex) >= dictionaryEntries.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(sourceIndex);
    }

    void updateActionState() {
        const bool hasSelected = selectedIndex().has_value();
        if (editButton != nullptr) {
            gtk_widget_set_sensitive(editButton, hasSelected);
        }
        if (deleteButton != nullptr) {
            gtk_widget_set_sensitive(deleteButton, hasSelected);
        }
    }

    bool saveEntries(const std::vector<pinyin::UserDictionaryEntry> &entries) {
        std::string error;
        if (!DataController::saveDictionary(path, entries, &error)) {
            notifyMessage(error);
            return false;
        }
        refresh();
        notifyMessage("用户词典已保存；重新加载输入法后生效");
        return true;
    }

    bool editEntry(std::optional<std::size_t> selected) {
        auto *dialog = gtk_dialog_new_with_buttons(
            selected.has_value() ? "编辑用户词条" : "添加用户词条",
            parentWindow(), GTK_DIALOG_MODAL, "取消", GTK_RESPONSE_CANCEL,
            "保存", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭对话框且不保存词条修改");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT,
                                       "保存", "校验并保存当前词条");
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
        auto *weight =
            gtk_spin_button_new_with_range(0.0, 1000000000.0, 1.0);
        setAccessibleWidgetText(pinyin, "拼音", "输入词条的完整拼音");
        setAccessibleWidgetText(phrase, "词条", "输入要保存的中文词条");
        setAccessibleWidgetText(weight, "权重",
                                "设置词条排序权重，数值越大越靠前");
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
            const auto &entry = dictionaryEntries[*selected];
            gtk_entry_set_text(GTK_ENTRY(pinyin), entry.pinyin.c_str());
            gtk_entry_set_text(GTK_ENTRY(phrase), entry.phrase.c_str());
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), entry.weight);
        }
        auto state = DictionaryDialogState{
            pinyin, phrase, weight, validation,
            gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                               GTK_RESPONSE_ACCEPT)};
        setAccessibleWidgetText(state.accept, "保存",
                                "校验通过后保存当前词条");
        auto *cancel = gtk_dialog_get_widget_for_response(
            GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        if (cancel != nullptr) {
            setAccessibleWidgetText(cancel, "取消",
                                    "关闭对话框且不保存词条修改");
        }
        setSettingsFocusChain(dialog,
                              {pinyin, phrase, weight, cancel, state.accept});
        gtk_dialog_set_default_response(GTK_DIALOG(dialog),
                                        GTK_RESPONSE_ACCEPT);
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
        gtk_widget_set_visible(validation,
                               !gtk_widget_get_sensitive(state.accept));
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
        for (std::size_t index = 0; index < dictionaryEntries.size(); ++index) {
            if (!selected.has_value() || index != *selected) {
                const auto &entry = dictionaryEntries[index];
                if (!dictionary.upsert(entry.pinyin, entry.phrase,
                                       entry.weight)) {
                    gtk_widget_destroy(dialog);
                    notifyMessage("现有用户词典包含非法词条");
                    return false;
                }
            }
        }
        const bool valid =
            dictionary.upsert(pinyinText, phraseText, weightValue);
        gtk_widget_destroy(dialog);
        if (!valid) {
            notifyMessage("词条未保存：拼音、词条或权重格式不合法");
            return false;
        }
        return saveEntries(dictionary.entries());
    }

    GtkWindow *parentWindow() const {
        auto *topLevel = gtk_widget_get_toplevel(page);
        return GTK_IS_WINDOW(topLevel) ? GTK_WINDOW(topLevel) : nullptr;
    }

    void notifyMessage(std::string message) const {
        if (notify) {
            notify(std::move(message));
        }
    }

    std::filesystem::path path;
    std::function<void(std::string)> notify;
    GtkWidget *page = nullptr;
    GtkListStore *dictionaryStore = nullptr;
    GtkWidget *dictionaryView = nullptr;
    GtkWidget *dictionarySearch = nullptr;
    GtkWidget *dictionaryCount = nullptr;
    GtkWidget *dictionaryState = nullptr;
    GtkWidget *addButton = nullptr;
    GtkWidget *editButton = nullptr;
    GtkWidget *deleteButton = nullptr;
    GtkWidget *importButton = nullptr;
    std::vector<pinyin::UserDictionaryEntry> dictionaryEntries;
};

DictionaryPage::DictionaryPage(std::filesystem::path dictionaryPath,
                               std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(std::move(dictionaryPath),
                                   std::move(notify))) {}

DictionaryPage::~DictionaryPage() = default;

GtkWidget *DictionaryPage::widget() const {
    return impl_->widget();
}

void DictionaryPage::refresh() {
    impl_->refresh();
}

bool DictionaryPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
