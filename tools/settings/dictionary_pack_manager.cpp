#include "modernime/settings/dictionary_pack_manager.h"

#include "modernime/settings/backend_client.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_widgets.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {

void target(GtkWidget *widget, const char *name) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(name), g_free);
}

GtkWidget *description(const std::string &text) {
    auto *label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 72);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "modernime-description");
    return label;
}

struct Pack final {
    std::string id, name, source, license, version;
    std::uint64_t entries = 0;
    bool enabled = false, bundled = false;
};

Pack readPack(const BackendValue &value) {
    return {value.text("id"), value.text("name"), value.text("source"),
            value.text("license"), value.text("version"), value.number("entryCount"),
            value.boolean("enabled"), value.boolean("bundled")};
}

std::string metadata(const Pack &pack) {
    return "标识：" + pack.id + "\n来源：" + pack.source + "\n许可：" + pack.license +
           "\n" + (pack.bundled ? "内置词库包" : "用户词库包");
}

} // namespace

class DictionaryPackManager::Impl final {
public:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;

    explicit Impl(std::function<void(std::string)> callback) : notify(std::move(callback)) {
        detail::GtkWidgetGuard guard(createSectionCard("词库包", "添加更多专业词和中英混合词。内置包仅供示例。"));
        root = guard.get();
        target(root, "dictionary-packs");
        g_signal_connect(root, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            auto &owner = *static_cast<Impl *>(data);
            owner.alive = false;
            owner.lifetime->deactivate();
            owner.closeDialog();
        }), this);

        auto *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        import = button(tools, "导入词库包…", "pack-import", +[](GtkButton *, gpointer p) {
            static_cast<Impl *>(p)->chooseImport();
        });
        gtk_style_context_add_class(gtk_widget_get_style_context(import), "suggested-action");
        reload = button(tools, "刷新", "pack-refresh", +[](GtkButton *, gpointer p) {
            static_cast<Impl *>(p)->refresh();
        });
        gtk_box_pack_start(GTK_BOX(root), tools, FALSE, FALSE, 6);
        count = description("尚未读取词库包");
        gtk_box_pack_start(GTK_BOX(root), count, FALSE, FALSE, 4);

        auto storeOwner = detail::GObjectHandle<GtkListStore>::adopt(
            gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT));
        store = storeOwner.get();
        list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        target(list, "pack-list");
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(list), FALSE);
        setAccessibleWidgetText(list, "词库包列表", "选择词库包后可启用、停用或移除用户副本");
        for (const auto &[name, column] : {std::pair{"名称", 0}, std::pair{"版本", 1},
                                          std::pair{"词条", 2}, std::pair{"状态", 3}}) {
            auto *renderer = gtk_cell_renderer_text_new();
            g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            auto *viewColumn = gtk_tree_view_column_new_with_attributes(name, renderer, "text", column, nullptr);
            gtk_tree_view_column_set_expand(viewColumn, column == 0);
            gtk_tree_view_column_set_min_width(viewColumn, column == 0 ? 170 : column == 1 ? 80 : column == 2 ? 56 : 110);
            gtk_tree_view_append_column(GTK_TREE_VIEW(list), viewColumn);
        }
        g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), "changed",
                         G_CALLBACK(+[](GtkTreeSelection *, gpointer p) {
            static_cast<Impl *>(p)->selectionChanged();
        }), this);
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_size_request(scroll, -1, 180);
        gtk_container_add(GTK_CONTAINER(scroll), list);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 6);

        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        toggle = button(actions, "启用", "pack-toggle", +[](GtkButton *, gpointer p) {
            static_cast<Impl *>(p)->changeSelected(false);
        });
        remove = button(actions, "移除用户副本", "pack-remove", +[](GtkButton *, gpointer p) {
            static_cast<Impl *>(p)->changeSelected(true);
        });
        gtk_box_pack_start(GTK_BOX(root), actions, FALSE, FALSE, 4);
        state = description("");
        target(state, "pack-state");
        gtk_box_pack_start(GTK_BOX(root), state, FALSE, FALSE, 6);

        detailsText = description("选择词库包查看来源与许可。");
        gtk_label_set_selectable(GTK_LABEL(detailsText), TRUE);
        detailsToggle = gtk_expander_new("来源与许可");
        target(detailsToggle, "pack-details");
        gtk_box_pack_start(GTK_BOX(root), createSettingsDisclosure(detailsToggle, detailsText), FALSE, FALSE, 0);
        setSettingsFocusChain(root, {import, reload, list, toggle, remove, detailsToggle});
        updateActions();
        guard.release();
    }

    ~Impl() {
        lifetime->deactivate();
        closeDialog();
        if (alive) {
            for (auto *control : {root, list, import, reload, toggle, remove})
                g_signal_handlers_disconnect_by_data(control, this);
            g_signal_handlers_disconnect_by_data(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), this);
        }
    }

    GtkWidget *widget() const { return root; }

    void refresh(bool preserveStatus = false) {
        if (!alive || busy) return;
        if (!preserveStatus) showStatus("");
        busy = true;
        loaded = false;
        const auto request = ++generation;
        updateActions();
        gtk_label_set_text(GTK_LABEL(count), "正在读取词库包…");
        client.execute("pack.list", {}, [life = lifetime, request](BackendReply reply) {
            life->withOwner([&](Impl &owner) {
                if (request != owner.generation) return;
                owner.busy = false;
                if (!reply.ok) {
                    owner.showError(reply, "读取词库包失败，请点击刷新重试。");
                    gtk_label_set_text(GTK_LABEL(owner.count), "读取失败");
                    owner.updateActions();
                    return;
                }
                std::string selectedId;
                if (const auto selected = owner.selection()) selectedId = owner.packs[*selected].id;
                owner.packs.clear();
                gtk_list_store_clear(owner.store);
                for (const auto &value : reply.data.array("packs")) {
                    auto pack = readPack(value);
                    if (pack.id.empty()) continue;
                    const auto index = static_cast<guint>(owner.packs.size());
                    owner.packs.push_back(std::move(pack));
                    const auto &entry = owner.packs.back();
                    GtkTreeIter row;
                    gtk_list_store_append(owner.store, &row);
                    const auto entries = std::to_string(entry.entries);
                    const auto status = std::string(entry.enabled ? "已启用" : "已停用");
                    gtk_list_store_set(owner.store, &row, 0, entry.name.c_str(), 1, entry.version.c_str(),
                                       2, entries.c_str(), 3, status.c_str(), 4, index, -1);
                    if (entry.id == selectedId)
                        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(owner.list)), &row);
                }
                owner.loaded = true;
                const auto summary = owner.packs.empty() ? "暂无词库包，可从本地目录导入。" :
                    "共 " + std::to_string(owner.packs.size()) + " 个词库包";
                gtk_label_set_text(GTK_LABEL(owner.count), summary.c_str());
                owner.selectionChanged();
                if (!owner.pendingFocus.empty()) {
                    const auto pending = std::exchange(owner.pendingFocus, {});
                    owner.focusTarget(pending);
                }
            });
        });
    }

    bool focusTarget(std::string_view name) {
        if (!alive) return false;
        if (busy) { pendingFocus = name; return true; }
        if (name == "dictionary-packs") return focusWidgetOrFallback(list, root);
        for (auto *control : {list, import, reload, toggle, remove, detailsToggle}) {
            const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(control), "modernime-settings-target"));
            if (id && name == id) return focusWidgetOrFallback(control, root);
        }
        return false;
    }

