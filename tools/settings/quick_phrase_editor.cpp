#include "modernime/settings/quick_phrase_editor.h"

#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {
void target(GtkWidget *widget, const char *name) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(name), g_free);
}
GtkWidget *description(const char *text) {
    auto *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "modernime-description");
    return label;
}
bool contains(const std::vector<QuickPhraseEntry> &entries, const QuickPhraseEntry &entry) {
    return std::find(entries.begin(), entries.end(), entry) != entries.end();
}
}

class QuickPhraseEditor::Impl final {
public:
    enum class Operation { Read, Save, Apply, ReadConflict };
    enum class DialogKind { Edit, Delete, Merge };
    using Lifetime = detail::DiagnosticsLifetime<Impl>;

    Impl(std::filesystem::path file, std::function<void(std::string)> notifyCallback,
         std::function<QuickPhraseReloadResult()> reloadCallback)
        : path(std::move(file)), notify(std::move(notifyCallback)), reload(std::move(reloadCallback)) {
        detail::GtkWidgetGuard guard(createSectionCard("", "把常用的固定文本保存为缩写，在快速输入中调用。"));
        root = guard.get();
        target(root, "quick-phrases");
        setAccessibleWidgetText(root, "快捷短语", "把常用的固定文本保存为缩写，在快速输入中调用");
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_style_context_add_class(gtk_widget_get_style_context(header), "modernime-editor-header");
        auto *title = gtk_label_new("快捷短语");
        gtk_style_context_add_class(gtk_widget_get_style_context(title), "modernime-section-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
        auto *help = description("默认按 Super+;，输入缩写后按空格或数字选词，Esc 取消。\n需要启用 Fcitx5“快速输入 / Quick Phrase”插件；自定义快捷键以系统设置为准。");
        gtk_box_pack_start(GTK_BOX(root), help, FALSE, FALSE, 0);
        auto *searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        search = gtk_search_entry_new();
        target(search, "quick-phrase-search");
        gtk_entry_set_placeholder_text(GTK_ENTRY(search), "搜索缩写或固定文本");
        setAccessibleWidgetText(search, "搜索快捷短语", "按缩写或固定文本筛选，忽略英文大小写");
        gtk_widget_set_hexpand(search, TRUE);
        gtk_box_pack_start(GTK_BOX(searchRow), search, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), searchRow, FALSE, FALSE, 8);

        auto *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        add = button(tools, "添加短语", "quick-phrase-add", +[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->edit({}); });
        editButton = button(tools, "编辑", "quick-phrase-edit", +[](GtkButton *, gpointer p) {
            auto &owner = *static_cast<Impl *>(p); if (auto i = owner.selection()) owner.edit(i);
        });
        remove = button(tools, "删除", "quick-phrase-delete", +[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->removeSelected(); });
        refreshButton = button(tools, "重新读取", "quick-phrase-refresh", +[](GtkButton *, gpointer p) {
            auto &owner = *static_cast<Impl *>(p); owner.start(owner.conflict ? Operation::ReadConflict : Operation::Read);
        });
        gtk_box_pack_start(GTK_BOX(root), tools, FALSE, FALSE, 4);
        count = description("尚未读取快捷短语");
        gtk_box_pack_start(GTK_BOX(root), count, FALSE, FALSE, 4);

        detail::GObjectHandle<GtkListStore> storeOwner = detail::GObjectHandle<GtkListStore>::adopt(
            gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64));
        store = storeOwner.get();
        list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        target(list, "quick-phrase-list");
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(list), FALSE);
        setAccessibleWidgetText(list, "快捷短语列表", "选择短语后编辑或删除，双击编辑，Delete 删除");
        for (const auto &[name, column] : {std::pair{"缩写", 0}, std::pair{"固定文本", 1}}) {
            auto *renderer = gtk_cell_renderer_text_new();
            g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            auto *viewColumn = gtk_tree_view_column_new_with_attributes(name, renderer, "text", column, nullptr);
            gtk_tree_view_column_set_expand(viewColumn, column == 1);
            if (column == 0) gtk_tree_view_column_set_min_width(viewColumn, 100);
            gtk_tree_view_append_column(GTK_TREE_VIEW(list), viewColumn);
        }
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(scroll, -1, 180);
        gtk_container_add(GTK_CONTAINER(scroll), list);
        listStack = gtk_stack_new();
        empty = description("还没有快捷短语。点击“添加短语”，为常用文本设置缩写。");
        gtk_widget_set_margin_top(empty, 24);
        gtk_widget_set_margin_bottom(empty, 24);
        gtk_stack_add_named(GTK_STACK(listStack), scroll, "list");
        gtk_stack_add_named(GTK_STACK(listStack), empty, "empty");
        gtk_widget_set_vexpand(listStack, TRUE);
        gtk_box_pack_start(GTK_BOX(root), listStack, TRUE, TRUE, 4);

        state = description("");
        target(state, "quick-phrase-state");
        gtk_box_pack_start(GTK_BOX(root), state, FALSE, FALSE, 6);
        gtk_box_reorder_child(GTK_BOX(root), state, 4);
        details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto *expander = gtk_expander_new("查看详情");
        auto *revealer = gtk_revealer_new();
        auto *detailScroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detailScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(detailScroll), 100);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(detailScroll), TRUE);
        detailLabel = description("");
        gtk_label_set_max_width_chars(GTK_LABEL(detailLabel), 72);
        gtk_label_set_selectable(GTK_LABEL(detailLabel), TRUE);
        gtk_container_add(GTK_CONTAINER(detailScroll), detailLabel);
        gtk_container_add(GTK_CONTAINER(revealer), detailScroll);
        gtk_box_pack_start(GTK_BOX(details), expander, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(details), revealer, FALSE, FALSE, 0);
        bindSettingsDisclosure(expander, revealer);
        gtk_widget_show_all(details);
        gtk_widget_set_no_show_all(details, TRUE);
        gtk_box_pack_start(GTK_BOX(root), details, FALSE, FALSE, 0);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        save = button(actions, "保存短语", "quick-phrase-save", +[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->start(Operation::Save); });
        gtk_widget_set_tooltip_text(save, "保存并应用快捷短语（Ctrl+S）");
        gtk_style_context_add_class(gtk_widget_get_style_context(save), "suggested-action");
        discard = button(actions, "放弃短语修改", "quick-phrase-discard", +[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->discardDraft(); });
        gtk_widget_set_tooltip_text(discard, "放弃尚未保存的快捷短语草稿");
        merge = button(actions, "检查并合并", "quick-phrase-merge", +[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->reviewMerge(); });
        apply = button(actions, "重试应用", "quick-phrase-apply", +[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->start(Operation::Apply); });
        for (auto *control : {merge, apply}) gtk_widget_set_no_show_all(control, TRUE);
        gtk_box_pack_end(GTK_BOX(header), actions, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), description("短语在此单独保存并应用。放弃设置不会删除已保存的短语。"), FALSE, FALSE, 0);
        g_signal_connect(search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer p) { static_cast<Impl *>(p)->render(); }), this);
        g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), "changed", G_CALLBACK(+[](GtkTreeSelection *, gpointer p) { static_cast<Impl *>(p)->update(); }), this);
        g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer p) {
            auto &owner = *static_cast<Impl *>(p); if (auto i = owner.selection()) owner.edit(i);
        }), this);
        g_signal_connect(root, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer p) -> gboolean {
            auto &owner = *static_cast<Impl *>(p);
            if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_s || event->keyval == GDK_KEY_S)) {
                if (gtk_widget_is_sensitive(owner.save)) owner.start(Operation::Save);
                return TRUE;
            }
            if (event->keyval == GDK_KEY_Delete && gtk_widget_has_focus(owner.list)) {
                owner.removeSelected(); return TRUE;
            }
            return FALSE;
        }), this);
        g_signal_connect(root, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Impl *>(p)->deactivate(); }), this);
        setSettingsFocusChain(root, {save, discard, merge, apply, search, add, editButton, remove, refreshButton, list});
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
        if (renderSource) { g_source_remove(renderSource); renderSource = 0; }
        if (activeDialog) { gtk_widget_destroy(activeDialog); activeDialog = nullptr; }
    }

    GtkWidget *button(GtkWidget *box, const char *label, const char *id, void (*handler)(GtkButton *, gpointer)) {
        auto *result = gtk_button_new_with_label(label);
        target(result, id);
        setAccessibleWidgetText(result, label, label);
        g_signal_connect(result, "clicked", G_CALLBACK(handler), this);
        gtk_box_pack_start(GTK_BOX(box), result, FALSE, FALSE, 0);
        return result;
    }

    bool editable() const { return uiAlive && loaded && baseline.error.empty() && !busy; }
    std::optional<std::size_t> selection() const {
        GtkTreeIter iter;
        GtkTreeModel *model = nullptr;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), &model, &iter)) return {};
        guint64 index = 0;
        gtk_tree_model_get(model, &iter, 2, &index, -1);
        return index < entries.size() ? std::optional<std::size_t>(index) : std::nullopt;
    }

    void update() {
        if (!uiAlive) return;
        const bool canEdit = editable();
        const bool selected = selection().has_value();
        gtk_widget_set_sensitive(add, canEdit);
        gtk_widget_set_sensitive(editButton, canEdit && selected);
        gtk_widget_set_sensitive(remove, canEdit && selected);
        gtk_widget_set_sensitive(save, canEdit && dirty && !conflict);
        gtk_widget_set_sensitive(discard, !busy && dirty);
        gtk_widget_set_sensitive(refreshButton, !busy && (!dirty || conflict));
        gtk_button_set_label(GTK_BUTTON(refreshButton), conflict ? "重新读取更新" : "重新读取");
        gtk_widget_set_visible(merge, conflict);
        gtk_widget_set_sensitive(merge, !busy && incoming && incoming->error.empty());
        gtk_widget_set_visible(apply, pendingApply);
        gtk_widget_set_sensitive(apply, !busy && pendingApply);
        std::string message;
        if (busy) {
            message = activeOperation == Operation::Save ? "正在保存并应用快捷短语…"
                : activeOperation == Operation::Apply ? "正在应用已保存的短语…" : "正在读取快捷短语…";
        } else if (conflict) message = "文件已被其他程序修改。草稿已保留，请检查合并结果后保存。";
        else if (!baseline.error.empty()) message = pendingApply ? "已保存，暂未应用；重新读取失败，请查看详情后重试。"
            : applied ? "已保存并应用；重新读取失败，请查看详情后重试。"
            : "无法读取快捷短语，请查看详情，修复后重新读取。";
        else if (dirty) message = pendingApply ? "有未保存的修改；上次保存的短语暂未应用。" : "有未保存的短语修改。";
        else if (pendingApply) message = "已保存，暂未应用。请启动 Fcitx5 并启用“快速输入”插件，再重试应用。";
        else if (applied) message = "已保存并应用，可以使用快捷键输入。";
        else message = loaded ? "修改后点击“保存短语”，即可保存并应用。" : "进入快捷短语后读取列表。";
        if (!operationError.empty() && !busy && !conflict && dirty && activeOperation == Operation::Save)
            message = "保存失败，草稿已保留。请查看详情后重试。";
        gtk_label_set_text(GTK_LABEL(state), message.c_str());
        const auto &error = !operationError.empty() ? operationError : baseline.error;
        gtk_label_set_text(GTK_LABEL(detailLabel), error.c_str());
        gtk_widget_set_visible(details, !error.empty());
        auto *style = gtk_widget_get_style_context(state);
        if (!error.empty() || conflict) gtk_style_context_add_class(style, "modernime-status-error");
        else gtk_style_context_remove_class(style, "modernime-status-error");
    }

    void render() {
        if (!uiAlive) return;
        if (renderSource) { g_source_remove(renderSource); renderSource = 0; }
        gtk_list_store_clear(store);
        indices = filterQuickPhrases(entries, gtk_entry_get_text(GTK_ENTRY(search)));
        const auto text = !loaded || !baseline.error.empty() ? "尚未成功读取快捷短语"
            : "共 " + std::to_string(entries.size()) + " 条，当前显示 " + std::to_string(indices.size()) + " 条";
        gtk_label_set_text(GTK_LABEL(count), text.c_str());
        gtk_label_set_text(GTK_LABEL(empty), !baseline.error.empty() ? "暂时无法显示短语，请重新读取。"
            : entries.empty() ? "还没有快捷短语。点击“添加短语”，为常用文本设置缩写。" : "没有匹配的短语，试试其他缩写或关键词。");
        gtk_stack_set_visible_child_name(GTK_STACK(listStack), indices.empty() ? "empty" : "list");
        renderIndex = 0;
        if (!indices.empty()) renderSource = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer p) -> gboolean {
            auto &owner = *static_cast<Impl *>(p);
            const auto start = std::chrono::steady_clock::now();
            std::size_t rows = 0;
            while (owner.renderIndex < owner.indices.size()) {
                const auto index = owner.indices[owner.renderIndex++];
                const auto &entry = owner.entries[index];
                GtkTreeIter iter;
                gtk_list_store_append(owner.store, &iter);
                gtk_list_store_set(owner.store, &iter, 0, entry.abbreviation.c_str(), 1, entry.text.c_str(), 2, static_cast<guint64>(index), -1);
                if (++rows == 200 || std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(4)) break;
            }
            if (owner.renderIndex < owner.indices.size()) return G_SOURCE_CONTINUE;
            owner.renderSource = 0;
            return G_SOURCE_REMOVE;
        }, this, nullptr);
        update();
    }

    struct Request {
        std::shared_ptr<Lifetime> lifetime;
        uint64_t generation;
        Operation operation;
        std::filesystem::path path;
        QuickPhraseSnapshot baseline;
        std::vector<QuickPhraseEntry> entries;
        std::function<QuickPhraseReloadResult()> reload;
    };
    struct Result {
        QuickPhraseSaveResult saved;
        QuickPhraseSnapshot snapshot;
        QuickPhraseReloadResult applied;
        std::string error;
    };

    void start(Operation operation) {
        if (!uiAlive || busy || activeDialog) return;
        if (operation == Operation::Save && (!editable() || !dirty || conflict)) return;
        if (operation == Operation::Read && dirty) return;
        if (operation == Operation::Apply && !pendingApply) return;
        busy = true;
        activeOperation = operation;
        operationError.clear();
        auto *operationRequest = new Request{lifetime, ++generation, operation, path, baseline, entries, reload};
        auto *asyncTask = g_task_new(nullptr, nullptr, +[](GObject *, GAsyncResult *async, gpointer) {
            const auto &request = *static_cast<Request *>(g_task_get_task_data(G_TASK(async)));
            auto result = std::unique_ptr<Result>(static_cast<Result *>(g_task_propagate_pointer(G_TASK(async), nullptr)));
            request.lifetime->withOwner([&](Impl &owner) {
                if (owner.generation == request.generation && result) owner.finish(request, std::move(*result));
            });
        }, nullptr);
        g_task_set_task_data(asyncTask, operationRequest, +[](gpointer p) { delete static_cast<Request *>(p); });
        g_task_run_in_thread(asyncTask, +[](GTask *workerTask, gpointer, gpointer p, GCancellable *) {
            const auto &request = *static_cast<Request *>(p);
            auto result = std::make_unique<Result>();
            try {
                if (request.operation == Operation::Save) {
                    result->saved = saveQuickPhrases(request.path, request.baseline, request.entries);
                    if (result->saved.saved() || result->saved.status == QuickPhraseSaveStatus::Conflict)
                        result->snapshot = loadQuickPhrases(request.path);
                    if (result->saved.saved()) result->applied = request.reload();
                } else if (request.operation == Operation::Apply) result->applied = request.reload();
                else result->snapshot = loadQuickPhrases(request.path);
            } catch (const std::exception &error) { result->error = error.what(); }
            g_task_return_pointer(workerTask, result.release(), +[](gpointer raw) { delete static_cast<Result *>(raw); });
        });
        g_object_unref(asyncTask);
        update();
    }

    void finish(const Request &request, Result result) {
        busy = false;
        operationError = std::move(result.error);
        bool changed = false;
        if (request.operation == Operation::Save) {
            if (result.saved.saved()) {
                baseline = std::move(result.snapshot);
                entries = baseline.error.empty() && operationError.empty() ? baseline.entries : request.entries;
                dirty = false;
                loaded = true;
                conflict = false;
                incoming.reset();
                pendingApply = !result.applied.applied;
                applied = result.applied.applied;
                if (operationError.empty()) operationError = result.applied.error;
                changed = true;
                if (notify) notify(pendingApply ? "快捷短语已保存，暂未应用" : "快捷短语已保存并应用");
            } else if (result.saved.status == QuickPhraseSaveStatus::Conflict) {
                conflict = true;
                incoming = std::move(result.snapshot);
                operationError = incoming->error.empty() ? result.saved.error : incoming->error;
            } else if (operationError.empty()) operationError = result.saved.error;
        } else if (request.operation == Operation::Apply) {
            pendingApply = !result.applied.applied;
            applied = result.applied.applied;
            if (operationError.empty()) operationError = result.applied.error;
            if (notify) notify(applied ? "快捷短语已应用" : "快捷短语已保存，暂未应用");
        } else if (request.operation == Operation::ReadConflict) {
            incoming = std::move(result.snapshot);
            if (operationError.empty()) operationError = incoming->error;
        } else {
            if (loaded && (baseline.content != result.snapshot.content || baseline.exists != result.snapshot.exists)) {
                applied = false;
                pendingApply = result.snapshot.error.empty() && operationError.empty();
            }
            baseline = std::move(result.snapshot);
            if (!operationError.empty()) baseline.error = operationError;
            entries = baseline.error.empty() ? baseline.entries : std::vector<QuickPhraseEntry>{};
            loaded = true;
            dirty = false;
            changed = true;
        }
        if (changed) render();
        else update();
    }

    struct Dialog {
        std::shared_ptr<Lifetime> lifetime;
        DialogKind kind;
        std::optional<std::size_t> index;
        GtkWidget *abbreviation = nullptr, *text = nullptr, *validation = nullptr;
        GtkListStore *mergeStore = nullptr;
        QuickPhraseSnapshot latest;
        std::vector<QuickPhraseEntry> proposed;
    };

    GtkWidget *newDialog(const char *title, const char *accept, DialogKind kind, Dialog **context) {
        auto *top = gtk_widget_get_toplevel(root);
        auto *popup = gtk_dialog_new_with_buttons(title, GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            "取消", GTK_RESPONSE_CANCEL, accept, GTK_RESPONSE_ACCEPT, nullptr);
        activeDialog = popup;
        gtk_style_context_add_class(gtk_widget_get_style_context(popup), "modernime-settings");
        gtk_window_set_default_size(GTK_WINDOW(popup), 560, -1);
        gtk_dialog_set_default_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT);
        setDialogResponseAccessibility(GTK_DIALOG(popup), GTK_RESPONSE_CANCEL, "取消", "保留当前草稿并关闭对话框");
        setDialogResponseAccessibility(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT, accept, accept);
        gtk_style_context_add_class(gtk_widget_get_style_context(
            gtk_dialog_get_widget_for_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT)),
            kind == DialogKind::Delete ? "destructive-action" : "suggested-action");
        auto *area = gtk_dialog_get_content_area(GTK_DIALOG(popup));
        gtk_container_set_border_width(GTK_CONTAINER(area), 18);
        gtk_box_set_spacing(GTK_BOX(area), 12);
        auto *contextData = new Dialog{};
        contextData->lifetime = lifetime;
        contextData->kind = kind;
        *context = contextData;
        g_object_set_data_full(G_OBJECT(popup), "modernime-quick-phrase-dialog", contextData, +[](gpointer p) { delete static_cast<Dialog *>(p); });
        g_signal_connect(popup, "destroy", G_CALLBACK(+[](GtkWidget *dialog, gpointer p) {
            auto &data = *static_cast<Dialog *>(p);
            data.lifetime->withOwner([&](Impl &owner) { if (owner.activeDialog == dialog) owner.activeDialog = nullptr; });
        }), contextData);
        g_signal_connect(popup, "response", G_CALLBACK((+[](GtkDialog *dialog, gint response, gpointer p) {
            auto &data = *static_cast<Dialog *>(p);
            bool close = true;
            if (response == GTK_RESPONSE_ACCEPT) data.lifetime->withOwner([&](Impl &owner) {
                if (!owner.editable()) { close = false; return; }
                if (data.kind == DialogKind::Edit) {
                    if (!owner.validDialog(data)) { close = false; return; }
                    QuickPhraseEntry entry{gtk_entry_get_text(GTK_ENTRY(data.abbreviation)), gtk_entry_get_text(GTK_ENTRY(data.text))};
                    if (data.index) owner.entries[*data.index] = std::move(entry);
                    else owner.entries.push_back(std::move(entry));
                } else if (data.kind == DialogKind::Delete) {
                    if (data.index && *data.index < owner.entries.size()) owner.entries.erase(owner.entries.begin() + static_cast<std::ptrdiff_t>(*data.index));
                } else {
                    if (owner.baseline.content != data.latest.content || owner.baseline.exists != data.latest.exists) {
                        owner.applied = false;
                        owner.pendingApply = true;
                    }
                    owner.baseline = std::move(data.latest);
                    owner.entries.clear();
                    GtkTreeIter iter;
                    bool present = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(data.mergeStore), &iter);
                    std::size_t index = 0;
                    while (present) {
                        gboolean keep = FALSE;
                        gtk_tree_model_get(GTK_TREE_MODEL(data.mergeStore), &iter, 0, &keep, -1);
                        if (keep) owner.entries.push_back(data.proposed[index]);
                        ++index;
                        present = gtk_tree_model_iter_next(GTK_TREE_MODEL(data.mergeStore), &iter);
                    }
                    owner.conflict = false;
                    owner.incoming.reset();
                }
                owner.dirty = owner.entries != owner.baseline.entries;
                owner.operationError.clear();
                owner.render();
            });
            if (close) gtk_widget_destroy(GTK_WIDGET(dialog));
        })), contextData);
        return popup;
    }

    bool validDialog(Dialog &data) {
        const QuickPhraseEntry entry{gtk_entry_get_text(GTK_ENTRY(data.abbreviation)), gtk_entry_get_text(GTK_ENTRY(data.text))};
        auto error = quickPhraseValidation(entry);
        if (error.empty()) for (std::size_t i = 0; i < entries.size(); ++i) {
            if ((!data.index || i != *data.index) && entries[i] == entry) { error = "相同的缩写和文本已存在。"; break; }
        }
        gtk_label_set_text(GTK_LABEL(data.validation), error.empty() ? "确认后加入草稿，点击“保存短语”保存并应用。" : error.c_str());
        if (activeDialog) gtk_dialog_set_response_sensitive(GTK_DIALOG(activeDialog), GTK_RESPONSE_ACCEPT, error.empty());
        return error.empty();
    }

    void edit(std::optional<std::size_t> index) {
        if (!editable() || activeDialog || (index && *index >= entries.size())) return;
        Dialog *data = nullptr;
        auto *dialog = newDialog(index ? "编辑快捷短语" : "添加快捷短语", "确认修改", DialogKind::Edit, &data);
        data->index = index;
        auto *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        data->abbreviation = gtk_entry_new();
        data->text = gtk_entry_new();
        target(data->abbreviation, "quick-phrase-abbreviation");
        target(data->text, "quick-phrase-text");
        gtk_entry_set_max_length(GTK_ENTRY(data->abbreviation), 30);
        gtk_entry_set_placeholder_text(GTK_ENTRY(data->abbreviation), "例如 dz，1–30 个小写英文字母");
        gtk_entry_set_placeholder_text(GTK_ENTRY(data->text), "填写要输入的完整单行文本");
        for (auto *entry : {data->abbreviation, data->text}) gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        setAccessibleWidgetText(data->abbreviation, "缩写", "1–30 个小写英文字母");
        setAccessibleWidgetText(data->text, "固定文本", "单行文本，保留原有空格、引号和反斜杠");
        if (index) {
            gtk_entry_set_text(GTK_ENTRY(data->abbreviation), entries[*index].abbreviation.c_str());
            gtk_entry_set_text(GTK_ENTRY(data->text), entries[*index].text.c_str());
        }
        gtk_box_pack_start(GTK_BOX(area), description("缩写"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(area), data->abbreviation, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(area), description("固定文本"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(area), data->text, FALSE, FALSE, 0);
        data->validation = description("");
        gtk_box_pack_start(GTK_BOX(area), data->validation, FALSE, FALSE, 0);
        for (auto *entry : {data->abbreviation, data->text}) g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable *, gpointer p) {
            auto &entryState = *static_cast<Dialog *>(p);
            entryState.lifetime->withOwner([&](Impl &owner) { owner.validDialog(entryState); });
        }), data);
        validDialog(*data);
        gtk_widget_show_all(dialog);
        gtk_widget_grab_focus(index ? data->text : data->abbreviation);
    }

    void removeSelected() {
        const auto index = selection();
        if (!editable() || activeDialog || !index) return;
        Dialog *data = nullptr;
        auto *dialog = newDialog("删除这条快捷短语？", "删除", DialogKind::Delete, &data);
        data->index = index;
        auto *text = description((entries[*index].abbreviation + "  " + entries[*index].text).c_str());
        gtk_label_set_max_width_chars(GTK_LABEL(text), 60);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), text, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), description("确认后从草稿移除，保存短语后生效。"), FALSE, FALSE, 0);
        gtk_widget_show_all(dialog);
    }

    void reviewMerge() {
        if (!editable() || activeDialog || !conflict || !incoming || !incoming->error.empty()) return;
        Dialog *data = nullptr;
        auto *dialog = newDialog("检查快捷短语合并结果", "使用合并结果", DialogKind::Merge, &data);
        data->latest = *incoming;
        // Preserve external additions; replay only removals/additions relative to our unchanged baseline.
        for (const auto &entry : incoming->entries)
            if (!contains(baseline.entries, entry) || contains(entries, entry)) data->proposed.push_back(entry);
        for (const auto &entry : entries)
            if (!contains(baseline.entries, entry) && !contains(data->proposed, entry)) data->proposed.push_back(entry);
        auto *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 440);
        gtk_box_pack_start(GTK_BOX(area), description("外部新增与草稿修改已合在下方。勾选要保留的短语；同一缩写可以保留不同文本。\n确认只更新草稿，检查后仍需点击“保存短语”。"), FALSE, FALSE, 0);
        detail::GObjectHandle<GtkListStore> storeOwner = detail::GObjectHandle<GtkListStore>::adopt(
            gtk_list_store_new(3, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING));
        data->mergeStore = storeOwner.get();
        for (const auto &entry : data->proposed) {
            GtkTreeIter iter;
            gtk_list_store_append(data->mergeStore, &iter);
            gtk_list_store_set(data->mergeStore, &iter, 0, TRUE, 1, entry.abbreviation.c_str(), 2, entry.text.c_str(), -1);
        }
        auto *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(data->mergeStore));
        auto *toggle = gtk_cell_renderer_toggle_new();
        g_signal_connect(toggle, "toggled", G_CALLBACK(+[](GtkCellRendererToggle *, gchar *rowPath, gpointer p) {
            auto *mergeModel = GTK_LIST_STORE(p);
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(mergeModel), &iter, rowPath)) {
                gboolean keep = FALSE;
                gtk_tree_model_get(GTK_TREE_MODEL(mergeModel), &iter, 0, &keep, -1);
                gtk_list_store_set(mergeModel, &iter, 0, !keep, -1);
            }
        }), data->mergeStore);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("保留", toggle, "active", 0, nullptr));
        for (const auto &[name, column] : {std::pair{"缩写", 1}, std::pair{"固定文本", 2}}) {
            auto *renderer = gtk_cell_renderer_text_new();
            g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            auto *viewColumn = gtk_tree_view_column_new_with_attributes(name, renderer, "text", column, nullptr);
            gtk_tree_view_column_set_expand(viewColumn, column == 2);
            gtk_tree_view_append_column(GTK_TREE_VIEW(view), viewColumn);
        }
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_container_add(GTK_CONTAINER(scroll), view);
        gtk_box_pack_start(GTK_BOX(area), scroll, TRUE, TRUE, 0);
        gtk_widget_show_all(dialog);
    }

    void discardDraft() {
        if (busy || activeDialog || !dirty) return;
        entries = baseline.entries;
        dirty = false;
        conflict = false;
        incoming.reset();
        operationError.clear();
        start(Operation::Read);
    }

    bool confirmLeave() {
        if (!uiAlive) return true;
        if (busy && activeOperation == Operation::Save) {
            if (notify) notify("正在保存快捷短语，请稍候再关闭或更新。");
            return false;
        }
        if (!dirty) return true;
        auto *top = gtk_widget_get_toplevel(root);
        auto *dialog = gtk_message_dialog_new(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "快捷短语还有未保存的修改");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "继续编辑后点击“保存短语”，或放弃本次短语草稿。已保存的内容不受影响。");
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "modernime-settings");
        gtk_dialog_add_button(GTK_DIALOG(dialog), "继续编辑", GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(dialog), "放弃修改并继续", GTK_RESPONSE_REJECT);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != GTK_RESPONSE_REJECT) return false;
        discardDraft();
        return true;
    }

    bool focusTarget(std::string_view name) {
        for (auto *control : {root, search, add, editButton, remove, refreshButton, save, discard, merge, apply}) {
            const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(control), "modernime-settings-target"));
            if (id && name == id) return focusWidgetOrFallback(control == root ? search : control, root);
        }
        return false;
    }

    std::filesystem::path path;
    std::function<void(std::string)> notify;
    std::function<QuickPhraseReloadResult()> reload;
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(this);
    QuickPhraseSnapshot baseline;
    std::optional<QuickPhraseSnapshot> incoming;
    std::vector<QuickPhraseEntry> entries;
    std::vector<std::size_t> indices;
    GtkWidget *root = nullptr, *search = nullptr, *count = nullptr, *list = nullptr, *listStack = nullptr, *empty = nullptr;
    GtkWidget *state = nullptr, *details = nullptr, *detailLabel = nullptr;
    GtkWidget *add = nullptr, *editButton = nullptr, *remove = nullptr, *refreshButton = nullptr;
    GtkWidget *save = nullptr, *discard = nullptr, *merge = nullptr, *apply = nullptr, *activeDialog = nullptr;
    GtkListStore *store = nullptr;
    uint64_t generation = 0;
    guint renderSource = 0;
    std::size_t renderIndex = 0;
    Operation activeOperation = Operation::Read;
    bool loaded = false, dirty = false, busy = false, conflict = false, pendingApply = false, applied = false, uiAlive = true;
    std::string operationError;
};

QuickPhraseEditor::QuickPhraseEditor(std::filesystem::path path, std::function<void(std::string)> notify,
                                   std::function<QuickPhraseReloadResult()> reload)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(notify), std::move(reload))) {}
QuickPhraseEditor::~QuickPhraseEditor() = default;
GtkWidget *QuickPhraseEditor::widget() const { return impl_->root; }
void QuickPhraseEditor::refresh() { if (!impl_->dirty && !impl_->busy) impl_->start(Impl::Operation::Read); }
bool QuickPhraseEditor::focusTarget(std::string_view target) { return impl_->focusTarget(target); }
bool QuickPhraseEditor::hasUnsavedChanges() const { return impl_->dirty; }
bool QuickPhraseEditor::confirmLeave() { return impl_->confirmLeave(); }

} // namespace modernime::settings
