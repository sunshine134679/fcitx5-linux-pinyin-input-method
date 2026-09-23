#include "modernime/settings/pages/clipboard_page.h"

#include "modernime/settings/clipboard_history_model.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include "modernime/core/clipboard_history.h"

#include <gtk/gtk.h>

#include <array>
#include <optional>
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

void setWidgetError(GtkWidget *widget, bool invalid, std::string_view message) {
    auto *context = gtk_widget_get_style_context(widget);
    if (invalid) {
        gtk_style_context_add_class(context, "error");
        gtk_widget_set_tooltip_text(widget, std::string(message).c_str());
    } else {
        gtk_style_context_remove_class(context, "error");
        gtk_widget_set_tooltip_text(widget, nullptr);
    }
}

std::size_t countUtf8Characters(std::string_view text) {
    std::size_t count = 0;
    for (char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::size_t countLines(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    std::size_t lines = 1;
    for (char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    return lines;
}

std::string sanitizePreview(std::string_view text, std::size_t maxChars = 100) {
    std::string result;
    result.reserve(std::min<std::size_t>(text.size(), maxChars + 10));
    bool inWhitespace = false;
    for (char c : text) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!inWhitespace && !result.empty()) {
                result.push_back(' ');
                inWhitespace = true;
            }
        } else {
            result.push_back(c);
            inWhitespace = false;
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    if (result.empty()) {
        return "(空白内容)";
    }
    if (countUtf8Characters(result) > maxChars) {
        std::size_t chars = 0;
        std::size_t byteIdx = 0;
        while (byteIdx < result.size() && chars < maxChars) {
            unsigned char byte = static_cast<unsigned char>(result[byteIdx]);
            std::size_t charLen = 1;
            if ((byte & 0xE0) == 0xC0) charLen = 2;
            else if ((byte & 0xF0) == 0xE0) charLen = 3;
            else if ((byte & 0xF8) == 0xF0) charLen = 4;
            if (byteIdx + charLen > result.size()) break;
            byteIdx += charLen;
            ++chars;
        }
        result.resize(byteIdx);
        result += "…";
    }
    return result;
}

struct ClipboardItemMeta {
    std::string typeBadge;
    std::string preview;
    std::string sizeLabel;
    std::size_t lineCount;
    std::size_t charCount;
};

ClipboardItemMeta analyzeClipboardContent(std::string_view text) {
    const auto lines = countLines(text);
    const auto chars = countUtf8Characters(text);
    const auto preview = sanitizePreview(text, 90);

    std::string badge;
    if (text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0 ||
        text.rfind("ftp://", 0) == 0 || text.rfind("file://", 0) == 0) {
        badge = "🔗 链接";
    } else if (lines > 1) {
        static const std::array<std::string_view, 14> codeKeywords = {
            "#include", "import ", "def ", "class ", "func ", "function",
            "void ", "int ", "const ", "var ", "let ", ":=", "$(", "all:"
        };
        bool isCode = false;
        for (const auto &kw : codeKeywords) {
            if (text.find(kw) != std::string_view::npos) {
                isCode = true;
                break;
            }
        }
        if (!isCode && (text.find('{') != std::string_view::npos && text.find('}') != std::string_view::npos)) {
            isCode = true;
        }
        if (!isCode && (text.find(" = ") != std::string_view::npos && text.find('\t') != std::string_view::npos)) {
            isCode = true;
        }
        if (isCode) {
            badge = "💻 代码 (" + std::to_string(lines) + "行)";
        } else {
            badge = "📄 多行 (" + std::to_string(lines) + "行)";
        }
    } else {
        badge = "📝 文本";
    }

    return ClipboardItemMeta{
        badge,
        preview,
        std::to_string(chars) + " 字",
        lines,
        chars
    };
}

} // namespace

class ClipboardPage::Impl final {
public:
    Impl(SettingsWindowModel &settingsModel, std::filesystem::path historyPath,
         std::function<void()> settingsChangedCallback,
         std::function<void(std::string)> notifyCallback)
        : model(settingsModel), history(std::move(historyPath)),
          settingsChanged(std::move(settingsChangedCallback)),
          notify(std::move(notifyCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "剪贴板", "配置 V+2 功能并查看保存在本地的剪贴板历史"));
        page = pageGuard.get();
        buildPage();
        refreshSettings();
        pageGuard.release();
    }

    void buildPage() {
        auto *settingsSection = createSectionCard(
            "触发方式",
            "中文输入状态下输入触发字母后紧跟触发数字即可打开剪贴板。");
        gtk_box_pack_start(GTK_BOX(page), settingsSection, FALSE, FALSE, 0);

        clipboardEnabled = gtk_switch_new();
        setAccessibleWidgetText(clipboardEnabled, "启用 V+2 剪贴板",
                                "启用或关闭本地剪贴板历史入口");
        setTarget(clipboardEnabled, "clipboard-enabled");
        gtk_box_pack_start(GTK_BOX(settingsSection),
                           createSettingRow("启用 V+2 剪贴板",
                                            "中文输入状态下输入触发字母后紧跟数字即可打开剪贴板。",
                                            clipboardEnabled),
                           FALSE, FALSE, 0);

        auto *grid = gtk_grid_new();
        clipboardTriggerFallback = grid;
        setAccessibleWidgetText(
            clipboardTriggerFallback, "剪贴板触发键",
            "剪贴板功能关闭时仍可定位并阅读触发键设置");
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        auto *triggerLabel = gtk_label_new("剪贴板触发键");
        gtk_widget_set_halign(triggerLabel, GTK_ALIGN_START);
        clipboardTrigger = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(clipboardTrigger), 3);
        gtk_entry_set_width_chars(GTK_ENTRY(clipboardTrigger), 6);
        gtk_entry_set_placeholder_text(GTK_ENTRY(clipboardTrigger), "例如 V+2");
        gtk_widget_set_hexpand(clipboardTrigger, TRUE);
        setTarget(clipboardTrigger, "clipboard-trigger");
        setAccessibleWidgetText(
            clipboardTrigger, "剪贴板触发键",
            "设置在中文输入状态打开本地剪贴板历史的按键");
        gtk_grid_attach(GTK_GRID(grid), triggerLabel, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), clipboardTrigger, 1, 0, 1, 1);
        gtk_box_pack_start(GTK_BOX(settingsSection), grid, FALSE, FALSE, 0);

        auto *description = gtk_label_new(
            "输入触发字母（默认 v）后立即按下触发数字（默认 2）即可打开剪贴板。"
            "触发字母本身按正常拼音输入处理，不会被吞掉或改写。");
        addStyleClass(description, "modernime-description");
        gtk_widget_set_halign(description, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
        gtk_box_pack_start(GTK_BOX(settingsSection), description, FALSE, FALSE,
                           0);
        auto *historySection = createSectionCard(
            "剪贴板历史", "按最近使用顺序显示，最多保留 30 条内容。");
        gtk_box_pack_start(GTK_BOX(page), historySection, TRUE, TRUE, 0);
        historyCount = gtk_label_new("正在读取…");
        addStyleClass(historyCount, "modernime-description");
        gtk_widget_set_halign(historyCount, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(historySection), historyCount, FALSE, FALSE,
                           0);

        auto historyStoreOwner =
            detail::GObjectHandle<GtkListStore>::adopt(
                gtk_list_store_new(5, G_TYPE_UINT, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING));
        historyStore = historyStoreOwner.get();
        historyView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(historyStore));
        historyStoreOwner.reset();
        setTarget(historyView, "clipboard-history");
        setAccessibleWidgetText(
            historyView, "剪贴板历史",
            "选择一条本地历史后可以查看详情、复制或删除");
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(historyView), TRUE);
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(historyView), TRUE);
        gtk_widget_set_tooltip_text(
            historyView, "选择一条历史后可以复制、删除；双击行可直接复制；支持键盘上下键移动浏览");

        auto *numberRenderer = gtk_cell_renderer_text_new();
        g_object_set(numberRenderer, "xalign", 0.5f, nullptr);
        auto *numberColumn = gtk_tree_view_column_new_with_attributes(
            "序号", numberRenderer, "text", 0, nullptr);
        gtk_tree_view_column_set_min_width(numberColumn, 52);
        gtk_tree_view_column_set_resizable(numberColumn, FALSE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(historyView), numberColumn);

        auto *typeRenderer = gtk_cell_renderer_text_new();
        auto *typeColumn = gtk_tree_view_column_new_with_attributes(
            "类型", typeRenderer, "text", 1, nullptr);
        gtk_tree_view_column_set_min_width(typeColumn, 110);
        gtk_tree_view_column_set_resizable(typeColumn, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(historyView), typeColumn);

        auto *textRenderer = gtk_cell_renderer_text_new();
        g_object_set(textRenderer, "ellipsize", PANGO_ELLIPSIZE_END,
                     "single-paragraph-mode", TRUE, nullptr);
        auto *textColumn = gtk_tree_view_column_new_with_attributes(
            "内容预览", textRenderer, "text", 2, nullptr);
        gtk_tree_view_column_set_expand(textColumn, TRUE);
        gtk_tree_view_column_set_resizable(textColumn, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(historyView), textColumn);

        auto *sizeRenderer = gtk_cell_renderer_text_new();
        g_object_set(sizeRenderer, "xalign", 1.0f, nullptr);
        auto *sizeColumn = gtk_tree_view_column_new_with_attributes(
            "字数", sizeRenderer, "text", 3, nullptr);
        gtk_tree_view_column_set_min_width(sizeColumn, 68);
        gtk_tree_view_column_set_resizable(sizeColumn, FALSE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(historyView), sizeColumn);

        auto *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(historyView));
        gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
        g_signal_connect(selection, "changed", G_CALLBACK(onSelectionChanged),
                         this);
        g_signal_connect(historyView, "row-activated",
                         G_CALLBACK(onRowActivated), this);

        auto *historyScrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_vexpand(historyScrolled, TRUE);
        gtk_widget_set_hexpand(historyScrolled, TRUE);
        gtk_widget_set_size_request(historyScrolled, -1, 150);
        gtk_container_add(GTK_CONTAINER(historyScrolled), historyView);
        gtk_box_pack_start(GTK_BOX(historySection), historyScrolled, TRUE, TRUE,
                           0);

        // Detail Inspector Card
        inspectorCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addStyleClass(inspectorCard, "modernime-clipboard-inspector");
        setAccessibleWidgetText(inspectorCard, "详细内容检查器",
                                "显示选中的剪贴板历史完整格式化内容");

        inspectorTitle = gtk_label_new("详细内容预览");
        addStyleClass(inspectorTitle, "modernime-clipboard-inspector-header");
        gtk_widget_set_halign(inspectorTitle, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(inspectorCard), inspectorTitle, FALSE, FALSE,
                           0);

        auto *inspectorScrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_size_request(inspectorScrolled, -1, 120);
        gtk_scrolled_window_set_shadow_type(
            GTK_SCROLLED_WINDOW(inspectorScrolled), GTK_SHADOW_IN);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inspectorScrolled),
                                       GTK_POLICY_AUTOMATIC,
                                       GTK_POLICY_AUTOMATIC);

        inspectorTextView = gtk_text_view_new();
        inspectorBuffer =
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(inspectorTextView));
        gtk_text_view_set_editable(GTK_TEXT_VIEW(inspectorTextView), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(inspectorTextView), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(inspectorTextView),
                                    GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(inspectorTextView), 8);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(inspectorTextView), 8);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(inspectorTextView), 6);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(inspectorTextView), 6);
        addStyleClass(inspectorTextView, "modernime-clipboard-view");
        setAccessibleWidgetText(inspectorTextView, "详细内容预览",
                                "显示选中的剪贴板历史完整格式化内容");

        gtk_container_add(GTK_CONTAINER(inspectorScrolled), inspectorTextView);
        gtk_box_pack_start(GTK_BOX(inspectorCard), inspectorScrolled, TRUE,
                           TRUE, 0);
        gtk_box_pack_start(GTK_BOX(historySection), inspectorCard, FALSE, FALSE,
                           0);

        historyState = gtk_label_new("正在读取…");
        addStyleClass(historyState, "modernime-description");
        gtk_widget_set_halign(historyState, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(historyState), TRUE);
        gtk_box_pack_start(GTK_BOX(historySection), historyState, FALSE, FALSE,
                           0);

        auto *historyActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        const auto actionLabels = settingsClipboardActionLabels();
        copyButton = gtk_button_new_with_label(actionLabels[0].data());
        deleteButton = gtk_button_new_with_label(actionLabels[1].data());
        clearButton = gtk_button_new_with_label(actionLabels[2].data());
        gtk_widget_set_tooltip_text(copyButton, "复制选中的历史内容到系统剪贴板");
        gtk_widget_set_tooltip_text(deleteButton, "删除选中的历史内容");
        gtk_widget_set_tooltip_text(clearButton, "清空全部历史内容，需要确认");
        setAccessibleWidgetText(copyButton, actionLabels[0],
                                "复制选中的历史内容到系统剪贴板");
        setAccessibleWidgetText(deleteButton, actionLabels[1],
                                "删除选中的历史内容");
        setAccessibleWidgetText(clearButton, actionLabels[2],
                                "确认后清空全部本地剪贴板历史");
        for (auto *button : {copyButton, deleteButton, clearButton}) {
            gtk_box_pack_start(GTK_BOX(historyActions), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(historySection), historyActions, FALSE, FALSE,
                           0);

        auto *refreshButton = gtk_button_new_with_label("刷新历史");
        gtk_widget_set_halign(refreshButton, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(refreshButton, "重新读取磁盘上的剪贴板历史");
        setAccessibleWidgetText(refreshButton, "刷新历史",
                                "重新读取磁盘上的剪贴板历史");
        gtk_box_pack_start(GTK_BOX(historySection), refreshButton, FALSE, FALSE,
                           0);
        g_signal_connect(refreshButton, "clicked", G_CALLBACK(onRefresh), this);
        g_signal_connect(copyButton, "clicked", G_CALLBACK(onCopy), this);
        g_signal_connect(deleteButton, "clicked", G_CALLBACK(onDelete), this);
        g_signal_connect(clearButton, "clicked", G_CALLBACK(onClear), this);
        g_signal_connect(clipboardEnabled, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(clipboardTrigger, "changed", G_CALLBACK(onChanged),
                         this);
        setSettingsFocusChain(
            page, {clipboardEnabled, clipboardTrigger, historyView,
                   copyButton, deleteButton, clearButton, refreshButton});
    }

    void refresh(bool shouldNotify) {
        gtk_list_store_clear(historyStore);
        std::string error;
        if (!history.reload(&error)) {
            gtk_label_set_text(GTK_LABEL(historyCount), "历史读取失败");
            gtk_label_set_text(GTK_LABEL(historyState),
                               error.empty() ? "无法读取剪贴板历史"
                                             : ("无法读取剪贴板历史：" + error)
                                                   .c_str());
            gtk_widget_set_visible(historyState, TRUE);
            if (inspectorCard != nullptr) {
                gtk_widget_set_visible(inspectorCard, FALSE);
            }
            updateHistoryActionState();
            if (shouldNotify) {
                notifyMessage(error);
            }
            return;
        }

        const auto &entries = history.entries();
        gtk_label_set_text(GTK_LABEL(historyState),
                           entries.empty()
                               ? "暂无剪贴板历史；复制内容后会自动记录"
                               : "");
        gtk_widget_set_visible(historyState, entries.empty());
        if (inspectorCard != nullptr) {
            gtk_widget_set_visible(inspectorCard, !entries.empty());
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto &entry = entries[index];
            const auto meta = analyzeClipboardContent(entry);
            GtkTreeIter iter;
            gtk_list_store_append(historyStore, &iter);
            gtk_list_store_set(historyStore, &iter,
                               0, static_cast<guint>(index + 1),
                               1, meta.typeBadge.c_str(),
                               2, meta.preview.c_str(),
                               3, meta.sizeLabel.c_str(),
                               4, entry.c_str(),
                               -1);
        }
        gtk_label_set_text(
            GTK_LABEL(historyCount),
            ("当前 " + std::to_string(entries.size()) + " 条，最多保存 " +
             std::to_string(core::ClipboardHistory::kMaxEntries) + " 条")
                .c_str());

        if (!entries.empty()) {
            auto *selection =
                gtk_tree_view_get_selection(GTK_TREE_VIEW(historyView));
            GtkTreePath *path = gtk_tree_path_new_first();
            gtk_tree_selection_select_path(selection, path);
            gtk_tree_path_free(path);
        }

        updateHistoryActionState();
        if (shouldNotify) {
            notifyMessage("剪贴板历史已刷新");
        }
    }

    void refreshSettings() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_switch_set_active(GTK_SWITCH(clipboardEnabled),
                              settings.clipboardEnabled);
        gtk_entry_set_text(GTK_ENTRY(clipboardTrigger),
                           settings.clipboardTrigger.c_str());
        refreshing = false;
        updateSettingsState();
    }

    bool focusTarget(std::string_view target) {
        const std::array controls{
            std::pair{clipboardEnabled, static_cast<GtkWidget *>(nullptr)},
            std::pair{clipboardTrigger, clipboardTriggerFallback},
            std::pair{historyView, static_cast<GtkWidget *>(nullptr)},
        };
        for (const auto &[control, fallback] : controls) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                return focusWidgetOrFallback(control, fallback);
            }
        }
        return false;
    }

    GtkWidget *widget() const { return page; }

