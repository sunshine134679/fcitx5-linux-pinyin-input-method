#include "modernime/settings/input_rule_dialog.h"
#include "modernime/settings/settings_widgets.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {
void target(GtkWidget *widget, const char *name) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(name), g_free);
}
GtkWidget *description(const char *text) {
    auto *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 72);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "modernime-description");
    return label;
}
void styleDialog(GtkWidget *dialog) {
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "modernime-settings");
    auto *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 20);
    gtk_box_set_spacing(GTK_BOX(body), 12);
}
bool matches(std::string_view value, const std::string &query) {
    auto *folded = g_utf8_casefold(std::string(value).c_str(), -1);
    const bool result = std::string_view(folded).find(query) != std::string_view::npos;
    g_free(folded);
    return result;
}

struct RuleDialog {
    core::ModernIMESettings draft;
    bool apps;
    std::function<void(core::ModernIMESettings)> accept;
    GtkWidget *dialog = nullptr, *search = nullptr, *list = nullptr, *count = nullptr;
    GtkWidget *editButton = nullptr, *deleteButton = nullptr, *child = nullptr;

    std::optional<std::string> selected() const {
        GtkTreeIter iter;
        GtkTreeModel *model = nullptr;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), &model, &iter)) return {};
        gchar *key = nullptr;
        gtk_tree_model_get(model, &iter, 0, &key, -1);
        std::string result(key);
        g_free(key);
        return result;
    }
    void selectionChanged() {
        const bool selectedRow = selected().has_value();
        gtk_widget_set_sensitive(editButton, selectedRow);
        gtk_widget_set_sensitive(deleteButton, selectedRow);
    }
    void render() {
        auto *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(list)));
        gtk_list_store_clear(store);
        auto *folded = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(search)), -1);
        const std::string query(folded);
        g_free(folded);
        std::size_t visible = 0;
        const auto append = [&](const std::string &key, const std::string &value) {
            if (!matches(key, query) && !matches(value, query)) return;
            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, key.c_str(), 1, value.c_str(), -1);
            ++visible;
        };
        if (apps) for (const auto &[key, mode] : draft.appInputModes)
            append(key, mode == core::InputMode::English ? "英文" : "中文");
        else for (const auto &[key, value] : draft.punctuationOverrides) append(std::string(1, key), value);
        const auto total = apps ? draft.appInputModes.size() : draft.punctuationOverrides.size();
        const auto message = total == 0 ? (apps ? "尚未设置应用规则，沿用全局默认状态。" : "尚未自定义标点，沿用智能转换与配对设置。")
            : visible == 0 ? std::string("没有匹配项，试试其他关键词。")
            : "共 " + std::to_string(total) + " 条，当前显示 " + std::to_string(visible) + " 条";
        gtk_label_set_text(GTK_LABEL(count), message.c_str());
        selectionChanged();
    }
    void track(GtkWidget *widget) {
        child = widget;
        g_signal_connect(widget, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer p) {
            static_cast<RuleDialog *>(p)->child = nullptr;
        }), this);
    }
    struct Form {
        RuleDialog *owner;
        std::optional<std::string> oldKey;
        GtkWidget *dialog, *key, *value, *error;
        core::ModernIMESettings trial;

        bool validate(bool showError = true) {
            auto &manager = *owner;
            const std::string keyText = gtk_entry_get_text(GTK_ENTRY(key));
            std::string message;
            trial = manager.draft;
            if (manager.apps) {
                if (oldKey) trial.appInputModes.erase(*oldKey);
                if (trial.appInputModes.contains(keyText)) message = "该应用已有规则，请编辑现有规则。";
                trial.appInputModes[keyText] = settingsChoiceActive(value) == 1 ? core::InputMode::English : core::InputMode::Chinese;
            } else {
                if (oldKey) trial.punctuationOverrides.erase(oldKey->front());
                if (keyText.size() != 1) message = "请输入一个英文标点按键，例如 / 或 _。";
                else {
                    if (trial.punctuationOverrides.contains(keyText.front())) message = "这个按键已有自定义，请编辑现有项。";
                    trial.punctuationOverrides[keyText.front()] = gtk_entry_get_text(GTK_ENTRY(value));
                }
            }
            if (message.empty()) {
                for (const auto &issue : core::validateSettings(trial).issues) {
                    if (manager.apps ? issue.key.starts_with("input.app") : issue.key.starts_with("punctuation.map.")) {
                        message = manager.apps ? (issue.key == "input.app" ? "最多 128 条规则，请删除一条后再添加。"
                            : "应用标识仅支持字母、数字和 . _ - / @，最多 128 个字符。")
                            : "请输入英文标点；文本限 1–32 字节，不含控制字符或首尾空白。";
                        break;
                    }
                }
            }
            gtk_label_set_text(GTK_LABEL(error), showError ? message.c_str() : "");
            gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, message.empty());
            return message.empty();
        }
    };
    void edit(std::optional<std::string> oldKey) {
        if (child) { gtk_window_present(GTK_WINDOW(child)); return; }
        auto *popup = gtk_dialog_new_with_buttons(oldKey ? "编辑规则" : "添加规则", GTK_WINDOW(dialog),
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            "取消", GTK_RESPONSE_CANCEL, oldKey ? "确定" : "添加", GTK_RESPONSE_ACCEPT, nullptr);
        styleDialog(popup);
        gtk_window_set_default_size(GTK_WINDOW(popup), 540, -1);
        gtk_dialog_set_default_response(GTK_DIALOG(popup), GTK_RESPONSE_ACCEPT);
        auto *body = gtk_dialog_get_content_area(GTK_DIALOG(popup));
        auto *form = new Form{this, std::move(oldKey), popup, gtk_entry_new(),
            apps ? createSettingsChoice() : gtk_entry_new(), description(""), {}};
        target(form->key, "input-rule-key"); target(form->value, "input-rule-value");
        setAccessibleWidgetText(form->key, apps ? "应用标识" : "英文标点按键", apps ? "精确匹配 Fcitx 的应用标识，区分大小写" : "一个英文标点按键");
        setAccessibleWidgetText(form->value, apps ? "默认输入状态" : "固定输出文本", apps ? "中文或英文" : "取代该按键的智能转换和自动配对");
        gtk_entry_set_placeholder_text(GTK_ENTRY(form->key), apps ? "例如 code、org.gnome.TextEditor" : "例如 / 或 _");
        gtk_entry_set_activates_default(GTK_ENTRY(form->key), TRUE);
        if (apps) {
            appendSettingsChoice(form->value, "中文"); appendSettingsChoice(form->value, "英文");
            setSettingsChoiceActive(form->value, form->oldKey && draft.appInputModes.at(*form->oldKey) == core::InputMode::English ? 1 : 0);
        } else {
            gtk_entry_set_activates_default(GTK_ENTRY(form->value), TRUE);
            gtk_entry_set_placeholder_text(GTK_ENTRY(form->value), "例如 _、/、——");
            if (form->oldKey) gtk_entry_set_text(GTK_ENTRY(form->value), draft.punctuationOverrides.at(form->oldKey->front()).c_str());
        }
        if (form->oldKey) gtk_entry_set_text(GTK_ENTRY(form->key), form->oldKey->c_str());
        gtk_box_pack_start(GTK_BOX(body), description(apps ? "输入 Fcitx 使用的应用标识，例如 code、org.gnome.TextEditor；区分大小写，不支持通配符。" : "固定输出优先于智能标点与自动配对；删除此规则即可恢复。"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(body), createSettingRow(apps ? "应用标识" : "按键", "", form->key), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(body), createSettingRow(apps ? "默认状态" : "输出文本", "", form->value), FALSE, FALSE, 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(form->error), "modernime-status-error");
        gtk_box_pack_start(GTK_BOX(body), form->error, FALSE, FALSE, 0);
        g_object_set_data_full(G_OBJECT(popup), "modernime-input-rule-form", form, +[](gpointer p) { delete static_cast<Form *>(p); });
        for (auto *control : {form->key, form->value}) g_signal_connect(control, "changed", G_CALLBACK(+[](GtkWidget *, gpointer p) { static_cast<Form *>(p)->validate(); }), form);
        g_signal_connect(popup, "response", G_CALLBACK(+[](GtkDialog *widget, gint response, gpointer p) {
            auto &f = *static_cast<Form *>(p);
            if (response == GTK_RESPONSE_ACCEPT) {
                if (!f.validate()) return;
                f.owner->draft = std::move(f.trial);
                f.owner->render();
            }
            gtk_widget_destroy(GTK_WIDGET(widget));
        }), form);
        track(popup);
        gtk_widget_show_all(popup);
        form->validate(false);
        gtk_widget_grab_focus(form->key);
    }
    void remove() {
        const auto key = selected();
        if (!key || child) return;
        auto *popup = gtk_message_dialog_new(GTK_WINDOW(dialog),
            static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, apps ? "删除应用“%s”的规则？" : "删除按键“%s”的自定义？", key->c_str());
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(popup), "确认后从规则草稿中移除，主窗口保存后生效。");
        gtk_dialog_add_buttons(GTK_DIALOG(popup), "取消", GTK_RESPONSE_CANCEL, "删除", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(popup), GTK_RESPONSE_CANCEL);
        gtk_style_context_add_class(gtk_widget_get_style_context(popup), "modernime-settings");
        g_object_set_data_full(G_OBJECT(popup), "modernime-input-rule-key", g_strdup(key->c_str()), g_free);
        g_signal_connect(popup, "response", G_CALLBACK(+[](GtkDialog *widget, gint response, gpointer p) {
            auto &owner = *static_cast<RuleDialog *>(p);
            if (response == GTK_RESPONSE_ACCEPT) {
                const auto *keyText = static_cast<const char *>(g_object_get_data(G_OBJECT(widget), "modernime-input-rule-key"));
                if (owner.apps) owner.draft.appInputModes.erase(keyText);
                else owner.draft.punctuationOverrides.erase(*keyText);
                owner.render();
            }
            gtk_widget_destroy(GTK_WIDGET(widget));
        }), this);
        track(popup); gtk_widget_show_all(popup);
    }
};
} // namespace

