#include "modernime/settings/pages/input_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <array>
#include <string>
#include <utility>

namespace modernime::settings {
namespace {

void setWidgetError(GtkWidget *widget, bool invalid, std::string_view message) {
    auto *context = gtk_widget_get_style_context(widget);
    if (invalid) {
        gtk_style_context_add_class(context, "error");
        gtk_widget_set_tooltip_text(widget, std::string(message).c_str());
    } else {
        gtk_style_context_remove_class(context, "error");
        gtk_widget_set_tooltip_text(widget, nullptr);
    }
}

void setTarget(GtkWidget *widget, std::string_view target) {
    g_object_set_data_full(G_OBJECT(widget), "modernime-settings-target",
                           g_strdup(std::string(target).c_str()), g_free);
}

} // namespace

class InputPage::Impl final {
public:
    Impl(SettingsWindowModel &settingsModel,
         std::function<void()> changedCallback)
        : model(settingsModel), changed(std::move(changedCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "输入体验", "配置输入状态、快捷键、标点和候选行为"));
        page = pageGuard.get();
        buildInputStatusSection();
        buildShortcutSection();
        buildPunctuationSection();
        buildCandidateBehaviorSection();
        buildCandidateAppearanceSection();
        setSettingsFocusChain(
            page, {inputEnabled, defaultMode, toggleKey, punctuation,
                   numberSelection, arrowNavigation, pageNavigation,
                   candidatePageSize, candidateFontSize});
        refresh();
        pageGuard.release();
    }

