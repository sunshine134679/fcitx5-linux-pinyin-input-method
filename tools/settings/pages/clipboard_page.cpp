#include "modernime/settings/pages/clipboard_page.h"

#include "modernime/settings/clipboard_history_model.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_ui_contract.h"
#include "modernime/settings/settings_widgets.h"

#include "modernime/core/clipboard_history.h"
#include "modernime/core/clipboard_queries.h"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <memory>
#include <chrono>
#include <optional>
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
} // namespace

class ClipboardPage::Impl final {
public:
    using Entries = std::shared_ptr<const std::vector<std::string>>;

    struct DisplayItem {
        std::string_view text;
        ClipboardItemMeta meta;
        bool favorite = false;
        std::optional<std::size_t> historyIndex;
    };

    struct ReloadTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path historyPath;
        std::filesystem::path favoritesPath;
        uint64_t generation = 0;
        bool shouldNotify = false;
        Entries history;
        Entries favorites;
        bool historySuccess = false;
        bool favoritesSuccess = false;
        std::string historyError;
        std::string favoritesError;
    };

    struct SearchTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        Entries history;
        Entries favorites;
        std::string query;
        bool favoritesOnly = false;
        bool shouldNotify = false;
        std::vector<DisplayItem> items;
    };

    struct BatchRenderContext {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        uint64_t generation = 0;
        std::size_t nextIndex = 0;
        bool shouldNotify = false;
    };

    enum class Operation { DeleteHistory, ClearHistory, AddFavorite, RemoveFavorite, ClearFavorites };
    struct OperationTaskData {
        std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
        std::filesystem::path path;
        uint64_t generation = 0;
        Operation operation = Operation::DeleteHistory;
        std::size_t historyIndex = 0;
        std::string expectedText;
        bool success = false;
        std::string error;
    };

    Impl(SettingsWindowModel &settingsModel, std::filesystem::path historyPathArg,
         std::function<void()> settingsChangedCallback,
         std::function<void(std::string)> notifyCallback,
         std::filesystem::path favoritesPathArg)
        : model(settingsModel), historyPath(std::move(historyPathArg)),
          favoritesPath(favoritesPathArg.empty()
              ? historyPath.parent_path() / "clipboard-favorites.bin"
              : std::move(favoritesPathArg)),
          settingsChanged(std::move(settingsChangedCallback)),
          notify(std::move(notifyCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "剪贴板", "搜索最近复制的内容，收藏常用文字。历史与收藏仅保存在本机。"));
        page = pageGuard.get();
        auto *destroyLifetime = new std::shared_ptr<detail::DiagnosticsLifetime<Impl>>(lifetime);
        g_signal_connect_data(page, "destroy",
            G_CALLBACK(+[](GtkWidget *, gpointer data) {
                const auto &life = *static_cast<std::shared_ptr<detail::DiagnosticsLifetime<Impl>> *>(data);
                life->withOwner([](Impl &owner) {
                    owner.widgetsAlive = false;
                    owner.stopAsyncDelivery();
                });
            }), destroyLifetime,
            +[](gpointer data, GClosure *) {
                delete static_cast<std::shared_ptr<detail::DiagnosticsLifetime<Impl>> *>(data);
            }, GConnectFlags(0));
        buildPage();
        refreshSettings();
        pageGuard.release();
    }

    ~Impl() {
        stopAsyncDelivery();
        if (widgetsAlive) {
            for (auto *control : {clipboardEnabled, clipboardTrigger, historyListBox,
                                  searchEntry, favoritesToggle, refreshButton, copyButton,
                                  deleteButton, clearButton, favoriteButton, clearFavoritesButton}) {
                g_signal_handlers_disconnect_by_data(control, this);
            }
        }
    }

    void stopAsyncDelivery() {
        if (searchSourceId != 0) {
            g_source_remove(searchSourceId);
            searchSourceId = 0;
        }
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        lifetime->deactivate();
    }

    void buildPage() {
        auto *settingsDisclosure = createSectionCard("", "");
        addStyleClass(settingsDisclosure, "modernime-compact-disclosure");
        gtk_box_pack_start(GTK_BOX(page), settingsDisclosure, FALSE, FALSE, 0);
        settingsToggle = gtk_toggle_button_new();
        addStyleClass(settingsToggle, "modernime-details-button");
        auto *settingsHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto *settingsTitle = gtk_label_new("剪贴板设置");
        gtk_label_set_xalign(GTK_LABEL(settingsTitle), 0.0F);
        auto *settingsArrow = gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        gtk_box_pack_start(GTK_BOX(settingsHeader), settingsTitle, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(settingsHeader), settingsArrow, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(settingsToggle), settingsHeader);
        setAccessibleWidgetText(settingsToggle, "剪贴板设置",
                                "展开或收起剪贴板开关与触发键设置");
        gtk_box_pack_start(GTK_BOX(settingsDisclosure), settingsToggle, FALSE, FALSE, 0);
        settingsRevealer = gtk_revealer_new();
        auto *settingsSection = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        addStyleClass(settingsSection, "modernime-details-content");
        gtk_container_add(GTK_CONTAINER(settingsRevealer), settingsSection);
        gtk_box_pack_start(GTK_BOX(settingsDisclosure), settingsRevealer, FALSE, FALSE, 0);
        bindSettingsDisclosure(settingsToggle, settingsRevealer, settingsArrow);

        clipboardEnabled = gtk_switch_new();
        setAccessibleWidgetText(clipboardEnabled, "启用剪贴板",
                                "启用或关闭本地剪贴板历史入口");
        setTarget(clipboardEnabled, "clipboard-enabled");
        gtk_box_pack_start(GTK_BOX(settingsSection),
                           createSettingRow("启用剪贴板",
                                            "在中文输入时快速取用最近复制的内容。",
                                            clipboardEnabled),
                           FALSE, FALSE, 0);

        clipboardTrigger = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(clipboardTrigger), 3);
        gtk_entry_set_width_chars(GTK_ENTRY(clipboardTrigger), 10);
        gtk_entry_set_max_width_chars(GTK_ENTRY(clipboardTrigger), 10);
        gtk_entry_set_placeholder_text(GTK_ENTRY(clipboardTrigger), "例如 V+2");
        setTarget(clipboardTrigger, "clipboard-trigger");
        clipboardTriggerFallback = createSettingRow(
            "触发键",
            "先输入字母，再紧跟数字。例如 V+2。",
            clipboardTrigger);
        setAccessibleWidgetText(clipboardTrigger, "剪贴板触发键",
            "设置在中文输入状态打开本地剪贴板历史的按键");
        setAccessibleWidgetText(
            clipboardTriggerFallback, "剪贴板触发键",
            "剪贴板功能关闭时仍可定位并阅读触发键设置");
        gtk_box_pack_start(GTK_BOX(settingsSection), clipboardTriggerFallback,
                           FALSE, FALSE, 0);
        auto *historySection = createSectionCard(
            "历史与收藏", "收藏优先显示，可双击复制或展开长文本。这里的操作即时保存。");
        gtk_box_pack_start(GTK_BOX(page), historySection, TRUE, TRUE, 0);
        auto *historySummary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        historyCount = gtk_label_new("正在读取…");
        addStyleClass(historyCount, "modernime-description");
        gtk_widget_set_halign(historyCount, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(historySummary), historyCount, TRUE, TRUE, 0);
        refreshButton = gtk_button_new_with_label("刷新历史");
        addStyleClass(refreshButton, "modernime-card-btn");
        gtk_widget_set_tooltip_text(refreshButton, "重新读取磁盘上的剪贴板历史与收藏");
        setAccessibleWidgetText(refreshButton, "刷新历史",
                                "重新读取磁盘上的剪贴板历史与收藏");
        gtk_box_pack_end(GTK_BOX(historySummary), refreshButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(historySection), historySummary, FALSE, FALSE, 0);

        auto *searchBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        searchEntry = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), "搜索历史与收藏");
        gtk_entry_set_max_length(GTK_ENTRY(searchEntry), 128);
        gtk_widget_set_hexpand(searchEntry, TRUE);
        setTarget(searchEntry, "clipboard-search");
        setAccessibleWidgetText(searchEntry, "搜索剪贴板", "按文字内容搜索，英文不区分大小写");
        favoritesToggle = gtk_check_button_new_with_label("仅收藏");
        setTarget(favoritesToggle, "clipboard-favorites");
        setAccessibleWidgetText(favoritesToggle, "仅收藏", "只显示已收藏的剪贴板内容");
        gtk_box_pack_start(GTK_BOX(searchBar), searchEntry, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(searchBar), favoritesToggle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(historySection), searchBar, FALSE, FALSE, 0);
        g_signal_connect(searchEntry, "search-changed", G_CALLBACK(onSearchChanged), this);
        g_signal_connect(favoritesToggle, "toggled", G_CALLBACK(onSearchChanged), this);

        historyListBox = gtk_list_box_new();
        addStyleClass(historyListBox, "modernime-clipboard-list");
        setTarget(historyListBox, "clipboard-history");
        setAccessibleWidgetText(
            historyListBox, "剪贴板历史",
            "查看、展开、复制、收藏或删除本地剪贴板记录");
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(historyListBox),
                                        GTK_SELECTION_SINGLE);
        g_signal_connect(historyListBox, "row-selected",
                         G_CALLBACK(onRowSelected), this);
        g_signal_connect(historyListBox, "row-activated",
                         G_CALLBACK(onRowActivated), this);

        auto *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        historyScrolled = scrolled;
        gtk_widget_set_vexpand(historyScrolled, TRUE);
        gtk_widget_set_hexpand(historyScrolled, TRUE);
        gtk_widget_set_size_request(historyScrolled, -1, 160);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(historyScrolled),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(historyScrolled), historyListBox);
        gtk_box_pack_start(GTK_BOX(historySection), historyScrolled, TRUE, TRUE,
                           0);

        historyState = gtk_label_new("正在读取…");
        addStyleClass(historyState, "modernime-description");
        gtk_widget_set_halign(historyState, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(historyState), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(historyState), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(historyState), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_margin_top(historyState, 6);
        gtk_box_pack_start(GTK_BOX(historySection), historyState, FALSE, FALSE,
                           0);

        errorDetails = gtk_expander_new("查看错误详情");
        setTarget(errorDetails, "clipboard-error-details");
        errorDetailsLabel = gtk_label_new("");
        gtk_label_set_selectable(GTK_LABEL(errorDetailsLabel), TRUE);
        gtk_label_set_xalign(GTK_LABEL(errorDetailsLabel), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(errorDetailsLabel), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(errorDetailsLabel), PANGO_WRAP_WORD_CHAR);
        gtk_container_add(GTK_CONTAINER(errorDetails), errorDetailsLabel);
        gtk_widget_set_no_show_all(errorDetails, TRUE);
        gtk_box_pack_start(GTK_BOX(historySection), errorDetails, FALSE, FALSE, 0);

        auto *historyActions = gtk_flow_box_new();
        addStyleClass(historyActions, "modernime-clipboard-toolbar");
        addStyleClass(historyActions, "modernime-action-flow");
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(historyActions), GTK_SELECTION_NONE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(historyActions), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(historyActions), 2);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(historyActions), 12);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(historyActions), 8);
        auto *selectionActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto *clearActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(selectionActions, GTK_ALIGN_START);
        gtk_widget_set_halign(clearActions, GTK_ALIGN_END);
        for (auto *group : {selectionActions, clearActions}) {
            gtk_flow_box_insert(GTK_FLOW_BOX(historyActions), group, -1);
            gtk_widget_set_can_focus(gtk_widget_get_parent(group), FALSE);
        }
        const auto actionLabels = settingsClipboardActionLabels();
        copyButton = gtk_button_new_with_label(actionLabels[0].data());
        deleteButton = gtk_button_new_with_label(actionLabels[1].data());
        clearButton = gtk_button_new_with_label(actionLabels[2].data());
        setTarget(copyButton, "clipboard-copy");
        setTarget(deleteButton, "clipboard-delete");
        setTarget(clearButton, "clipboard-clear-history");
        favoriteButton = gtk_button_new_with_label("收藏");
        setTarget(favoriteButton, "clipboard-favorite");
        clearFavoritesButton = gtk_button_new_with_label("清空收藏");
        setTarget(clearFavoritesButton, "clipboard-clear-favorites");
        addStyleClass(clearFavoritesButton, "destructive-action");
        setAccessibleWidgetText(favoriteButton, "收藏", "收藏选中的完整内容，即时保存");
        setAccessibleWidgetText(clearFavoritesButton, "清空收藏", "单独清空收藏，保留剪贴板历史");
        addStyleClass(clearButton, "destructive-action");
        gtk_widget_set_tooltip_text(copyButton, "复制选中的完整内容到系统剪贴板");
        gtk_widget_set_tooltip_text(deleteButton, "删除选中的历史内容");
        gtk_widget_set_tooltip_text(clearButton, "清空全部历史内容，保留收藏，需要确认");
        setAccessibleWidgetText(copyButton, actionLabels[0],
                                "复制选中的完整内容到系统剪贴板");
        setAccessibleWidgetText(deleteButton, actionLabels[1],
                                "删除选中的历史内容");
        setAccessibleWidgetText(clearButton, actionLabels[2],
                                "确认后清空全部本地剪贴板历史，保留收藏");
        for (auto *button : {copyButton, deleteButton, favoriteButton}) {
            gtk_box_pack_start(GTK_BOX(selectionActions), button, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(clearActions), clearButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(clearActions), clearFavoritesButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(historySection), historyActions, FALSE, FALSE,
                           0);
        g_signal_connect(refreshButton, "clicked", G_CALLBACK(onRefresh), this);
        g_signal_connect(copyButton, "clicked", G_CALLBACK(onCopy), this);
        g_signal_connect(deleteButton, "clicked", G_CALLBACK(onDelete), this);
        g_signal_connect(clearButton, "clicked", G_CALLBACK(onClear), this);
        g_signal_connect(favoriteButton, "clicked", G_CALLBACK(onFavorite), this);
        g_signal_connect(clearFavoritesButton, "clicked", G_CALLBACK(onClearFavorites), this);
        g_signal_connect(clipboardEnabled, "notify::active", G_CALLBACK(onSwitchChanged),
                         this);
        g_signal_connect(clipboardTrigger, "changed", G_CALLBACK(onChanged),
                         this);
        setSettingsFocusChain(
            page, {settingsToggle, clipboardEnabled, clipboardTrigger, searchEntry, favoritesToggle,
                   historyListBox, copyButton, deleteButton, favoriteButton,
                   clearButton, clearFavoritesButton, refreshButton});
    }

    void clearHistoryListBox() {
        if (historyListBox == nullptr) {
            return;
        }
        auto *children =
            gtk_container_get_children(GTK_CONTAINER(historyListBox));
        for (auto *c = children; c != nullptr; c = c->next) {
            gtk_widget_destroy(GTK_WIDGET(c->data));
        }
        g_list_free(children);
    }

    void refresh(bool shouldNotify) {
        if (!widgetsAlive || isOperating || isLoading) {
            return;
        }
        isLoading = true;
        updateHistoryActionState();
        auto *data = new ReloadTaskData;
        data->lifetime = lifetime;
        data->historyPath = historyPath;
        data->favoritesPath = favoritesPath;
        data->generation = ++reloadGeneration;
        data->shouldNotify = shouldNotify;
        auto *task = g_task_new(nullptr, nullptr, onReloadFinished, nullptr);
        g_task_set_task_data(task, data, nullptr);
        g_task_run_in_thread(task, onReloadWorker);
        g_object_unref(task);
    }

    static void onReloadWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
        auto *td = static_cast<ReloadTaskData *>(taskData);
        core::ClipboardHistory history;
        td->historySuccess = history.load(td->historyPath, &td->historyError);
        if (td->historySuccess) {
            td->history = std::make_shared<const std::vector<std::string>>(history.entries());
        }
        core::ClipboardFavorites favorites(td->favoritesPath);
        td->favoritesSuccess = favorites.load(&td->favoritesError);
        if (td->favoritesSuccess) {
            td->favorites = std::make_shared<const std::vector<std::string>>(favorites.entries());
        }
        g_task_return_pointer(task, td, nullptr);
    }

    static void onReloadFinished(GObject *, GAsyncResult *result, gpointer) {
        auto *td = static_cast<ReloadTaskData *>(g_task_propagate_pointer(G_TASK(result), nullptr));
        if (td == nullptr) {
            return;
        }
        td->lifetime->withOwner([td](Impl &owner) {
            if (td->generation != owner.reloadGeneration) {
                return;
            }
            owner.isLoading = false;
            owner.historyReadable = td->historySuccess;
            owner.favoritesReadable = td->favoritesSuccess;
            owner.historyError = td->historySuccess ? "" : td->historyError;
            owner.favoritesError = td->favoritesSuccess ? "" : td->favoritesError;
            if (td->historySuccess) {
                owner.historyEntries = std::move(td->history);
            }
            if (td->favoritesSuccess) {
                owner.favoriteEntries = std::move(td->favorites);
            }
            owner.hasLoaded = true;
            owner.requestSearch(false, td->shouldNotify);
        });
        delete td;
    }

    static void onSearchChanged(GtkWidget *, gpointer data) {
        static_cast<Impl *>(data)->requestSearch(true, false);
    }

    void requestSearch(bool debounce, bool shouldNotify) {
        if (!widgetsAlive) return;
        if (searchSourceId != 0) {
            g_source_remove(searchSourceId);
            searchSourceId = 0;
        }
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        ++historyGeneration;
        isFiltering = true;
        updateHistoryActionState();
        if (debounce) {
            auto *token = new std::shared_ptr<detail::DiagnosticsLifetime<Impl>>(lifetime);
            searchSourceId = g_timeout_add_full(G_PRIORITY_DEFAULT, 120,
                +[](gpointer data) -> gboolean {
                    const auto &life = *static_cast<std::shared_ptr<detail::DiagnosticsLifetime<Impl>> *>(data);
                    life->withOwner([](Impl &owner) {
                        owner.searchSourceId = 0;
                        owner.startSearch(false);
                    });
                    return G_SOURCE_REMOVE;
                }, token, +[](gpointer data) {
                    delete static_cast<std::shared_ptr<detail::DiagnosticsLifetime<Impl>> *>(data);
                });
        } else {
            startSearch(shouldNotify);
        }
    }

    void startSearch(bool shouldNotify) {
        auto *data = new SearchTaskData;
        data->lifetime = lifetime;
        data->generation = historyGeneration;
        data->history = historyEntries;
        data->favorites = favoriteEntries;
        data->query = gtk_entry_get_text(GTK_ENTRY(searchEntry));
        data->favoritesOnly = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(favoritesToggle));
        data->shouldNotify = shouldNotify;
        auto *task = g_task_new(nullptr, nullptr, onSearchFinished, nullptr);
        g_task_set_task_data(task, data, nullptr);
        g_task_run_in_thread(task, onSearchWorker);
        g_object_unref(task);
    }

    static void onSearchWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
        auto *td = static_cast<SearchTaskData *>(taskData);
        const auto matches = core::searchClipboard(*td->history, *td->favorites,
                                                   td->query, td->favoritesOnly, 60);
        td->items.reserve(matches.size());
        for (const auto &match : matches) {
            td->items.push_back({match.text, analyzeClipboardContent(match.text),
                                 match.favorite, match.historyIndex});
        }
        g_task_return_pointer(task, td, nullptr);
    }

    static void onSearchFinished(GObject *, GAsyncResult *result, gpointer) {
        auto *td = static_cast<SearchTaskData *>(g_task_propagate_pointer(G_TASK(result), nullptr));
        if (td == nullptr) {
            return;
        }
        td->lifetime->withOwner([td](Impl &owner) {
            if (td->generation != owner.historyGeneration) {
                return;
            }
            // Keep both immutable input snapshots alive for every borrowed text.
            owner.displayItems.clear();
            owner.displayHistory = std::move(td->history);
            owner.displayFavorites = std::move(td->favorites);
            owner.displayItems = std::move(td->items);
            owner.startBatchRender(td->generation, td->shouldNotify);
        });
        delete td;
    }

    void startBatchRender(uint64_t gen, bool shouldNotify) {
        clearHistoryListBox();
        if (displayItems.empty()) {
            finishRender(shouldNotify);
            return;
        }
        auto *ctx = new BatchRenderContext{lifetime, gen, 0, shouldNotify};
        idleBatchSourceId = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, onBatchIdle,
            ctx, [](gpointer data) { delete static_cast<BatchRenderContext *>(data); });
    }

    static gboolean onBatchIdle(gpointer data) {
        auto *ctx = static_cast<BatchRenderContext *>(data);
        bool keepGoing = false;
        const bool alive = ctx->lifetime->withOwner([ctx, &keepGoing](Impl &owner) {
            if (ctx->generation != owner.historyGeneration) {
                return;
            }
            const auto start = std::chrono::steady_clock::now();
            while (ctx->nextIndex < owner.displayItems.size()) {
                const auto index = ctx->nextIndex++;
                gtk_container_add(GTK_CONTAINER(owner.historyListBox),
                                  owner.buildCardRow(index, owner.displayItems[index]));
                if (ctx->nextIndex % 10 == 0 ||
                    std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(4)) {
                    break;
                }
            }
            if (ctx->nextIndex < owner.displayItems.size()) {
                keepGoing = true;
            } else {
                owner.idleBatchSourceId = 0;
                gtk_widget_show_all(owner.historyListBox);
                auto *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(owner.historyListBox), 0);
                gtk_list_box_select_row(GTK_LIST_BOX(owner.historyListBox), first);
                owner.finishRender(ctx->shouldNotify);
            }
        });
        return alive && keepGoing ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }

    void finishRender(bool shouldNotify) {
        isFiltering = false;
        const auto summary = std::to_string(displayItems.size()) + " 条结果 · 历史 " +
            std::to_string(historyEntries->size()) + " 条 · 收藏 " +
            std::to_string(favoriteEntries->size()) + "/30 条";
        gtk_label_set_text(GTK_LABEL(historyCount), summary.c_str());
        updateHistoryActionState();
        updateStatus();
        if (shouldNotify) {
            notifyMessage(historyReadable && favoritesReadable
                ? "剪贴板历史与收藏已刷新" : "部分内容读取失败，请查看错误详情");
        }
    }

    void updateStatus() {
        std::string summary;
        std::string details;
        const auto appendError = [&](std::string_view message, const std::string &detail) {
            if (!summary.empty()) summary += "\n";
            summary += message;
            if (!details.empty()) details += "\n\n";
            details += std::string(message) + "\n" + (detail.empty() ? "无法访问数据文件。请检查路径及访问权限后刷新。" : detail);
        };
        if (hasLoaded && !historyReadable) {
            appendError("历史读取失败，已保留上次内容。删除与清空暂不可用。", historyError);
        }
        if (hasLoaded && !favoritesReadable) {
            appendError("收藏读取失败，已保留上次内容。收藏修改暂不可用。", favoritesError);
        }
        if (!operationErrorSummary.empty()) {
            appendError(operationErrorSummary, operationErrorDetail);
        }
        if (displayItems.empty() && !isFiltering) {
            if (!summary.empty()) summary += "\n";
            if (gtk_entry_get_text(GTK_ENTRY(searchEntry))[0] != '\0') {
                summary += "没有匹配的内容，试试其他关键词。";
            } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(favoritesToggle))) {
                summary += "还没有收藏。取消“仅收藏”后，选择内容并点击“收藏”。";
            } else {
                summary += "还没有剪贴板内容。复制一段文字后刷新历史，或在这里查看已收藏的内容。";
            }
        }
        gtk_label_set_text(GTK_LABEL(historyState), summary.c_str());
        gtk_widget_set_visible(historyState, !summary.empty());
        gtk_label_set_text(GTK_LABEL(errorDetailsLabel), details.c_str());
        gtk_widget_set_visible(errorDetails, !details.empty());
        if (!details.empty()) gtk_widget_show(errorDetailsLabel);
    }

    void refreshSettings() {
        if (!widgetsAlive) return;
        refreshing = true;
        const auto &settings = model.settings();
        gtk_switch_set_active(GTK_SWITCH(clipboardEnabled), settings.clipboardEnabled);
        gtk_entry_set_text(GTK_ENTRY(clipboardTrigger), settings.clipboardTrigger.c_str());
        refreshing = false;
        updateSettingsState();
    }

    bool focusTarget(std::string_view target) {
        if (!widgetsAlive) return false;
        const std::array controls{
            std::pair{clipboardEnabled, static_cast<GtkWidget *>(nullptr)},
            std::pair{clipboardTrigger, clipboardTriggerFallback},
            std::pair{historyListBox, static_cast<GtkWidget *>(nullptr)},
            std::pair{searchEntry, static_cast<GtkWidget *>(nullptr)},
            std::pair{favoritesToggle, static_cast<GtkWidget *>(nullptr)},
            std::pair{favoriteButton, historyListBox},
            std::pair{clearFavoritesButton, favoritesToggle},
        };
        for (const auto &[control, fallback] : controls) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                if (control == clipboardEnabled || control == clipboardTrigger) {
                    setSettingsDisclosureExpanded(settingsToggle, true, false);
                }
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
        if (!impl->widgetsAlive || impl->refreshing) return;
        impl->model.setClipboardOptions(
            gtk_switch_get_active(GTK_SWITCH(impl->clipboardEnabled)),
            gtk_entry_get_text(GTK_ENTRY(impl->clipboardTrigger)));
        impl->updateSettingsState();
        impl->settingsChanged();
    }

    static void onRefresh(GtkButton *, gpointer data) {
        static_cast<Impl *>(data)->refresh(true);
    }

    static void onCopy(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (selected) impl->copyIndex(*selected);
    }

    static void onDelete(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (selected) impl->deleteIndex(*selected);
    }

    bool confirmClear(bool favorites) {
        const auto count = favorites ? favoriteEntries->size() : historyEntries->size();
        if (count == 0) return false;
        // Confirmation owns the same operation slot as its eventual write. The
        // nested dialog loop may still finish or restart a view-only search.
        isOperating = true;
        updateHistoryActionState();
        auto *dialog = gtk_message_dialog_new(parentWindow(), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
            favorites ? "确定清空全部 %zu 条收藏吗？剪贴板历史会保留。"
                      : "确定清空全部 %zu 条剪贴板历史吗？收藏会保留。", count);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "此操作不可撤销。");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_YES,
            favorites ? "清空收藏" : "清空历史",
            favorites ? "只清空收藏，保留剪贴板历史" : "只清空剪贴板历史，保留收藏");
        addStyleClass(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES),
                      "destructive-action");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO, "取消", "保留全部内容");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_YES || !widgetsAlive) {
            isOperating = false;
            updateHistoryActionState();
            return false;
        }
        return true;
    }

    static void onClear(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->busy() || !impl->historyReadable || !impl->confirmClear(false)) return;
        impl->beginOperation(Operation::ClearHistory, 0, {}, true);
    }

    static void onClearFavorites(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->busy() || !impl->favoritesReadable || !impl->confirmClear(true)) return;
        impl->beginOperation(Operation::ClearFavorites, 0, {}, true);
    }

    static void onFavorite(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        const auto selected = impl->selectedHistoryIndex();
        if (impl->busy() || !impl->favoritesReadable || !selected) return;
        const auto &item = impl->displayItems[*selected];
        impl->beginOperation(item.favorite ? Operation::RemoveFavorite : Operation::AddFavorite,
                             0, std::string(item.text));
    }

    void beginOperation(Operation operation, std::size_t index = 0,
                        std::string expected = {}, bool confirmed = false) {
        if (!widgetsAlive || isLoading || (isOperating && !confirmed) ||
            (!confirmed && isFiltering)) return;
        isOperating = true;
        // A confirmed full-text identity is independent of the current filter.
        // Retire old filter/render callbacks before the storage operation begins.
        if (searchSourceId != 0) {
            g_source_remove(searchSourceId);
            searchSourceId = 0;
        }
        if (idleBatchSourceId != 0) {
            g_source_remove(idleBatchSourceId);
            idleBatchSourceId = 0;
        }
        ++historyGeneration;
        isFiltering = false;
        operationErrorSummary.clear();
        operationErrorDetail.clear();
        updateStatus();
        updateHistoryActionState();
        auto *td = new OperationTaskData;
        td->lifetime = lifetime;
        td->operation = operation;
        td->path = operation == Operation::ClearHistory || operation == Operation::DeleteHistory
            ? historyPath : favoritesPath;
        td->generation = ++operationGeneration;
        td->historyIndex = index;
        td->expectedText = std::move(expected);
        auto *task = g_task_new(nullptr, nullptr, onOperationFinished, nullptr);
        g_task_set_task_data(task, td, nullptr);
        g_task_run_in_thread(task, onOperationWorker);
        g_object_unref(task);
    }

    static void onOperationWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
        auto *td = static_cast<OperationTaskData *>(taskData);
        if (td->operation == Operation::DeleteHistory || td->operation == Operation::ClearHistory) {
            ClipboardHistoryModel history(td->path);
            td->success = td->operation == Operation::ClearHistory
                ? history.clear(&td->error)
                : history.remove(td->historyIndex, td->expectedText, &td->error);
        } else {
            core::ClipboardFavorites favorites(td->path);
            switch (td->operation) {
            case Operation::AddFavorite:
                td->success = favorites.add(td->expectedText, &td->error); break;
            case Operation::RemoveFavorite:
                td->success = favorites.remove(td->expectedText, &td->error); break;
            case Operation::ClearFavorites:
                td->success = favorites.clear(&td->error); break;
            default: break;
            }
        }
        g_task_return_pointer(task, td, nullptr);
    }

    static void onOperationFinished(GObject *, GAsyncResult *result, gpointer) {
        auto *td = static_cast<OperationTaskData *>(g_task_propagate_pointer(G_TASK(result), nullptr));
        if (td == nullptr) return;
        td->lifetime->withOwner([td](Impl &owner) {
            if (td->generation != owner.operationGeneration) return;
            owner.isOperating = false;
            const char *success = "";
            const char *failure = "";
            switch (td->operation) {
            case Operation::DeleteHistory: success = "已删除历史，收藏已保留"; failure = "历史删除失败"; break;
            case Operation::ClearHistory: success = "历史已清空，收藏已保留"; failure = "历史清空失败"; break;
            case Operation::AddFavorite: success = "已收藏"; failure = "收藏失败"; break;
            case Operation::RemoveFavorite: success = "已取消收藏，历史已保留"; failure = "取消收藏失败"; break;
            case Operation::ClearFavorites: success = "收藏已清空，历史已保留"; failure = "收藏清空失败"; break;
            }
            if (td->success) {
                owner.notifyMessage(success);
                owner.refresh(false);
            } else {
                owner.operationErrorSummary = std::string(failure) + "，已保留上次内容。请查看错误详情。";
                owner.operationErrorDetail = td->error;
                owner.notifyMessage(failure);
                owner.requestSearch(false, false);
            }
        });
        delete td;
    }

    static void onRowSelected(GtkListBox *, GtkListBoxRow *, gpointer data) {
        static_cast<Impl *>(data)->updateHistoryActionState();
    }

    static void onRowActivated(GtkListBox *, GtkListBoxRow *row, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (row == nullptr || impl->busy()) return;
        const auto index = gtk_list_box_row_get_index(row);
        if (index >= 0) impl->copyIndex(static_cast<std::size_t>(index));
    }

    void copyIndex(std::size_t index) {
        if (busy() || index >= displayItems.size()) return;
        const auto entry = displayItems[index].text;
        auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (clipboard == nullptr) {
            notifyMessage("系统剪贴板不可用");
            return;
        }
        gtk_clipboard_set_text(clipboard, entry.data(), static_cast<gint>(entry.size()));
        notifyMessage("已复制完整内容");
    }

    void deleteIndex(std::size_t index) {
        if (busy() || !historyReadable || index >= displayItems.size() ||
            !displayItems[index].historyIndex) return;
        const auto expected = std::string(displayItems[index].text);
        const auto sourceIndex = *displayItems[index].historyIndex;
        isOperating = true;
        updateHistoryActionState();
        auto *dialog = gtk_message_dialog_new(parentWindow(), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "确定删除选中的剪贴板历史吗？收藏会保留。");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_YES, "删除历史", "删除选中的历史，保留收藏");
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_NO, "取消", "保留历史");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response == GTK_RESPONSE_YES && widgetsAlive && historyReadable) {
            // Resolve the original source index against its captured full text,
            // even when a search restarted during the confirmation dialog.
            beginOperation(Operation::DeleteHistory, sourceIndex, expected, true);
        } else {
            isOperating = false;
            updateHistoryActionState();
        }
    }

    std::optional<std::size_t> selectedHistoryIndex() const {
        if (historyListBox == nullptr) return std::nullopt;
        auto *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(historyListBox));
        if (row == nullptr) return std::nullopt;
        const auto index = gtk_list_box_row_get_index(row);
        if (index < 0 || static_cast<std::size_t>(index) >= displayItems.size()) return std::nullopt;
        return static_cast<std::size_t>(index);
    }

    bool busy() const { return isLoading || isFiltering || isOperating; }

    void updateHistoryActionState() {
        if (!widgetsAlive) return;
        const auto selected = selectedHistoryIndex();
        const bool ready = !busy();
        const auto *item = selected ? &displayItems[*selected] : nullptr;
        gtk_widget_set_sensitive(copyButton, ready && item != nullptr);
        gtk_widget_set_sensitive(deleteButton, ready && historyReadable && item != nullptr && item->historyIndex.has_value());
        gtk_widget_set_sensitive(clearButton, ready && historyReadable && !historyEntries->empty());
        const bool favoritesFull = favoriteEntries->size() >= 30;
        gtk_widget_set_sensitive(favoriteButton, ready && favoritesReadable && item != nullptr && (item->favorite || !favoritesFull));
        gtk_widget_set_tooltip_text(favoriteButton, item && !item->favorite && favoritesFull
            ? "收藏已满，请先取消一个收藏。" : "收藏独立保存，清空历史时会保留。");
        gtk_button_set_label(GTK_BUTTON(favoriteButton), item && item->favorite ? "取消收藏" : "收藏");
        setAccessibleWidgetText(favoriteButton, item && item->favorite ? "取消收藏" : "收藏", "操作选中的完整内容，即时保存");
        gtk_widget_set_sensitive(clearFavoritesButton, ready && favoritesReadable && !favoriteEntries->empty());
        gtk_widget_set_sensitive(refreshButton, !isLoading && !isOperating);
    }

    GtkWidget *buildCardRow(std::size_t index, const DisplayItem &item) {
        auto *row = gtk_list_box_row_new();
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), TRUE);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);

        auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addStyleClass(card, "modernime-clipboard-card");

        const auto &meta = item.meta;

        // Secondary metadata stays on one line; the content has its own reading area.
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(header, GTK_ALIGN_CENTER);

        const char *typeName = meta.badgeClass == "modernime-badge-code" ? "代码" :
                               meta.badgeClass == "modernime-badge-url" ? "链接" :
                               meta.badgeClass == "modernime-badge-multiline" ? "多行文本" : "文本";
        const char *typeIcon = meta.badgeClass == "modernime-badge-code" ? "text-x-script-symbolic" :
                               meta.badgeClass == "modernime-badge-url" ? "insert-link-symbolic" :
                               "text-x-generic-symbolic";
        auto *typeBadge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        addStyleClass(typeBadge, "modernime-clipboard-type");
        addStyleClass(typeBadge, meta.badgeClass.c_str());
        gtk_widget_set_valign(typeBadge, GTK_ALIGN_CENTER);
        auto *typeImage = gtk_image_new_from_icon_name(typeIcon, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(typeImage), 14);
        addStyleClass(typeImage, "modernime-clipboard-type-icon");
        gtk_box_pack_start(GTK_BOX(typeBadge), typeImage, FALSE, FALSE, 0);
        auto *badgeLabel = gtk_label_new(typeName);
        gtk_box_pack_start(GTK_BOX(typeBadge), badgeLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(header), typeBadge, FALSE, FALSE, 0);

        const std::string metaStr =
            (meta.lineCount > 1 ? std::to_string(meta.lineCount) + " 行 · " : "") +
            std::to_string(meta.charCount) + " 字";
        auto *metaLabel = gtk_label_new(metaStr.c_str());
        addStyleClass(metaLabel, "modernime-card-meta");
        gtk_box_pack_start(GTK_BOX(header), metaLabel, FALSE, FALSE, 0);

        if (item.favorite) {
            auto *favoriteMark = gtk_label_new("★ 收藏");
            addStyleClass(favoriteMark, "modernime-card-meta");
            gtk_box_pack_start(GTK_BOX(header), favoriteMark, FALSE, FALSE, 0);
        }
        auto *spacer = gtk_label_new("");
        gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);

        auto *previewLabel = gtk_label_new(meta.preview.c_str());
        addStyleClass(previewLabel, "modernime-card-preview");
        gtk_label_set_xalign(GTK_LABEL(previewLabel), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(previewLabel), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(previewLabel), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_ellipsize(GTK_LABEL(previewLabel), PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(GTK_LABEL(previewLabel), 2);
        gtk_widget_set_hexpand(previewLabel, TRUE);
        if (meta.badgeClass == "modernime-badge-code") {
            addStyleClass(previewLabel, "modernime-card-code");
        } else if (meta.badgeClass == "modernime-badge-url") {
            addStyleClass(previewLabel, "modernime-card-url");
        }

        auto *previewRevealer = gtk_revealer_new();
        setSettingsRevealed(previewRevealer, true, false);
        gtk_container_add(GTK_CONTAINER(previewRevealer), previewLabel);

        GtkWidget *expandedBox = nullptr;
        GtkWidget *expandedRevealer = nullptr;
        if (meta.isLong || meta.lineCount > 1 || meta.charCount > 40) {
            auto *toggleBtn = gtk_button_new_with_label("展开");
            addStyleClass(toggleBtn, "modernime-toggle-btn");
            gtk_widget_set_tooltip_text(toggleBtn, "展开查看完整内容");
            gtk_widget_set_no_show_all(toggleBtn, TRUE);
            gtk_widget_set_visible(toggleBtn, meta.isLong);
            g_object_set_data(G_OBJECT(toggleBtn), "clipboard-preview-truncated",
                               GINT_TO_POINTER(meta.isLong));
            g_signal_connect_object(
                previewLabel, "size-allocate",
                G_CALLBACK(+[](GtkWidget *label, GtkAllocation *, gpointer button) {
                    auto *preview = gtk_widget_get_parent(label);
                    if (gtk_widget_get_visible(label) &&
                        gtk_revealer_get_reveal_child(GTK_REVEALER(preview))) {
                        const bool truncated = g_object_get_data(
                            G_OBJECT(button), "clipboard-preview-truncated") != nullptr;
                        gtk_widget_set_visible(GTK_WIDGET(button), truncated ||
                            pango_layout_is_ellipsized(gtk_label_get_layout(GTK_LABEL(label))));
                    }
                }), toggleBtn, GConnectFlags(0));
            gtk_box_pack_start(GTK_BOX(header), toggleBtn, FALSE, FALSE, 0);

            expandedBox = gtk_scrolled_window_new(nullptr, nullptr);
            addStyleClass(expandedBox, "modernime-card-expanded-box");
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(expandedBox),
                                           GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
            gtk_widget_set_size_request(expandedBox, -1, 180);
            expandedRevealer = gtk_revealer_new();
            gtk_container_add(GTK_CONTAINER(expandedRevealer), expandedBox);

            struct ToggleData {
                Impl *impl;
                std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
                GtkWidget *preview;
                GtkWidget *expanded;
                GtkWidget *revealer;
                std::size_t index;
                uint64_t generation;
                bool isCode;
            };
            auto *tdata = new ToggleData{this, lifetime, previewRevealer, expandedBox,
                                         expandedRevealer, index, historyGeneration,
                                         meta.badgeClass == "modernime-badge-code"};
            g_signal_connect_data(
                toggleBtn, "clicked",
                G_CALLBACK(+[](GtkButton *b, gpointer d) {
                    auto *td = static_cast<ToggleData *>(d);
                    if (!td->lifetime->withOwner([](Impl &) {})) return;
                    if (td->generation != td->impl->historyGeneration) return;
                    auto *rowWidget = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
                    if (rowWidget != nullptr && td->impl->historyListBox != nullptr) {
                        gtk_list_box_select_row(GTK_LIST_BOX(td->impl->historyListBox), GTK_LIST_BOX_ROW(rowWidget));
                    }
                    const bool isExpanded =
                        !gtk_revealer_get_reveal_child(GTK_REVEALER(td->revealer));
                    if (isExpanded) {
                        // Allocate the full text only when requested, not for all 30 records.
                        if (gtk_bin_get_child(GTK_BIN(td->expanded)) == nullptr) {
                            auto *reader = gtk_text_view_new();
                            gtk_text_view_set_editable(GTK_TEXT_VIEW(reader), FALSE);
                            gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(reader), FALSE);
                            gtk_text_view_set_monospace(GTK_TEXT_VIEW(reader), td->isCode);
                            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(reader), GTK_WRAP_WORD_CHAR);
                            gtk_text_view_set_left_margin(GTK_TEXT_VIEW(reader), 12);
                            gtk_text_view_set_right_margin(GTK_TEXT_VIEW(reader), 12);
                            gtk_text_view_set_top_margin(GTK_TEXT_VIEW(reader), 10);
                            gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(reader), 10);
                            addStyleClass(reader, "modernime-clipboard-reader");
                            if (td->isCode) {
                                addStyleClass(reader, "modernime-card-code");
                            }
                            if (td->index < td->impl->displayItems.size()) {
                                const auto &text = td->impl->displayItems[td->index].text;
                                gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(reader)),
                                                         text.data(), static_cast<gint>(text.size()));
                            }
                            setAccessibleWidgetText(reader, "剪贴板完整内容", "可选择文字，内容不会被编辑");
                            gtk_container_add(GTK_CONTAINER(td->expanded), reader);
                            gtk_widget_show(reader);
                        }
                        gtk_button_set_label(b, "收起");
                        gtk_widget_set_tooltip_text(GTK_WIDGET(b), "收起完整内容预览");
                    } else {
                        auto *window = gtk_widget_get_toplevel(GTK_WIDGET(b));
                        auto *focus = GTK_IS_WINDOW(window)
                            ? gtk_window_get_focus(GTK_WINDOW(window)) : nullptr;
                        if (focus != nullptr && gtk_widget_is_ancestor(focus, td->expanded)) {
                            gtk_widget_grab_focus(GTK_WIDGET(b));
                        }
                        gtk_button_set_label(b, "展开");
                        gtk_widget_set_tooltip_text(GTK_WIDGET(b), "展开查看完整内容");
                    }
                    setSettingsRevealed(td->preview, !isExpanded);
                    setSettingsRevealed(td->revealer, isExpanded);
                }),
                tdata,
                [](gpointer d, GClosure *) { delete static_cast<ToggleData *>(d); },
                GConnectFlags(0));
        }

        auto *copyCardBtn = gtk_button_new_with_label("复制");
        addStyleClass(copyCardBtn, "modernime-card-btn");
        gtk_widget_set_tooltip_text(copyCardBtn, "复制此条内容到系统剪贴板");
        gtk_box_pack_start(GTK_BOX(header), copyCardBtn, FALSE, FALSE, 0);

        struct CardActionData {
            Impl *impl;
            std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime;
            std::size_t index;
            uint64_t generation;
        };
        auto *cdata = new CardActionData{this, lifetime, index, historyGeneration};
        g_signal_connect_data(
            copyCardBtn, "clicked",
            G_CALLBACK(+[](GtkButton *b, gpointer d) {
                auto *cd = static_cast<CardActionData *>(d);
                if (!cd->lifetime->withOwner([](Impl &) {})) return;
                if (cd->generation != cd->impl->historyGeneration) return;
                auto *rowWidget = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_LIST_BOX_ROW);
                if (rowWidget != nullptr && cd->impl->historyListBox != nullptr) {
                    gtk_list_box_select_row(GTK_LIST_BOX(cd->impl->historyListBox), GTK_LIST_BOX_ROW(rowWidget));
                }
                cd->impl->copyIndex(cd->index);
            }),
            cdata,
            [](gpointer d, GClosure *) { delete static_cast<CardActionData *>(d); },
            GConnectFlags(0));

        gtk_box_pack_start(GTK_BOX(card), header, FALSE, FALSE, 0);
        auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(content), previewRevealer, FALSE, FALSE, 0);
        if (expandedRevealer != nullptr) {
            gtk_box_pack_start(GTK_BOX(content), expandedRevealer, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(card), content, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(row), card);
        return row;
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
    std::filesystem::path historyPath;
    std::filesystem::path favoritesPath;
    Entries historyEntries = std::make_shared<const std::vector<std::string>>();
    Entries favoriteEntries = std::make_shared<const std::vector<std::string>>();
    Entries displayHistory;
    Entries displayFavorites;
    std::function<void()> settingsChanged;
    std::function<void(std::string)> notify;
    GtkWidget *page = nullptr;
    GtkWidget *settingsToggle = nullptr;
    GtkWidget *settingsRevealer = nullptr;
    GtkWidget *clipboardEnabled = nullptr;
    GtkWidget *clipboardTrigger = nullptr;
    GtkWidget *clipboardTriggerFallback = nullptr;
    GtkWidget *historyCount = nullptr;
    GtkWidget *historyState = nullptr;
    GtkWidget *historyListBox = nullptr;
    GtkWidget *historyScrolled = nullptr;
    GtkWidget *copyButton = nullptr;
    GtkWidget *deleteButton = nullptr;
    GtkWidget *clearButton = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *favoriteButton = nullptr;
    GtkWidget *clearFavoritesButton = nullptr;
    GtkWidget *searchEntry = nullptr;
    GtkWidget *favoritesToggle = nullptr;
    GtkWidget *errorDetails = nullptr;
    GtkWidget *errorDetailsLabel = nullptr;
    bool refreshing = false;
    bool widgetsAlive = true;

    std::shared_ptr<detail::DiagnosticsLifetime<Impl>> lifetime =
        std::make_shared<detail::DiagnosticsLifetime<Impl>>(this);
    uint64_t historyGeneration = 0;
    uint64_t reloadGeneration = 0;
    uint64_t operationGeneration = 0;
    guint searchSourceId = 0;
    guint idleBatchSourceId = 0;
    bool isLoading = false;
    bool isOperating = false;
    bool isFiltering = false;
    bool hasLoaded = false;
    bool historyReadable = false;
    bool favoritesReadable = false;
    std::string historyError;
    std::string favoritesError;
    std::string operationErrorSummary;
    std::string operationErrorDetail;
    std::vector<DisplayItem> displayItems;
};

ClipboardPage::ClipboardPage(SettingsWindowModel &settings,
                             std::filesystem::path historyPath,
                             std::function<void()> settingsChanged,
                             std::function<void(std::string)> notify,
                             std::filesystem::path favoritesPath)
    : impl_(std::make_unique<Impl>(settings, std::move(historyPath),
                                   std::move(settingsChanged),
                                   std::move(notify),
                                   std::move(favoritesPath))) {}

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
