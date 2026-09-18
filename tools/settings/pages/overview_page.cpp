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

struct DestinationAction final {
    std::function<void(SettingsPageId)> navigate;
    SettingsPageId destination;
};

void onNavigate(GtkButton *, gpointer data) {
    const auto *action = static_cast<const DestinationAction *>(data);
    if (action->navigate) {
        action->navigate(action->destination);
    }
}

void destroyDestinationAction(gpointer data, GClosure *) {
    delete static_cast<DestinationAction *>(data);
}

GtkWidget *createDestinationButton(
    std::string_view label, SettingsPageId destination,
    const std::function<void(SettingsPageId)> &navigate,
    std::string_view description = "打开对应设置页面") {
    auto *button = gtk_button_new_with_label(std::string(label).c_str());
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    setAccessibleWidgetText(button, label, description);
    gtk_widget_set_tooltip_text(button, std::string(description).c_str());
    g_signal_connect_data(
        button, "clicked", G_CALLBACK(onNavigate),
        new DestinationAction{navigate, destination},
        destroyDestinationAction, static_cast<GConnectFlags>(0));
    return button;
}

void clearContainer(GtkWidget *container) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (auto *item = children; item != nullptr; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

} // namespace

class OverviewPage::Impl final {
public:
    explicit Impl(std::function<void(SettingsPageId)> navigateCallback)
        : navigate(std::move(navigateCallback)) {
        detail::GtkWidgetGuard pageGuard(
            createPageShell("概览", "查看 ModernIME 设置和本地数据概况"));
        page = pageGuard.get();
        content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_hexpand(content, TRUE);
        gtk_box_pack_start(GTK_BOX(page), content, TRUE, TRUE, 0);
        gtk_box_pack_start(
            GTK_BOX(content),
            createEmptyState("正在加载概览…",
                             "正在后台读取运行状态和本地数据摘要。"),
            FALSE, FALSE, 0);
        pageGuard.release();
    }

