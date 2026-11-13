#include "modernime/settings/candidate_pin_dialog.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_widgets.h"
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {
void target(GtkWidget *widget, const char *id) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(id), g_free);
}
GtkWidget *description(const char *text) {
    auto *label = gtk_label_new(text); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(label), 70);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "modernime-description"); return label;
}
GtkWidget *button(GtkWidget *box, const char *label, const char *id) {
    auto *widget = gtk_button_new_with_label(label); target(widget, id);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0); return widget;
}
GtkWidget *tree(GtkWidget *box, const char *id, bool rules) {
    auto *store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64);
    auto *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)); g_object_unref(store); target(view, id);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);
    for (int column = 0; column < 3; ++column) {
        auto *renderer = gtk_cell_renderer_text_new(); g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        const auto *title = rules ? (column == 0 ? "方案" : column == 1 ? "输入按键" : "置顶候选") : (column == 0 ? "候选" : column == 1 ? "完整拼音" : "");
        auto *viewColumn = gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, nullptr);
        gtk_tree_view_column_set_expand(viewColumn, TRUE); gtk_tree_view_append_column(GTK_TREE_VIEW(view), viewColumn);
        if (!rules && column == 2) gtk_tree_view_column_set_visible(viewColumn, FALSE);
    }
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), view); gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    return view;
}
std::optional<std::size_t> selection(GtkWidget *view) {
    GtkTreeModel *model = nullptr; GtkTreeIter iter; guint64 index = 0;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model, &iter)) return {};
    gtk_tree_model_get(model, &iter, 3, &index, -1); return static_cast<std::size_t>(index);
}
void append(GtkWidget *view, const char *a, const char *b, const char *c, std::size_t index) {
    auto *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(view))); GtkTreeIter iter;
    gtk_list_store_append(store, &iter); gtk_list_store_set(store, &iter, 0, a, 1, b, 2, c, 3, static_cast<guint64>(index), -1);
}
struct PinDialog {
    using Lifetime = detail::DiagnosticsLifetime<PinDialog>;
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(this);
    core::ModernIMESettings settings;
    std::function<void(std::vector<core::CandidatePin>)> accept;
    std::vector<core::CandidatePin> choices;
    std::optional<std::size_t> replacing;
    GtkWidget *dialog, *stack, *rules, *search, *count, *edit, *remove, *add;
    GtkWidget *scheme, *input, *query, *candidates, *status, *choose, *more, *details, *detailLabel;
    unsigned generation = 0;
    bool busy = false;
    std::size_t limit = 32;
    core::PinyinScheme currentScheme() const { return core::kPinyinSchemes[std::clamp(settingsChoiceActive(scheme), 0, 2)].scheme; }
    bool sameScope(const core::CandidatePin &a, const core::CandidatePin &b) const { return a.scheme == b.scheme && a.input == b.input; }
    void ruleSelection() {
        const bool selected = selection(rules).has_value(); gtk_widget_set_sensitive(edit, selected); gtk_widget_set_sensitive(remove, selected);
    }
    void render() {
        auto *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(rules))); gtk_list_store_clear(store);
        auto *fold = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(search)), -1); const std::string queryText(fold); g_free(fold);
        std::size_t visible = 0;
        for (std::size_t i = 0; i < settings.candidatePins.size(); ++i) {
            const auto &pin = settings.candidatePins[i]; const char *name = "";
            for (const auto &option : core::kPinyinSchemes) if (option.scheme == pin.scheme) name = option.label;
            auto *text = g_utf8_casefold((std::string(name) + " " + pin.input + " " + pin.text + " " + pin.fullPinyin).c_str(), -1);
            const bool matches = std::string_view(text).find(queryText) != std::string_view::npos; g_free(text);
            if (matches) { append(rules, name, pin.input.c_str(), pin.text.c_str(), i); ++visible; }
        }
        const auto message = settings.candidatePins.empty() ? std::string("还没有置顶规则。添加后只影响对应方案和输入。")
            : "共 " + std::to_string(settings.candidatePins.size()) + " 条，当前显示 " + std::to_string(visible) + " 条";
        gtk_label_set_text(GTK_LABEL(count), message.c_str()); ruleSelection();
    }
    void invalidate() {
        ++generation; choices.clear(); limit = 32;
        gtk_list_store_clear(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(candidates))));
        gtk_widget_set_sensitive(choose, FALSE); gtk_widget_set_sensitive(more, FALSE);
        gtk_label_set_text(GTK_LABEL(status), "填写实际输入按键，点击“查找候选”。"); gtk_widget_hide(details);
    }
    void open(std::optional<std::size_t> index) {
        replacing = index; invalidate();
        const auto selectedScheme = index ? settings.candidatePins[*index].scheme : settings.pinyinScheme;
        for (std::size_t i = 0; i < core::kPinyinSchemes.size(); ++i) if (core::kPinyinSchemes[i].scheme == selectedScheme) setSettingsChoiceActive(scheme, static_cast<int>(i));
        gtk_entry_set_text(GTK_ENTRY(input), index ? settings.candidatePins[*index].input.c_str() : "");
        gtk_widget_set_sensitive(scheme, !index); gtk_widget_set_sensitive(input, !index);
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "picker");
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, FALSE);
        gtk_widget_grab_focus(index ? query : input);
    }
    void back() {
        invalidate(); replacing.reset(); gtk_stack_set_visible_child_name(GTK_STACK(stack), "rules");
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
        gtk_widget_grab_focus(add);
    }
    void updateChoose() {
        const auto index = selection(candidates);
        const bool valid = index && *index < choices.size();
        bool replacingScope = false;
        if (valid) for (const auto &pin : settings.candidatePins) if (sameScope(pin, choices[*index])) replacingScope = true;
        gtk_widget_set_sensitive(choose, valid && !busy);
        gtk_button_set_label(GTK_BUTTON(choose), replacingScope ? "替换该输入的置顶" : "置顶所选候选");
    }
    void use() {
        const auto index = selection(candidates); if (busy || !index || *index >= choices.size()) return;
        const auto pin = choices[*index]; auto updated = settings;
        if (replacing) updated.candidatePins.erase(updated.candidatePins.begin() + static_cast<std::ptrdiff_t>(*replacing));
        std::erase_if(updated.candidatePins, [&](const auto &existing) { return sameScope(existing, pin); });
        updated.candidatePins.push_back(pin);
        for (const auto &issue : core::validateSettings(updated).issues) if (issue.key == "candidate.pin") {
            gtk_label_set_text(GTK_LABEL(status), issue.message.c_str()); return;
        }
        settings = std::move(updated); render(); back();
    }
    struct Request {
        std::shared_ptr<Lifetime> lifetime;
        unsigned generation;
        core::SettingsPaths paths;
        core::ModernIMESettings settings;
        core::PinyinScheme scheme;
        std::string input;
        std::size_t limit;
        PinCandidateResult result;
    };
    void lookup() {
        if (busy) return;
        busy = true; choices.clear();
        gtk_list_store_clear(GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(candidates))));
        gtk_widget_set_sensitive(query, FALSE); gtk_widget_set_sensitive(choose, FALSE); gtk_widget_set_sensitive(more, FALSE);
        gtk_label_set_text(GTK_LABEL(status), "正在查找完整候选…"); gtk_widget_hide(details);
        const auto env = [](const char *name) { const auto *value = std::getenv(name); return value ? value : ""; };
        auto *task = g_task_new(nullptr, nullptr, +[](GObject *, GAsyncResult *result, gpointer) {
            auto &request = *static_cast<Request *>(g_task_get_task_data(G_TASK(result)));
            request.lifetime->withOwner([&](PinDialog &o) {
                o.busy = false; gtk_widget_set_sensitive(o.query, TRUE);
                if (o.generation != request.generation) return;
                if (!request.result.error.empty()) {
                    gtk_label_set_text(GTK_LABEL(o.status), "查找失败，请检查输入或数据安装后重试。");
                    gtk_label_set_text(GTK_LABEL(o.detailLabel), request.result.error.substr(0, 1000).c_str());
                    gtk_widget_show(o.details); return;
                }
                o.choices = std::move(request.result.candidates);
                for (std::size_t i = 0; i < o.choices.size(); ++i) append(o.candidates, o.choices[i].text.c_str(), o.choices[i].fullPinyin.c_str(), "", i);
                gtk_label_set_text(GTK_LABEL(o.status), o.choices.empty() ? "没有可置顶的完整候选。请检查方案或输入更完整的拼音。" : "选择完整候选作为置顶；不在这里提交或学习词条。");
                gtk_widget_set_sensitive(o.more, request.result.hasMore && o.limit < 1024);
            });
        }, nullptr);
        g_task_set_task_data(task, new Request{lifetime, generation, core::SettingsPaths::fromEnvironment(env("XDG_CONFIG_HOME"), env("XDG_DATA_HOME"), env("HOME")), settings,
            currentScheme(), gtk_entry_get_text(GTK_ENTRY(input)), limit, {}}, +[](gpointer p) { delete static_cast<Request *>(p); });
        g_task_run_in_thread(task, +[](GTask *t, gpointer, gpointer p, GCancellable *) {
            auto &request = *static_cast<Request *>(p);
            request.result = queryPinCandidates(request.paths, request.settings, request.scheme, request.input, request.limit);
            g_task_return_boolean(t, TRUE);
        });
        g_object_unref(task);
    }
};
}

