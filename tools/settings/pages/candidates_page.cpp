#include "modernime/settings/pages/candidates_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace modernime::settings {
namespace {

void addStyleClass(GtkWidget *widget, std::string_view className) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget),
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
        gtk_combo_box_set_active(GTK_COMBO_BOX(candidatePageSize),
                                 settings.candidatePageSize - core::kMinimumCandidatePageSize);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidateFontSize),
                                  settings.candidateFontSize);
        gtk_switch_set_active(GTK_SWITCH(numberSelection),
                              settings.numberSelection);
        gtk_switch_set_active(GTK_SWITCH(arrowNavigation),
                              settings.arrowNavigation);
        gtk_switch_set_active(GTK_SWITCH(pageNavigation),
                              settings.pageNavigation);
        gtk_switch_set_active(GTK_SWITCH(englishDefinition),
                              settings.englishDefinitionEnabled);
        updatePreview();
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

private:
    void buildUi() {
        detail::GtkWidgetGuard pageGuard(
            createPageShell("候选与外观", "调整候选栏的显示方式，不改变候选排序规则"));
        page = pageGuard.get();

        buildPreviewCard();
        buildSettingsCard();

        setSettingsFocusChain(
            page, {candidatePageSize, candidateFontSize, numberSelection,
                   englishDefinition, arrowNavigation, pageNavigation});
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
        addStyleClass(candidateScroller, "modernime-candidate-preview-scroller");
        gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(candidateScroller), FALSE);
        gtk_widget_set_tooltip_text(candidateScroller,
            "横向滚动可查看本页全部候选；此预览不记录学习");
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(candidateScroller),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(candidateScroller),
                                            GTK_SHADOW_NONE);
        gtk_scrolled_window_set_propagate_natural_width(
            GTK_SCROLLED_WINDOW(candidateScroller), FALSE);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(candidateScroller), TRUE);

        gtk_widget_set_hexpand(candidateScroller, TRUE);
        gtk_widget_set_halign(candidateScroller, GTK_ALIGN_FILL);

        candidateBarBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        setTarget(candidateBarBox, "candidate-preview");
        addStyleClass(candidateBarBox, "modernime-candidate-bar-preview");
        gtk_widget_set_halign(candidateBarBox, GTK_ALIGN_START);
        gtk_container_add(GTK_CONTAINER(candidateScroller), candidateBarBox);

        gtk_box_pack_start(GTK_BOX(previewCard), candidateScroller, FALSE, FALSE, 0);
        definitionPreview = gtk_label_new("hello · int. 你好；喂");
        addStyleClass(definitionPreview, "modernime-description");
        gtk_widget_set_halign(definitionPreview, GTK_ALIGN_START);
        gtk_widget_set_margin_top(definitionPreview, 8);
        gtk_widget_set_no_show_all(definitionPreview, TRUE);
        gtk_box_pack_start(GTK_BOX(previewCard), definitionPreview, FALSE, FALSE, 0);
    }

    void buildSettingsCard() {
        auto *card = createSectionCard("", "");
        gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);

        // Compact native controls keep all supported values keyboard-accessible.
        candidatePageSize = gtk_combo_box_text_new();
        for (int value = core::kMinimumCandidatePageSize;
             value <= core::kMaximumCandidatePageSize; ++value) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(candidatePageSize),
                                          (std::to_string(value) + " 个").c_str());
        }
        setTarget(candidatePageSize, "candidate-page-size");
        candidatePageSizeFallback = createSettingRow(
            "每页候选数量", "可选 3–9 个，数字选词最多对应 1–9", candidatePageSize);
        gtk_box_pack_start(GTK_BOX(card), candidatePageSizeFallback, FALSE, FALSE, 0);

        auto *fontControl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        candidateFontSize = gtk_spin_button_new_with_range(
            core::kMinimumCandidateFontSize, core::kMaximumCandidateFontSize, 1.0);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(candidateFontSize), TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(candidateFontSize), 2);
        setTarget(candidateFontSize, "candidate-font-size");
        gtk_box_pack_start(GTK_BOX(fontControl), candidateFontSize, FALSE, FALSE, 0);
        auto *unit = gtk_label_new("pt");
        addStyleClass(unit, "modernime-description");
        gtk_box_pack_start(GTK_BOX(fontControl), unit, FALSE, FALSE, 0);
        candidateFontSizeFallback = createSettingRow(
            "候选文字大小", "可设置 14–28 pt，预览随调整即时更新", fontControl);
        gtk_box_pack_start(GTK_BOX(card), candidateFontSizeFallback, FALSE, FALSE, 0);

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

        auto *tip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        addStyleClass(tip, "modernime-tip-note");
        auto *tipIcon = gtk_image_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_MENU);
        gtk_box_pack_start(GTK_BOX(tip), tipIcon, FALSE, FALSE, 0);
        auto *tipLabel = gtk_label_new("中文状态下，常用中文与用户明确选择优先");
        gtk_label_set_line_wrap(GTK_LABEL(tipLabel), TRUE);
        gtk_label_set_xalign(GTK_LABEL(tipLabel), 0);
        gtk_box_pack_start(GTK_BOX(tip), tipLabel, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(card), tip, FALSE, FALSE, 0);

        auto *navigation = createSectionCard("候选导航", "控制方向键与翻页键的行为");
        gtk_box_pack_start(GTK_BOX(page), navigation, FALSE, FALSE, 0);
        arrowNavigation = gtk_switch_new();
        pageNavigation = gtk_switch_new();
        setTarget(arrowNavigation, "arrow-navigation");
        setTarget(pageNavigation, "page-navigation");
        arrowNavigationFallback = createSettingRow(
            "方向键编辑与选词", "左右移动拼音光标，上下切换候选项", arrowNavigation);
        pageNavigationFallback = createSettingRow(
            "候选翻页", "使用 PageUp / PageDown 和 + / = 翻页", pageNavigation);
        gtk_box_pack_start(GTK_BOX(navigation), arrowNavigationFallback, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(navigation), pageNavigationFallback, FALSE, FALSE, 0);

        g_signal_connect(candidatePageSize, "changed", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateFontSize, "value-changed", G_CALLBACK(onSpinChanged), this);

        for (auto *control : {numberSelection, arrowNavigation, pageNavigation,
                              englishDefinition}) {
            g_signal_connect(control, "notify::active",
                             G_CALLBACK(onSwitchChanged), this);
        }
    }

    void updatePreview() {
        if (candidateBarBox == nullptr) return;
        clearBoxChildren(candidateBarBox);
        static constexpr std::array kSamples{
            "你好", "你号", "拟好", "你好啊", "你好呀",
            "你好吗", "你还好", "你好吧", "你好哇"};
        const int count = gtk_combo_box_get_active(GTK_COMBO_BOX(candidatePageSize)) +
                          core::kMinimumCandidatePageSize;
        const int fontSize = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(candidateFontSize));
        const bool showNumbers = gtk_switch_get_active(GTK_SWITCH(numberSelection));
        for (int i = 0; i < count; ++i) {
            auto *item = gtk_label_new(nullptr);
            std::ostringstream markup;
            markup << "<span size='" << fontSize * PANGO_SCALE << "'>";
            if (showNumbers) markup << i + 1 << ".";
            markup << kSamples[static_cast<std::size_t>(i)] << "</span>";
            gtk_label_set_markup(GTK_LABEL(item), markup.str().c_str());
            addStyleClass(item, i == 0 ? "modernime-candidate-badge" : "modernime-candidate-item");
            gtk_box_pack_start(GTK_BOX(candidateBarBox), item, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(candidateBarBox);
        gtk_widget_set_visible(definitionPreview,
                               gtk_switch_get_active(GTK_SWITCH(englishDefinition)));
    }

    static void onSpinChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) return;
        impl->updatePreview();
        impl->applyToModel();
    }

    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl == nullptr || impl->refreshing) {
            return;
        }
        impl->updatePreview();
        impl->applyToModel();
    }

    void applyToModel() {
        const int pageSize = gtk_combo_box_get_active(GTK_COMBO_BOX(candidatePageSize)) +
            core::kMinimumCandidatePageSize;
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
    GtkWidget *definitionPreview = nullptr;

    GtkWidget *candidatePageSize = nullptr;
    GtkWidget *candidatePageSizeFallback = nullptr;

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
