#include "modernime/settings/input_assist.h"
#include "modernime/core/calculator.h"
#include "modernime/core/component_lookup.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/settings_widgets.h"
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {
void target(GtkWidget *widget, const char *id) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(id), g_free);
}
GtkWidget *label(const char *text) {
    auto *result = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(result), 0);
    gtk_label_set_line_wrap(GTK_LABEL(result), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(result), 65);
    gtk_style_context_add_class(gtk_widget_get_style_context(result), "modernime-description");
    return result;
}
struct Preview {
    using Lifetime = detail::DiagnosticsLifetime<Preview>;
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(this);
    bool component;
    GtkWidget *dialog, *entry, *status, *results, *copy, *retry, *details;
    std::shared_ptr<const core::ComponentLookup> lookup;
    std::vector<core::ComponentCandidate> matches;
    std::string value;
    unsigned generation = 0;

    void update() {
        value.clear();
        const std::string query = gtk_entry_get_text(GTK_ENTRY(entry));
        if (component) {
            auto *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(results)));
            gtk_list_store_clear(store);
            if (!lookup || !lookup->available()) { gtk_widget_set_sensitive(copy, FALSE); return; }
            if (query.size() > 128) {
                gtk_label_set_text(GTK_LABEL(status), "查询内容过长，请减少到 128 字节以内。");
                gtk_widget_set_sensitive(copy, FALSE); return;
            }
            matches = lookup->lookup(query, 30);
            for (const auto &match : matches) {
                GtkTreeIter iter;
                gtk_list_store_append(store, &iter);
                gtk_list_store_set(store, &iter, 0, match.text.c_str(), 1, match.components.c_str(), -1);
            }
            gtk_label_set_text(GTK_LABEL(status), query.empty() ? "填写完整部件组合，支持原部件或拼音。" : matches.empty() ? "没有匹配的字，试试完整的部件组合。" : "选择汉字后可复制；正文输入时使用 u+ 触发。" );
        } else {
            const auto calculation = core::calculate(query);
            value = calculation.valid ? calculation.text : "";
            gtk_label_set_text(GTK_LABEL(results), value.c_str());
            gtk_label_set_text(GTK_LABEL(status), query.empty() ? "输入表达式即可预览结果。" : calculation.valid ? "Enter、空格或末尾 = 可在正文中上屏。" : calculation.error.c_str());
        }
        gtk_widget_set_sensitive(copy, !value.empty());
    }
    struct Load {
        std::shared_ptr<Lifetime> lifetime;
        unsigned generation;
        std::shared_ptr<const core::ComponentLookup> index;
        std::string error;
    };
    void load() {
        const auto current = ++generation;
        lookup.reset(); value.clear();
        gtk_widget_set_sensitive(copy, FALSE); gtk_widget_hide(retry);
        gtk_label_set_text(GTK_LABEL(status), "正在加载离线拆字数据…");
        gtk_widget_hide(details);
        auto *task = g_task_new(nullptr, nullptr, +[](GObject *, GAsyncResult *result, gpointer) {
            auto &request = *static_cast<Load *>(g_task_get_task_data(G_TASK(result)));
            request.lifetime->withOwner([&](Preview &owner) {
                if (request.generation != owner.generation) return;
                owner.lookup = std::move(request.index);
                if (!request.error.empty()) {
                    gtk_label_set_text(GTK_LABEL(owner.status), "拆字数据不可用。请检查安装后重新加载。");
                    auto *detail = gtk_bin_get_child(GTK_BIN(owner.details));
                    gtk_label_set_text(GTK_LABEL(detail), request.error.substr(0, 1000).c_str());
                    gtk_widget_show(detail);
                    gtk_widget_show(owner.details);
                    gtk_widget_show(owner.retry);
                } else owner.update();
            });
        }, nullptr);
        g_task_set_task_data(task, new Load{lifetime, current, {}, {}}, +[](gpointer p) { delete static_cast<Load *>(p); });
        g_task_run_in_thread(task, +[](GTask *taskObject, gpointer, gpointer p, GCancellable *) {
            auto &request = *static_cast<Load *>(p);
            try {
                request.index = std::make_shared<const core::ComponentLookup>();
                if (!request.index->available()) request.error = request.index->loadError();
            } catch (const std::exception &error) { request.error = error.what(); }
            g_task_return_boolean(taskObject, TRUE);
        });
        g_object_unref(task);
    }
};
GtkWidget *preview(GtkWindow *parent, bool component) {
    auto *dialog = gtk_dialog_new_with_buttons(component ? "试用拆字查字" : "试用离线计算器", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "关闭", GTK_RESPONSE_CLOSE, nullptr);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "modernime-settings");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 590, component ? 440 : 270);
    auto *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 20); gtk_box_set_spacing(GTK_BOX(body), 12);
    auto *owner = new Preview{{}, component, dialog, gtk_entry_new(), label(""),
        component ? gtk_tree_view_new() : gtk_label_new(""), gtk_button_new_with_label("复制结果"),
        component ? gtk_button_new_with_label("重新加载") : nullptr, gtk_expander_new("技术详情"), {}, {}, {}, 0};
    owner->lifetime = std::make_shared<Preview::Lifetime>(owner);
    gtk_box_pack_start(GTK_BOX(body), label(component ? "这里填写部件或部件拼音，无需 u+；例如 木木、mumu、ri+yue。" : "这里直接填写表达式，无需 v=。支持 + − * / % 和括号；% 表示余数，结果为近似值。"), FALSE, FALSE, 0);
    target(owner->entry, "assist-preview-input"); target(owner->status, "assist-preview-state");
    target(owner->results, "assist-preview-results"); target(owner->copy, "assist-preview-copy");
    gtk_entry_set_placeholder_text(GTK_ENTRY(owner->entry), component ? "木木 / mumu" : "(2+3)*4");
    gtk_entry_set_max_length(GTK_ENTRY(owner->entry), component ? 128 : 512);
    setAccessibleWidgetText(owner->entry, component ? "部件查询" : "表达式", component ? "填写原部件或完整部件拼音，无需 u+" : "填写算式，无需 v=");
    gtk_box_pack_start(GTK_BOX(body), owner->entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), owner->status, FALSE, FALSE, 0);
    if (component) {
        auto *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        gtk_tree_view_set_model(GTK_TREE_VIEW(owner->results), GTK_TREE_MODEL(store)); g_object_unref(store);
        for (int i = 0; i < 2; ++i) gtk_tree_view_append_column(GTK_TREE_VIEW(owner->results),
            gtk_tree_view_column_new_with_attributes(i ? "部件" : "汉字", gtk_cell_renderer_text_new(), "text", i, nullptr));
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), owner->results);
        gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
        g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(owner->results)), "changed", G_CALLBACK(+[](GtkTreeSelection *selection, gpointer p) {
            auto &o = *static_cast<Preview *>(p); o.value.clear(); GtkTreeIter iter; GtkTreeModel *model = nullptr;
            if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
                gchar *text = nullptr; gtk_tree_model_get(model, &iter, 0, &text, -1); o.value = text; g_free(text);
            }
            gtk_widget_set_sensitive(o.copy, !o.value.empty());
        }), owner);
    } else {
        gtk_label_set_selectable(GTK_LABEL(owner->results), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(owner->results), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(owner->results), 55);
        gtk_box_pack_start(GTK_BOX(body), owner->results, TRUE, TRUE, 0);
    }
    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(body), actions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), owner->copy, FALSE, FALSE, 0);
    if (component) {
        gtk_box_pack_start(GTK_BOX(actions), owner->retry, FALSE, FALSE, 0);
        gtk_widget_set_no_show_all(owner->retry, TRUE);
    }
    gtk_container_add(GTK_CONTAINER(owner->details), label(""));
    gtk_box_pack_start(GTK_BOX(body), owner->details, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(owner->details, TRUE);
    g_signal_connect(owner->entry, "changed", G_CALLBACK(+[](GtkEditable *, gpointer p) { static_cast<Preview *>(p)->update(); }), owner);
    if (component) g_signal_connect(owner->retry, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<Preview *>(p)->load(); }), owner);
    g_signal_connect(owner->copy, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
        auto &o = *static_cast<Preview *>(p); if (o.value.empty()) return;
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), o.value.c_str(), -1);
        gtk_label_set_text(GTK_LABEL(o.status), "已复制结果。");
    }), owner);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *widget, gint, gpointer) { gtk_widget_destroy(GTK_WIDGET(widget)); }), nullptr);
    g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Preview *>(p)->lifetime->deactivate(); }), owner);
    g_object_set_data_full(G_OBJECT(dialog), "modernime-input-assist-preview", owner, +[](gpointer p) { delete static_cast<Preview *>(p); });
    gtk_widget_show_all(dialog);
    if (component) owner->load(); else owner->update();
    gtk_widget_grab_focus(owner->entry);
    return dialog;
}
} // namespace

