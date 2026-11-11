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
        setAccessibleWidgetText(refreshButton, "重新检查输入状态", "更新概览中的运行状态和本地内容");
        gtk_widget_set_tooltip_text(refreshButton, "重新检查输入状态");
        g_signal_connect(refreshButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (self.refresh) self.refresh();
        }), this);
        detail::GtkWidgetGuard pageGuard(createPageShell("概览", "", refreshButton));
        page = pageGuard.get();
        buildInputWorkbench();
        content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_hexpand(content, TRUE);
        gtk_box_pack_start(GTK_BOX(page), content, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content),
            createEmptyState("正在读取本地内容…", "词典、剪贴板和学习记录会显示在这里。"),
            FALSE, FALSE, 0);
        pageGuard.release();
    }

    void setSnapshot(const OverviewSnapshot &snapshot) {
        clearContainer(summary);
        clearContainer(content);
        setupAction = snapshot.setupAction;
        gtk_box_pack_start(GTK_BOX(summary),
            label(snapshot.runtimeSummary, "modernime-overview-status"), TRUE, TRUE, 0);
        gtk_label_set_text(GTK_LABEL(setupDescription), snapshot.setupDescription.c_str());
        gtk_button_set_label(GTK_BUTTON(setupButton), snapshot.setupActionLabel.c_str());
        // Healthy state needs no duplicate setup paragraph or redundant action.
        gtk_widget_set_visible(setupActions, setupAction != OverviewSetupAction::None);
        gtk_widget_set_tooltip_text(trialEntry, snapshot.trialInstructions.c_str());
        setAccessibleWidgetText(trialEntry, "试输入", snapshot.trialInstructions);

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

        // Two task-oriented shortcuts, followed by a single compact content list.
        auto *shortcuts = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(shortcuts), GTK_SELECTION_NONE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(shortcuts), TRUE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(shortcuts), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(shortcuts), 2);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(shortcuts), 10);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(shortcuts), 10);
        addStyleClass(shortcuts, "modernime-overview-shortcuts");
        gtk_flow_box_insert(GTK_FLOW_BOX(shortcuts), createShortcut(
            "候选与外观", "配色、字体与候选栏",
            SettingsPageId::Candidates), -1);
        gtk_flow_box_insert(GTK_FLOW_BOX(shortcuts), createShortcut(
            "输入与按键", "默认" + snapshot.defaultMode + "，" + snapshot.toggleKey + " 切换中英",
            SettingsPageId::Input), -1);
        gtk_box_pack_start(GTK_BOX(content), shortcuts, FALSE, FALSE, 0);

        auto *library = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        addStyleClass(library, "modernime-overview-library");
        auto *libraryTitle = label("本地内容", "modernime-overview-library-title");
        gtk_box_pack_start(GTK_BOX(library), libraryTitle, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(library), createContentRow(
            "个人词典", snapshot.dictionaryEntries == 0 ? "添加常用词与专用短语" : "管理常用词与专用短语",
            snapshot.dictionaryEntries, "词条", SettingsPageId::Dictionary),
            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(library), createContentRow(
            "剪贴板", snapshot.clipboardShortcut,
            snapshot.clipboardEntries, "记录", SettingsPageId::Clipboard), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(library), createContentRow(
            "智能学习", "查看与管理输入习惯",
            snapshot.learningEntries, "记录", SettingsPageId::Learning), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(content), library, FALSE, FALSE, 0);
        gtk_widget_show_all(summary);
        gtk_widget_show_all(content);
    }

    GtkWidget *createShortcut(std::string_view title, std::string_view description,
                              SettingsPageId destination) {
        auto *button = createDestinationButton(title, destination, navigate, description);
        gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(button)));
        addStyleClass(button, "modernime-overview-shortcut");
        gtk_widget_set_size_request(button, 250, -1);
        auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto *icon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        addStyleClass(icon, "modernime-overview-shortcut-icon");
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(icon), createSettingsPageIcon(destination, 24), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        auto *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_box_pack_start(GTK_BOX(text), label(title, "modernime-setting-title"), FALSE, FALSE, 0);
        auto *descriptionLabel = label(description, "modernime-setting-desc");
        gtk_label_set_max_width_chars(GTK_LABEL(descriptionLabel), 20);
        gtk_box_pack_start(GTK_BOX(text), descriptionLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(button), row);
        return button;
    }

    GtkWidget *createContentRow(std::string_view title, std::string_view description,
                               std::size_t count,
                               std::string_view unit, SettingsPageId destination) {
        auto *button = createDestinationButton(title, destination, navigate, description);
        gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(button)));
        // Pointer activation navigates immediately; keyboard traversal retains
        // its own visible focus instead of leaving a selected-looking row.
        gtk_widget_set_focus_on_click(button, FALSE);
        addStyleClass(button, "modernime-overview-content-row");
        auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        auto *icon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        addStyleClass(icon, "modernime-overview-content-icon");
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(icon), createSettingsPageIcon(destination, 20), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
        auto *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        auto *titleLabel = label(title, "modernime-setting-title");
        addStyleClass(titleLabel, "modernime-overview-content-title");
        gtk_box_pack_start(GTK_BOX(text), titleLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text), label(description, "modernime-setting-desc"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
        auto *value = label(std::to_string(count) + " " + std::string(unit), "modernime-overview-count");
        gtk_label_set_line_wrap(GTK_LABEL(value), FALSE);
        gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(row), value, FALSE, FALSE, 0);
        auto *arrow = gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        addStyleClass(arrow, "modernime-overview-content-arrow");
        gtk_widget_set_valign(arrow, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(row), arrow, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(button), row);
        return button;
    }

    void buildInputWorkbench() {
        auto *workbench = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        addStyleClass(workbench, "modernime-overview-workbench");
        gtk_box_pack_start(GTK_BOX(page), workbench, FALSE, FALSE, 0);
        auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(summary), label("正在检查输入状态…", "modernime-overview-status"), TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(header), summary, TRUE, TRUE, 0);
        auto *diagnostics = createDestinationButton("查看诊断", SettingsPageId::Diagnostics,
            navigate, "检查输入法运行状态并处理问题");
        addStyleClass(diagnostics, "modernime-overview-diagnostics");
        gtk_widget_set_valign(diagnostics, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(header), diagnostics, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(workbench), header, FALSE, FALSE, 0);

        setupActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_no_show_all(setupActions, TRUE);
        setupDescription = label("正在检查输入法状态…", "modernime-setting-desc");
        gtk_box_pack_start(GTK_BOX(setupActions), setupDescription, TRUE, TRUE, 0);
        setupButton = gtk_button_new_with_label("启用 ModernIME");
        addStyleClass(setupButton, "suggested-action");
        gtk_widget_set_valign(setupButton, GTK_ALIGN_CENTER);
        g_signal_connect(setupButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            gtk_widget_grab_focus(self.trialEntry);
            if (self.setup) self.setup(self.setupAction);
        }), this);
        gtk_box_pack_start(GTK_BOX(setupActions), setupButton, FALSE, FALSE, 0);
        gtk_widget_show_all(setupDescription);
        gtk_widget_show_all(setupButton);
        gtk_box_pack_start(GTK_BOX(workbench), setupActions, FALSE, FALSE, 0);

        trialEntry = gtk_entry_new();
        addStyleClass(trialEntry, "modernime-overview-trial");
        gtk_entry_set_placeholder_text(GTK_ENTRY(trialEntry), "试试输入中文");
        gtk_entry_set_width_chars(GTK_ENTRY(trialEntry), 12);
        setAccessibleWidgetText(trialEntry, "试输入", "使用当前系统输入法试输入，不修改设置");
        gtk_box_pack_start(GTK_BOX(workbench), trialEntry, FALSE, FALSE, 0);
    }

    void setSetupBusy(bool busy) {
        gtk_widget_set_sensitive(setupButton, !busy);
        gtk_widget_set_sensitive(refreshButton, !busy);
        if (busy) gtk_label_set_text(GTK_LABEL(setupDescription), "正在处理，请稍候…");
    }

    std::function<void(SettingsPageId)> navigate;
    std::function<void(OverviewSetupAction)> setup;
    std::function<void()> refresh;
    OverviewSetupAction setupAction = OverviewSetupAction::None;
    GtkWidget *summary = nullptr;
    GtkWidget *setupActions = nullptr;
    GtkWidget *setupDescription = nullptr;
    GtkWidget *setupButton = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *trialEntry = nullptr;
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
