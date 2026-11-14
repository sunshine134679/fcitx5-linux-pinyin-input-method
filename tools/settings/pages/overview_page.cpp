#include "modernime/settings/pages/overview_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

GtkWidget *label(std::string_view text, std::string_view style) {
    auto *widget = gtk_label_new(std::string(text).c_str());
    gtk_label_set_xalign(GTK_LABEL(widget), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 45);
    addStyleClass(widget, style);
    return widget;
}

struct DestinationAction final {
    std::function<void(SettingsPageId)> navigate;
    SettingsPageId destination;
};

void onNavigate(GtkButton *, gpointer data) {
    const auto *action = static_cast<const DestinationAction *>(data);
    if (action->navigate) action->navigate(action->destination);
}

void destroyDestinationAction(gpointer data, GClosure *) {
    delete static_cast<DestinationAction *>(data);
}

GtkWidget *createDestinationButton(
    std::string_view title, SettingsPageId destination,
    const std::function<void(SettingsPageId)> &navigate,
    std::string_view description) {
    auto *button = gtk_button_new_with_label(std::string(title).c_str());
    setAccessibleWidgetText(button, title, description);
    g_signal_connect_data(
        button, "clicked", G_CALLBACK(onNavigate),
        new DestinationAction{navigate, destination},
        destroyDestinationAction, static_cast<GConnectFlags>(0));
    return button;
}

void clearContainer(GtkWidget *container) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (auto *item = children; item; item = item->next)
        gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
}

} // namespace

