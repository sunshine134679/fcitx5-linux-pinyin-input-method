#include "modernime/settings/application_diagnostics.h"

#include "modernime/settings/backend_client.h"
#include "modernime/settings/detail/diagnostics_lifetime.h"
#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_disclosure.h"
#include "modernime/settings/settings_widgets.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {

void style(GtkWidget *widget, const char *name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

void target(GtkWidget *widget, const char *id) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target", g_strdup(id), g_free);
}

GtkWidget *wrappedLabel(const char *text) {
    auto *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    return label;
}

void setLabel(GtkWidget *widget, const std::string &text) {
    gtk_label_set_text(GTK_LABEL(widget), text.c_str());
}

std::string knownText(const BackendValue &data, std::string_view key) {
    const auto value = data.text(key);
    return value.empty() || value == "unknown" ? "未知" : value;
}

std::string booleanText(const BackendValue &data, std::string_view key, bool observed,
                        const char *yes, const char *no) {
    return observed && data.get(key).kind == BackendValue::Kind::Boolean
        ? (data.boolean(key) ? yes : no) : "未知";
}

std::string numberText(const BackendValue &data, std::string_view key, bool observed,
                       const char *unit = "") {
    return observed && data.get(key).kind == BackendValue::Kind::Number
        ? std::to_string(data.number(key)) + unit : "未知";
}

// Application identities can be long, but the exact value remains in the tooltip.
void constrainChoiceLabels(GtkWidget *widget) {
    if (GTK_IS_LABEL(widget)) {
        gtk_label_set_max_width_chars(GTK_LABEL(widget), 32);
        gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    }
    if (!GTK_IS_CONTAINER(widget)) return;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (auto *item = children; item; item = item->next)
        constrainChoiceLabels(GTK_WIDGET(item->data));
    g_list_free(children);
}

} // namespace

