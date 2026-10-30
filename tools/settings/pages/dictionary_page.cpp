#include "modernime/settings/pages/dictionary_page.h"

#include "modernime/settings/data_controller.h"
#include "modernime/settings/dictionary_pronunciation.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/runtime_environment.h"
#include "modernime/settings/settings_widgets.h"

#include "modernime/pinyin/user_dictionary.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <gtk/gtk.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
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


struct DictionaryDialogState final {
    GtkWidget *pinyin = nullptr;
    GtkWidget *phrase = nullptr;
    GtkWidget *weight = nullptr;
    GtkWidget *validation = nullptr;
    GtkWidget *accept = nullptr;
    GtkWidget *readings = nullptr;
    GtkWidget *automatic = nullptr;
    std::shared_ptr<const DictionaryPronunciation> pronunciation;
    std::vector<pinyin::UserDictionaryEntry> existing;
    detail::GObjectHandle<GCancellable> cancel =
        detail::GObjectHandle<GCancellable>::adopt(g_cancellable_new());
    bool alive = true;
    bool loading = true;
    bool autoPinyin = true;
    bool syncing = false;
    bool approximate = false;
};

void updateDictionaryDialogValidity(DictionaryDialogState *state) {
    if (!state->alive) return;
    const auto *phrase = gtk_entry_get_text(GTK_ENTRY(state->phrase));
    const auto *reading = gtk_entry_get_text(GTK_ENTRY(state->pinyin));
    pinyin::UserDictionary dictionary;
    const auto valid = dictionary.upsert(
        gtk_entry_get_text(GTK_ENTRY(state->pinyin)),
        gtk_entry_get_text(GTK_ENTRY(state->phrase)),
        static_cast<float>(gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(state->weight))));
    gtk_widget_set_sensitive(state->accept, valid);
    std::string message;
    bool error = false;
    if (*phrase == '\0') {
        message = "输入常用词、人名或专业名词，拼音会自动建议。";
    } else if (*reading == '\0') {
        message = state->loading && state->autoPinyin
            ? "正在读取本地词库，稍后自动填写拼音…"
            : "无法自动建议这个词的读音，请填写完整拼音，例如 ni'hao。";
    } else if (!valid) {
        message = "请填写不带声调的完整拼音，用 ' 分隔音节，ü 写作 v。";
        error = true;
    } else if (!state->autoPinyin) {
        message = "已使用你填写的读音。";
    } else if (state->approximate) {
        message = "这是按汉字组合的建议读音；人名和多音字请确认，也可直接修改。";
    } else {
        message = "已填写词库读音；人名和多音字可修改或从读音列表选择。";
    }
    auto *style = gtk_widget_get_style_context(state->validation);
    if (error) gtk_style_context_add_class(style, "modernime-status-error");
    else gtk_style_context_remove_class(style, "modernime-status-error");
    gtk_label_set_text(GTK_LABEL(state->validation), message.c_str());
    gtk_widget_set_sensitive(state->automatic, !state->loading && *phrase != '\0' &&
                              state->pronunciation && state->pronunciation->available());
}

void suggestDictionaryReading(DictionaryDialogState *state) {
    if (!state->alive || !state->autoPinyin) return;
    const auto *phrase = gtk_entry_get_text(GTK_ENTRY(state->phrase));
    PronunciationSuggestion suggestion;
    if (state->pronunciation) suggestion = state->pronunciation->suggest(phrase);
    // Previously saved personal pronunciations take precedence over guesses.
    for (const auto &entry : state->existing) {
        if (entry.phrase == phrase &&
            std::find(suggestion.readings.begin(), suggestion.readings.end(), entry.pinyin) == suggestion.readings.end()) {
            suggestion.readings.insert(suggestion.readings.begin(), entry.pinyin);
            suggestion.approximate = false;
        }
    }
    state->approximate = suggestion.approximate;
    state->syncing = true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(state->readings));
    for (const auto &reading : suggestion.readings) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->readings), reading.c_str());
    }
    if (!suggestion.readings.empty()) gtk_combo_box_set_active(GTK_COMBO_BOX(state->readings), 0);
    else gtk_entry_set_text(GTK_ENTRY(state->pinyin), "");
    state->syncing = false;
    updateDictionaryDialogValidity(state);
}

void onDictionaryPhraseChanged(GtkEditable *, gpointer data) {
    auto *state = static_cast<DictionaryDialogState *>(data);
    if (state->autoPinyin) suggestDictionaryReading(state);
    else updateDictionaryDialogValidity(state);
}

void onDictionaryReadingChanged(GtkEditable *, gpointer data) {
    auto *state = static_cast<DictionaryDialogState *>(data);
    if (!state->alive || state->syncing) return;
    state->autoPinyin = false;
    updateDictionaryDialogValidity(state);
}

void onDictionarySuggest(GtkButton *, gpointer data) {
    auto *state = static_cast<DictionaryDialogState *>(data);
    state->autoPinyin = true;
    suggestDictionaryReading(state);
}

void onDictionaryDialogDestroyed(GtkWidget *, gpointer data) {
    auto *state = static_cast<DictionaryDialogState *>(data);
    state->alive = false;
    g_cancellable_cancel(state->cancel.get());
}

void loadDictionaryPronunciation(GTask *task, gpointer, gpointer data, GCancellable *cancel) {
    const auto &paths = *static_cast<std::vector<std::filesystem::path> *>(data);
    try {
        auto result = std::make_unique<DictionaryPronunciation>(
            DictionaryPronunciation::load(paths, [cancel] { return g_cancellable_is_cancelled(cancel); }));
        g_task_return_pointer(task, result.release(), [](gpointer value) {
            delete static_cast<DictionaryPronunciation *>(value);
        });
    } catch (const std::exception &error) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
    }
}