    void buildInputStatusSection() {
        auto *section = createSectionCard(
            "输入状态", "这些设置决定 ModernIME 何时接收键盘输入。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        inputEnabled = gtk_check_button_new_with_label("启用 ModernIME");
        setTarget(inputEnabled, "input-enabled");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("启用 ModernIME",
                                            "控制 ModernIME 是否接收键盘输入。",
                                            inputEnabled),
                           FALSE, FALSE, 0);

        defaultMode = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(defaultMode), "中文");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(defaultMode), "英文");
        gtk_widget_set_hexpand(defaultMode, TRUE);
        setTarget(defaultMode, "default-mode");
        defaultModeFallback = createSettingRow(
            "默认输入状态", "选择启动时默认使用中文或英文。", defaultMode);
        gtk_box_pack_start(GTK_BOX(section), defaultModeFallback, FALSE, FALSE,
                           0);
        g_signal_connect(inputEnabled, "toggled", G_CALLBACK(onChanged), this);
        g_signal_connect(defaultMode, "changed", G_CALLBACK(onChanged), this);
    }

    void buildShortcutSection() {
        auto *section = createSectionCard(
            "快捷键", "设置在中文和英文输入状态之间切换的按键。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        toggleKey = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(toggleKey),
                                       "例如 Ctrl+Shift+Space");
        gtk_widget_set_hexpand(toggleKey, TRUE);
        setTarget(toggleKey, "toggle-key");
        toggleKeyFallback = createSettingRow(
            "中英文切换快捷键",
            "支持 Ctrl+Space、Alt+Space、Super+Space 或 Ctrl+Shift+Space。",
            toggleKey);
        gtk_box_pack_start(GTK_BOX(section), toggleKeyFallback, FALSE, FALSE,
                           0);
        g_signal_connect(toggleKey, "changed", G_CALLBACK(onChanged), this);
    }

    void buildPunctuationSection() {
        auto *section = createSectionCard(
            "标点", "配置中文输入状态下的标点输出方式。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        punctuation = gtk_check_button_new_with_label("中文标点");
        setTarget(punctuation, "punctuation");
        punctuationFallback = createSettingRow(
            "中文标点", "在中文状态下使用全角标点。", punctuation);
        gtk_box_pack_start(GTK_BOX(section), punctuationFallback, FALSE, FALSE,
                           0);
        g_signal_connect(punctuation, "toggled", G_CALLBACK(onChanged), this);
    }

    void buildCandidateBehaviorSection() {
        auto *section = createSectionCard(
            "候选行为", "关闭某项后，对应按键会交给其他输入行为处理。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        numberSelection = gtk_check_button_new_with_label("数字键选择候选");
        gtk_widget_set_tooltip_text(numberSelection, "使用数字键选择当前候选项");
        setTarget(numberSelection, "number-selection");
        arrowNavigation = gtk_check_button_new_with_label("方向键编辑与选词");
        gtk_widget_set_tooltip_text(
            arrowNavigation, "左右移动拼音光标，上下切换候选项");
        setTarget(arrowNavigation, "arrow-navigation");
        pageNavigation = gtk_check_button_new_with_label("候选翻页");
        gtk_widget_set_tooltip_text(pageNavigation,
                                    "使用 PageUp / PageDown 或 + / = 翻页");
        setTarget(pageNavigation, "page-navigation");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("数字键选择候选",
                                            "使用数字键选择当前候选项。",
                                            numberSelection),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("方向键编辑与选词",
                                            "左右移动拼音光标，上下切换候选项。",
                                            arrowNavigation),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("候选翻页",
                                            "使用 PageUp / PageDown 和 + / = 翻页。",
                                            pageNavigation),
                           FALSE, FALSE, 0);
        for (auto *control : {numberSelection, arrowNavigation, pageNavigation}) {
            g_signal_connect(control, "toggled", G_CALLBACK(onChanged), this);
        }
    }

    void buildCandidateAppearanceSection() {
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

        g_signal_connect(candidatePageSize, "value-changed", G_CALLBACK(onChanged), this);
        g_signal_connect(candidateFontSize, "value-changed", G_CALLBACK(onChanged), this);
    }

    static void onChanged(GtkWidget *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        auto settings = impl->model.settings();
        settings.inputEnabled = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->inputEnabled));
        settings.defaultMode = gtk_combo_box_get_active(
            GTK_COMBO_BOX(impl->defaultMode)) == 1
                                   ? core::InputMode::English
                                   : core::InputMode::Chinese;
        settings.toggleKey = gtk_entry_get_text(GTK_ENTRY(impl->toggleKey));
        settings.punctuationEnabled = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->punctuation));
        settings.numberSelection = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->numberSelection));
        settings.arrowNavigation = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->arrowNavigation));
        settings.pageNavigation = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(impl->pageNavigation));
        settings.candidatePageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidatePageSize));
        settings.candidateFontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        impl->model.setSettings(std::move(settings));
        impl->updateState();
        impl->changed();
    }

    void refresh() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inputEnabled),
                                     settings.inputEnabled);
        gtk_combo_box_set_active(GTK_COMBO_BOX(defaultMode),
                                 settings.defaultMode == core::InputMode::English
                                     ? 1
                                     : 0);
        gtk_entry_set_text(GTK_ENTRY(toggleKey), settings.toggleKey.c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(punctuation),
                                     settings.punctuationEnabled);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(numberSelection),
                                     settings.numberSelection);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(arrowNavigation),
                                     settings.arrowNavigation);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pageNavigation),
                                     settings.pageNavigation);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidatePageSize),
                                  settings.candidatePageSize);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidateFontSize),
                                  settings.candidateFontSize);
        refreshing = false;
        updateState();
    }

    void updateState() {
        const auto state = deriveInputPageState(model);
        for (auto *control : {defaultMode, toggleKey, punctuation}) {
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive);
        }
        setWidgetError(toggleKey, !state.toggleKeyValid,
                       state.toggleKeyMessage);
    }

    bool focusTarget(std::string_view target) {
        const std::array controls{
            std::pair{inputEnabled, static_cast<GtkWidget *>(nullptr)},
            std::pair{defaultMode, defaultModeFallback},
            std::pair{toggleKey, toggleKeyFallback},
            std::pair{punctuation, punctuationFallback},
            std::pair{numberSelection, static_cast<GtkWidget *>(nullptr)},
            std::pair{arrowNavigation, static_cast<GtkWidget *>(nullptr)},
            std::pair{pageNavigation, static_cast<GtkWidget *>(nullptr)},
            std::pair{candidatePageSize, candidatePageSizeFallback},
            std::pair{candidateFontSize, candidateFontSizeFallback},
        };
        for (const auto &[control, fallback] : controls) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                return focusWidgetOrFallback(control, fallback);
            }
        }
        return false;
    }

    SettingsWindowModel &model;
    std::function<void()> changed;
    GtkWidget *page = nullptr;
    GtkWidget *inputEnabled = nullptr;
    GtkWidget *defaultMode = nullptr;
    GtkWidget *defaultModeFallback = nullptr;
    GtkWidget *toggleKey = nullptr;
    GtkWidget *toggleKeyFallback = nullptr;
    GtkWidget *punctuation = nullptr;
    GtkWidget *punctuationFallback = nullptr;
    GtkWidget *numberSelection = nullptr;
    GtkWidget *arrowNavigation = nullptr;
    GtkWidget *pageNavigation = nullptr;
    GtkWidget *candidatePageSize = nullptr;
    GtkWidget *candidatePageSizeFallback = nullptr;
    GtkWidget *candidateFontSize = nullptr;
    GtkWidget *candidateFontSizeFallback = nullptr;
    bool refreshing = false;
};

InputPage::InputPage(SettingsWindowModel &model, std::function<void()> changed)
    : impl_(std::make_unique<Impl>(model, std::move(changed))) {}

InputPage::~InputPage() = default;

GtkWidget *InputPage::widget() const {
    return impl_->page;
}

void InputPage::refresh() {
    impl_->refresh();
}

bool InputPage::focusTarget(std::string_view target) {
    return impl_->focusTarget(target);
}

} // namespace modernime::settings