    void setSnapshot(const OverviewSnapshot &snapshot) {
        clearContainer(content);

        // 1. Hero Status Card
        auto *heroCard = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        addStyleClass(heroCard, "modernime-hero-card");

        auto *heroLeft = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_hexpand(heroLeft, TRUE);
        gtk_widget_set_valign(heroLeft, GTK_ALIGN_CENTER);

        auto *heroTitle = gtk_label_new("ModernIME 拼音输入法");
        addStyleClass(heroTitle, "modernime-hero-title");
        gtk_widget_set_halign(heroTitle, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(heroLeft), heroTitle, FALSE, FALSE, 0);

        const bool isRunning = snapshot.runtimeSummary.find("运行") != std::string::npos &&
                               snapshot.runtimeSummary.find("未运行") == std::string::npos;
        auto *statusBadge = gtk_label_new(
            (std::string(isRunning ? "● " : "○ ") + snapshot.runtimeSummary).c_str());
        addStyleClass(statusBadge, isRunning ? "modernime-badge-green" : "modernime-badge-amber");
        gtk_widget_set_halign(statusBadge, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(heroLeft), statusBadge, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(heroCard), heroLeft, TRUE, TRUE, 0);

        auto *diagBtn = createDestinationButton("系统诊断与状态", SettingsPageId::Diagnostics,
                                                navigate, "打开系统与诊断页面查看详细日志");
        gtk_widget_set_valign(diagBtn, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(heroCard), diagBtn, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(content), heroCard, FALSE, FALSE, 0);

        // 2. 2x2 Bento Grid
        auto *grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_widget_set_hexpand(grid, TRUE);

        const auto createBentoCard = [this](std::string_view iconName,
                                            std::string_view title,
                                            std::string_view value,
                                            std::string_view desc,
                                            std::string_view actionText,
                                            SettingsPageId dest) {
            auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            addStyleClass(card, "modernime-bento-card");
            gtk_widget_set_hexpand(card, TRUE);

            auto *hBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            auto *icon = gtk_image_new_from_icon_name(std::string(iconName).c_str(), GTK_ICON_SIZE_BUTTON);
            gtk_box_pack_start(GTK_BOX(hBox), icon, FALSE, FALSE, 0);
            auto *titleLabel = gtk_label_new(std::string(title).c_str());
            addStyleClass(titleLabel, "modernime-bento-title");
            gtk_widget_set_halign(titleLabel, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(hBox), titleLabel, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(card), hBox, FALSE, FALSE, 0);

            auto *valLabel = gtk_label_new(std::string(value).c_str());
            addStyleClass(valLabel, "modernime-bento-value");
            gtk_widget_set_halign(valLabel, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(card), valLabel, FALSE, FALSE, 0);

            auto *descLabel = gtk_label_new(std::string(desc).c_str());
            addStyleClass(descLabel, "modernime-bento-desc");
            gtk_widget_set_halign(descLabel, GTK_ALIGN_START);
            gtk_label_set_line_wrap(GTK_LABEL(descLabel), TRUE);
            gtk_box_pack_start(GTK_BOX(card), descLabel, TRUE, TRUE, 0);

            auto *btn = createDestinationButton(actionText, dest, navigate);
            gtk_widget_set_halign(btn, GTK_ALIGN_START);
            gtk_box_pack_end(GTK_BOX(card), btn, FALSE, FALSE, 0);

            return card;
        };

        auto *inputCard = createBentoCard(
            "input-keyboard-symbolic",
            "输入体验",
            snapshot.defaultMode + "模式",
            "快捷键：" + snapshot.toggleKey,
            "配置输入体验",
            SettingsPageId::Input);
        gtk_grid_attach(GTK_GRID(grid), inputCard, 0, 0, 1, 1);

        auto *dictCard = createBentoCard(
            "accessories-dictionary-symbolic",
            "个人词典",
            std::to_string(snapshot.dictionaryEntries) + " 条词条",
            "本地专业词汇与短语管理",
            "管理个人词典",
            SettingsPageId::Dictionary);
        gtk_grid_attach(GTK_GRID(grid), dictCard, 1, 0, 1, 1);

        auto *learningCard = createBentoCard(
            "starred-symbolic",
            "智能学习",
            std::to_string(snapshot.learningEntries) + " 条记录",
            "自适应调频与高频词置顶",
            "查看学习数据",
            SettingsPageId::Learning);
        gtk_grid_attach(GTK_GRID(grid), learningCard, 0, 1, 1, 1);

        auto *clipCard = createBentoCard(
            "edit-paste-symbolic",
            "剪贴板",
            std::to_string(snapshot.clipboardEntries) + " 条记录",
            "输入 v+2 快捷呼出面板",
            "查看剪贴板",
            SettingsPageId::Clipboard);
        gtk_grid_attach(GTK_GRID(grid), clipCard, 1, 1, 1, 1);

        gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

        if (!snapshot.notices.empty()) {
            auto *notices = createSectionCard(
                "待处理事项", "以下项目需要您关注或处理：");
            for (const auto &notice : snapshot.notices) {
                gtk_box_pack_start(
                    GTK_BOX(notices),
                    createDestinationButton(notice.message,
                                            notice.destination, navigate,
                                            "打开相关页面处理此问题"),
                    FALSE, FALSE, 0);
            }
            gtk_box_pack_start(GTK_BOX(content), notices, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(content);
    }

    std::function<void(SettingsPageId)> navigate;
    GtkWidget *page = nullptr;
    GtkWidget *content = nullptr;
};

OverviewPage::OverviewPage(std::function<void(SettingsPageId)> navigate)
    : impl_(std::make_unique<Impl>(std::move(navigate))) {}

OverviewPage::~OverviewPage() = default;

GtkWidget *OverviewPage::widget() const {
    return impl_->page;
}

void OverviewPage::setSnapshot(const OverviewSnapshot &snapshot) {
    impl_->setSnapshot(snapshot);
}

} // namespace modernime::settings