private:
    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        onChanged(nullptr, data);
    }

    static void onChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        impl->model.setClipboardOptions(
            gtk_switch_get_active(GTK_SWITCH(impl->clipboardEnabled)),
            gtk_entry_get_text(GTK_ENTRY(impl->clipboardTrigger)));
        impl->updateSettingsState();
        impl->settingsChanged();
    }

    static void onSelectionChanged(GtkTreeSelection *, gpointer data) {
        static_cast<Impl *>(data)->updateHistoryActionState();
    }

    static void onRefresh(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->refresh(true);
    }

    static void onCopy(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要复制的剪贴板历史");
            return;
        }
        const auto &entry = impl->history.entries()[*selected];
        auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (clipboard == nullptr) {
            impl->notifyMessage("系统剪贴板不可用");
            return;
        }
        gtk_clipboard_set_text(clipboard, entry.c_str(), -1);
        impl->notifyMessage("已复制选中的剪贴板历史");
    }

    static void onDelete(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要删除的剪贴板历史");
            return;
        }
        const auto number = std::to_string(*selected + 1);
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO, "确定删除第 %s 条剪贴板历史吗？", number.c_str());
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "删除",
            "删除选中的本地剪贴板历史");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留剪贴板历史并关闭对话框");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        std::string error;
        if (impl->history.remove(*selected, &error)) {
            impl->refresh(false);
            impl->notifyMessage("已删除选中的剪贴板历史");
        } else {
            impl->notifyMessage(error.empty() ? "剪贴板历史删除失败" : error);
        }
    }

    static void onClear(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->history.entries().empty()) {
            impl->notifyMessage("当前没有可清空的剪贴板历史");
            return;
        }
        const auto count = std::to_string(impl->history.entries().size());
        auto *dialog = gtk_message_dialog_new(
            impl->parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_YES_NO,
            "确定清空全部 %s 条剪贴板历史吗？此操作不可撤销。", count.c_str());
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_YES, "清空",
            "清空全部本地剪贴板历史且无法撤销");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO,
                                       "取消", "保留全部剪贴板历史");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES) {
            return;
        }

        std::string error;
        if (impl->history.clear(&error)) {
            impl->refresh(false);
            impl->notifyMessage("剪贴板历史已清空");
        } else {
            impl->notifyMessage(error.empty() ? "剪贴板历史清空失败" : error);
        }
    }

    static void onRowActivated(GtkTreeView *, GtkTreePath *,
                               GtkTreeViewColumn *, gpointer data) {
        onCopy(nullptr, data);
    }

    std::optional<std::size_t> selectedHistoryIndex() const {
        auto *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(historyView));
        GtkTreeModel *treeModel = nullptr;
        GtkTreeIter iterator;
        if (!gtk_tree_selection_get_selected(selection, &treeModel, &iterator)) {
            return std::nullopt;
        }
        auto *path = gtk_tree_model_get_path(treeModel, &iterator);
        if (path == nullptr) {
            return std::nullopt;
        }
        const auto *indices = gtk_tree_path_get_indices(path);
        const auto index = indices == nullptr ? -1 : indices[0];
        gtk_tree_path_free(path);
        if (index < 0 || static_cast<std::size_t>(index) >= history.entries().size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(index);
    }

    void updateHistoryActionState() {
        const auto selected = selectedHistoryIndex();
        const bool hasSelected = selected.has_value();
        const bool hasEntries = !history.entries().empty();
        gtk_widget_set_sensitive(copyButton, hasSelected);
        gtk_widget_set_sensitive(deleteButton, hasSelected);
        gtk_widget_set_sensitive(clearButton, hasEntries);
        updateInspector(selected);
    }

    void updateInspector(std::optional<std::size_t> selected) {
        if (inspectorCard == nullptr || inspectorBuffer == nullptr) {
            return;
        }
        if (!selected.has_value()) {
            gtk_label_set_text(GTK_LABEL(inspectorTitle),
                               "详细内容预览（请在上方列表中选择一条记录）");
            gtk_text_buffer_set_text(inspectorBuffer, "", -1);
            return;
        }
        const auto &entries = history.entries();
        if (*selected >= entries.size()) {
            gtk_label_set_text(GTK_LABEL(inspectorTitle), "详细内容预览");
            gtk_text_buffer_set_text(inspectorBuffer, "", -1);
            return;
        }
        const auto &entry = entries[*selected];
        const auto meta = analyzeClipboardContent(entry);
        const auto title = "详细内容预览 · 第 " + std::to_string(*selected + 1) +
                           " 条 · " + meta.typeBadge + " · " +
                           std::to_string(meta.lineCount) + " 行，" +
                           std::to_string(meta.charCount) + " 字符";
        gtk_label_set_text(GTK_LABEL(inspectorTitle), title.c_str());
        gtk_text_buffer_set_text(inspectorBuffer, entry.c_str(), -1);
    }

    void updateSettingsState() {
        gtk_widget_set_sensitive(
            clipboardTrigger,
            gtk_switch_get_active(GTK_SWITCH(clipboardEnabled)));
        const auto validation = model.validation();
        for (const auto &issue : validation.issues) {
            if (issue.key == "clipboard.trigger") {
                setWidgetError(clipboardTrigger, true, issue.message);
                return;
            }
        }
        setWidgetError(clipboardTrigger, false, "");
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

    SettingsWindowModel &model;
    ClipboardHistoryModel history;
    std::function<void()> settingsChanged;
    std::function<void(std::string)> notify;
    GtkWidget *page = nullptr;
    GtkWidget *clipboardEnabled = nullptr;
    GtkWidget *clipboardTrigger = nullptr;
    GtkWidget *clipboardTriggerFallback = nullptr;
    GtkListStore *historyStore = nullptr;
    GtkWidget *historyCount = nullptr;
    GtkWidget *historyState = nullptr;
    GtkWidget *historyView = nullptr;
    GtkWidget *inspectorCard = nullptr;
    GtkWidget *inspectorTitle = nullptr;
    GtkWidget *inspectorTextView = nullptr;
    GtkTextBuffer *inspectorBuffer = nullptr;
    GtkWidget *copyButton = nullptr;
    GtkWidget *deleteButton = nullptr;
    GtkWidget *clearButton = nullptr;
    bool refreshing = false;
};

ClipboardPage::ClipboardPage(SettingsWindowModel &settings,
                             std::filesystem::path historyPath,
                             std::function<void()> settingsChanged,
                             std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(settings, std::move(historyPath),
                                   std::move(settingsChanged),
                                   std::move(notify))) {}

ClipboardPage::~ClipboardPage() = default;

GtkWidget *ClipboardPage::widget() const {
    return impl_->widget();
}

void ClipboardPage::refresh(bool notify) {
    impl_->refresh(notify);
}

void ClipboardPage::refreshSettings() {
    impl_->refreshSettings();
}

bool ClipboardPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
