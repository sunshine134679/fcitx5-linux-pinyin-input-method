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
    Impl(std::function<void(SettingsPageId)> navigateCallback,
         std::function<void(OverviewSetupAction)> setupCallback,
         std::function<void()> refreshCallback)
        : navigate(std::move(navigateCallback)), setup(std::move(setupCallback)),
          refresh(std::move(refreshCallback)) {
        detail::GtkWidgetGuard pageGuard(
            createPageShell("概览", "查看 ModernIME 设置和本地数据概况"));
        page = pageGuard.get();
        summary = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(page), summary, FALSE, FALSE, 0);
        buildSetupGuide();
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
        clearContainer(summary);
        clearContainer(content);
        setupAction = snapshot.setupAction;
        gtk_label_set_text(GTK_LABEL(setupDescription), snapshot.setupDescription.c_str());
        gtk_button_set_label(GTK_BUTTON(setupButton), snapshot.setupActionLabel.c_str());
        gtk_widget_set_visible(setupButton, setupAction != OverviewSetupAction::None);
        trialInstructions = snapshot.trialInstructions;
        updateTrialFeedback();

        // Runtime summary, followed by direct navigation to local settings.
        auto *heroCard = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        addStyleClass(heroCard, "modernime-hero-card");

        auto *heroLeft = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_hexpand(heroLeft, TRUE);
        gtk_widget_set_valign(heroLeft, GTK_ALIGN_CENTER);

        auto *heroTitle = gtk_label_new("ModernIME 拼音输入法");
        addStyleClass(heroTitle, "modernime-hero-title");
        gtk_widget_set_halign(heroTitle, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(heroLeft), heroTitle, FALSE, FALSE, 0);

        auto *statusBadge = gtk_label_new(snapshot.runtimeSummary.c_str());
        addStyleClass(statusBadge, "modernime-setting-desc");
        gtk_widget_set_halign(statusBadge, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(heroLeft), statusBadge, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(heroCard), heroLeft, TRUE, TRUE, 0);

        auto *diagBtn = createDestinationButton("系统诊断与状态", SettingsPageId::Diagnostics,
                                                navigate, "打开系统与诊断页面查看详细日志");
        gtk_widget_set_valign(diagBtn, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(heroCard), diagBtn, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(summary), heroCard, FALSE, FALSE, 0);

        auto *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(list, TRUE);

        const auto createOverviewRow = [this](std::string_view iconName,
                                            std::string_view title,
                                            std::string_view value,
                                            std::string_view desc,
                                            std::string_view actionText,
                                            SettingsPageId dest) {
            auto *button = createDestinationButton(actionText, dest, navigate, desc);
            gtk_widget_destroy(gtk_bin_get_child(GTK_BIN(button)));
            addStyleClass(button, "modernime-overview-row");
            gtk_widget_set_halign(button, GTK_ALIGN_FILL);
            auto *hBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
            auto *icon = gtk_image_new_from_icon_name(std::string(iconName).c_str(), GTK_ICON_SIZE_BUTTON);
            gtk_box_pack_start(GTK_BOX(hBox), icon, FALSE, FALSE, 0);
            auto *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            gtk_widget_set_hexpand(labels, TRUE);
            auto *titleLabel = gtk_label_new(std::string(title).c_str());
            addStyleClass(titleLabel, "modernime-setting-title");
            gtk_widget_set_halign(titleLabel, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(labels), titleLabel, FALSE, FALSE, 0);

            auto *descLabel = gtk_label_new(std::string(desc).c_str());
            addStyleClass(descLabel, "modernime-setting-desc");
            gtk_label_set_xalign(GTK_LABEL(descLabel), 0.0F);
            gtk_label_set_line_wrap(GTK_LABEL(descLabel), TRUE);
            gtk_box_pack_start(GTK_BOX(labels), descLabel, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(hBox), labels, TRUE, TRUE, 0);
            auto *valLabel = gtk_label_new(std::string(value).c_str());
            addStyleClass(valLabel, "modernime-overview-value");
            gtk_box_pack_start(GTK_BOX(hBox), valLabel, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(hBox),
                gtk_image_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU),
                FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(button), hBox);
            return button;
        };

        auto *inputCard = createOverviewRow(
            "input-keyboard-symbolic",
            "输入与按键",
            snapshot.defaultMode + "模式",
            "快捷键：" + snapshot.toggleKey,
            "配置输入体验",
            SettingsPageId::Input);
        gtk_box_pack_start(GTK_BOX(list), inputCard, FALSE, FALSE, 0);

        auto *dictCard = createOverviewRow(
            "accessories-dictionary-symbolic",
            "个人词典",
            std::to_string(snapshot.dictionaryEntries) + " 条词条",
            "本地专业词汇与短语管理",
            "管理个人词典",
            SettingsPageId::Dictionary);
        gtk_box_pack_start(GTK_BOX(list), dictCard, FALSE, FALSE, 0);

        auto *learningCard = createOverviewRow(
            "starred-symbolic",
            "智能学习",
            std::to_string(snapshot.learningEntries) + " 条记录",
            "自适应调频与高频词置顶",
            "查看学习数据",
            SettingsPageId::Learning);
        gtk_box_pack_start(GTK_BOX(list), learningCard, FALSE, FALSE, 0);

        auto *clipCard = createOverviewRow(
            "edit-paste-symbolic",
            "剪贴板",
            std::to_string(snapshot.clipboardEntries) + " 条记录",
            snapshot.clipboardShortcut,
            "查看剪贴板",
            SettingsPageId::Clipboard);
        gtk_box_pack_start(GTK_BOX(list), clipCard, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(content), list, FALSE, FALSE, 0);

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
        gtk_widget_show_all(summary);
        gtk_widget_show_all(content);
    }

    void buildSetupGuide() {
        auto *guide = createSectionCard("开始使用", "启用输入法并试输入");
        gtk_box_set_spacing(GTK_BOX(guide), 10);
        gtk_box_pack_start(GTK_BOX(page), guide, FALSE, FALSE, 0);
        auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        setupDescription = gtk_label_new("正在检查输入法状态…");
        gtk_label_set_xalign(GTK_LABEL(setupDescription), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(setupDescription), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(setupDescription), 48);
        addStyleClass(setupDescription, "modernime-setting-desc");
        gtk_box_pack_start(GTK_BOX(actions), setupDescription, TRUE, TRUE, 0);
        setupButton = gtk_button_new_with_label("启用 ModernIME");
        gtk_widget_set_no_show_all(setupButton, TRUE);
        gtk_widget_set_valign(setupButton, GTK_ALIGN_CENTER);
        g_signal_connect(setupButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            gtk_widget_grab_focus(self.trialEntry);
            if (self.setup) self.setup(self.setupAction);
        }), this);
        gtk_box_pack_start(GTK_BOX(actions), setupButton, FALSE, FALSE, 0);
        refreshButton = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_valign(refreshButton, GTK_ALIGN_CENTER);
        setAccessibleWidgetText(refreshButton, "重新检查输入状态", "更新概览中的运行状态");
        gtk_widget_set_tooltip_text(refreshButton, "重新检查输入状态");
        g_signal_connect(refreshButton, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &self = *static_cast<Impl *>(data);
            if (self.refresh) self.refresh();
        }), this);
        gtk_box_pack_start(GTK_BOX(actions), refreshButton, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(guide), actions, FALSE, FALSE, 0);
        trialEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(trialEntry), "在这里试着输入中文");
        setAccessibleWidgetText(trialEntry, "试输入", "使用当前系统输入法试输入，不修改设置");
        gtk_box_pack_start(GTK_BOX(guide), trialEntry, FALSE, FALSE, 0);
        trialFeedback = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(trialFeedback), 0.0F);
        gtk_label_set_line_wrap(GTK_LABEL(trialFeedback), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(trialFeedback), 60);
        addStyleClass(trialFeedback, "modernime-setting-desc");
        gtk_box_pack_start(GTK_BOX(guide), trialFeedback, FALSE, FALSE, 0);
        g_signal_connect(trialEntry, "changed", G_CALLBACK(+[](GtkEditable *, gpointer data) {
            static_cast<Impl *>(data)->updateTrialFeedback();
        }), this);
    }

    void updateTrialFeedback() {
        const std::string_view text = gtk_entry_get_text(GTK_ENTRY(trialEntry));
        gtk_label_set_text(GTK_LABEL(trialFeedback), text == "你好"
            ? "已收到“你好”。可以继续试长句，或到其他应用输入。" : trialInstructions.c_str());
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
    std::string trialInstructions;
    GtkWidget *summary = nullptr;
    GtkWidget *setupDescription = nullptr;
    GtkWidget *setupButton = nullptr;
    GtkWidget *refreshButton = nullptr;
    GtkWidget *trialEntry = nullptr;
    GtkWidget *trialFeedback = nullptr;
    GtkWidget *page = nullptr;
    GtkWidget *content = nullptr;
};

OverviewPage::OverviewPage(std::function<void(SettingsPageId)> navigate,
                           std::function<void(OverviewSetupAction)> setup,
                           std::function<void()> refresh)
    : impl_(std::make_unique<Impl>(std::move(navigate), std::move(setup), std::move(refresh))) {}

OverviewPage::~OverviewPage() = default;

GtkWidget *OverviewPage::widget() const {
    return impl_->page;
}

void OverviewPage::setSnapshot(const OverviewSnapshot &snapshot) {
    impl_->setSnapshot(snapshot);
}

void OverviewPage::setSetupBusy(bool busy) { impl_->setSetupBusy(busy); }

} // namespace modernime::settings
