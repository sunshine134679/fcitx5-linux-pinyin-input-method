#include "modernime/settings/pages/candidates_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
                                std::string(className).c_str());
}

void removeStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_remove_class(gtk_widget_get_style_context(widget),
                                   std::string(className).c_str());
}

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

void clearBoxChildren(GtkWidget *box) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (auto *item = children; item != nullptr; item = item->next) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
    }
    g_list_free(children);
}

} // namespace

class CandidatesPage::Impl final {
public:
    Impl(SettingsWindowModel &modelRef, std::function<void()> changedCallback)
        : model(modelRef), changed(std::move(changedCallback)) {
        buildUi();
    }

    GtkWidget *widget() const { return page; }

    void refresh() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidatePageSize),
                                  settings.candidatePageSize);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidateFontSize),
                                  settings.candidateFontSize);
        updateSegmentedButtons(settings.candidatePageSize);
        updateFontSizeLabel(settings.candidateFontSize);
        gtk_switch_set_active(GTK_SWITCH(numberSelection),
                              settings.numberSelection);
        gtk_switch_set_active(GTK_SWITCH(arrowNavigation),
                              settings.arrowNavigation);
        gtk_switch_set_active(GTK_SWITCH(pageNavigation),
                              settings.pageNavigation);
        gtk_switch_set_active(GTK_SWITCH(englishDefinition),
                              settings.englishDefinitionEnabled);
        updatePreview(settings.candidatePageSize, settings.candidateFontSize);
        refreshing = false;
    }

    bool focusTarget(std::string_view target) {
        if (target == "candidate-page-size") {
            return focusWidgetOrFallback(candidatePageSize, candidatePageSizeFallback);
        }
        if (target == "candidate-font-size") {
            return focusWidgetOrFallback(candidateFontSize, candidateFontSizeFallback);
        }
        if (target == "number-selection") {
            return focusWidgetOrFallback(numberSelection, numberSelectionFallback);
        }
        if (target == "arrow-navigation") {
            return focusWidgetOrFallback(arrowNavigation, arrowNavigationFallback);
        }
        if (target == "page-navigation") {
            return focusWidgetOrFallback(pageNavigation, pageNavigationFallback);
        }
        if (target == "english-definition") {
            return focusWidgetOrFallback(englishDefinition, englishDefinitionFallback);
        }
        return false;
    }

    void onSegmentClicked(int value) {
        if (refreshing) {
            return;
        }
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidatePageSize), value);
    }