GtkWidget *createInputRuleDialog(GtkWindow *parent, const core::ModernIMESettings &settings,
    InputRuleKind kind, std::function<void(core::ModernIMESettings)> accept) {
    auto *owner = new RuleDialog{settings, kind == InputRuleKind::Application, std::move(accept)};
    auto *dialog = gtk_dialog_new_with_buttons(owner->apps ? "应用默认输入状态" : "逐键自定义标点", parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "取消", GTK_RESPONSE_CANCEL, "使用这些规则", GTK_RESPONSE_ACCEPT, nullptr);
    owner->dialog = dialog;
    styleDialog(dialog); gtk_window_set_default_size(GTK_WINDOW(dialog), 680, 470);
    auto *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(body), description(owner->apps
        ? "开启按应用记忆时，已记住的状态优先；其他情况先用这里的规则，再用全局默认。保存规则后，下次激活应用时生效。"
        : "为指定英文标点设置固定输出。自定义优先于智能转换和自动配对，删除后恢复原行为。"), FALSE, FALSE, 0);
    owner->search = gtk_search_entry_new(); target(owner->search, "input-rule-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(owner->search), owner->apps ? "搜索应用标识或中英文状态" : "搜索按键或输出文本");
    setAccessibleWidgetText(owner->search, "搜索规则", "按规则内容筛选，忽略英文大小写");
    gtk_box_pack_start(GTK_BOX(body), owner->search, FALSE, FALSE, 0);
    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto *add = gtk_button_new_with_label("添加规则");
    owner->editButton = gtk_button_new_with_label("编辑");
    owner->deleteButton = gtk_button_new_with_label("删除");
    target(add, "input-rule-add"); target(owner->editButton, "input-rule-edit"); target(owner->deleteButton, "input-rule-delete");
    for (auto *button : {add, owner->editButton, owner->deleteButton}) gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), actions, FALSE, FALSE, 0);
    auto *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    owner->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store)); g_object_unref(store);
    target(owner->list, "input-rule-list");
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(owner->list), FALSE);
    for (int column = 0; column < 2; ++column) {
        auto *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        auto *viewColumn = gtk_tree_view_column_new_with_attributes(column == 0 ? (owner->apps ? "应用标识" : "按键") : (owner->apps ? "默认状态" : "固定输出"), renderer, "text", column, nullptr);
        gtk_tree_view_column_set_expand(viewColumn, TRUE);
        gtk_tree_view_column_set_min_width(viewColumn, column == 0 && owner->apps ? 240 : 100);
        gtk_tree_view_append_column(GTK_TREE_VIEW(owner->list), viewColumn);
    }
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), owner->list);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);
    owner->count = description("");
    gtk_box_pack_start(GTK_BOX(body), owner->count, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), description("确认后返回设置草稿，点击主窗口“保存设置”才会生效。"), FALSE, FALSE, 0);
    g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<RuleDialog *>(p)->edit({}); }), owner);
    g_signal_connect(owner->editButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { auto &o = *static_cast<RuleDialog *>(p); if (auto key = o.selected()) o.edit(key); }), owner);
    g_signal_connect(owner->deleteButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<RuleDialog *>(p)->remove(); }), owner);
    g_signal_connect(owner->search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer p) { static_cast<RuleDialog *>(p)->render(); }), owner);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(owner->list)), "changed", G_CALLBACK(+[](GtkTreeSelection *, gpointer p) { static_cast<RuleDialog *>(p)->selectionChanged(); }), owner);
    g_signal_connect(owner->list, "row-activated", G_CALLBACK(+[](GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer p) { auto &o = *static_cast<RuleDialog *>(p); if (auto key = o.selected()) o.edit(key); }), owner);
    g_signal_connect(owner->list, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer p) -> gboolean {
        if (event->keyval != GDK_KEY_Delete) return GDK_EVENT_PROPAGATE;
        static_cast<RuleDialog *>(p)->remove(); return GDK_EVENT_STOP;
    }), owner);
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *widget, gint response, gpointer p) {
        auto &o = *static_cast<RuleDialog *>(p);
        if (o.child) return;
        if (response == GTK_RESPONSE_ACCEPT) {
            auto callback = o.accept;
            auto draft = o.draft;
            gtk_widget_destroy(GTK_WIDGET(widget));
            callback(std::move(draft));
        } else gtk_widget_destroy(GTK_WIDGET(widget));
    }), owner);
    g_object_set_data_full(G_OBJECT(dialog), "modernime-input-rules", owner, +[](gpointer p) { delete static_cast<RuleDialog *>(p); });
    setSettingsFocusChain(body, {owner->search, add, owner->editButton, owner->deleteButton, owner->list});
    owner->render();
    return dialog;
}
} // namespace modernime::settings