class ApplicationDiagnostics::Impl final {
public:
    Impl() : lifetime(std::make_shared<Lifetime>(this)) {
        detail::GtkWidgetGuard guard(createSectionCard("应用兼容性诊断", ""));
        root = guard.get();
        target(root, "application-diagnostics");
        setAccessibleWidgetText(root, "应用兼容性诊断", "查看应用的输入状态");
        g_signal_connect_data(root, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            (*static_cast<SharedLifetime *>(data))->withOwner([](Impl &owner) {
                owner.root = nullptr;
                ++owner.generation;
                owner.busy = false;
                owner.lifetime->deactivate();
            });
        }), new SharedLifetime(lifetime), destroySignalState, GConnectFlags(0));

        auto *intro = wrappedLabel("先在要检查的应用里打几个字，再回来点“刷新”。");
        style(intro, "modernime-setting-desc");
        gtk_widget_set_margin_bottom(intro, 14);
        gtk_box_pack_start(GTK_BOX(root), intro, FALSE, FALSE, 0);

        auto *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        selectorSlot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(selectorSlot, TRUE);
        gtk_box_pack_start(GTK_BOX(controls), selectorSlot, TRUE, TRUE, 0);
        refreshButton = gtk_button_new_with_label("刷新");
        target(refreshButton, "application-refresh");
        setAccessibleWidgetText(refreshButton, "刷新应用诊断", "重新读取已观察的应用和所选应用的诊断信息");
        gtk_box_pack_end(GTK_BOX(controls), refreshButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(root), controls, FALSE, FALSE, 0);
        g_signal_connect_data(refreshButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            (*static_cast<SharedLifetime *>(data))->withOwner([](Impl &owner) { owner.refresh(); });
        }), new SharedLifetime(lifetime), destroySignalState, GConnectFlags(0));
        rebuildSelector({});

        auto *summaryRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(summaryRow, 18);
        spinner = gtk_spinner_new();
        gtk_widget_set_no_show_all(spinner, TRUE);
        gtk_widget_set_valign(spinner, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(summaryRow), spinner, FALSE, FALSE, 0);
        summary = wrappedLabel("正在读取应用状态…");
        target(summary, "application-summary");
        style(summary, "modernime-status-heading");
        gtk_box_pack_start(GTK_BOX(summaryRow), summary, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(root), summaryRow, FALSE, FALSE, 0);
        status = wrappedLabel("");
        target(status, "application-status");
        style(status, "modernime-status-hint");
        gtk_widget_set_margin_top(status, 6);
        gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);

        facts = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(facts), 18);
        gtk_grid_set_row_spacing(GTK_GRID(facts), 8);
        gtk_widget_set_margin_top(facts, 16);
        const auto addFact = [&](int row, const char *title, const char *id, GtkWidget *&value) {
            auto *caption = gtk_label_new(title);
            gtk_label_set_xalign(GTK_LABEL(caption), 0.0F);
            gtk_widget_set_valign(caption, GTK_ALIGN_START);
            style(caption, "modernime-setting-desc");
            value = wrappedLabel("未知");
            target(value, id);
            setAccessibleWidgetText(value, {}, title);
            gtk_grid_attach(GTK_GRID(facts), caption, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(facts), value, 1, row, 1, 1);
        };
        addFact(0, "连接方式", "application-transport", transport);
        addFact(1, "输入框状态", "application-focused", focused);
        addFact(2, "敏感输入", "application-sensitive", sensitive);
        addFact(3, "ModernIME", "application-selected", selected);
        gtk_box_pack_start(GTK_BOX(root), facts, FALSE, FALSE, 0);

        suggestions = wrappedLabel("");
        target(suggestions, "application-suggestions");
        gtk_widget_set_margin_top(suggestions, 16);
        gtk_box_pack_start(GTK_BOX(root), suggestions, FALSE, FALSE, 0);

        detailsToggle = gtk_expander_new("技术详情");
        target(detailsToggle, "application-details");
        setAccessibleWidgetText(detailsToggle, "应用诊断技术详情", "展开或收起宿主能力、观察时钟和引擎派发耗时");
        auto *detailsContent = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        style(detailsContent, "modernime-details-content");
        technical = wrappedLabel("");
        gtk_label_set_selectable(GTK_LABEL(technical), TRUE);
        target(technical, "application-technical-details");
        gtk_box_pack_start(GTK_BOX(detailsContent), technical, FALSE, FALSE, 0);
        disclosure = createSettingsDisclosure(detailsToggle, detailsContent);
        style(disclosure, "modernime-details");
        style(disclosure, "modernime-compact-disclosure");
        gtk_widget_set_margin_top(disclosure, 12);
        gtk_box_pack_start(GTK_BOX(root), disclosure, FALSE, FALSE, 0);
        for (auto *dynamic : {facts, suggestions, disclosure}) {
            gtk_widget_show_all(dynamic);
            gtk_widget_set_no_show_all(dynamic, TRUE);
            gtk_widget_hide(dynamic);
        }
        updateFocusChain();
        refresh();
        guard.release();
    }

    ~Impl() {
        ++generation;
        lifetime->deactivate();
    }

    void refresh() {
        if (!root || busy) return;
        const auto request = ++generation;
        startLoading("正在读取应用状态…", "正在读取应用列表…", true);
        auto weak = std::weak_ptr<Lifetime>(lifetime);
        backend.execute("applications", {}, [weak, request](BackendReply reply) {
            if (const auto state = weak.lock()) state->withOwner([&](Impl &owner) {
                if (!owner.isCurrent(request)) return;
                if (!reply.ok) { owner.showError(reply); return; }
                if (reply.data.get("applications").kind != BackendValue::Kind::Array) {
                    owner.showInvalidReply("无法识别应用列表，请刷新重试。");
                    return;
                }
                std::vector<std::string> identities;
                for (const auto &item : reply.data.array("applications")) {
                    if (item.kind != BackendValue::Kind::String) continue;
                    auto identity = item.text();
                    if (!identity.empty() && std::find(identities.begin(), identities.end(), identity) == identities.end())
                        identities.push_back(std::move(identity));
                    if (identities.size() == 32) break;
                }
                owner.rebuildSelector(std::move(identities));
                owner.readReport(request);
            });
        });
    }

    bool focusTarget(std::string_view id) {
        if (!root) return false;
        GtkWidget *control = nullptr;
        if (id == "application-diagnostics" || id == "application-selector") control = selector;
        else if (id == "application-refresh") control = refreshButton;
        else if (id == "application-details") {
            if (!gtk_widget_get_visible(disclosure)) return focusWidgetOrFallback(selector, root);
            control = detailsToggle;
        } else return false;
        if (!gtk_widget_get_sensitive(control) && busy) {
            pendingFocus = id;
            return true;
        }
        return focusWidgetOrFallback(control, root);
    }

    GtkWidget *root = nullptr;

