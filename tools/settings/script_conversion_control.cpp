#include "modernime/settings/script_conversion_control.h"
#include "modernime/settings/settings_widgets.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace modernime::settings {
namespace {
constexpr auto uri = "fcitx://config/addon/chttrans";
void target(GtkWidget *widget, const char *id) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(id), g_free);
}
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
Variant readConfig(GDBusConnection *bus, GCancellable *cancel, GError **error) {
    Variant reply(g_dbus_connection_call_sync(bus, "org.fcitx.Fcitx5", "/controller",
        "org.fcitx.Fcitx.Controller1", "GetConfig", g_variant_new("(s)", uri),
        G_VARIANT_TYPE("(va(sa(sssva{sv})))"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, cancel, error), g_variant_unref);
    if (!reply) return {nullptr, g_variant_unref};
    Variant boxed(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
    return {g_variant_get_variant(boxed.get()), g_variant_unref};
}
bool methods(GVariant *config, std::vector<std::string> &values) {
    if (!g_variant_is_of_type(config, G_VARIANT_TYPE_VARDICT)) return false;
    Variant enabled(g_variant_lookup_value(config, "EnabledIM", nullptr), g_variant_unref);
    if (!enabled) return false;
    if (g_variant_is_of_type(enabled.get(), G_VARIANT_TYPE_STRING))
        return std::string_view(g_variant_get_string(enabled.get(), nullptr)).empty();
    if (!g_variant_is_of_type(enabled.get(), G_VARIANT_TYPE_VARDICT)) return false;
    GVariantIter iter; g_variant_iter_init(&iter, enabled.get());
    const char *key = nullptr; GVariant *value = nullptr;
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        Variant owned(value, g_variant_unref);
        if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) return false;
        values.emplace_back(g_variant_get_string(value, nullptr));
    }
    return true;
}
std::string hotkeys(GVariant *config) {
    Variant keys(g_variant_lookup_value(config, "Hotkey", G_VARIANT_TYPE_VARDICT), g_variant_unref);
    std::string text;
    if (keys) {
        GVariantIter iter; g_variant_iter_init(&iter, keys.get());
        const char *key = nullptr; GVariant *value = nullptr;
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
            Variant owned(value, g_variant_unref);
            if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) continue;
            std::string shortcut(g_variant_get_string(value, nullptr));
            if (shortcut.empty()) continue;
            if (shortcut.starts_with("Control+")) shortcut.replace(0, 8, "Ctrl+");
            if (!text.empty()) text += " / ";
            text += shortcut;
        }
    }
    return text.empty() ? "未设置切换快捷键，可在 Fcitx 配置中调整。" : "当前快捷键：" + text;
}
} // namespace

class ScriptConversionControl::Impl {
    struct Lifetime { Impl *owner = nullptr; };
    struct Work {
        std::shared_ptr<Lifetime> life;
        unsigned generation;
        std::optional<bool> desired;
        bool configure = false;
        bool traditional = false;
        std::string keys = {}, error = {};
    };
public:
    GtkWidget *root = nullptr, *control, *retry, *configure, *status, *shortcut, *details, *detailText;
    std::shared_ptr<Lifetime> life = std::make_shared<Lifetime>();
    GCancellable *cancel = g_cancellable_new();
    unsigned generation = 0;
    guint timer = 0;
    bool busy = false, known = false, traditional = false, updating = false;

