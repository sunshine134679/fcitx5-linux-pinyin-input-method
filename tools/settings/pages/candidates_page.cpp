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

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
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

private:
    void buildUi() {
        detail::GtkWidgetGuard pageGuard(
            createPageShell("候选与外观", "调整候选框排版、字号大小与选词翻页快捷键。"));
        page = pageGuard.get();

        buildAppearanceSection();
        buildBehaviorSection();

        setSettingsFocusChain(
            page, {candidatePageSize, candidateFontSize, numberSelection,
                   arrowNavigation, pageNavigation, englishDefinition});
        refresh();
        pageGuard.release();
    }

    void buildAppearanceSection() {
        auto *section = createSectionCard(
            "候选外观与排版", "调整候选框的显示字号与每页候选词容量。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        candidatePageSize = gtk_spin_button_new_with_range(
            core::kMinimumCandidatePageSize, core::kMaximumCandidatePageSize, 1.0);
        gtk_widget_set_tooltip_text(candidatePageSize, "设置每页最多展示的候选词数量（3 ~ 9）");
        setTarget(candidatePageSize, "candidate-page-size");
        candidatePageSizeFallback = createSettingRow(
            "每页候选词数", "每页展示的候选词数量（支持 3 ~ 9 个，默认 9 个）。",
            candidatePageSize);
        gtk_box_pack_start(GTK_BOX(section), candidatePageSizeFallback, FALSE,
                           FALSE, 0);

        candidateFontSize = gtk_spin_button_new_with_range(
            core::kMinimumCandidateFontSize, core::kMaximumCandidateFontSize, 1.0);
        gtk_widget_set_tooltip_text(candidateFontSize, "设置候选词显示的字体大小（14 ~ 28 pt）");
        setTarget(candidateFontSize, "candidate-font-size");
        candidateFontSizeFallback = createSettingRow(
            "候选字号大小", "候选窗显示的文字大小（支持 14 ~ 28 pt，默认 20 pt）。",
            candidateFontSize);
        gtk_box_pack_start(GTK_BOX(section), candidateFontSizeFallback, FALSE,
                           FALSE, 0);

        previewBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addStyleClass(previewBox, "modernime-preview-bar");
        auto *previewTitle = gtk_label_new("候选栏排版静态预览");
        addStyleClass(previewTitle, "modernime-setting-desc");
        gtk_widget_set_halign(previewTitle, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewBox), previewTitle, FALSE, FALSE, 0);

        previewLabel = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(previewLabel), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(previewLabel), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_halign(previewLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewBox), previewLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), previewBox, FALSE, FALSE, 0);

        g_signal_connect(candidatePageSize, "value-changed", G_CALLBACK(onSpinChanged), this);
        g_signal_connect(candidateFontSize, "value-changed", G_CALLBACK(onSpinChanged), this);
    }

    void buildBehaviorSection() {
        auto *section = createSectionCard(
            "候选行为与选词", "配置按键选择、翻页导航以及英文简明释义。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        numberSelection = gtk_switch_new();
        gtk_widget_set_tooltip_text(numberSelection, "使用数字键选择当前候选项");
        setTarget(numberSelection, "number-selection");
        numberSelectionFallback = createSettingRow(
            "数字键选择候选", "使用键盘顶部 1~9 数字键快速选择对应候选项上屏。",
            numberSelection);
        gtk_box_pack_start(GTK_BOX(section), numberSelectionFallback, FALSE,
                           FALSE, 0);

        arrowNavigation = gtk_switch_new();
        gtk_widget_set_tooltip_text(
            arrowNavigation, "左右移动拼音光标，上下切换候选项");
        setTarget(arrowNavigation, "arrow-navigation");
        arrowNavigationFallback = createSettingRow(
            "方向键编辑与选词",
            "左右键移动拼音光标，上下键快速整页翻页与切换候选项。",
            arrowNavigation);
        gtk_box_pack_start(GTK_BOX(section), arrowNavigationFallback, FALSE,
                           FALSE, 0);

        pageNavigation = gtk_switch_new();
        gtk_widget_set_tooltip_text(pageNavigation,
                                    "使用 PageUp / PageDown 或 + / = 翻页");
        setTarget(pageNavigation, "page-navigation");
        pageNavigationFallback = createSettingRow(
            "候选翻页", "使用 PageUp / PageDown 和 + / = 键进行前后翻页。",
            pageNavigation);
        gtk_box_pack_start(GTK_BOX(section), pageNavigationFallback, FALSE,
                           FALSE, 0);

        englishDefinition = gtk_switch_new();
        gtk_widget_set_tooltip_text(
            englishDefinition,
            "输入英文时提示词性与简明中文释义（当前仅在开启英文输入时提供释义展示，不自动改变当前中英文模式）");
        setTarget(englishDefinition, "english-definition");
        englishDefinitionFallback = createSettingRow(
            "显示英文单词中文释义",
            "输入英文时提示词性与简明中文释义（当前仅在开启英文输入时提供释义展示，不自动改变当前中英文模式）。",
            englishDefinition);
        gtk_box_pack_start(GTK_BOX(section), englishDefinitionFallback, FALSE,
                           FALSE, 0);

        for (auto *control : {numberSelection, arrowNavigation, pageNavigation, englishDefinition}) {
            g_signal_connect(control, "notify::active", G_CALLBACK(onSwitchChanged), this);
        }
    }

    void updatePreview(int pageSize, int fontSize) {
        if (previewLabel == nullptr) {
            return;
        }
        static constexpr const char *kSamples[] = {
            "你好", "现代", "拼音", "输入法", "世界",
            "设置", "测试", "候选", "外观"
        };
        std::ostringstream ss;
        ss << "<span font='" << fontSize << "pt'>";
        const int count = std::clamp(pageSize, 3, 9);
        for (int i = 1; i <= count; ++i) {
            if (i > 1) {
                ss << "   ";
            }
            ss << "<span alpha='65%'>" << i << ".</span> ";
            ss << kSamples[(i - 1) % 9];
        }
        ss << "</span>";
        gtk_label_set_markup(GTK_LABEL(previewLabel), ss.str().c_str());
    }

    static void onSpinChanged(GtkSpinButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        const int pageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidatePageSize));
        const int fontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        impl->updatePreview(pageSize, fontSize);
        impl->applyToModel();
    }

    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        impl->applyToModel();
    }

    void applyToModel() {
        const int pageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(candidatePageSize));
        const int fontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(candidateFontSize));
        const bool numSelect = gtk_switch_get_active(GTK_SWITCH(numberSelection));
        const bool arrowNav = gtk_switch_get_active(GTK_SWITCH(arrowNavigation));
        const bool pageNav = gtk_switch_get_active(GTK_SWITCH(pageNavigation));
        const bool engDef = gtk_switch_get_active(GTK_SWITCH(englishDefinition));

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
    GtkWidget *previewBox = nullptr;
    GtkWidget *previewLabel = nullptr;
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
