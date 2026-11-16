#include "modernime/settings/learning_manager.h"

#include "modernime/settings/backend_client.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {
constexpr std::uint64_t pageSize = 100;
constexpr std::uint64_t maximumOffset = 200000;

void target(GtkWidget *widget, const char *name) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(name), g_free);
}

GtkWidget *label(const char *text) {
    auto *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 1);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), "modernime-description");
    return widget;
}

struct Identity final {
    std::string phrase;
    std::string pinyin;
    bool operator==(const Identity &other) const {
        return phrase == other.phrase && pinyin == other.pinyin;
    }
};

struct Record final {
    Identity identity;
    std::uint64_t frequency = 0;
    std::uint64_t lastSelectedMs = 0;
    std::uint64_t negativeFeedback = 0;
    std::uint64_t contextCount = 0;
    bool suppressed = false;
};

std::string selectedTime(std::uint64_t milliseconds) {
    if (milliseconds == 0) return "尚无选用时间";
    const auto seconds = milliseconds / 1000;
    if (seconds > static_cast<std::uint64_t>(std::numeric_limits<gint64>::max())) return "选用时间不可用";
    auto *time = g_date_time_new_from_unix_local(static_cast<gint64>(seconds));
    if (!time) return "选用时间不可用";
    auto *formatted = g_date_time_format(time, "%Y-%m-%d %H:%M");
    std::string result = formatted ? formatted : "选用时间不可用";
    g_free(formatted);
    g_date_time_unref(time);
    return result;
}
} // namespace

class LearningManager::Impl final {
public:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;
    enum Column { Phrase, Pinyin, Frequency, Suppressed, ColumnCount };