private:
    void buildUi() {
        detail::GtkWidgetGuard pageGuard(
            createPageShell("候选与外观", "调整候选栏的显示方式，不改变候选排序规则"));
        page = pageGuard.get();

        buildPreviewCard();
        buildSettingsCard();

        setSettingsFocusChain(
            page, {candidatePageSize, candidateFontSize, numberSelection,
                   arrowNavigation, pageNavigation, englishDefinition});
        refresh();
        pageGuard.release();
    }

    void buildPreviewCard() {
        previewCard = createSectionCard("", "");
        addStyleClass(previewCard, "modernime-preview-card");
        gtk_box_pack_start(GTK_BOX(page), previewCard, FALSE, FALSE, 0);

        auto *headerLabel = gtk_label_new("外观预览 · 不记录学习");
        addStyleClass(headerLabel, "modernime-preview-header");
        gtk_widget_set_halign(headerLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewCard), headerLabel, FALSE, FALSE, 0);

        auto *compLabel = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(compLabel),
                             "nihao<span foreground='#2563eb' font_weight='bold'>|</span>");
        addStyleClass(compLabel, "modernime-composition-text");
        gtk_widget_set_halign(compLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewCard), compLabel, FALSE, FALSE, 0);

        auto *candidateScroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(candidateScroller),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(candidateScroller),
                                            GTK_SHADOW_NONE);
        gtk_scrolled_window_set_propagate_natural_width(
            GTK_SCROLLED_WINDOW(candidateScroller), TRUE);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(candidateScroller), TRUE);
        gtk_scrolled_window_set_max_content_width(
            GTK_SCROLLED_WINDOW(candidateScroller), 460);
        gtk_widget_set_halign(candidateScroller, GTK_ALIGN_START);

        candidateBarBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        addStyleClass(candidateBarBox, "modernime-candidate-bar-preview");
        gtk_widget_set_halign(candidateBarBox, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(candidateScroller), candidateBarBox);

        gtk_box_pack_start(GTK_BOX(previewCard), candidateScroller, FALSE, FALSE, 0);
    }

    void buildSettingsCard() {
        auto *card = createSectionCard("", "");
        gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

        // Row 1: 每页候选数量
        auto *pageSizeControlBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        for (int val : {5, 7, 9}) {
            auto *btn = gtk_button_new_with_label(std::to_string(val).c_str());
            addStyleClass(btn, "modernime-segmented-btn");
            gtk_widget_set_tooltip_text(
                btn, ("快捷设为每页 " + std::to_string(val) + " 个候选词").c_str());
            g_signal_connect_data(
                btn, "clicked", G_CALLBACK(onSegmentedPageSizeClicked),
                new PageSizeSegmentAction{this, val}, destroySegmentAction,
                static_cast<GConnectFlags>(0));
            gtk_box_pack_start(GTK_BOX(pageSizeControlBox), btn, FALSE, FALSE, 0);
            segmentedButtons.push_back({val, btn});
        }
        candidatePageSize = gtk_spin_button_new_with_range(
            core::kMinimumCandidatePageSize, core::kMaximumCandidatePageSize, 1.0);
        gtk_widget_set_tooltip_text(candidatePageSize, "支持 3 ~ 9 个候选词");
        setTarget(candidatePageSize, "candidate-page-size");
        gtk_box_pack_start(GTK_BOX(pageSizeControlBox), candidatePageSize, FALSE,
                           FALSE, 0);
        candidatePageSizeFallback = createSettingRow(
            "每页候选数量", "数字选词最多对应 1–9", pageSizeControlBox);
        gtk_box_pack_start(GTK_BOX(card), candidatePageSizeFallback, FALSE,
                           FALSE, 0);

        // Row 2: 候选文字大小
        auto *stepperBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        addStyleClass(stepperBox, "modernime-stepper-box");
        fontSizeMinusBtn = gtk_button_new_with_label("−");
        addStyleClass(fontSizeMinusBtn, "modernime-stepper-btn");
        gtk_widget_set_tooltip_text(fontSizeMinusBtn, "减小字号（最小 14 pt）");

        fontSizeLabel = gtk_label_new("20 pt");
        addStyleClass(fontSizeLabel, "modernime-stepper-label");
        gtk_widget_set_size_request(fontSizeLabel, 56, -1);
        gtk_widget_set_halign(fontSizeLabel, GTK_ALIGN_CENTER);

        fontSizePlusBtn = gtk_button_new_with_label("+");
        addStyleClass(fontSizePlusBtn, "modernime-stepper-btn");
        gtk_widget_set_tooltip_text(fontSizePlusBtn, "增大字号（最大 28 pt）");

        gtk_box_pack_start(GTK_BOX(stepperBox), fontSizeMinusBtn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(stepperBox), fontSizeLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(stepperBox), fontSizePlusBtn, FALSE, FALSE, 0);

        candidateFontSize = gtk_spin_button_new_with_range(
            core::kMinimumCandidateFontSize, core::kMaximumCandidateFontSize, 1.0);
        setTarget(candidateFontSize, "candidate-font-size");
        setTarget(stepperBox, "candidate-font-size");

        candidateFontSizeFallback = createSettingRow(
            "候选文字大小", "可设置 14–28 pt", stepperBox);
        gtk_box_pack_start(GTK_BOX(card), candidateFontSizeFallback, FALSE,
                           FALSE, 0);

        // Row 3: 数字键选词
        numberSelection = gtk_switch_new();
        gtk_widget_set_tooltip_text(numberSelection, "空格确认首选，数字键选择对应候选");
        setTarget(numberSelection, "number-selection");
        numberSelectionFallback = createSettingRow(
            "数字键选词", "空格确认首选，数字键选择对应候选", numberSelection);
        gtk_box_pack_start(GTK_BOX(card), numberSelectionFallback, FALSE,
                           FALSE, 0);

        // Row 4: 显示英文释义
        englishDefinition = gtk_switch_new();
        gtk_widget_set_tooltip_text(
            englishDefinition, "解释仅作为附注显示，不占编号、不改变排序");
        setTarget(englishDefinition, "english-definition");
        englishDefinitionFallback = createSettingRow(
            "显示英文释义", "解释仅作为附注显示，不占编号、不改变排序",
            englishDefinition);
        gtk_box_pack_start(GTK_BOX(card), englishDefinitionFallback, FALSE,
                           FALSE, 0);

        // Card bottom info tip: ⓘ 中文状态下，常用中文与用户明确选择优先
        auto *tipLabel = gtk_label_new("ⓘ  中文状态下，常用中文与用户明确选择优先");
        addStyleClass(tipLabel, "modernime-tip-note");
        gtk_widget_set_halign(tipLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(card), tipLabel, FALSE, FALSE, 0);

        // Keep internal navigation switches in fallback chain
        arrowNavigation = gtk_switch_new();
        pageNavigation = gtk_switch_new();
        setTarget(arrowNavigation, "arrow-navigation");
        setTarget(pageNavigation, "page-navigation");
        arrowNavigationFallback = card;
        pageNavigationFallback = card;

        g_signal_connect(candidatePageSize, "value-changed",
                         G_CALLBACK(onSpinChanged), this);
        g_signal_connect(fontSizeMinusBtn, "clicked",
                         G_CALLBACK(onFontSizeMinus), this);
        g_signal_connect(fontSizePlusBtn, "clicked",
                         G_CALLBACK(onFontSizePlus), this);
        g_signal_connect(candidateFontSize, "value-changed",
                         G_CALLBACK(onFontSizeSpinChanged), this);

        for (auto *control : {numberSelection, arrowNavigation, pageNavigation,
                              englishDefinition}) {
            g_signal_connect(control, "notify::active",
                             G_CALLBACK(onSwitchChanged), this);
        }
    }

    void updatePreview(int pageSize, int fontSize) {
        if (candidateBarBox == nullptr) {
            return;
        }
        clearBoxChildren(candidateBarBox);

        static constexpr const char *kSamples[] = {
            "你好", "你号", "拟好", "你好啊", "你好呀",
            "设置", "测试", "候选", "外观"
        };
        const int count = std::min(std::max(pageSize, 3), 5);

        for (int i = 1; i <= count; ++i) {
            auto *itemLabel = gtk_label_new(nullptr);
            std::ostringstream ss;
            ss << "<span font='" << fontSize << "pt'>";
            if (i == 1) {
                ss << "1." << kSamples[0] << "</span>";
                gtk_label_set_markup(GTK_LABEL(itemLabel), ss.str().c_str());
                addStyleClass(itemLabel, "modernime-candidate-badge");
            } else {
                ss << "<span alpha='70%'>" << i << ".</span>" << kSamples[(i - 1) % 9]
                   << "</span>";
                gtk_label_set_markup(GTK_LABEL(itemLabel), ss.str().c_str());
                addStyleClass(itemLabel, "modernime-candidate-item");
            }
            gtk_box_pack_start(GTK_BOX(candidateBarBox), itemLabel, FALSE, FALSE,
                               0);
        }
        gtk_widget_show_all(candidateBarBox);
    }

    void updateSegmentedButtons(int pageSize) {
        for (const auto &[val, btn] : segmentedButtons) {
            if (val == pageSize) {
                addStyleClass(btn, "active");
            } else {
                removeStyleClass(btn, "active");
            }
        }
    }

    void updateFontSizeLabel(int fontSize) {
        if (fontSizeLabel != nullptr) {
            gtk_label_set_text(GTK_LABEL(fontSizeLabel),
                               (std::to_string(fontSize) + " pt").c_str());
        }
        if (fontSizeMinusBtn != nullptr) {
            gtk_widget_set_sensitive(fontSizeMinusBtn,
                                     fontSize > core::kMinimumCandidateFontSize);
        }
        if (fontSizePlusBtn != nullptr) {
            gtk_widget_set_sensitive(fontSizePlusBtn,
                                     fontSize < core::kMaximumCandidateFontSize);
        }
    }

    struct PageSizeSegmentAction final {
        Impl *impl = nullptr;
        int value = 0;
    };

    static void destroySegmentAction(gpointer data, GClosure *) {
        delete static_cast<PageSizeSegmentAction *>(data);
    }

    static void onSegmentedPageSizeClicked(GtkButton *, gpointer data) {
        auto *action = static_cast<PageSizeSegmentAction *>(data);
        if (action != nullptr && action->impl != nullptr) {
            action->impl->onSegmentClicked(action->value);
        }
    }

    static void onFontSizeMinus(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        const int cur = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        if (cur > core::kMinimumCandidateFontSize) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(impl->candidateFontSize),
                                      cur - 1);
        }
    }

    static void onFontSizePlus(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        const int cur = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        if (cur < core::kMaximumCandidateFontSize) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(impl->candidateFontSize),
                                      cur + 1);
        }
    }

    static void onFontSizeSpinChanged(GtkSpinButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        const int fontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        impl->updateFontSizeLabel(fontSize);
        const int pageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidatePageSize));
        impl->updatePreview(pageSize, fontSize);
        impl->applyToModel();
    }

    static void onSpinChanged(GtkSpinButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        const int pageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidatePageSize));
        impl->updateSegmentedButtons(pageSize);
        const int fontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        impl->updatePreview(pageSize, fontSize);
        impl->applyToModel();
    }

    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        impl->applyToModel();
    }

    void applyToModel() {
        const int pageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(candidatePageSize));
        const int fontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(candidateFontSize));
        const bool numSelect =
            gtk_switch_get_active(GTK_SWITCH(numberSelection));
        const bool arrowNav =
            gtk_switch_get_active(GTK_SWITCH(arrowNavigation));
        const bool pageNav =
            gtk_switch_get_active(GTK_SWITCH(pageNavigation));
        const bool engDef =
            gtk_switch_get_active(GTK_SWITCH(englishDefinition));

        model.setCandidateSettings(pageSize, fontSize, numSelect, arrowNav,
                                   pageNav, engDef);
        if (changed) {
            changed();
        }
    }

    SettingsWindowModel &model;
    std::function<void()> changed;
    bool refreshing = false;

    GtkWidget *page = nullptr;
    GtkWidget *previewCard = nullptr;
    GtkWidget *candidateBarBox = nullptr;

    std::vector<std::pair<int, GtkWidget *>> segmentedButtons;
    GtkWidget *candidatePageSize = nullptr;
    GtkWidget *candidatePageSizeFallback = nullptr;

    GtkWidget *fontSizeMinusBtn = nullptr;
    GtkWidget *fontSizeLabel = nullptr;
    GtkWidget *fontSizePlusBtn = nullptr;
    GtkWidget *candidateFontSize = nullptr;
    GtkWidget *candidateFontSizeFallback = nullptr;

    GtkWidget *numberSelection = nullptr;
    GtkWidget *numberSelectionFallback = nullptr;
    GtkWidget *arrowNavigation = nullptr;
    GtkWidget *arrowNavigationFallback = nullptr;
    GtkWidget *pageNavigation = nullptr;
    GtkWidget *pageNavigationFallback = nullptr;
    GtkWidget *englishDefinition = nullptr;
    GtkWidget *englishDefinitionFallback = nullptr;
};

CandidatesPage::CandidatesPage(SettingsWindowModel &model,
                               std::function<void()> changed)
    : impl_(std::make_unique<Impl>(model, std::move(changed))) {}

CandidatesPage::~CandidatesPage() = default;

GtkWidget *CandidatesPage::widget() const {
    return impl_->widget();
}

void CandidatesPage::refresh() {
    impl_->refresh();
}

bool CandidatesPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