class InputAssist::Impl {
public:
    SettingsWindowModel &model;
    std::function<void()> changed;
    GtkWidget *root, *temporary, *component, *calculator, *componentPreview, *calculatorPreview, *dialog = nullptr;
    bool refreshing = false;
    Impl(SettingsWindowModel &m, std::function<void()> c) : model(m), changed(std::move(c)) {
        root = createSectionCard("输入辅助", "在中文状态下临时输入英文、查找汉字或计算，完成后继续中文输入。");
        const auto row = [&](const char *title, const char *description, const char *id, GtkWidget *&control, GtkWidget **trial) {
            control = gtk_switch_new(); target(control, id);
            auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            if (trial) { *trial = gtk_button_new_with_label("试用"); gtk_box_pack_start(GTK_BOX(actions), *trial, FALSE, FALSE, 0); }
            gtk_box_pack_start(GTK_BOX(actions), control, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(root), createSettingRow(title, description, actions), FALSE, FALSE, 0);
            setAccessibleWidgetText(control, title, description);
            g_signal_connect(control, "notify::active", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer p) {
                auto &o = *static_cast<Impl *>(p); if (o.refreshing) return;
                auto settings = o.model.settings();
                settings.temporaryEnglishEnabled = gtk_switch_get_active(GTK_SWITCH(o.temporary));
                settings.componentLookupEnabled = gtk_switch_get_active(GTK_SWITCH(o.component));
                settings.calculatorEnabled = gtk_switch_get_active(GTK_SWITCH(o.calculator));
                o.model.setSettings(std::move(settings)); o.changed();
            }), this);
        };
        row("临时英文", "Ctrl+Shift+E 输入一段英文；Enter 确认、Esc 取消，结束后回到中文。", "input.temporary_english", temporary, nullptr);
        row("拆字查字", "输入 u+ 后填写部件拼音，如 u+mumu → 林；支持空格、数字或鼠标选字。", "input.component_lookup", component, &componentPreview);
        row("离线计算器", "输入 v= 后填写表达式，如 v=(2+3)*4 → 20；Enter 或空格上屏。", "input.calculator", calculator, &calculatorPreview);
        target(componentPreview, "component-preview"); target(calculatorPreview, "calculator-preview");
        setAccessibleWidgetText(componentPreview, "试用拆字查字", "输入部件或拼音，预览离线查询结果");
        setAccessibleWidgetText(calculatorPreview, "试用离线计算器", "填写表达式并预览结果");
        for (auto *button : {componentPreview, calculatorPreview}) g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *clicked, gpointer p) {
            auto &o = *static_cast<Impl *>(p); if (o.dialog) { gtk_window_present(GTK_WINDOW(o.dialog)); return; }
            auto *top = gtk_widget_get_toplevel(o.root);
            o.dialog = preview(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr, GTK_WIDGET(clicked) == o.componentPreview);
            g_signal_connect(o.dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) { static_cast<Impl *>(data)->dialog = nullptr; }), &o);
        }), this);
        setSettingsFocusChain(root, {temporary, componentPreview, component, calculatorPreview, calculator});
        refresh();
    }
    ~Impl() { if (dialog) gtk_widget_destroy(dialog); }
    void refresh() {
        refreshing = true;
        gtk_switch_set_active(GTK_SWITCH(temporary), model.settings().temporaryEnglishEnabled);
        gtk_switch_set_active(GTK_SWITCH(component), model.settings().componentLookupEnabled);
        gtk_switch_set_active(GTK_SWITCH(calculator), model.settings().calculatorEnabled);
        for (auto *control : {temporary, component, calculator}) gtk_widget_set_sensitive(control, model.settings().inputEnabled);
        refreshing = false;
    }
    bool focus(std::string_view id) {
        for (auto *control : {temporary, component, calculator, componentPreview, calculatorPreview}) {
            const auto *name = static_cast<const char *>(g_object_get_data(G_OBJECT(control), "modernime-settings-target"));
            if (name && id == name) return focusWidgetOrFallback(control, gtk_widget_get_parent(gtk_widget_get_parent(control)));
        }
        return false;
    }
};
InputAssist::InputAssist(SettingsWindowModel &model, std::function<void()> changed) : impl_(std::make_unique<Impl>(model, std::move(changed))) {}
InputAssist::~InputAssist() = default;
GtkWidget *InputAssist::widget() const { return impl_->root; }
void InputAssist::refresh() { impl_->refresh(); }
bool InputAssist::focusTarget(std::string_view id) { return impl_->focus(id); }
}