GtkWidget *createCandidatePinDialog(GtkWindow *parent, const core::ModernIMESettings &settings,
    std::function<void(std::vector<core::CandidatePin>)> accept) {
    auto *o = new PinDialog; o->settings = settings; o->accept = std::move(accept);
    o->dialog = gtk_dialog_new_with_buttons("候选置顶规则", parent, static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "取消", GTK_RESPONSE_CANCEL, "使用这些规则", GTK_RESPONSE_ACCEPT, nullptr);
    auto *dialog = o->dialog; gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "modernime-settings");
    setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, "取消", "保留原来的置顶规则");
    setDialogResponseAccessibility(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, "使用这些规则", "写入设置草稿，仍需在主窗口保存");
    gtk_style_context_add_class(gtk_widget_get_style_context(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)), "suggested-action");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 560);
    auto *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(body), 20); gtk_box_set_spacing(GTK_BOX(body), 12);
    gtk_box_pack_start(GTK_BOX(body), description("规则只提升已召回且未屏蔽的完整候选，不生成新词。取消规则后恢复原排序。"), FALSE, FALSE, 0);
    o->stack = gtk_stack_new(); gtk_stack_set_homogeneous(GTK_STACK(o->stack), FALSE); gtk_box_pack_start(GTK_BOX(body), o->stack, TRUE, TRUE, 0);
    auto *manager = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_stack_add_named(GTK_STACK(o->stack), manager, "rules");
    o->search = gtk_search_entry_new(); target(o->search, "pin-search"); gtk_entry_set_placeholder_text(GTK_ENTRY(o->search), "搜索方案、输入或置顶词");
    setAccessibleWidgetText(o->search, "搜索置顶规则", "按方案、输入、候选文本或拼音筛选");
    gtk_box_pack_start(GTK_BOX(manager), o->search, FALSE, FALSE, 0);
    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_pack_start(GTK_BOX(manager), actions, FALSE, FALSE, 0);
    o->add = button(actions, "添加置顶", "pin-add"); o->edit = button(actions, "更换候选", "pin-edit"); o->remove = button(actions, "取消置顶", "pin-delete");
    o->rules = tree(manager, "pin-rules", true); o->count = description(""); gtk_box_pack_start(GTK_BOX(manager), o->count, FALSE, FALSE, 0);
    auto *picker = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_stack_add_named(GTK_STACK(o->stack), picker, "picker");
    auto *back = button(picker, "返回规则列表", "pin-back");
    gtk_box_pack_start(GTK_BOX(picker), description("选择输入方案并填写实际按键，再从完整候选中选择。此处不会修改词典或记录学习。"), FALSE, FALSE, 0);
    o->scheme = createSettingsChoice(); target(o->scheme, "pin-scheme"); for (const auto &scheme : core::kPinyinSchemes) appendSettingsChoice(o->scheme, scheme.label);
    gtk_box_pack_start(GTK_BOX(picker), createSettingRow("拼音方案", "全拼和两种双拼分别保存", o->scheme), FALSE, FALSE, 0);
    auto *queryRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_pack_start(GTK_BOX(picker), queryRow, FALSE, FALSE, 0);
    o->input = gtk_entry_new(); target(o->input, "pin-input"); gtk_widget_set_hexpand(o->input, TRUE); gtk_entry_set_placeholder_text(GTK_ENTRY(o->input), "例如 shi、nihao，或对应双拼按键");
    setAccessibleWidgetText(o->input, "输入按键", "填写对应方案下的实际拼音或双拼按键");
    gtk_entry_set_max_length(GTK_ENTRY(o->input), 128);
    gtk_box_pack_start(GTK_BOX(queryRow), o->input, TRUE, TRUE, 0); o->query = button(queryRow, "查找候选", "pin-query");
    o->status = description(""); target(o->status, "pin-state"); gtk_box_pack_start(GTK_BOX(picker), o->status, FALSE, FALSE, 0);
    o->candidates = tree(picker, "pin-candidates", false);
    setAccessibleWidgetText(o->rules, "候选置顶规则", "选择规则后更换或取消置顶，双击更换，Delete 取消");
    setAccessibleWidgetText(o->candidates, "可置顶的完整候选", "选择后确认置顶，也可双击或按 Enter 确认");
    auto *pickActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_box_pack_start(GTK_BOX(picker), pickActions, FALSE, FALSE, 0);
    o->choose = button(pickActions, "置顶所选候选", "pin-choose"); o->more = button(pickActions, "更多候选", "pin-more");
    auto *detailsToggle = gtk_expander_new("技术详情");
    setAccessibleWidgetText(detailsToggle, "技术详情", "展开查看并复制查找失败的具体原因");
    target(detailsToggle, "pin-error-details");
    o->detailLabel = description("");
    gtk_label_set_selectable(GTK_LABEL(o->detailLabel), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(o->detailLabel), PANGO_WRAP_WORD_CHAR);
    auto *detailsScroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detailsScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(detailsScroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(detailsScroll), 120);
    gtk_container_add(GTK_CONTAINER(detailsScroll), o->detailLabel);
    o->details = createSettingsDisclosure(detailsToggle, detailsScroll);
    gtk_widget_show_all(o->details);
    gtk_widget_set_no_show_all(o->details, TRUE);
    gtk_widget_hide(o->details);
    gtk_box_pack_start(GTK_BOX(picker), o->details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), description("确认后返回设置草稿，点击主窗口“保存设置”才会生效。"), FALSE, FALSE, 0);
    g_signal_connect(o->search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer p) { static_cast<PinDialog *>(p)->render(); }), o);
    g_signal_connect(o->add, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<PinDialog *>(p)->open({}); }), o);
    g_signal_connect(o->edit, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { auto &owner = *static_cast<PinDialog *>(p); if (auto i = selection(owner.rules)) owner.open(i); }), o);
    g_signal_connect(o->remove, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { auto &owner = *static_cast<PinDialog *>(p); if (auto i = selection(owner.rules)) { owner.settings.candidatePins.erase(owner.settings.candidatePins.begin() + static_cast<std::ptrdiff_t>(*i)); owner.render(); } }), o);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(o->rules)), "changed", G_CALLBACK(+[](GtkTreeSelection *, gpointer p) { static_cast<PinDialog *>(p)->ruleSelection(); }), o);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(o->candidates)), "changed", G_CALLBACK(+[](GtkTreeSelection *, gpointer p) { static_cast<PinDialog *>(p)->updateChoose(); }), o);
    g_signal_connect(o->rules, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer p) { auto &owner = *static_cast<PinDialog *>(p); if (auto i = selection(owner.rules)) owner.open(i); }), o);
    g_signal_connect(o->candidates, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer p) { static_cast<PinDialog *>(p)->use(); }), o);
    g_signal_connect(o->rules, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer p) -> gboolean {
        if (event->keyval != GDK_KEY_Delete) return GDK_EVENT_PROPAGATE;
        gtk_button_clicked(GTK_BUTTON(static_cast<PinDialog *>(p)->remove)); return GDK_EVENT_STOP;
    }), o);
    for (auto *control : {o->scheme, o->input}) g_signal_connect(control, "changed", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<PinDialog *>(p)->invalidate(); }), o);
    g_signal_connect(o->input, "activate", G_CALLBACK(+[](GtkEntry *, gpointer p) { static_cast<PinDialog *>(p)->lookup(); }), o);
    g_signal_connect(o->query, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<PinDialog *>(p)->lookup(); }), o);
    g_signal_connect(o->more, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { auto &owner = *static_cast<PinDialog *>(p); owner.limit = std::min<std::size_t>(owner.limit + 128, 1024); owner.lookup(); }), o);
    g_signal_connect(o->choose, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<PinDialog *>(p)->use(); }), o);
    g_signal_connect(back, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<PinDialog *>(p)->back(); }), o);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *widget, gint response, gpointer p) {
        auto &owner = *static_cast<PinDialog *>(p);
        if (response == GTK_RESPONSE_ACCEPT) {
            if (std::string_view(gtk_stack_get_visible_child_name(GTK_STACK(owner.stack))) != "rules") return;
            auto callback = owner.accept; auto pins = owner.settings.candidatePins; gtk_widget_destroy(GTK_WIDGET(widget)); callback(std::move(pins));
        } else gtk_widget_destroy(GTK_WIDGET(widget));
    }), o);
    g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<PinDialog *>(p)->lifetime->deactivate(); }), o);
    g_object_set_data_full(G_OBJECT(dialog), "modernime-pin-dialog", o, +[](gpointer p) { delete static_cast<PinDialog *>(p); });
    o->render(); o->back(); return dialog;
}
}
