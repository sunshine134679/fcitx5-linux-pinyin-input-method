#include "modernime/settings/pages/overview_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

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

void addValue(GtkWidget *card, std::string_view value) {
    auto *label = createStatusPill(value);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(card), label, FALSE, FALSE, 0);
}

GtkWidget *createOverviewCard(
    std::string_view title, std::string_view description,
    std::string_view value, std::string_view actionLabel,
    SettingsPageId destination,
    const std::function<void(SettingsPageId)> &navigate) {
    auto *card = createSectionCard(title, description);
    addValue(card, value);
    gtk_box_pack_start(
        GTK_BOX(card),
        createDestinationButton(actionLabel, destination, navigate,
                                "打开对应页面查看并修改设置"), FALSE,
        FALSE, 0);
    return card;
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
        content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
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

        gtk_box_pack_start(
            GTK_BOX(content),
            createOverviewCard("运行状态",
                               "查看 Fcitx5 和 ModernIME 当前状态。",
                               snapshot.runtimeSummary, "打开系统与诊断",
                               SettingsPageId::Diagnostics, navigate),
            FALSE, FALSE, 0);

        const auto inputSummary =
            "默认" + snapshot.defaultMode + " · " + snapshot.toggleKey;
        gtk_box_pack_start(
            GTK_BOX(content),
            createOverviewCard("输入体验", "默认输入状态和切换快捷键。",
                               inputSummary, "打开输入体验",
                               SettingsPageId::Input, navigate),
            FALSE, FALSE, 0);

        const auto dictionarySummary =
            std::to_string(snapshot.dictionaryEntries) + " 条词条";
        gtk_box_pack_start(
            GTK_BOX(content),
            createOverviewCard("个人词典", "本地保存的个人词条。",
                               dictionarySummary, "打开个人词典",
                               SettingsPageId::Dictionary, navigate),
            FALSE, FALSE, 0);

        const auto clipboardSummary =
            std::to_string(snapshot.clipboardEntries) + " 条记录";
        gtk_box_pack_start(
            GTK_BOX(content),
            createOverviewCard("剪贴板", "本地保存的剪贴板历史。",
                               clipboardSummary, "打开剪贴板",
                               SettingsPageId::Clipboard, navigate),
            FALSE, FALSE, 0);

        const auto learningSummary =
            std::to_string(snapshot.learningEntries) + " 条记录";
        gtk_box_pack_start(
            GTK_BOX(content),
            createOverviewCard("智能学习", "本地保存的输入习惯记录。",
                               learningSummary, "打开智能学习",
                               SettingsPageId::Learning, navigate),
            FALSE, FALSE, 0);

        if (!snapshot.notices.empty()) {
            auto *notices = createSectionCard(
                "需要处理", "打开对应页面查看详细信息并处理问题。");
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