void onDictionaryPronunciationReady(GObject *, GAsyncResult *result, gpointer data) {
    std::unique_ptr<std::shared_ptr<DictionaryDialogState>> owner(
        static_cast<std::shared_ptr<DictionaryDialogState> *>(data));
    auto &state = **owner;
    GError *error = nullptr;
    std::unique_ptr<DictionaryPronunciation> pronunciation(
        static_cast<DictionaryPronunciation *>(g_task_propagate_pointer(G_TASK(result), &error)));
    g_clear_error(&error);
    if (!state.alive) return;
    state.loading = false;
    state.pronunciation = std::shared_ptr<const DictionaryPronunciation>(pronunciation.release());
    if (state.autoPinyin) suggestDictionaryReading(&state);
    else updateDictionaryDialogValidity(&state);
}

void startDictionaryPronunciation(const std::shared_ptr<DictionaryDialogState> &state) {
    std::filesystem::path fallback;
#ifdef MODERNIME_INSTALL_PREFIX
    fallback = MODERNIME_INSTALL_PREFIX;
#endif
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    const auto prefix = installationPrefix(error ? std::filesystem::path{} : executable, fallback);
    auto *paths = new std::vector<std::filesystem::path>{pinyin::defaultDictionaryPath()};
    for (const auto &root : {prefix, fallback}) {
        for (const auto *name : {"modernime-knowledge.dict", "modernime-hotwords.dict"}) {
            paths->push_back(root / "share/modernime/pinyin" / name);
        }
    }
    auto *task = g_task_new(nullptr, state->cancel.get(), onDictionaryPronunciationReady,
                           new std::shared_ptr<DictionaryDialogState>(state));
    g_task_set_task_data(task, paths, [](gpointer value) {
        delete static_cast<std::vector<std::filesystem::path> *>(value);
    });
    g_task_run_in_thread(task, loadDictionaryPronunciation);
    g_object_unref(task);
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
            "个人词典", "添加常用词、人名和专业名词，输入法会记住你的用词"));
        page = pageGuard.get();
        buildPage();
        refresh();
        pageGuard.release();
    }

    ~Impl() {
        if (lifetime) {
            lifetime->deactivate();
        }
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
    }

    void buildPage() {
        auto *section = createSectionCard(
            "我的词条", "输入词条即可建议拼音；选中已有词条可修改读音或删除。");
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
        gtk_widget_set_margin_bottom(searchRow, 10);
        gtk_box_pack_start(GTK_BOX(section), searchRow, FALSE, FALSE, 0);

        dictionaryCount = gtk_label_new("正在读取用户词典…");
        addStyleClass(dictionaryCount, "modernime-description");
        gtk_widget_set_halign(dictionaryCount, GTK_ALIGN_START);
        gtk_widget_set_margin_bottom(dictionaryCount, 8);
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
        for (const auto &column : {std::pair<const char *, int>{"词条", 1}, {"拼音", 0}}) {
            auto *renderer = gtk_cell_renderer_text_new();
            auto *viewColumn = gtk_tree_view_column_new_with_attributes(
                column.first, renderer, "text", column.second, nullptr);
            gtk_tree_view_column_set_expand(viewColumn, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(dictionaryView),
                                        viewColumn);
        }
        auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_vexpand(scrolled, TRUE);
        gtk_widget_set_size_request(scrolled, -1, 160);
        gtk_container_add(GTK_CONTAINER(scrolled), dictionaryView);
        gtk_box_pack_start(GTK_BOX(section), scrolled, TRUE, TRUE, 0);
        dictionaryState = gtk_label_new("");
        addStyleClass(dictionaryState, "modernime-description");
        gtk_widget_set_halign(dictionaryState, GTK_ALIGN_START);
        gtk_label_set_line_wrap(GTK_LABEL(dictionaryState), TRUE);
        gtk_widget_set_margin_top(dictionaryState, 6);
        gtk_box_pack_start(GTK_BOX(section), dictionaryState, FALSE, FALSE, 0);
        auto *selection =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(dictionaryView));
        gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
        g_signal_connect(selection, "changed", G_CALLBACK(onSelectionChanged),
                         this);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(actions, 10);
        gtk_widget_set_margin_bottom(actions, 2);
        addButton = gtk_button_new_with_label("添加");
        addStyleClass(addButton, kSettingsPrimaryButtonClass);
        editButton = gtk_button_new_with_label("编辑");
        deleteButton = gtk_button_new_with_label("删除");
        importButton = gtk_button_new_with_label("导入");
        exportButton = gtk_button_new_with_label("导出");
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
        gtk_box_pack_start(GTK_BOX(section), actions, FALSE, FALSE, 0);
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

    struct LoadTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        uint64_t generation = 0;
    };

    static void onLoadFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *error = nullptr;
        auto *snapshotPtr = static_cast<DictionarySnapshot *>(
            g_task_propagate_pointer(G_TASK(result), &error));
        g_clear_error(&error);
        const auto *taskData = static_cast<const LoadTaskData *>(
            g_task_get_task_data(G_TASK(result)));
        if (taskData != nullptr && snapshotPtr != nullptr) {
            const auto gen = taskData->generation;
            taskData->lifetime->withOwner([snapshotPtr, gen](Impl &owner) {
                if (gen == owner.loadGeneration) {
                    owner.currentSnapshot = *snapshotPtr;
                    owner.currentBaseline =
                        DictionaryExpectedBaseline::fromSnapshot(owner.currentSnapshot);
                    owner.dictionaryEntries = std::move(snapshotPtr->entries);
                    owner.setButtonsSensitive(true);
                    owner.applyFilter();
                }
            });
        }
        delete snapshotPtr;
    }

    void setButtonsSensitive(bool sensitive) {
        const bool allow = sensitive && !operationInProgress;
        const bool hasCorruptedLines =
            (currentSnapshot.status == DictionaryLoadStatus::CorruptedLines);
        const bool hasIoError =
            (currentSnapshot.status == DictionaryLoadStatus::IoError);

        const bool canMutate = allow && !hasCorruptedLines && !hasIoError;
        const bool canExport = allow && !hasIoError;

        if (addButton != nullptr) gtk_widget_set_sensitive(addButton, canMutate);
        if (importButton != nullptr) gtk_widget_set_sensitive(importButton, canMutate);
        if (exportButton != nullptr) gtk_widget_set_sensitive(exportButton, canExport);
        if (!canMutate) {
            if (editButton != nullptr) gtk_widget_set_sensitive(editButton, FALSE);
            if (deleteButton != nullptr) gtk_widget_set_sensitive(deleteButton, FALSE);
        } else {
            updateActionState();
        }
    }

    void refresh() {
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        ++loadGeneration;
        setButtonsSensitive(false);
        if (dictionaryCount != nullptr) {
            gtk_label_set_text(GTK_LABEL(dictionaryCount), "正在读取用户词典…");
        }
        auto *taskData = new LoadTaskData{lifetime, path, loadGeneration};
        auto *task = g_task_new(nullptr, nullptr, onLoadFinished, nullptr);
        g_task_set_task_data(task, taskData, [](gpointer d) {
            delete static_cast<LoadTaskData *>(d);
        });
        g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer data, GCancellable *) {
            const auto *td = static_cast<const LoadTaskData *>(data);
            auto snapshot = DataController::loadDictionaryForEditing(td->path);
            g_task_return_pointer(
                t,
                new DictionarySnapshot(std::move(snapshot)),
                [](gpointer v) {
                    delete static_cast<DictionarySnapshot *>(v);
                });
        });
        g_object_unref(task);
    }

    struct BatchRenderContext {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::size_t nextIndex = 0;
    };

    static gboolean onBatchIdle(gpointer data) {
        auto *ctx = static_cast<BatchRenderContext *>(data);
        bool keepGoing = false;
        const bool alive = ctx->lifetime->withOwner([ctx, &keepGoing](Impl &owner) {
            if (ctx->generation != owner.batchGeneration) {
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            std::size_t count = 0;
            while (ctx->nextIndex < owner.filteredIndices.size()) {
                const auto sourceIndex = owner.filteredIndices[ctx->nextIndex];
                const auto &entry = owner.dictionaryEntries[sourceIndex];
                auto weight = std::ostringstream{};
                weight << std::setprecision(9) << entry.weight;
                GtkTreeIter iterator;
                gtk_list_store_append(owner.dictionaryStore, &iterator);
                gtk_list_store_set(owner.dictionaryStore, &iterator, 0,
                                   entry.pinyin.c_str(), 1, entry.phrase.c_str(),
                                   2, weight.str().c_str(), 3,
                                   static_cast<guint>(sourceIndex), -1);
                ++ctx->nextIndex;
                ++count;
                if (count >= 200) {
                    break;
                }
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start) >=
                    std::chrono::milliseconds(4)) {
                    break;
                }
            }
            if (ctx->nextIndex < owner.filteredIndices.size()) {
                keepGoing = true;
            } else {
                owner.idleBatchSourceId = 0;
                const auto query = owner.dictionarySearch == nullptr
                                       ? std::string()
                                       : gtk_entry_get_text(GTK_ENTRY(owner.dictionarySearch));
                std::string countText;
                if (owner.currentSnapshot.status == DictionaryLoadStatus::CorruptedLines) {
                    countText = (query.empty()
                        ? "共 " + std::to_string(owner.filteredIndices.size()) + " 条"
                        : "显示 " + std::to_string(owner.filteredIndices.size()) + " 条，共 " +
                              std::to_string(owner.dictionaryEntries.size()) + " 条") +
                        "（存在受损行，已禁用写入）";
                } else if (owner.currentSnapshot.status == DictionaryLoadStatus::IoError) {
                    countText = "读取失败";
                } else {
                    countText =
                        query.empty()
                            ? "共 " + std::to_string(owner.filteredIndices.size()) + " 条"
                            : "显示 " + std::to_string(owner.filteredIndices.size()) + " 条，共 " +
                                  std::to_string(owner.dictionaryEntries.size()) + " 条";
                }
                gtk_label_set_text(GTK_LABEL(owner.dictionaryCount), countText.c_str());
                owner.updateActionState();
            }
        });
        return (alive && keepGoing) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }

    void applyFilter() {
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        gtk_list_store_clear(dictionaryStore);
        const auto query = dictionarySearch == nullptr
                               ? std::string()
                               : gtk_entry_get_text(GTK_ENTRY(dictionarySearch));
        filteredIndices = DataController::filterDictionary(dictionaryEntries, query);

        if (filteredIndices.empty()) {
            if (dictionaryCount != nullptr) {
                std::string countText;
                if (currentSnapshot.status == DictionaryLoadStatus::CorruptedLines) {
                    countText = (query.empty()
                        ? "共 0 条"
                        : "显示 0 条，共 " + std::to_string(dictionaryEntries.size()) + " 条") +
                        "（存在受损行，已禁用写入）";
                } else if (currentSnapshot.status == DictionaryLoadStatus::IoError) {
                    countText = "读取失败";
                } else {
                    countText =
                        query.empty()
                            ? "共 0 条"
                            : "显示 0 条，共 " + std::to_string(dictionaryEntries.size()) + " 条";
                }
                gtk_label_set_text(GTK_LABEL(dictionaryCount), countText.c_str());
            }
            if (dictionaryState != nullptr) {
                std::string state;
                if (currentSnapshot.status == DictionaryLoadStatus::CorruptedLines) {
                    state = "词典包含损坏行（第 " +
                            std::to_string(currentSnapshot.diagnostics.front().lineNumber) + " 行：" +
                            currentSnapshot.diagnostics.front().reason + "），已禁用保存以防静默覆盖损坏数据；可导出备份修复。";
                } else if (currentSnapshot.status == DictionaryLoadStatus::IoError) {
                    state = "无法读取用户词典：" + currentSnapshot.error;
                } else if (dictionaryEntries.empty()) {
                    state = "暂无用户词条；可以添加个人词条或导入专业词典";
                } else {
                    state = "没有匹配的词条；请更换拼音或词条关键词";
                }
                gtk_label_set_text(GTK_LABEL(dictionaryState), state.c_str());
                gtk_widget_set_visible(dictionaryState, TRUE);
            }
            updateActionState();
            return;
        }

        if (dictionaryState != nullptr) {
            if (currentSnapshot.status == DictionaryLoadStatus::CorruptedLines) {
                std::string state = "词典包含损坏行（第 " +
                                    std::to_string(currentSnapshot.diagnostics.front().lineNumber) + " 行：" +
                                    currentSnapshot.diagnostics.front().reason + "），已禁用保存以防静默覆盖损坏数据；可导出备份修复。";
                gtk_label_set_text(GTK_LABEL(dictionaryState), state.c_str());
                gtk_widget_set_visible(dictionaryState, TRUE);
            } else if (currentSnapshot.status == DictionaryLoadStatus::IoError) {
                std::string state = "无法读取用户词典：" + currentSnapshot.error;
                gtk_label_set_text(GTK_LABEL(dictionaryState), state.c_str());
                gtk_widget_set_visible(dictionaryState, TRUE);
            } else {
                gtk_widget_set_visible(dictionaryState, FALSE);
            }
        }

        ++batchGeneration;
        auto *batchCtx = new BatchRenderContext{lifetime, batchGeneration, 0};
        idleBatchSourceId = g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE, onBatchIdle, batchCtx,
            [](gpointer d) { delete static_cast<BatchRenderContext *>(d); });
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
        static_cast<Impl *>(data)->applyFilter();
    }

    static void onAdd(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->operationInProgress) {
            return;
        }
        impl->editEntry(std::nullopt);
    }

    static void onEdit(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->operationInProgress) {
            return;
        }
        const auto selected = impl->selectedIndex();
        if (!selected.has_value()) {
            impl->notifyMessage("请先选择要编辑的词条");
            return;
        }
        impl->editEntry(selected);
    }

    static void onDelete(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->operationInProgress) {
            return;
        }
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
        if (impl->operationInProgress) {
            return;
        }
        auto entries = impl->dictionaryEntries;
        entries.erase(entries.begin() +
                      static_cast<std::ptrdiff_t>(*selected));
        impl->saveEntriesAsync(std::move(entries), "词条已删除；输入法会自动读取（约 2 秒）");
    }

    struct ImportPreviewTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::filesystem::path filename;
        std::vector<pinyin::UserDictionaryEntry> currentEntries;
        DictionaryImportPreview preview;
    };

    static void onImportPreviewFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *taskData = static_cast<ImportPreviewTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (taskData != nullptr) {
            const auto gen = taskData->generation;
            taskData->lifetime->withOwner([taskData, gen](Impl &owner) {
                if (gen != owner.operationGeneration) {
                    return;
                }
                if (!taskData->preview.valid) {
                    owner.operationInProgress = false;
                    owner.setButtonsSensitive(true);
                    owner.updateActionState();
                    owner.notifyMessage(taskData->preview.error.empty()
                                            ? "导入失败：文件格式无效"
                                            : taskData->preview.error);
                    return;
                }
                owner.showImportOptionsDialog(std::move(taskData->preview));
            });
            delete taskData;
        }
    }

    struct ImportSaveTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::filesystem::path path;
        DictionaryExpectedBaseline expectedBaseline;
        bool isReplace = false;
        DictionaryConflictPolicy policy = DictionaryConflictPolicy::KeepExisting;
        std::vector<pinyin::UserDictionaryEntry> parsedEntries;
        std::vector<pinyin::UserDictionaryEntry> currentEntries;
        std::size_t newEntries = 0;
        std::size_t conflictEntries = 0;
        DictionarySaveResult result = DictionarySaveResult::Invalid;
        std::filesystem::path backupPath;
        std::string error;
        std::string successMessage;
    };

    static void onImportSaveFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *taskData = static_cast<ImportSaveTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (taskData != nullptr) {
            const auto gen = taskData->generation;
            taskData->lifetime->withOwner([taskData, gen](Impl &owner) {
                if (gen != owner.operationGeneration) {
                    return;
                }
                owner.operationInProgress = false;
                if (taskData->result == DictionarySaveResult::Saved) {
                    owner.notifyMessage(taskData->successMessage);
                    owner.refresh();
                } else if (taskData->result == DictionarySaveResult::Conflict) {
                    owner.notifyMessage("导入保存失败：词典已被外部程序修改（版本冲突）。未覆盖磁盘内容，请刷新后重试。");
                    owner.setButtonsSensitive(true);
                    owner.updateActionState();
                } else {
                    owner.notifyMessage(taskData->error.empty() ? "导入词典保存失败" : taskData->error);
                    owner.setButtonsSensitive(true);
                    owner.updateActionState();
                }
            });
            delete taskData;
        }
    }

    void showImportOptionsDialog(DictionaryImportPreview preview) {
        auto *dialog = gtk_dialog_new_with_buttons(
            "导入个人词典", parentWindow(), GTK_DIALOG_MODAL,
            "取消", GTK_RESPONSE_CANCEL, "确认导入", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭导入对话框且不修改词典");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT,
                                       "确认导入", "根据选定的规则执行词典导入");

        auto *contentArea = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        auto *mainBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(mainBox), 16);
        gtk_container_add(GTK_CONTAINER(contentArea), mainBox);

        auto *headerLabel = gtk_label_new("词典导入预览统计");
        addStyleClass(headerLabel, "modernime-section-title");
        gtk_widget_set_halign(headerLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(mainBox), headerLabel, FALSE, FALSE, 0);

        auto *statsCard = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        addStyleClass(statsCard, "modernime-card");
        gtk_container_set_border_width(GTK_CONTAINER(statsCard), 8);

        std::ostringstream s1, s2, s3, s4;
        s1 << "• 文件包含有效词条：" << preview.totalParsed << " 条";
        s2 << "• 新增词条（合并后追加）：" << preview.newEntries << " 条";
        s3 << "• 完全一致词条（拼音、词条与权重相同）：" << preview.duplicateEntries << " 条";
        s4 << "• 权重冲突词条（拼音与词条相同但权重不同）：" << preview.conflictEntries << " 条";

        for (const auto &text : {s1.str(), s2.str(), s3.str(), s4.str()}) {
            auto *label = gtk_label_new(text.c_str());
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(statsCard), label, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(mainBox), statsCard, FALSE, FALSE, 0);

        auto *modeLabel = gtk_label_new("请选择导入方式：");
        gtk_widget_set_halign(modeLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(mainBox), modeLabel, FALSE, FALSE, 0);

        auto *mergeRadio = gtk_radio_button_new_with_label(
            nullptr, "合并导入（推荐：保留当前词典，追加新词条）");
        auto *replaceRadio = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(mergeRadio),
            "整体替换（清空现有词典，自动创建安全备份后写入导入词条）");
        setAccessibleWidgetText(mergeRadio, "合并导入", "保留当前词典，追加新词条");
        setAccessibleWidgetText(replaceRadio, "整体替换", "清空现有词典，自动创建安全备份后写入导入词条");

        gtk_box_pack_start(GTK_BOX(mainBox), mergeRadio, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(mainBox), replaceRadio, FALSE, FALSE, 0);

        GtkWidget *conflictBox = nullptr;
        GtkWidget *keepConflictRadio = nullptr;
        GtkWidget *overwriteConflictRadio = nullptr;
        if (preview.conflictEntries > 0) {
            conflictBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_widget_set_margin_start(conflictBox, 20);
            auto *conflictTitle = gtk_label_new("检测到权重冲突，合并时的冲突处理规则：");
            gtk_widget_set_halign(conflictTitle, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(conflictBox), conflictTitle, FALSE, FALSE, 0);

            keepConflictRadio = gtk_radio_button_new_with_label(
                nullptr, "保留现有词条权重（忽略导入文件中的新权重）");
            overwriteConflictRadio = gtk_radio_button_new_with_label_from_widget(
                GTK_RADIO_BUTTON(keepConflictRadio),
                "使用导入权重覆盖现有词条权重");
            setAccessibleWidgetText(keepConflictRadio, "保留现有词条权重", "合并时保留现有词条权重");
            setAccessibleWidgetText(overwriteConflictRadio, "使用导入权重覆盖", "合并时使用导入文件权重覆盖现有词条");

            gtk_box_pack_start(GTK_BOX(conflictBox), keepConflictRadio, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(conflictBox), overwriteConflictRadio, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(mainBox), conflictBox, FALSE, FALSE, 0);

            g_signal_connect_swapped(
                mergeRadio, "toggled",
                G_CALLBACK(+[](GtkWidget *cBox, GtkToggleButton *mRadio) {
                    gtk_widget_set_sensitive(cBox, gtk_toggle_button_get_active(mRadio));
                }),
                conflictBox);
        }

        gtk_widget_show_all(dialog);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response != GTK_RESPONSE_ACCEPT) {
            gtk_widget_destroy(dialog);
            operationInProgress = false;
            setButtonsSensitive(true);
            updateActionState();
            notifyMessage("已取消导入，现有词典未修改");
            return;
        }

        const bool isReplace = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(replaceRadio));
        DictionaryConflictPolicy policy = DictionaryConflictPolicy::KeepExisting;
        if (overwriteConflictRadio != nullptr &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(overwriteConflictRadio))) {
            policy = DictionaryConflictPolicy::OverwriteWithImport;
        }
        gtk_widget_destroy(dialog);

        operationInProgress = true;
        ++operationGeneration;
        setButtonsSensitive(false);

        auto *saveTaskData = new ImportSaveTaskData{
            lifetime, operationGeneration, path, currentBaseline, isReplace, policy,
            std::move(preview.parsedEntries), dictionaryEntries,
            preview.newEntries, preview.conflictEntries,
            DictionarySaveResult::Invalid, {}, {}, {}
        };

        auto *saveTask = g_task_new(nullptr, nullptr, onImportSaveFinished, nullptr);
        g_task_set_task_data(saveTask, saveTaskData, nullptr);
        g_task_run_in_thread(saveTask, [](GTask *t, gpointer, gpointer task_data, GCancellable *) {
            auto *td = static_cast<ImportSaveTaskData *>(task_data);
            if (td->isReplace) {
                std::filesystem::path backupPath;
                std::string backupError;
                if (!DataController::createDictionaryBackup(td->path, &backupPath, &backupError)) {
                    td->error = "整体替换已中止：无法创建词典备份（" + backupError + "）";
                    td->result = DictionarySaveResult::IoError;
                } else {
                    td->backupPath = backupPath;
                    td->result = DataController::saveDictionaryIfUnchanged(
                        td->path, td->parsedEntries, td->expectedBaseline, &td->error);
                    if (td->result == DictionarySaveResult::Saved) {
                        td->successMessage = "已整体替换个人词典（原词典已自动备份至 " +
                                            backupPath.filename().string() + "）";
                    }
                }
            } else {
                const auto merged = DataController::mergeDictionary(
                    td->currentEntries, td->parsedEntries, td->policy);
                td->result = DataController::saveDictionaryIfUnchanged(
                    td->path, merged, td->expectedBaseline, &td->error);
                if (td->result == DictionarySaveResult::Saved) {
                    std::ostringstream msg;
                    msg << "已成功合并导入 " << td->newEntries << " 条新词条";
                    if (td->conflictEntries > 0) {
                        msg << "（" << (td->policy == DictionaryConflictPolicy::OverwriteWithImport
                                            ? "已覆盖 "
                                            : "已保留现有 ")
                            << td->conflictEntries << " 条冲突词条）";
                    }
                    td->successMessage = msg.str();
                }
            }
            g_task_return_pointer(t, td, nullptr);
        });
        g_object_unref(saveTask);
    }

    static void onImport(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->operationInProgress) {
            return;
        }
        auto *fileChooser = gtk_file_chooser_dialog_new(
            "导入用户词典", impl->parentWindow(), GTK_FILE_CHOOSER_ACTION_OPEN,
            "取消", GTK_RESPONSE_CANCEL, "下一步", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(fileChooser), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭文件选择器且不导入词典");
        setDialogResponseAccessibility(
            GTK_DIALOG(fileChooser), GTK_RESPONSE_ACCEPT, "下一步",
            "读取选中的文本文件并生成合并预览");
        gchar *filename = nullptr;
        if (gtk_dialog_run(GTK_DIALOG(fileChooser)) == GTK_RESPONSE_ACCEPT) {
            filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fileChooser));
        }
        gtk_widget_destroy(fileChooser);
        if (filename == nullptr) {
            return;
        }

        impl->operationInProgress = true;
        ++impl->operationGeneration;
        impl->setButtonsSensitive(false);

        auto *previewTaskData = new ImportPreviewTaskData{
            impl->lifetime, impl->operationGeneration, std::filesystem::path(filename),
            impl->dictionaryEntries, {}
        };
        g_free(filename);

        auto *task = g_task_new(nullptr, nullptr, onImportPreviewFinished, nullptr);
        g_task_set_task_data(task, previewTaskData, nullptr);
        g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer task_data, GCancellable *) {
            auto *td = static_cast<ImportPreviewTaskData *>(task_data);
            td->preview = DataController::previewDictionaryImport(td->filename, td->currentEntries);
            g_task_return_pointer(t, td, nullptr);
        });
        g_object_unref(task);
    }

    struct ExportTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::filesystem::path filename;
        std::vector<pinyin::UserDictionaryEntry> entries;
        bool success = false;
        std::string error;
    };

    static void onExportFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *taskData = static_cast<ExportTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (taskData != nullptr) {
            const auto gen = taskData->generation;
            taskData->lifetime->withOwner([taskData, gen](Impl &owner) {
                if (gen != owner.operationGeneration) {
                    return;
                }
                owner.operationInProgress = false;
                owner.setButtonsSensitive(true);
                owner.updateActionState();
                if (taskData->success) {
                    owner.notifyMessage("用户词典已导出");
                } else {
                    owner.notifyMessage(taskData->error.empty() ? "导出用户词典失败" : taskData->error);
                }
            });
            delete taskData;
        }
    }

    static void onExport(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->operationInProgress) {
            return;
        }
        auto *dialog = gtk_file_chooser_dialog_new(
            "导出用户词典", impl->parentWindow(), GTK_FILE_CHOOSER_ACTION_SAVE,
            "取消", GTK_RESPONSE_CANCEL, "导出", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭文件选择器且不导出词典");
        setDialogResponseAccessibility(
            GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "导出",
            "将当前个人词典写入选择的文件");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
        gchar *filename = nullptr;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        }
        gtk_widget_destroy(dialog);
        if (filename == nullptr) {
            return;
        }

        impl->operationInProgress = true;
        ++impl->operationGeneration;
        impl->setButtonsSensitive(false);

        auto *taskData = new ExportTaskData{
            impl->lifetime, impl->operationGeneration, std::filesystem::path(filename),
            impl->dictionaryEntries, false, {}
        };
        g_free(filename);

        auto *task = g_task_new(nullptr, nullptr, onExportFinished, nullptr);
        g_task_set_task_data(task, taskData, nullptr);
        g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer task_data, GCancellable *) {
            auto *td = static_cast<ExportTaskData *>(task_data);
            td->success = DataController::saveDictionary(td->filename, td->entries, &td->error);
            g_task_return_pointer(t, td, nullptr);
        });
        g_object_unref(task);
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
        const bool hasSelected = selectedIndex().has_value() && !operationInProgress;
        if (editButton != nullptr) {
            gtk_widget_set_sensitive(editButton, hasSelected);
        }
        if (deleteButton != nullptr) {
            gtk_widget_set_sensitive(deleteButton, hasSelected);
        }
    }

    struct DraftEntry {
        std::string pinyin;
        std::string phrase;
        double weight = 0.0;
        std::optional<std::string> originalPhrase;
        std::optional<std::string> originalPinyin;
        std::optional<double> originalWeight;
        DictionaryExpectedBaseline baseBaseline;
    };

    static std::string makeDraftKey(std::string_view phrase, std::string_view pinyin) {
        std::string key;
        key.reserve(phrase.size() + pinyin.size() + 1);
        key.append(phrase);
        key.push_back('\t');
        key.append(pinyin);
        return key;
    }

    struct SaveTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::filesystem::path path;
        DictionaryExpectedBaseline expectedBaseline;
        std::vector<pinyin::UserDictionaryEntry> entries;
        std::string successMessage;
        DictionarySaveResult result = DictionarySaveResult::Invalid;
        std::string error;
        std::optional<DraftEntry> draft;
    };

    static void onSaveFinished(GObject *, GAsyncResult *result, gpointer) {
        GError *err = nullptr;
        auto *taskData = static_cast<SaveTaskData *>(
            g_task_propagate_pointer(G_TASK(result), &err));
        g_clear_error(&err);
        if (taskData != nullptr) {
            const auto gen = taskData->generation;
            taskData->lifetime->withOwner([taskData, gen](Impl &owner) {
                if (gen != owner.operationGeneration) {
                    return;
                }
                owner.operationInProgress = false;
                if (taskData->result == DictionarySaveResult::Saved) {
                    if (taskData->draft.has_value()) {
                        if (taskData->draft->originalPhrase.has_value() &&
                            taskData->draft->originalPinyin.has_value()) {
                            owner.conflictDrafts.erase(makeDraftKey(
                                *taskData->draft->originalPhrase, *taskData->draft->originalPinyin));
                        } else {
                            owner.newEntryConflictDraft.reset();
                        }
                    }
                    owner.notifyMessage(taskData->successMessage);
                    owner.refresh();
                } else if (taskData->result == DictionarySaveResult::Conflict) {
                    if (taskData->draft.has_value()) {
                        if (taskData->draft->originalPhrase.has_value() &&
                            taskData->draft->originalPinyin.has_value()) {
                            owner.conflictDrafts[makeDraftKey(
                                *taskData->draft->originalPhrase, *taskData->draft->originalPinyin)] =
                                *taskData->draft;
                        } else {
                            owner.newEntryConflictDraft = taskData->draft;
                        }
                        owner.notifyMessage("保存失败：词典已被外部程序修改（版本冲突）。已保留您的编辑草稿，请刷新后重试。");
                    } else {
                        owner.notifyMessage("保存失败：词典已被外部程序修改（版本冲突）。未覆盖磁盘内容，请刷新后重试。");
                    }
                    owner.setButtonsSensitive(true);
                    owner.updateActionState();
                } else {
                    owner.notifyMessage(taskData->error.empty() ? "保存个人词典失败" : taskData->error);
                    owner.setButtonsSensitive(true);
                    owner.updateActionState();
                }
            });
            delete taskData;
        }
    }

    void saveEntriesAsync(std::vector<pinyin::UserDictionaryEntry> entries,
                          std::string successMessage,
                          std::optional<DraftEntry> draft = std::nullopt) {
        if (operationInProgress) {
            return;
        }
        operationInProgress = true;
        ++operationGeneration;
        setButtonsSensitive(false);

        auto *taskData = new SaveTaskData{
            lifetime, operationGeneration, path, currentBaseline, std::move(entries),
            std::move(successMessage), DictionarySaveResult::Invalid, {}, std::move(draft)
        };
        auto *task = g_task_new(nullptr, nullptr, onSaveFinished, nullptr);
        g_task_set_task_data(task, taskData, nullptr);
        g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer task_data, GCancellable *) {
            auto *td = static_cast<SaveTaskData *>(task_data);
            td->result = DataController::saveDictionaryIfUnchanged(
                td->path, td->entries, td->expectedBaseline, &td->error);
            g_task_return_pointer(t, td, nullptr);
        });
        g_object_unref(task);
    }

    bool editEntry(std::optional<std::size_t> selected) {
        if (operationInProgress) {
            return false;
        }
        auto *dialog = gtk_dialog_new_with_buttons(
            selected.has_value() ? "编辑用户词条" : "添加用户词条",
            parentWindow(), GTK_DIALOG_MODAL, "取消", GTK_RESPONSE_CANCEL,
            "保存", GTK_RESPONSE_ACCEPT, nullptr);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL,
                                       "取消", "关闭对话框且不保存词条修改");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT,
                                       "保存", "校验并保存当前词条");
        addStyleClass(dialog, kSettingsWindowClass);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 460, -1);
        auto *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        auto *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(body), 20);
        gtk_container_add(GTK_CONTAINER(content), body);
        auto *intro = gtk_label_new("保存后，输入法会自动读取这个词条。");
        addStyleClass(intro, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(intro), 0);
        gtk_box_pack_start(GTK_BOX(body), intro, FALSE, FALSE, 0);
        auto *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
        gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);

        auto *phraseLabel = gtk_label_new("词条");
        auto *pinyinLabel = gtk_label_new("拼音");
        gtk_widget_set_halign(phraseLabel, GTK_ALIGN_START);
        gtk_widget_set_halign(pinyinLabel, GTK_ALIGN_START);
        auto *phrase = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(phrase), "例如：王小明、项目名称");
        gtk_widget_set_hexpand(phrase, TRUE);
        auto *readings = gtk_combo_box_text_new_with_entry();
        auto *pinyin = gtk_bin_get_child(GTK_BIN(readings));
        gtk_entry_set_placeholder_text(GTK_ENTRY(pinyin), "自动建议，可直接修改读音");
        gtk_widget_set_hexpand(readings, TRUE);
        auto *automatic = gtk_button_new_with_label("自动填写");
        setTarget(automatic, "dictionary-suggest-pinyin");
        setAccessibleWidgetText(automatic, "自动填写", "重新根据词条建议拼音");
        setAccessibleWidgetText(pinyin, "拼音", "自动建议的读音，可修改人名和多音字读音");
        setAccessibleWidgetText(phrase, "词条", "输入要保存的中文词条");
        gtk_grid_attach(GTK_GRID(grid), phraseLabel, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), phrase, 1, 0, 2, 1);
        gtk_grid_attach(GTK_GRID(grid), pinyinLabel, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), readings, 1, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), automatic, 2, 1, 1, 1);
        auto *validation = gtk_label_new("");
        addStyleClass(validation, "modernime-description");
        gtk_label_set_xalign(GTK_LABEL(validation), 0);
        gtk_label_set_line_wrap(GTK_LABEL(validation), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(validation), 42);
        gtk_grid_attach(GTK_GRID(grid), validation, 0, 2, 3, 1);

        auto *advanced = gtk_expander_new("高级设置");
        setAccessibleWidgetText(advanced, "高级设置", "可选的词条排序加成，通常无需修改");
        auto *advancedBody = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_top(advancedBody, 10);
        gtk_container_add(GTK_CONTAINER(advanced), advancedBody);
        auto *weightRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto *weightLabel = gtk_label_new("排序加成");
        auto *weight = gtk_spin_button_new_with_range(0.0, 1000000000.0, 1.0);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(weight), 3);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(weight), TRUE);
        setAccessibleWidgetText(weight, "权重", "可选排序加成，默认 0，通常无需修改");
        gtk_box_pack_start(GTK_BOX(weightRow), weightLabel, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(weightRow), weight, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(advancedBody), weightRow, FALSE, FALSE, 0);
        auto *weightHelp = gtk_label_new("默认即可；输入法会根据你的使用记录调整排序。");
        gtk_label_set_xalign(GTK_LABEL(weightHelp), 0);
        gtk_label_set_line_wrap(GTK_LABEL(weightHelp), TRUE);
        addStyleClass(weightHelp, "modernime-description");
        gtk_box_pack_start(GTK_BOX(advancedBody), weightHelp, FALSE, FALSE, 0);
        gtk_widget_show_all(advancedBody);
        gtk_widget_hide(advancedBody);
        gtk_widget_set_no_show_all(advancedBody, TRUE);
        g_signal_connect(advanced, "notify::expanded", G_CALLBACK(+[](
            GtkExpander *expander, GParamSpec *, gpointer child) {
                gtk_widget_set_visible(GTK_WIDGET(child), gtk_expander_get_expanded(expander));
            }), advancedBody);
        gtk_box_pack_start(GTK_BOX(body), advanced, FALSE, FALSE, 0);

        auto state = std::make_shared<DictionaryDialogState>();
        state->pinyin = pinyin;
        state->phrase = phrase;
        state->weight = weight;
        state->validation = validation;
        state->readings = readings;
        state->automatic = automatic;
        state->existing = dictionaryEntries;
        state->autoPinyin = !selected.has_value();
        state->accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        addStyleClass(state->accept, kSettingsPrimaryButtonClass);
        setAccessibleWidgetText(state->accept, "保存", "校验通过后保存当前词条");
        auto *cancel = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        if (selected.has_value() && *selected < dictionaryEntries.size()) {
            const auto &entry = dictionaryEntries[*selected];
            auto it = conflictDrafts.find(makeDraftKey(entry.phrase, entry.pinyin));
            if (it != conflictDrafts.end()) {
                gtk_entry_set_text(GTK_ENTRY(pinyin), it->second.pinyin.c_str());
                gtk_entry_set_text(GTK_ENTRY(phrase), it->second.phrase.c_str());
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), it->second.weight);
            } else {
                gtk_entry_set_text(GTK_ENTRY(pinyin), entry.pinyin.c_str());
                gtk_entry_set_text(GTK_ENTRY(phrase), entry.phrase.c_str());
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), entry.weight);
            }
        } else if (!selected.has_value() && newEntryConflictDraft.has_value()) {
            gtk_entry_set_text(GTK_ENTRY(pinyin), newEntryConflictDraft->pinyin.c_str());
            gtk_entry_set_text(GTK_ENTRY(phrase), newEntryConflictDraft->phrase.c_str());
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(weight), newEntryConflictDraft->weight);
        }
        setSettingsFocusChain(dialog, {phrase, pinyin, automatic, advanced, weight, cancel, state->accept});
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        gtk_entry_set_activates_default(GTK_ENTRY(pinyin), TRUE);
        gtk_entry_set_activates_default(GTK_ENTRY(phrase), TRUE);
        g_signal_connect(pinyin, "changed", G_CALLBACK(onDictionaryReadingChanged), state.get());
        g_signal_connect(phrase, "changed", G_CALLBACK(onDictionaryPhraseChanged), state.get());
        g_signal_connect(automatic, "clicked", G_CALLBACK(onDictionarySuggest), state.get());
        g_signal_connect(weight, "value-changed", G_CALLBACK(onDictionaryDialogWeightChanged), state.get());
        g_signal_connect(dialog, "destroy", G_CALLBACK(onDictionaryDialogDestroyed), state.get());
        updateDictionaryDialogValidity(state.get());
        gtk_widget_show_all(dialog);
        gtk_widget_grab_focus(phrase);
        startDictionaryPronunciation(state);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response != GTK_RESPONSE_ACCEPT) {
            gtk_widget_destroy(dialog);
            return false;
        }

        const std::string pinyinText = gtk_entry_get_text(GTK_ENTRY(pinyin));
        const std::string phraseText = gtk_entry_get_text(GTK_ENTRY(phrase));
        const auto weightValue = static_cast<float>(
            gtk_spin_button_get_value(GTK_SPIN_BUTTON(weight)));
        gtk_widget_destroy(dialog);

        pinyin::UserDictionary dictionary;
        for (std::size_t index = 0; index < dictionaryEntries.size(); ++index) {
            if (!selected.has_value() || index != *selected) {
                const auto &entry = dictionaryEntries[index];
                if (!dictionary.upsert(entry.pinyin, entry.phrase,
                                       entry.weight)) {
                    notifyMessage("现有用户词典包含非法词条");
                    return false;
                }
            }
        }
        const bool valid =
            dictionary.upsert(pinyinText, phraseText, weightValue);
        if (!valid) {
            notifyMessage("词条未保存：拼音、词条或权重格式不合法");
            return false;
        }
        DraftEntry draft;
        draft.pinyin = pinyinText;
        draft.phrase = phraseText;
        draft.weight = weightValue;
        draft.baseBaseline = currentBaseline;
        if (selected.has_value() && *selected < dictionaryEntries.size()) {
            draft.originalPhrase = dictionaryEntries[*selected].phrase;
            draft.originalPinyin = dictionaryEntries[*selected].pinyin;
            draft.originalWeight = dictionaryEntries[*selected].weight;
        }
        saveEntriesAsync(dictionary.entries(), "词条已保存；输入法会自动读取（约 2 秒）", std::move(draft));
        return true;
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
    GtkWidget *exportButton = nullptr;
    std::vector<pinyin::UserDictionaryEntry> dictionaryEntries;
    std::vector<std::size_t> filteredIndices;
    DictionarySnapshot currentSnapshot;
    DictionaryExpectedBaseline currentBaseline = DictionaryExpectedBaseline::unchecked();
    std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime =
        std::make_shared<detail::DiagnosticsLifetime<Impl>>(this);
    uint64_t loadGeneration = 0;
    uint64_t batchGeneration = 0;
    uint64_t operationGeneration = 0;
    bool operationInProgress = false;
    guint idleBatchSourceId = 0;
    std::unordered_map<std::string, DraftEntry> conflictDrafts;
    std::optional<DraftEntry> newEntryConflictDraft;
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