private:
    struct DialogContext final {
        std::shared_ptr<Lifetime> lifetime;
        std::uint64_t generation;
        std::function<void(Impl &, GtkWidget *, int)> response;
    };

    GtkWidget *button(GtkWidget *box, const char *text, const char *name,
                      void (*callback)(GtkButton *, gpointer)) {
        auto *control = gtk_button_new_with_label(text);
        target(control, name);
        gtk_box_pack_start(GTK_BOX(box), control, FALSE, FALSE, 0);
        g_signal_connect(control, "clicked", G_CALLBACK(callback), this);
        return control;
    }

    GtkWindow *parentWindow() const {
        auto *window = gtk_widget_get_toplevel(root);
        return GTK_IS_WINDOW(window) ? GTK_WINDOW(window) : nullptr;
    }

    void closeDialog() {
        if (dialog) {
            auto *closing = std::exchange(dialog, nullptr);
            gtk_widget_destroy(closing);
        }
    }

    void showDialog(GtkWidget *next, std::function<void(Impl &, GtkWidget *, int)> response) {
        dialog = next;
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "modernime-settings");
        auto *context = new DialogContext{lifetime, generation, std::move(response)};
        g_signal_connect_data(dialog, "response", G_CALLBACK(+[](GtkDialog *source, gint result, gpointer p) {
            auto *data = static_cast<DialogContext *>(p);
            // The response may replace the current dialog; retain its callback while destroying it.
            auto life = data->lifetime;
            const auto request = data->generation;
            const auto callback = data->response;
            life->withOwner([&](Impl &owner) {
                if (request != owner.generation || owner.dialog != GTK_WIDGET(source)) return;
                owner.dialog = nullptr;
                callback(owner, GTK_WIDGET(source), result);
            });
            gtk_widget_destroy(GTK_WIDGET(source));
        }), context, +[](gpointer p, GClosure *) { delete static_cast<DialogContext *>(p); }, GConnectFlags(0));
        g_signal_connect_data(dialog, "destroy", G_CALLBACK(+[](GtkWidget *source, gpointer p) {
            auto *data = static_cast<std::shared_ptr<Lifetime> *>(p);
            (*data)->withOwner([&](Impl &owner) {
                if (owner.dialog == source) {
                    owner.dialog = nullptr;
                    owner.busy = false;
                    owner.updateActions();
                }
            });
        }), new std::shared_ptr<Lifetime>(lifetime),
           +[](gpointer p, GClosure *) { delete static_cast<std::shared_ptr<Lifetime> *>(p); }, GConnectFlags(0));
        gtk_widget_show_all(dialog);
    }

    std::optional<std::size_t> selection() const {
        GtkTreeIter row;
        GtkTreeModel *model = nullptr;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), &model, &row)) return {};
        guint index = 0;
        gtk_tree_model_get(model, &row, 4, &index, -1);
        return index < packs.size() ? std::optional<std::size_t>(index) : std::nullopt;
    }

    void selectionChanged() {
        const auto selected = selection();
        const auto text = selected ? metadata(packs[*selected]) : "选择词库包查看来源与许可。";
        gtk_label_set_text(GTK_LABEL(detailsText), text.c_str());
        updateActions();
    }

    void updateActions() {
        const auto selected = selection();
        gtk_widget_set_sensitive(import, !busy && loaded);
        gtk_widget_set_sensitive(reload, !busy);
        gtk_widget_set_sensitive(list, !busy && loaded);
        gtk_widget_set_sensitive(toggle, !busy && loaded && selected.has_value());
        gtk_widget_set_sensitive(remove, !busy && loaded && selected && !packs[*selected].bundled);
        gtk_button_set_label(GTK_BUTTON(toggle), selected && packs[*selected].enabled ? "停用" : "启用");
    }

    void showError(const BackendReply &reply, const std::string &fallback) {
        const auto message = reply.message.empty() ? fallback : reply.message;
        gtk_label_set_text(GTK_LABEL(state), message.c_str());
        gtk_style_context_add_class(gtk_widget_get_style_context(state), "modernime-status-error");
    }

    void showStatus(const std::string &message) {
        gtk_style_context_remove_class(gtk_widget_get_style_context(state), "modernime-status-error");
        gtk_label_set_text(GTK_LABEL(state), message.c_str());
    }

    void finishMutation(std::string action, std::string argument) {
        if (!alive || !busy) return;
        const auto request = ++generation;
        showStatus("正在保存词库包…");
        client.execute(std::move(action), {std::move(argument)}, [life = lifetime, request](BackendReply reply) {
            life->withOwner([&](Impl &owner) {
                if (request != owner.generation) return;
                owner.busy = false;
                if (reply.ok) {
                    const std::string message = "已保存，稍等片刻后重新输入即可使用。";
                    owner.showStatus(message);
                    if (owner.notify) owner.notify(message);
                    owner.refresh(true);
                } else if (reply.uncertain) {
                    owner.showError(reply, "操作结果尚未确认，正在重新读取；请核对列表后再操作。");
                    owner.refresh(true);
                } else {
                    owner.showError(reply, "词库包操作失败，请刷新后重试。");
                    owner.updateActions();
                }
            });
        });
    }

    void changeSelected(bool removing) {
        if (!alive || busy || !loaded) return;
        const auto selected = selection();
        if (!selected) return;
        const auto pack = packs[*selected];
        if (removing && pack.bundled) return;
        busy = true;
        updateActions();
        const auto action = removing ? "pack.remove" : pack.enabled ? "pack.disable" : "pack.enable";
        const auto verb = removing ? "移除用户副本" : pack.enabled ? "停用" : "启用";
        const auto title = std::string(verb) + "“" + pack.name + "”？";
        auto *confirm = gtk_message_dialog_new(parentWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                               GTK_BUTTONS_NONE, "%s", title.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(confirm), "取消", GTK_RESPONSE_CANCEL, verb, GTK_RESPONSE_ACCEPT, nullptr);
        const auto detail = removing ? "移除后，随软件提供的词库仍会保留。不会删除学习记录。" :
                            pack.enabled ? "停用后不再使用这个包里的词。不会删除学习记录，相同词仍可能来自其他词库。" :
                            "启用后，稍等片刻即可使用。";
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm), "%s", detail);
        gtk_dialog_set_default_response(GTK_DIALOG(confirm), GTK_RESPONSE_CANCEL);
        showDialog(confirm, [action = std::string(action), id = pack.id](Impl &owner, GtkWidget *, int response) {
            if (response == GTK_RESPONSE_ACCEPT) owner.finishMutation(action, id);
            else { owner.busy = false; owner.updateActions(); }
        });
    }

    void chooseImport() {
        if (!alive || busy || !loaded) return;
        busy = true;
        updateActions();
        auto *chooser = gtk_file_chooser_dialog_new("选择词库包目录", parentWindow(),
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "取消", GTK_RESPONSE_CANCEL,
            "查看词库包", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
        showDialog(chooser, [](Impl &owner, GtkWidget *source, int response) {
            if (response != GTK_RESPONSE_ACCEPT) { owner.busy = false; owner.updateActions(); return; }
            auto *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(source));
            const std::string directory = filename ? filename : "";
            g_free(filename);
            if (directory.empty()) { owner.busy = false; owner.updateActions(); return; }
            owner.inspect(directory);
        });
    }

    void inspect(std::string directory) {
        const auto request = ++generation;
        showStatus("正在校验词库包…");
        client.execute("pack.inspect", {directory}, [life = lifetime, request, directory](BackendReply reply) {
            life->withOwner([&](Impl &owner) {
                if (request != owner.generation) return;
                if (!reply.ok) {
                    owner.busy = false;
                    owner.showError(reply, "词库包校验失败，请检查目录内容。");
                    owner.updateActions();
                    return;
                }
                owner.preview(readPack(reply.data.get("pack")), directory);
            });
        });
    }

    void preview(const Pack &pack, const std::string &directory) {
        const bool exists = std::any_of(packs.begin(), packs.end(), [&](const auto &value) { return value.id == pack.id; });
        const auto verb = exists ? "更新词库包" : "导入词库包";
        auto *preview = gtk_dialog_new_with_buttons(verb, parentWindow(), GTK_DIALOG_MODAL,
            "取消", GTK_RESPONSE_CANCEL, verb, GTK_RESPONSE_ACCEPT, nullptr);
        gtk_window_set_default_size(GTK_WINDOW(preview), 440, -1);
        auto *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_container_set_border_width(GTK_CONTAINER(body), 20);
        gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(preview))), body);
        auto *name = gtk_label_new(pack.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_style_context_add_class(gtk_widget_get_style_context(name), "modernime-section-title");
        gtk_box_pack_start(GTK_BOX(body), name, FALSE, FALSE, 0);
        const auto summary = "版本 " + pack.version + "，共 " + std::to_string(pack.entries) + " 条\n" +
                             metadata(pack) + "\n\n" + (exists ? "将更新已有词库，保留启用或停用状态。" : "将添加这个词库包。");
        auto *text = description(summary);
        gtk_label_set_selectable(GTK_LABEL(text), TRUE);
        gtk_box_pack_start(GTK_BOX(body), text, FALSE, FALSE, 0);
        gtk_dialog_set_default_response(GTK_DIALOG(preview), GTK_RESPONSE_CANCEL);
        showStatus("");
        showDialog(preview, [exists, directory](Impl &owner, GtkWidget *, int response) {
            if (response == GTK_RESPONSE_ACCEPT) owner.finishMutation(exists ? "pack.update" : "pack.import", directory);
            else { owner.busy = false; owner.updateActions(); }
        });
    }

    std::function<void(std::string)> notify;
    BackendClient client;
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>(this);
    GtkWidget *root = nullptr, *list = nullptr, *import = nullptr, *reload = nullptr;
    GtkWidget *toggle = nullptr, *remove = nullptr, *count = nullptr, *state = nullptr;
    GtkWidget *detailsText = nullptr, *detailsToggle = nullptr, *dialog = nullptr;
    GtkListStore *store = nullptr;
    std::string pendingFocus;
    std::vector<Pack> packs;
    std::uint64_t generation = 0;
    bool busy = false, loaded = false, alive = true;
};

DictionaryPackManager::DictionaryPackManager(std::function<void(std::string)> notify)
    : impl_(std::make_unique<Impl>(std::move(notify))) {}
DictionaryPackManager::~DictionaryPackManager() = default;
GtkWidget *DictionaryPackManager::widget() const { return impl_->widget(); }
void DictionaryPackManager::refresh() { impl_->refresh(); }
bool DictionaryPackManager::focusTarget(std::string_view target) { return impl_->focusTarget(target); }

} // namespace modernime::settings