    Impl() {
        life->owner = this;
        root = createSectionCard("简繁输出", "此项立即生效；其他设置仍需保存。");
        g_object_add_weak_pointer(G_OBJECT(root), reinterpret_cast<gpointer *>(&root));
        control = gtk_switch_new(); target(control, "script-traditional");
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        retry = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        target(retry, "script-refresh");
        setAccessibleWidgetText(retry, "重新查询简繁状态", "读取 Fcitx 当前简繁状态和快捷键");
        gtk_widget_set_tooltip_text(retry, "重新查询状态");
        gtk_box_pack_start(GTK_BOX(actions), retry, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), control, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), createSettingRow("繁体输出", "由 Fcitx 统一转换候选栏与上屏文字。", actions), FALSE, FALSE, 0);
        setAccessibleWidgetText(control, "繁体输出", "使用 Fcitx 简繁转换，切换立即生效");
        shortcut = gtk_label_new("正在读取快捷键…");
        status = gtk_label_new("正在读取简繁状态…"); target(status, "script-status");
        for (auto *label : {shortcut, status}) {
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
            gtk_style_context_add_class(gtk_widget_get_style_context(label), "modernime-description");
            gtk_box_pack_start(GTK_BOX(root), label, FALSE, FALSE, 0);
        }
        configure = gtk_button_new_with_label("打开 Fcitx 配置"); target(configure, "script-configure");
        gtk_widget_set_halign(configure, GTK_ALIGN_START);
        gtk_widget_set_no_show_all(configure, TRUE);
        gtk_box_pack_start(GTK_BOX(root), configure, FALSE, FALSE, 0);
        details = gtk_expander_new("技术详情"); gtk_widget_set_no_show_all(details, TRUE);
        detailText = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(detailText), 0);
        gtk_label_set_line_wrap(GTK_LABEL(detailText), TRUE); gtk_label_set_selectable(GTK_LABEL(detailText), TRUE);
        gtk_container_add(GTK_CONTAINER(details), detailText);
        gtk_box_pack_start(GTK_BOX(root), details, FALSE, FALSE, 0);
        g_signal_connect(control, "notify::active", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (!self.updating && !self.busy && self.known)
                self.run(gtk_switch_get_active(GTK_SWITCH(self.control)) != FALSE);
        }), this);
        g_signal_connect(retry, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->run(); }), this);
        g_signal_connect(configure, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) { static_cast<Impl *>(p)->run({}, true); }), this);
        g_signal_connect(root, "map", G_CALLBACK(+[](GtkWidget *, gpointer p) {
            auto &self = *static_cast<Impl *>(p); self.run();
            if (!self.timer) self.timer = g_timeout_add(1500, +[](gpointer p) -> gboolean {
                auto &owner = *static_cast<Impl *>(p); if (!owner.busy) owner.run(); return G_SOURCE_CONTINUE;
            }, &self);
        }), this);
        g_signal_connect(root, "unmap", G_CALLBACK(+[](GtkWidget *, gpointer p) {
            auto &self = *static_cast<Impl *>(p); if (self.timer) g_source_remove(self.timer); self.timer = 0;
        }), this);
        setSettingsFocusChain(root, {retry, control, configure, details});
        gtk_widget_set_sensitive(control, FALSE);
    }
    ~Impl() {
        life->owner = nullptr; g_cancellable_cancel(cancel); g_object_unref(cancel);
        if (timer) g_source_remove(timer);
        if (root) {
            g_signal_handlers_disconnect_by_data(root, this);
            g_object_remove_weak_pointer(G_OBJECT(root), reinterpret_cast<gpointer *>(&root));
            for (auto *widget : {control, retry, configure}) g_signal_handlers_disconnect_by_data(widget, this);
        }
    }
    void run(std::optional<bool> desired = {}, bool openConfiguration = false) {
        if (busy || !root) return;
        busy = true; ++generation;
        gtk_widget_set_sensitive(control, FALSE); gtk_widget_set_sensitive(retry, FALSE); gtk_widget_set_sensitive(configure, FALSE);
        if (desired) gtk_label_set_text(GTK_LABEL(status), "正在切换…");
        auto *task = g_task_new(nullptr, cancel, +[](GObject *, GAsyncResult *result, gpointer) {
            auto *work = static_cast<Work *>(g_task_get_task_data(G_TASK(result)));
            auto *owner = work->life->owner;
            if (owner && owner->root && owner->generation == work->generation) owner->complete(*work);
        }, nullptr);
        g_task_set_task_data(task, new Work{life, generation, desired, openConfiguration}, +[](gpointer p) { delete static_cast<Work *>(p); });
        g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancel) {
            auto &work = *static_cast<Work *>(data); GError *error = nullptr;
            auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, &error);
            if (bus) {
                if (work.configure) {
                    Variant reply(g_dbus_connection_call_sync(bus, "org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1",
                        "Configure", nullptr, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, cancel, &error), g_variant_unref);
                } else {
                    auto config = readConfig(bus, cancel, &error);
                    std::vector<std::string> enabled;
                    if (config && methods(config.get(), enabled)) {
                        if (work.desired) {
                            std::erase(enabled, "modernime"); if (*work.desired) enabled.emplace_back("modernime");
                            GVariantBuilder list; g_variant_builder_init(&list, G_VARIANT_TYPE_VARDICT);
                            for (std::size_t i = 0; i < enabled.size(); ++i)
                                g_variant_builder_add(&list, "{sv}", std::to_string(i).c_str(), g_variant_new_string(enabled[i].c_str()));
                            GVariantDict draft; g_variant_dict_init(&draft, config.get());
                            g_variant_dict_insert_value(&draft, "EnabledIM", g_variant_builder_end(&list));
                            Variant updated(g_variant_ref_sink(g_variant_dict_end(&draft)), g_variant_unref);
                            Variant saved(g_dbus_connection_call_sync(bus, "org.fcitx.Fcitx5", "/controller", "org.fcitx.Fcitx.Controller1", "SetConfig",
                                g_variant_new("(sv)", uri, updated.get()), nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, cancel, &error), g_variant_unref);
                            if (saved) { config = readConfig(bus, cancel, &error); enabled.clear(); }
                            if (!saved || !config || !methods(config.get(), enabled)) work.error = "写入后无法确认简繁状态";
                        }
                        if (work.error.empty() && !error) {
                            work.traditional = std::find(enabled.begin(), enabled.end(), "modernime") != enabled.end();
                            work.keys = hotkeys(config.get());
                            if (work.desired && work.traditional != *work.desired) work.error = "Fcitx 返回的状态与请求不一致";
                        }
                    } else if (config) work.error = "简繁配置格式无法识别，本次没有写入";
                }
                g_object_unref(bus);
            }
            if (error) { work.error = error->message; g_error_free(error); }
            g_task_return_boolean(task, TRUE);
        });
        g_object_unref(task);
    }
    void complete(const Work &work) {
        busy = false;
        known = work.error.empty() && !work.configure;
        updating = true;
        if (known) traditional = work.traditional;
        gtk_switch_set_active(GTK_SWITCH(control), traditional); updating = false;
        gtk_widget_set_sensitive(control, known); gtk_widget_set_sensitive(retry, TRUE); gtk_widget_set_sensitive(configure, TRUE);
        gtk_widget_set_visible(configure, !work.error.empty()); gtk_widget_set_visible(details, !work.error.empty());
        gtk_label_set_text(GTK_LABEL(detailText), work.error.c_str());
        if (!work.error.empty()) {
            gtk_label_set_text(GTK_LABEL(status), work.desired ? "切换失败。请检查 Fcitx 简繁转换插件后重试。" : "简繁转换暂不可用。请启动 Fcitx 并启用简繁转换插件，再刷新。");
            gtk_label_set_text(GTK_LABEL(shortcut), "无法读取当前快捷键");
        } else if (work.configure) {
            gtk_label_set_text(GTK_LABEL(status), "已请求打开 Fcitx 配置，调整后请刷新。");
        } else {
            gtk_label_set_text(GTK_LABEL(shortcut), work.keys.c_str());
            if (work.desired) gtk_label_set_text(GTK_LABEL(status), traditional ? "已切换为繁体，立即生效。" : "已切换为简体，立即生效。");
            else gtk_label_set_text(GTK_LABEL(status), "立即生效，不随主窗口“保存设置”或“放弃修改”改变。");
        }
        if (!work.error.empty()) gtk_widget_show_all(detailText);
    }
};

ScriptConversionControl::ScriptConversionControl() : impl_(std::make_unique<Impl>()) {}
ScriptConversionControl::~ScriptConversionControl() = default;
GtkWidget *ScriptConversionControl::widget() const { return impl_->root; }
void ScriptConversionControl::refresh() { impl_->run(); }
bool ScriptConversionControl::focusTarget(std::string_view id) {
    if (id == "script-traditional") return focusWidgetOrFallback(impl_->control, impl_->root);
    if (id == "script-refresh") return focusWidgetOrFallback(impl_->retry, impl_->root);
    return false;
}
} // namespace modernime::settings