class OverviewPage::Impl final {
public:
    Impl(std::function<void(SettingsPageId)> navigateCallback,
         std::function<void(OverviewSetupAction)> setupCallback,
         std::function<void()> refreshCallback)
        : navigate(std::move(navigateCallback)), setup(std::move(setupCallback)),
          refresh(std::move(refreshCallback)) {
        refreshButton = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        setAccessibleWidgetText(refreshButton, "重新检查输入状态", "更新工作台中的运行状态和本地内容");
        gtk_widget_set_tooltip_text(refreshButton, "重新检查输入状态");
        g_signal_connect(refreshButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (self.refresh) self.refresh();
        }), this);
        detail::GtkWidgetGuard pageGuard(createPageShell("工作台", "", refreshButton));
        page = pageGuard.get();
        buildStatusLine();
        content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_hexpand(content, TRUE);
        gtk_box_pack_start(GTK_BOX(page), content, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content),
            createEmptyState("正在读取…", ""),
            FALSE, FALSE, 0);
        pageGuard.release();
    }

    void setSnapshot(const OverviewSnapshot &snapshot) {
        clearContainer(summary);
        clearContainer(content);
        setupAction = snapshot.setupAction;
        gtk_box_pack_start(GTK_BOX(summary), statusLabel(snapshot.runtimeSummary), TRUE, TRUE, 0);
        gtk_widget_set_tooltip_text(setupButton, snapshot.setupDescription.c_str());
        gtk_button_set_label(GTK_BUTTON(setupButton), snapshot.setupActionLabel.c_str());
        // Healthy state needs no duplicate setup paragraph or redundant action.
        gtk_widget_set_visible(setupActions, setupAction != OverviewSetupAction::None);

        if (!snapshot.notices.empty()) {
            auto *notices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            addStyleClass(notices, "modernime-overview-notices");
            for (const auto &notice : snapshot.notices) {
                auto *button = createDestinationButton("查看需处理的问题", notice.destination,
                    navigate, notice.message);
                gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(button)));
                addStyleClass(button, "modernime-overview-notice");
                auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
                gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_icon_name(
                    "dialog-information-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(row), label(notice.message, "modernime-setting-desc"), TRUE, TRUE, 0);
                gtk_box_pack_end(GTK_BOX(row), gtk_image_new_from_icon_name(
                    "go-next-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
                gtk_container_add(GTK_CONTAINER(button), row);
                gtk_box_pack_start(GTK_BOX(notices), button, FALSE, FALSE, 0);
            }
            gtk_box_pack_start(GTK_BOX(content), notices, FALSE, FALSE, 0);
        }

        auto *library = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        addStyleClass(library, "modernime-overview-list");
        gtk_box_pack_start(GTK_BOX(library), createListRow("我的词语", SettingsPageId::Dictionary,
            std::to_string(snapshot.dictionaryEntries) + " 词条"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(library), createListRow("剪贴板", SettingsPageId::Clipboard,
            std::to_string(snapshot.clipboardEntries) + " 条"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(library), createListRow("学习记录", SettingsPageId::Learning,
            std::to_string(snapshot.learningEntries) + " 条"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), library, FALSE, FALSE, 0);

        auto *settings = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        addStyleClass(settings, "modernime-overview-list");
        gtk_box_pack_start(GTK_BOX(settings), createListRow("候选与外观", SettingsPageId::Candidates), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(settings), createListRow("输入习惯", SettingsPageId::Input), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), settings, FALSE, FALSE, 0);
        gtk_widget_show_all(summary);
        gtk_widget_show_all(content);
    }

    GtkWidget *createListRow(std::string_view title, SettingsPageId destination,
                             std::string_view count = {}) {
        auto *button = createDestinationButton(title, destination, navigate, count);
        gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(button)));
        gtk_widget_set_focus_on_click(button, FALSE);
        addStyleClass(button, "modernime-overview-row");
        auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto *icon = createSettingsPageIcon(destination, 20);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        auto *titleLabel = label(title, "modernime-overview-row-title");
        gtk_widget_set_valign(titleLabel, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(row), titleLabel, TRUE, TRUE, 0);
        if (!count.empty()) {
            auto *value = label(count, "modernime-overview-row-count");
            gtk_label_set_line_wrap(GTK_LABEL(value), FALSE);
            gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(row), value, FALSE, FALSE, 0);
        }
        auto *arrow = gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_valign(arrow, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(row), arrow, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), row);
        return button;
    }

    static GtkWidget *statusLabel(std::string_view text) {
        auto *value = label(text, "modernime-overview-status-label");
        gtk_label_set_line_wrap(GTK_LABEL(value), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
        gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
        return value;
    }

    void buildStatusLine() {
        auto *statusLine = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        addStyleClass(statusLine, "modernime-overview-status-line");
        gtk_box_pack_start(GTK_BOX(page), statusLine, FALSE, FALSE, 0);
        summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(summary), statusLabel("正在检查输入状态…"), TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(statusLine), summary, TRUE, TRUE, 0);

        setupActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_no_show_all(setupActions, TRUE);
        setupButton = gtk_button_new_with_label("启用 ModernIME");
        addStyleClass(setupButton, "suggested-action");
        gtk_widget_set_valign(setupButton, GTK_ALIGN_CENTER);
        g_signal_connect(setupButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (self.setup) self.setup(self.setupAction);
        }), this);
        gtk_box_pack_start(GTK_BOX(setupActions), setupButton, FALSE, FALSE, 0);
        gtk_widget_show_all(setupButton);
        gtk_box_pack_start(GTK_BOX(statusLine), setupActions, FALSE, FALSE, 0);

        auto *diagnostics = createDestinationButton("诊断", SettingsPageId::Diagnostics, navigate, "");
        gtk_widget_set_valign(diagnostics, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(statusLine), diagnostics, FALSE, FALSE, 0);
    }

    void setSetupBusy(bool busy) {
        gtk_widget_set_sensitive(setupButton, !busy);
        gtk_widget_set_sensitive(refreshButton, !busy);
    }

    std::function<void(SettingsPageId)> navigate;
    std::function<void(OverviewSetupAction)> setup;
    std::function<void()> refresh;
    OverviewSetupAction setupAction = OverviewSetupAction::None;
    GtkWidget *summary = nullptr;
    GtkWidget *setupActions = nullptr;
    GtkWidget *setupButton = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *page = nullptr;
    GtkWidget *content = nullptr;
};

OverviewPage::OverviewPage(std::function<void(SettingsPageId)> navigate,
                           std::function<void(OverviewSetupAction)> setup,
                           std::function<void()> refresh)
    : impl_(std::make_unique<Impl>(std::move(navigate), std::move(setup), std::move(refresh))) {}

OverviewPage::~OverviewPage() = default;

GtkWidget *OverviewPage::widget() const { return impl_->page; }
void OverviewPage::setSnapshot(const OverviewSnapshot &snapshot) { impl_->setSnapshot(snapshot); }
void OverviewPage::setSetupBusy(bool busy) { impl_->setSetupBusy(busy); }

} // namespace modernime::settings