private:
    using Lifetime = detail::DiagnosticsLifetime<Impl>;
    using SharedLifetime = std::shared_ptr<Lifetime>;

    static void destroySignalState(gpointer data, GClosure *) {
        delete static_cast<SharedLifetime *>(data);
    }

    bool isCurrent(std::uint64_t request) const { return root && request == generation; }

    void rebuildSelector(std::vector<std::string> identities) {
        if (selector && identities == applications) return;
        if (selector) gtk_widget_destroy(selector);
        applications = std::move(identities);
        const auto found = std::find(applications.begin(), applications.end(), application);
        const int active = application.empty() || found == applications.end()
            ? 0 : static_cast<int>(found - applications.begin()) + 1;
        if (!active) application.clear();
        selector = createSettingsChoice();
        target(selector, "application-selector");
        setAccessibleWidgetText(selector, "查看应用", "选择最近应用或最近使用过的应用");
        gtk_widget_set_halign(selector, GTK_ALIGN_START);
        appendSettingsChoice(selector, "最近应用");
        for (const auto &identity : applications) appendSettingsChoice(selector, identity);
        setSettingsChoiceActive(selector, active);
        constrainChoiceLabels(selector);
        constrainChoiceLabels(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(selector))));
        gtk_box_pack_start(GTK_BOX(selectorSlot), selector, FALSE, FALSE, 0);
        gtk_widget_show_all(selector);
        g_signal_connect_data(selector, "changed", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            (*static_cast<SharedLifetime *>(data))->withOwner([](Impl &owner) {
                const auto index = settingsChoiceActive(owner.selector);
                if (index < 0 || index > static_cast<int>(owner.applications.size())) return;
                owner.application = index ? owner.applications[static_cast<std::size_t>(index - 1)] : "";
                owner.readReport(++owner.generation);
            });
        }), new SharedLifetime(lifetime), destroySignalState, GConnectFlags(0));
        updateFocusChain();
    }

    void updateFocusChain() {
        if (refreshButton && detailsToggle) setSettingsFocusChain(root, {selectorSlot, refreshButton, detailsToggle});
    }

    void startLoading(const char *title, const char *message, bool listLoading) {
        busy = true;
        setLabel(summary, title);
        setLabel(status, message);
        gtk_button_set_label(GTK_BUTTON(refreshButton), "读取中…");
        gtk_widget_set_sensitive(refreshButton, FALSE);
        gtk_widget_set_sensitive(selector, !listLoading);
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_widget_show(spinner);
        gtk_widget_hide(facts);
        gtk_widget_hide(suggestions);
        gtk_widget_hide(disclosure);
    }

    void readReport(std::uint64_t request) {
        if (!isCurrent(request)) return;
        startLoading("正在读取应用诊断…", "正在读取这个应用的输入状态…", false);
        const auto identity = application;
        auto weak = std::weak_ptr<Lifetime>(lifetime);
        backend.execute("diagnostics", identity.empty() ? std::vector<std::string>{} : std::vector<std::string>{identity},
            [weak, request, identity](BackendReply reply) {
                if (const auto state = weak.lock()) state->withOwner([&](Impl &owner) {
                    if (!owner.isCurrent(request) || identity != owner.application) return;
                    if (!reply.ok) { owner.showError(reply); return; }
                    if (reply.data.get("observed").kind != BackendValue::Kind::Boolean) {
                        owner.showInvalidReply("暂时无法查看结果，请刷新重试。");
                        return;
                    }
                    owner.showReport(reply.data);
                });
            });
    }

    void finishLoading(bool failed) {
        busy = false;
        gtk_spinner_stop(GTK_SPINNER(spinner));
        gtk_widget_hide(spinner);
        gtk_button_set_label(GTK_BUTTON(refreshButton), failed ? "重试" : "刷新");
        gtk_widget_set_sensitive(refreshButton, TRUE);
        gtk_widget_set_sensitive(selector, TRUE);
        if (!pendingFocus.empty()) {
            auto id = std::exchange(pendingFocus, {});
            focusTarget(id);
        }
    }

    void showInvalidReply(const char *message) {
        BackendReply failure;
        failure.error = "invalid_response";
        failure.message = message;
        showError(failure);
    }

    void showError(const BackendReply &reply) {
        setLabel(summary, "暂时无法读取应用诊断");
        setLabel(status, reply.message.empty()
            ? "请确认 Fcitx5 与 ModernIME 正在运行，然后重试。" : reply.message);
        setLabel(technical, "错误标识：" + (reply.error.empty() ? std::string("未知") : reply.error));
        gtk_widget_show(disclosure);
        finishLoading(true);
    }

    void showReport(const BackendValue &data) {
        const bool observed = data.boolean("observed");
        const auto name = data.text("application");
        setLabel(summary, observed ? (name.empty() ? "未知应用" : name)
            : (application.empty() ? "还没有检测到应用" : "还没有检测到这个应用"));
        setLabel(status, observed ? "最近一次使用输入法时的状态。"
            : "先在这个应用里打几个字，再回来点“刷新”。");
        const auto protocol = data.text("transport");
        setLabel(transport, observed && protocol == "x11" ? "X11"
            : observed && protocol == "wayland" ? "Wayland" : "未知");
        setLabel(focused, booleanText(data, "focused", observed, "已激活", "未激活"));
        setLabel(sensitive, booleanText(data, "sensitive", observed, "是（隐私保护中）", "否"));
        setLabel(selected, booleanText(data, "modernimeSelected", observed, "已选用", "未选用"));
        gtk_widget_show(facts);

        std::string advice;
        for (const auto &item : data.array("suggestions")) {
            if (item.kind != BackendValue::Kind::String || item.text().empty()) continue;
            if (!advice.empty()) advice += '\n';
            advice += "• " + item.text();
        }
        setLabel(suggestions, advice);
        gtk_widget_set_visible(suggestions, !advice.empty());

        const auto textFact = [&](std::string_view key) {
            return observed ? knownText(data, key) : std::string("未知");
        };
        std::string detail = "应用身份：" + (name.empty() ? (application.empty() ? std::string("未知") : application) : name) +
            "\nFcitx 前端：" + textFact("frontend") +
            "\n显示身份：" + textFact("display") +
            "\n工具包：" + textFact("toolkit") +
            "\n沙盒：" + textFact("sandbox") +
            "\n周边文本能力：" + booleanText(data, "surroundingText", observed, "已提供", "未提供") +
            "\n客户端预编辑：" + booleanText(data, "clientPreedit", observed, "已提供", "未提供") +
            "\n有效光标矩形：" + booleanText(data, "cursorRectangle", observed, "已观察到", "尚未观察到") +
            "\n观察时间：" + numberText(data, "observedAtMs", observed, " ms") +
            "（进程单调时钟，仅可在同一会话比较）" +
            "\n已处理按键：" + numberText(data, "handledKeys", observed) + "（敏感输入期间不计数）" +
            "\n引擎事件派发最大耗时：" + numberText(data, "maximumDispatchUs", observed, " μs") +
            "\n派发耗时不含应用绘制及端到端显示延迟。";
        if (observed && data.boolean("sensitive"))
            detail += "\n敏感输入受隐私边界限制；周边文本能力值不代表应用的普通输入能力。";
        setLabel(technical, detail);
        gtk_widget_show(disclosure);
        finishLoading(false);
    }

    BackendClient backend;
    SharedLifetime lifetime;
    std::uint64_t generation = 0;
    bool busy = false;
    std::vector<std::string> applications;
    std::string application;
    std::string pendingFocus;
    GtkWidget *selectorSlot = nullptr;
    GtkWidget *selector = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *spinner = nullptr;
    GtkWidget *summary = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *facts = nullptr;
    GtkWidget *transport = nullptr;
    GtkWidget *focused = nullptr;
    GtkWidget *sensitive = nullptr;
    GtkWidget *selected = nullptr;
    GtkWidget *suggestions = nullptr;
    GtkWidget *disclosure = nullptr;
    GtkWidget *detailsToggle = nullptr;
    GtkWidget *technical = nullptr;
};

ApplicationDiagnostics::ApplicationDiagnostics() : impl_(std::make_unique<Impl>()) {}
ApplicationDiagnostics::~ApplicationDiagnostics() = default;
GtkWidget *ApplicationDiagnostics::widget() const { return impl_->root; }
void ApplicationDiagnostics::refresh() { impl_->refresh(); }
bool ApplicationDiagnostics::focusTarget(std::string_view target) { return impl_->focusTarget(target); }

} // namespace modernime::settings
