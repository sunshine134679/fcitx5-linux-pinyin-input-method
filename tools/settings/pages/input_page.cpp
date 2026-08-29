#include "modernime/settings/pages/input_page.h"

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
        page = createPageShell("输入体验",
                               "配置输入状态、快捷键、标点和候选行为");
        buildInputStatusSection();
        buildShortcutSection();
        buildPunctuationSection();
        buildCandidateBehaviorSection();
        refresh();
    }

    void buildInputStatusSection() {
        auto *section = createSectionCard(
            "输入状态", "这些设置决定 ModernIME 何时接收键盘输入。");
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
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("默认输入状态",
                                            "选择启动时默认使用中文或英文。",
                                            defaultMode),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        g_signal_connect(inputEnabled, "toggled", G_CALLBACK(onChanged), this);
        g_signal_connect(defaultMode, "changed", G_CALLBACK(onChanged), this);
    }

    void buildShortcutSection() {
        auto *section = createSectionCard(
            "快捷键", "设置在中文和英文输入状态之间切换的按键。");
        toggleKey = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(toggleKey), "例如 Ctrl+Space");
        gtk_widget_set_hexpand(toggleKey, TRUE);
        setTarget(toggleKey, "toggle-key");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("中英文切换快捷键",
                                            "只能包含字母、数字、+ 或 -。",
                                            toggleKey),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        g_signal_connect(toggleKey, "changed", G_CALLBACK(onChanged), this);
    }

    void buildPunctuationSection() {
        auto *section = createSectionCard(
            "标点", "配置中文输入状态下的标点输出方式。");
        punctuation = gtk_check_button_new_with_label(
            "中文标点使用全角（，。？！等）");
        setTarget(punctuation, "punctuation");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("中文标点",
                                            "在中文状态下使用全角标点。",
                                            punctuation),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        g_signal_connect(punctuation, "toggled", G_CALLBACK(onChanged), this);
    }

    void buildCandidateBehaviorSection() {
        auto *section = createSectionCard(
            "候选行为", "关闭某项后，对应按键会交给其他输入行为处理。");
        numberSelection = gtk_check_button_new_with_label("数字键选择候选");
        gtk_widget_set_tooltip_text(numberSelection, "使用数字键选择当前候选项");
        setTarget(numberSelection, "number-selection");
        arrowNavigation = gtk_check_button_new_with_label("左右方向键切换候选");
        gtk_widget_set_tooltip_text(arrowNavigation, "使用左右方向键切换候选项");
        setTarget(arrowNavigation, "arrow-navigation");
        pageNavigation = gtk_check_button_new_with_label(
            "上下方向键和 + / = 翻页");
        gtk_widget_set_tooltip_text(pageNavigation,
                                    "使用上下方向键或 + / = 翻页");
        setTarget(pageNavigation, "page-navigation");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("数字键选择候选",
                                            "使用数字键选择当前候选项。",
                                            numberSelection),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("左右方向键切换候选",
                                            "使用左右方向键切换候选项。",
                                            arrowNavigation),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("候选翻页",
                                            "使用上下方向键和 + / = 翻页。",
                                            pageNavigation),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        for (auto *control : {numberSelection, arrowNavigation, pageNavigation}) {
            g_signal_connect(control, "toggled", G_CALLBACK(onChanged), this);
        }
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
            inputEnabled, defaultMode, toggleKey, punctuation, numberSelection,
            arrowNavigation, pageNavigation,
        };
        for (auto *control : controls) {
            const auto *id = static_cast<const char *>(g_object_get_data(
                G_OBJECT(control), "modernime-settings-target"));
            if (id != nullptr && target == id) {
                gtk_widget_grab_focus(control);
                return true;
            }
        }
        return false;
    }

    SettingsWindowModel &model;
    std::function<void()> changed;
    GtkWidget *page = nullptr;
    GtkWidget *inputEnabled = nullptr;
    GtkWidget *defaultMode = nullptr;
    GtkWidget *toggleKey = nullptr;
    GtkWidget *punctuation = nullptr;
    GtkWidget *numberSelection = nullptr;
    GtkWidget *arrowNavigation = nullptr;
    GtkWidget *pageNavigation = nullptr;
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