    explicit Impl(std::function<void(std::string)> callback) : notify(std::move(callback)) {
        detail::GtkWidgetGuard guard(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
        root = guard.get();
        gtk_widget_set_hexpand(root, TRUE);
        gtk_widget_set_vexpand(root, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(root), "modernime-learning-panel");

        auto *searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        search = gtk_search_entry_new();
        target(search, "learning-record-search");
        gtk_entry_set_placeholder_text(GTK_ENTRY(search), "搜索词语或拼音");
        setAccessibleWidgetText(search, "搜索学习记录", "按词语或拼音搜索学习记录");
        gtk_widget_set_hexpand(search, TRUE);
        gtk_box_pack_start(GTK_BOX(searchRow), search, TRUE, TRUE, 0);
        refreshButton = button(searchRow, "刷新", "learning-record-refresh", +[](GtkButton *, gpointer data) {
            static_cast<Impl *>(data)->refresh();
        });
        pruneStaleButton = button(searchRow, "清理旧单次", "learning-prune-stale", +[](GtkButton *, gpointer data) {
            static_cast<Impl *>(data)->confirmPruneStale();
        });
        gtk_widget_set_tooltip_text(pruneStaleButton,
            "保留高频习惯词、7天内记录和已屏蔽词，清理长期未用或曾误触撤销的孤立单次记录");
        gtk_box_pack_start(GTK_BOX(root), searchRow, FALSE, FALSE, 4);
        suppressedOnly = gtk_check_button_new_with_label("仅显示屏蔽记录");
        target(suppressedOnly, "learning-record-suppressed-only");

        auto *stateRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(stateRow), "modernime-learning-filterbar");
        gtk_box_pack_start(GTK_BOX(stateRow), suppressedOnly, FALSE, FALSE, 0);
        state = label("正在读取学习记录…");
        target(state, "learning-record-status");
        gtk_label_set_xalign(GTK_LABEL(state), 1.0F);
        gtk_box_pack_start(GTK_BOX(stateRow), state, TRUE, TRUE, 0);
        retry = button(stateRow, "重试", "learning-record-retry", +[](GtkButton *, gpointer data) {
            static_cast<Impl *>(data)->refresh();
        });
        gtk_widget_set_no_show_all(retry, TRUE);
        gtk_box_pack_start(GTK_BOX(root), stateRow, FALSE, FALSE, 4);

        auto *hintStrip = gtk_flow_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(hintStrip), "modernime-keycap-strip");
        target(hintStrip, "learning-correction-hints");
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(hintStrip), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(hintStrip), FALSE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(hintStrip), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(hintStrip), 3);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(hintStrip), 14);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(hintStrip), 6);
        gtk_widget_set_can_focus(hintStrip, FALSE);
        for (const auto &[keyText, actionText, tipText] : std::array<std::tuple<const char *, const char *, const char *>, 3>{{
                 {"BackSpace", "误选后删掉自动降权", "选错字词后直接退格删掉，系统会自动降低该词权重，不再霸占首位"},
                 {"Ctrl+Shift+BackSpace", "15 秒内撤回重选", "撤回刚上屏的候选并恢复原拼音，同时对误选词记录一次降权"},
                 {"Ctrl+Delete", "候选框内屏蔽词条", "高亮不想再见到的候选时按下，立即从后续候选中屏蔽"}}}) {
            auto *pair = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            auto *cap = gtk_label_new(keyText);
            gtk_style_context_add_class(gtk_widget_get_style_context(cap), "modernime-keycap");
            auto *action = gtk_label_new(actionText);
            gtk_style_context_add_class(gtk_widget_get_style_context(action), "modernime-keycap-action");
            gtk_box_pack_start(GTK_BOX(pair), cap, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(pair), action, FALSE, FALSE, 0);
            gtk_widget_set_tooltip_text(pair, tipText);
            auto *child = gtk_flow_box_child_new();
            gtk_widget_set_can_focus(child, FALSE);
            gtk_container_add(GTK_CONTAINER(child), pair);
            gtk_container_add(GTK_CONTAINER(hintStrip), child);
        }
        gtk_box_pack_start(GTK_BOX(root), hintStrip, FALSE, FALSE, 0);

        auto storeOwner = detail::GObjectHandle<GtkListStore>::adopt(
            gtk_list_store_new(ColumnCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_STRING));
        store = storeOwner.get();
        list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        target(list, "learning-records");
        gtk_style_context_add_class(gtk_widget_get_style_context(list), "modernime-content-list");
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(list), FALSE);
        gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), GTK_SELECTION_SINGLE);
        setAccessibleWidgetText(list, "学习记录列表", "每页最多一百条，选择词语及读音后忘记或解除屏蔽");
        for (const auto &[title, column] : {
                 std::pair{"词语", Phrase}, std::pair{"拼音", Pinyin},
                 std::pair{"选用次数", Frequency}, std::pair{"状态", Suppressed}}) {
            auto *renderer = gtk_cell_renderer_text_new();
            g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            auto *viewColumn = gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, nullptr);
            gtk_tree_view_column_set_expand(viewColumn, column == Phrase || column == Pinyin);
            gtk_tree_view_column_set_min_width(viewColumn,
                column == Phrase || column == Pinyin ? 100 : column == Frequency ? 86 : 64);
            gtk_tree_view_append_column(GTK_TREE_VIEW(list), viewColumn);
        }
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, -1, 72);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(scroll), "modernime-learning-table");
        gtk_container_add(GTK_CONTAINER(scroll), list);
        content = gtk_stack_new();
        gtk_stack_add_named(GTK_STACK(content), scroll, "list");
        empty = label("正在读取学习记录…");
        gtk_widget_set_margin_top(empty, 12);
        gtk_widget_set_margin_bottom(empty, 12);
        gtk_stack_add_named(GTK_STACK(content), empty, "empty");
        gtk_widget_set_vexpand(content, TRUE);
        gtk_box_pack_start(GTK_BOX(root), content, TRUE, TRUE, 0);

        auto *pager = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(pager), "modernime-learning-pager");
        g_object_set_data(G_OBJECT(root), "modernime-learning-footer", pager);
        pageStatus = label("");
        target(pageStatus, "learning-record-page-status");
        gtk_box_pack_start(GTK_BOX(pager), pageStatus, TRUE, TRUE, 0);
        previous = button(pager, "上一页", "learning-record-previous", +[](GtkButton *, gpointer data) {
            auto &owner = *static_cast<Impl *>(data);
            if (!owner.ready() || owner.offset == 0) return;
            owner.offset = owner.offset >= pageSize ? owner.offset - pageSize : 0;
            owner.request();
        });
        next = button(pager, "下一页", "learning-record-next", +[](GtkButton *, gpointer data) {
            auto &owner = *static_cast<Impl *>(data);
            if (!owner.ready() || owner.offset + pageSize >= owner.total || owner.offset + pageSize > maximumOffset) return;
            owner.offset += pageSize;
            owner.request();
        });
        gtk_box_pack_end(GTK_BOX(root), pager, FALSE, FALSE, 0);

        selectionBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(selectionBar), "modernime-learning-selection");
        detailLabel = label("");
        target(detailLabel, "learning-record-details");
        gtk_label_set_selectable(GTK_LABEL(detailLabel), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(detailLabel), PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(GTK_LABEL(detailLabel), 2);
        gtk_box_pack_start(GTK_BOX(selectionBar), detailLabel, TRUE, TRUE, 0);
        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        forgetButton = button(actions, "忘记", "learning-forget", +[](GtkButton *, gpointer data) {
            static_cast<Impl *>(data)->confirmForget();
        });
        gtk_style_context_add_class(gtk_widget_get_style_context(forgetButton), "destructive-action");
        unsuppress = button(actions, "解除屏蔽", "learning-unsuppress", +[](GtkButton *, gpointer data) {
            auto &owner = *static_cast<Impl *>(data);
            const auto record = owner.selection();
            if (owner.ready() && record && record->suppressed) owner.mutate(record->identity, false);
        });
        gtk_box_pack_start(GTK_BOX(selectionBar), actions, FALSE, FALSE, 0);
        gtk_widget_show_all(selectionBar);
        gtk_widget_set_no_show_all(selectionBar, TRUE);
        gtk_widget_hide(selectionBar);
        gtk_box_pack_start(GTK_BOX(root), selectionBar, FALSE, FALSE, 0);
        operationState = label("");
        target(operationState, "learning-record-operation-status");
        gtk_label_set_selectable(GTK_LABEL(operationState), TRUE);
        gtk_widget_set_no_show_all(operationState, TRUE);
        gtk_box_pack_start(GTK_BOX(root), operationState, FALSE, FALSE, 0);

        g_signal_connect(search, "changed", G_CALLBACK(+[](GtkEditable *, gpointer data) {
            static_cast<Impl *>(data)->queryChanged(true);
        }), this);
        g_signal_connect(suppressedOnly, "toggled", G_CALLBACK(+[](GtkToggleButton *, gpointer data) {
            static_cast<Impl *>(data)->queryChanged(false);
        }), this);
        g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), "changed",
                         G_CALLBACK(+[](GtkTreeSelection *, gpointer data) { static_cast<Impl *>(data)->update(); }), this);
        g_signal_connect(root, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            static_cast<Impl *>(data)->deactivate();
        }), this);
        setSettingsFocusChain(root, {search, refreshButton, pruneStaleButton, suppressedOnly, retry, list,
                                    forgetButton, unsuppress, pager});
        update();
        guard.release();
    }

    ~Impl() {
        if (uiAlive) gtk_widget_destroy(root);
        deactivate();
    }

    void deactivate() {
        uiAlive = false;
        lifetime->deactivate();
        if (debounce) { g_source_remove(debounce); debounce = 0; }
        if (activeDialog) {
            auto *dialog = std::exchange(activeDialog, nullptr);
            gtk_widget_destroy(dialog);
        }
    }

    GtkWidget *button(GtkWidget *box, const char *text, const char *id, void (*callback)(GtkButton *, gpointer)) {
        auto *result = gtk_button_new_with_label(text);
        target(result, id);
        setAccessibleWidgetText(result, text, text);
        gtk_box_pack_start(GTK_BOX(box), result, FALSE, FALSE, 0);
        g_signal_connect(result, "clicked", G_CALLBACK(callback), this);
        return result;
    }

    bool ready() const {
        return uiAlive && gtk_widget_is_sensitive(root) && loaded && !loading && !mutating && !activeDialog;
    }

    std::optional<Record> selection() const {
        if (!uiAlive) return {};
        GtkTreeIter iter;
        GtkTreeModel *model = nullptr;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), &model, &iter)) return {};
        gchar *phrase = nullptr;
        gchar *pinyin = nullptr;
        gtk_tree_model_get(model, &iter, Phrase, &phrase, Pinyin, &pinyin, -1);
        const Identity identity{phrase ? phrase : "", pinyin ? pinyin : ""};
        g_free(phrase);
        g_free(pinyin);
        const auto found = std::find_if(records.begin(), records.end(), [&](const auto &record) { return record.identity == identity; });
        return found == records.end() ? std::nullopt : std::optional<Record>(*found);
    }

    void update() {
        if (!uiAlive) return;
        const auto selected = selection();
        gtk_widget_set_sensitive(search, !mutating);
        gtk_widget_set_sensitive(suppressedOnly, !mutating);
        gtk_widget_set_sensitive(refreshButton, !mutating && !loading);
        gtk_widget_set_sensitive(pruneStaleButton, ready() && total != 0);
        gtk_widget_set_sensitive(retry, !mutating && !loading);
        gtk_widget_set_sensitive(list, ready());
        gtk_widget_set_sensitive(previous, ready() && offset != 0);
        gtk_widget_set_sensitive(next, ready() && offset + pageSize < total && offset + pageSize <= maximumOffset);
        gtk_widget_set_sensitive(forgetButton, ready() && selected.has_value());
        gtk_widget_set_sensitive(unsuppress, ready() && selected && selected->suppressed);
        std::string details;
        std::string metadata;
        if (selected) {
            details = selected->identity.phrase + "（" + selected->identity.pinyin + "）\n最近选用：" + selectedTime(selected->lastSelectedMs);
            metadata = "使用场景：" + std::to_string(selected->contextCount) +
                "　降低推荐次数：" + std::to_string(selected->negativeFeedback);
        }
        gtk_label_set_text(GTK_LABEL(detailLabel), details.c_str());
        gtk_widget_set_tooltip_text(detailLabel, (details + "\n" + metadata).c_str());
        setAccessibleWidgetText(detailLabel, details, metadata);
        gtk_widget_set_visible(selectionBar, selected.has_value());
        if (loading) gtk_label_set_text(GTK_LABEL(state), "正在读取学习记录…");
        else if (!readError.empty()) gtk_label_set_text(GTK_LABEL(state), readError.c_str());
        else {
            const auto text = total == 0 ? "暂无学习记录" : "共 " + std::to_string(total) + " 条学习记录";
            gtk_label_set_text(GTK_LABEL(state), text.c_str());
        }
        auto *style = gtk_widget_get_style_context(state);
        if (!readError.empty() && !loading) gtk_style_context_add_class(style, "modernime-status-error");
        else gtk_style_context_remove_class(style, "modernime-status-error");
        gtk_widget_set_visible(retry, !readError.empty() && !loading);
        const auto pageText = loaded && !records.empty()
            ? "第 " + std::to_string(offset + 1) + "–" + std::to_string(offset + records.size()) + " 条"
            : std::string{};
        gtk_label_set_text(GTK_LABEL(pageStatus), pageText.c_str());
        if (records.empty()) {
            const char *message = loading ? "正在读取学习记录…" : !readError.empty()
                ? "未能读取学习记录，请重试。" : *gtk_entry_get_text(GTK_ENTRY(search))
                ? "没有匹配的记录，可修改搜索词。" : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(suppressedOnly))
                ? "暂无屏蔽记录。" : "暂无学习记录";
            gtk_label_set_text(GTK_LABEL(empty), message);
        }
        gtk_stack_set_visible_child_name(GTK_STACK(content), records.empty() ? "empty" : "list");
    }

    void queryChanged(bool delay) {
        if (!uiAlive || mutating) return;
        ++generation; // Invalidate a previous query before the debounce fires.
        if (debounce) { g_source_remove(debounce); debounce = 0; }
        offset = 0;
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)));
        loading = true;
        update();
        if (delay) debounce = g_timeout_add(220, +[](gpointer data) -> gboolean {
            auto &owner = *static_cast<Impl *>(data);
            owner.debounce = 0;
            owner.request();
            return G_SOURCE_REMOVE;
        }, this);
        else request();
    }

    void refresh() {
        if (!uiAlive || mutating) return;
        request();
    }

    void request() {
        if (!uiAlive || mutating) return;
        if (debounce) { g_source_remove(debounce); debounce = 0; }
        const auto selected = selection();
        const auto selectedIdentity = selected ? std::optional<Identity>(selected->identity) : std::nullopt;
        const auto requestGeneration = ++generation;
        loading = true;
        readError.clear();
        update();
        std::string query = gtk_entry_get_text(GTK_ENTRY(search));
        std::vector<std::string> args;
        if (!query.empty()) args.push_back(std::move(query));
        args.push_back(std::to_string(offset));
        args.push_back(std::to_string(pageSize));
        args.push_back(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(suppressedOnly)) ? "true" : "false");
        const auto action = args.size() == 4 ? "learning.search" : "learning.list";
        backend.execute(action, std::move(args),
            [life = lifetime, requestGeneration, selectedIdentity](BackendReply reply) {
                life->withOwner([&](Impl &owner) {
                    if (requestGeneration != owner.generation || owner.mutating) return;
                    owner.loading = false;
                    if (!reply.ok) {
                        owner.loaded = false;
                        owner.readError = "读取失败：" + (reply.message.empty() ? "请确认输入法正在运行后重试。" : reply.message);
                        owner.update();
                        return;
                    }
                    owner.loaded = true;
                    owner.total = reply.data.number("total");
                    if (owner.total != 0 && owner.offset >= owner.total) {
                        owner.offset = std::min(maximumOffset, ((owner.total - 1) / pageSize) * pageSize);
                        owner.request();
                        return;
                    }
                    owner.records.clear();
                    for (const auto &item : reply.data.array("records")) {
                        if (owner.records.size() == pageSize) break;
                        owner.records.push_back({{item.text("phrase"), item.text("pinyin")},
                            item.number("frequency"), item.number("lastSelectedMs"),
                            item.number("negativeFeedback"), item.number("contextCount"), item.boolean("suppressed")});
                    }
                    gtk_list_store_clear(owner.store);
                    for (const auto &record : owner.records) {
                        GtkTreeIter iter;
                        gtk_list_store_append(owner.store, &iter);
                        const std::string statusText = record.suppressed
                            ? "已屏蔽"
                            : (record.negativeFeedback > 0
                                   ? ("已降权×" + std::to_string(record.negativeFeedback))
                                   : "正常");
                        gtk_list_store_set(owner.store, &iter, Phrase, record.identity.phrase.c_str(),
                            Pinyin, record.identity.pinyin.c_str(), Frequency, static_cast<guint64>(record.frequency),
                            Suppressed, statusText.c_str(), -1);
                        if (selectedIdentity && record.identity == *selectedIdentity)
                            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(owner.list)), &iter);
                    }
                    owner.update();
                });
            });
    }

    void confirmForget() {
        const auto selected = selection();
        if (!ready() || !selected) return;
        auto *top = gtk_widget_get_toplevel(root);
        auto *dialog = gtk_message_dialog_new(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "忘记所选词的学习记录？");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "%s（%s）\n\n清除这个词的学习记录和屏蔽设置，不改动词库。这个词以后仍可能出现。",
            selected->identity.phrase.c_str(), selected->identity.pinyin.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "取消", GTK_RESPONSE_CANCEL, "忘记", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "忘记", "删除所选词和读音的学习记录，保留基础词库");
        gtk_style_context_add_class(gtk_widget_get_style_context(
            gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)), "destructive-action");
        struct Confirmation final { std::shared_ptr<Lifetime> life; Identity identity; };
        auto *confirmation = new Confirmation{lifetime, selected->identity};
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](GtkDialog *dialog, gint response, gpointer data) {
            const auto confirmation = *static_cast<Confirmation *>(data);
            confirmation.life->withOwner([&](Impl &owner) { owner.activeDialog = nullptr; });
            gtk_widget_destroy(GTK_WIDGET(dialog));
            confirmation.life->withOwner([&](Impl &owner) {
                owner.update();
                if (response == GTK_RESPONSE_ACCEPT) owner.mutate(confirmation.identity, true);
            });
        }), confirmation, +[](gpointer data, GClosure *) { delete static_cast<Confirmation *>(data); }, GConnectFlags(0));
        activeDialog = dialog;
        update();
        gtk_widget_show_all(dialog);
    }

    void confirmPruneStale() {
        if (!ready() || total == 0) return;
        auto *top = gtk_widget_get_toplevel(root);
        auto *dialog = gtk_message_dialog_new(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "清理陈旧的单次学习记录？");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "保留所有选用 ≥ 2 次的常用习惯词、7 天内最近选词以及已屏蔽词；\n仅清理 7 天以上未使用或曾误触撤销的单次孤立记录，不改动基础词库。");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "取消", GTK_RESPONSE_CANCEL, "清理", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "清理旧单次", "批量清理陈旧孤立单次学习记录，保留常用习惯与基础词库");
        gtk_style_context_add_class(gtk_widget_get_style_context(
            gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)), "destructive-action");
        auto *life = new std::shared_ptr<Lifetime>(lifetime);
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](GtkDialog *dialog, gint response, gpointer data) {
            const auto lifeCopy = *static_cast<std::shared_ptr<Lifetime> *>(data);
            lifeCopy->withOwner([&](Impl &owner) { owner.activeDialog = nullptr; });
            gtk_widget_destroy(GTK_WIDGET(dialog));
            lifeCopy->withOwner([&](Impl &owner) {
                owner.update();
                if (response == GTK_RESPONSE_ACCEPT) owner.pruneStale();
            });
        }), life, +[](gpointer data, GClosure *) { delete static_cast<std::shared_ptr<Lifetime> *>(data); }, GConnectFlags(0));
        activeDialog = dialog;
        update();
        gtk_widget_show_all(dialog);
    }

    void pruneStale() {
        if (!ready()) return;
        if (debounce) { g_source_remove(debounce); debounce = 0; }
        ++generation;
        mutating = true;
        g_object_set_data(G_OBJECT(root), "modernime-learning-mutation-busy", GINT_TO_POINTER(1));
        gtk_label_set_text(GTK_LABEL(operationState), "正在清理陈旧单次记录…");
        gtk_style_context_remove_class(gtk_widget_get_style_context(operationState), "modernime-status-error");
        gtk_widget_show(operationState);
        update();
        backend.execute("learning.prune_stale", {"7"},
            [life = lifetime](BackendReply reply) {
                life->withOwner([&](Impl &owner) {
                    owner.mutating = false;
                    g_object_set_data(G_OBJECT(owner.root), "modernime-learning-mutation-busy", nullptr);
                    std::string message;
                    if (reply.ok) {
                        const auto removed = reply.data.number("removed");
                        const auto remaining = reply.data.number("remaining");
                        message = removed == 0
                            ? "暂无可清理的陈旧单次记录。"
                            : ("已清理 " + std::to_string(removed) + " 条陈旧单次记录，当前剩余 " +
                               std::to_string(remaining) + " 条。");
                    } else if (reply.uncertain) {
                        message = "清理结果未确认，请刷新列表核对后再操作。" +
                            (reply.message.empty() ? std::string{} : "\n" + reply.message);
                    } else {
                        message = "清理失败：" + (reply.message.empty() ? std::string("请确认输入法版本已更新或稍后重试。") : reply.message);
                    }
                    gtk_label_set_text(GTK_LABEL(owner.operationState), message.c_str());
                    if (!reply.ok) gtk_style_context_add_class(gtk_widget_get_style_context(owner.operationState), "modernime-status-error");
                    owner.loaded = false;
                    owner.request();
                    if (owner.notify) owner.notify(message);
                });
            });
    }

    void mutate(const Identity &identity, bool forget) {
        if (!ready()) return;
        if (debounce) { g_source_remove(debounce); debounce = 0; }
        ++generation;
        mutating = true;
        g_object_set_data(G_OBJECT(root), "modernime-learning-mutation-busy", GINT_TO_POINTER(1));
        gtk_label_set_text(GTK_LABEL(operationState), forget ? "正在忘记所选词…" : "正在解除屏蔽…");
        gtk_style_context_remove_class(gtk_widget_get_style_context(operationState), "modernime-status-error");
        gtk_widget_show(operationState);
        update();
        backend.execute(forget ? "learning.forget" : "learning.unsuppress", {identity.phrase, identity.pinyin},
            [life = lifetime, forget](BackendReply reply) {
                life->withOwner([&](Impl &owner) {
                    owner.mutating = false;
                    g_object_set_data(G_OBJECT(owner.root), "modernime-learning-mutation-busy", nullptr);
                    const auto message = reply.ok
                        ? std::string(forget ? "已忘记所选词的学习记录。" : "已解除所选词的屏蔽。")
                        : reply.uncertain ? "操作结果未确认，请刷新列表核对后再操作。" +
                            (reply.message.empty() ? std::string{} : "\n" + reply.message)
                        : "操作失败：" + (reply.message.empty() ? std::string("请刷新后重试。") : reply.message);
                    gtk_label_set_text(GTK_LABEL(owner.operationState), message.c_str());
                    if (!reply.ok) gtk_style_context_add_class(gtk_widget_get_style_context(owner.operationState), "modernime-status-error");
                    // Re-query even after a failure; never retry a possibly started mutation.
                    owner.loaded = false;
                    owner.request();
                    if (owner.notify) owner.notify(message);
                });
            });
    }

    bool focusTarget(std::string_view id) {
        if (!uiAlive) return false;
        if (id.starts_with("learning-query:")) {
            if (mutating) return false;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(suppressedOnly), FALSE);
            gtk_entry_set_text(GTK_ENTRY(search), std::string(id.substr(15)).c_str());
            queryChanged(false);
            return focusWidgetOrFallback(search, root);
        }
        for (auto *control : {search, list, forgetButton, unsuppress, refreshButton, pruneStaleButton, suppressedOnly, retry, previous, next}) {
            const auto *name = static_cast<const char *>(g_object_get_data(G_OBJECT(control), "modernime-settings-target"));
            if (name && id == name) return focusWidgetOrFallback(control, root);
        }
        return false;
    }

    GtkWidget *root = nullptr;
    GtkWidget *search = nullptr;
    GtkWidget *suppressedOnly = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *pruneStaleButton = nullptr;
    GtkWidget *state = nullptr;
    GtkWidget *retry = nullptr;
    GtkWidget *list = nullptr;
    GtkListStore *store = nullptr;
    GtkWidget *content = nullptr;
    GtkWidget *empty = nullptr;
    GtkWidget *previous = nullptr;
    GtkWidget *next = nullptr;
    GtkWidget *pageStatus = nullptr;
    GtkWidget *selectionBar = nullptr;
    GtkWidget *detailLabel = nullptr;
    GtkWidget *forgetButton = nullptr;
    GtkWidget *unsuppress = nullptr;
    GtkWidget *operationState = nullptr;
    GtkWidget *activeDialog = nullptr;
    std::function<void(std::string)> notify;
    BackendClient backend;
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(this);
    std::vector<Record> records;
    std::string readError;
    std::uint64_t total = 0;
    std::uint64_t offset = 0;
    std::uint64_t generation = 0;
    guint debounce = 0;
    bool uiAlive = true;
    bool loaded = false;
    bool loading = false;
    bool mutating = false;
};

LearningManager::LearningManager(std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(std::move(notify))) {}
LearningManager::~LearningManager() = default;
_GtkWidget *LearningManager::widget() const { return impl_->root; }
void LearningManager::refresh() { impl_->refresh(); }
bool LearningManager::focusTarget(std::string_view target) { return impl_->focusTarget(target); }

} // namespace modernime::settings
