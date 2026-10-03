#include "modernime/settings/pages/input_page.h"

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

bool isModifierKey(unsigned int keyval) {
    switch (keyval) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_Hyper_L:
    case GDK_KEY_Hyper_R:
    case GDK_KEY_ISO_Level3_Shift:
    case GDK_KEY_Mode_switch:
        return true;
    default:
        return false;
    }
}

std::string formatModifierPrompt(unsigned int keyval, unsigned int state) {
    const bool ctrl = (state & GDK_CONTROL_MASK) != 0 ||
                      keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R;
    const bool alt = (state & GDK_MOD1_MASK) != 0 ||
                     keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R;
    const bool super = (state & (GDK_SUPER_MASK | GDK_MOD4_MASK)) != 0 ||
                       keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R ||
                       keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R;
    const bool shift = (state & GDK_SHIFT_MASK) != 0 ||
                       keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R;

    std::vector<std::string> parts;
    if (ctrl) parts.emplace_back("Ctrl");
    if (alt) parts.emplace_back("Alt");
    if (super) parts.emplace_back("Super");
    if (shift) parts.emplace_back("Shift");

    if (parts.empty()) {
        return "请按快捷键... (Esc取消)";
    }
    std::string result;
    for (const auto &p : parts) {
        if (!result.empty()) result += " + ";
        result += p;
    }
    result += " + ...";
    return result;
}

std::string buildShortcutString(unsigned int keyval, unsigned int state) {
    const bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    const bool alt = (state & GDK_MOD1_MASK) != 0;
    const bool super = (state & (GDK_SUPER_MASK | GDK_MOD4_MASK)) != 0;
    const bool shift = (state & GDK_SHIFT_MASK) != 0;

    std::string keyName;
    if (keyval == GDK_KEY_space || keyval == GDK_KEY_KP_Space) {
        keyName = "Space";
    } else if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
        keyName = std::string(1, static_cast<char>(keyval - GDK_KEY_a + 'A'));
    } else if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
        keyName = std::string(1, static_cast<char>(keyval));
    } else if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
        keyName = std::string(1, static_cast<char>(keyval));
    } else if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12) {
        keyName = "F" + std::to_string(keyval - GDK_KEY_F1 + 1);
    } else {
        const char *name = gdk_keyval_name(keyval);
        if (name != nullptr) {
            keyName = name;
        }
    }

    if (keyName.empty()) {
        return "";
    }

    std::vector<std::string> parts;
    if (ctrl) parts.emplace_back("Ctrl");
    if (alt) parts.emplace_back("Alt");
    if (super) parts.emplace_back("Super");
    if (shift) parts.emplace_back("Shift");
    parts.push_back(std::move(keyName));

    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "+";
        result += parts[i];
    }
    return result;
}

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

void setWidgetError(GtkWidget *widget, bool error,
                    std::string_view tooltip) {
    if (error) {
        addStyleClass(widget, "error");
    } else {
        removeStyleClass(widget, "error");
    }
    gtk_widget_set_tooltip_text(
        widget, tooltip.empty() ? nullptr : std::string(tooltip).c_str());
}

} // namespace


class InputPage::Impl final {
public:
    Impl(SettingsWindowModel &modelRef, std::function<void()> changedCallback)
        : model(modelRef), changed(std::move(changedCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "输入体验", "调整默认输入状态、快捷键、标点和候选外观"));
        page = pageGuard.get();
        buildInputStatusSection();
        buildShortcutSection();
        buildPunctuationSection();
        buildCandidateBehaviorSection();
        buildCandidateAppearanceSection();
        setSettingsFocusChain(
            page, {inputEnabled, defaultMode, toggleKey, punctuation,
                   numberSelection, arrowNavigation, pageNavigation,
                   englishDefinition, candidatePageSize, candidateFontSize});
        refresh();
        pageGuard.release();
    }

    void buildInputStatusSection() {
        auto *section = createSectionCard(
            "输入状态", "这些设置决定 ModernIME 何时接收键盘输入。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        inputEnabled = gtk_check_button_new();
        setTarget(inputEnabled, "input-enabled");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("启用 ModernIME",
                                            "控制 ModernIME 是否接收键盘输入并弹出候选框。",
                                            inputEnabled),
                           FALSE, FALSE, 0);

        defaultMode = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(defaultMode), "中文");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(defaultMode), "英文");
        gtk_widget_set_hexpand(defaultMode, FALSE);
        setTarget(defaultMode, "default-mode");
        defaultModeFallback = createSettingRow(
            "默认输入状态", "选择启动或重置时默认使用中文或英文。", defaultMode);
        gtk_box_pack_start(GTK_BOX(section), defaultModeFallback, FALSE, FALSE,
                           0);

        g_signal_connect(inputEnabled, "notify::active", G_CALLBACK(onSwitchChanged), this);
        g_signal_connect(defaultMode, "changed", G_CALLBACK(onChanged), this);
    }

    void buildShortcutSection() {
        auto *section = createSectionCard(
            "快捷键", "设置在中文和英文输入状态之间切换的按键。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        toggleKey = gtk_button_new_with_label("");
        addStyleClass(toggleKey, kSettingsShortcutButtonClass);
        gtk_widget_set_hexpand(toggleKey, FALSE);
        setTarget(toggleKey, "toggle-key");
        setAccessibleWidgetText(toggleKey, "中英文切换快捷键",
                                "点击后按下键盘按键以设置中英文切换快捷键");
        toggleKeyFallback = createSettingRow(
            "中英文切换快捷键",
            "支持 Ctrl+Space、Alt+Space、Super+Space 或 Ctrl+Shift+Space。",
            toggleKey);
        gtk_box_pack_start(GTK_BOX(section), toggleKeyFallback, FALSE, FALSE,
                           0);

        g_signal_connect(toggleKey, "clicked", G_CALLBACK(onToggleKeyClicked), this);
        g_signal_connect(toggleKey, "key-press-event", G_CALLBACK(onToggleKeyPress), this);
        g_signal_connect(toggleKey, "key-release-event", G_CALLBACK(onToggleKeyRelease), this);
        g_signal_connect(toggleKey, "focus-out-event", G_CALLBACK(onToggleKeyFocusOut), this);
    }

    void buildPunctuationSection() {
        auto *section = createSectionCard(
            "标点", "配置中文输入状态下的标点输出方式。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        punctuation = gtk_switch_new();
        setTarget(punctuation, "punctuation");
        punctuationFallback = createSettingRow(
            "默认中文标点", "在中文状态下使用全角标点符号（，。！？）。", punctuation);
        gtk_box_pack_start(GTK_BOX(section), punctuationFallback, FALSE, FALSE,
                           0);
        g_signal_connect(punctuation, "notify::active", G_CALLBACK(onSwitchChanged), this);
    }

    void buildCandidateBehaviorSection() {
        auto *section = createSectionCard(
            "候选行为", "关闭某项后，对应按键会交给其他输入行为处理。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        numberSelection = gtk_switch_new();
        gtk_widget_set_tooltip_text(numberSelection, "使用数字键选择当前候选项");
        setTarget(numberSelection, "number-selection");

        arrowNavigation = gtk_switch_new();
        gtk_widget_set_tooltip_text(
            arrowNavigation, "左右移动拼音光标，上下切换候选项");
        setTarget(arrowNavigation, "arrow-navigation");

        pageNavigation = gtk_switch_new();
        gtk_widget_set_tooltip_text(pageNavigation,
                                    "使用 PageUp / PageDown 或 + / = 翻页");
        setTarget(pageNavigation, "page-navigation");

        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("数字键选择候选",
                                            "使用键盘顶部 1~9 数字键快速选择对应候选项上屏。",
                                            numberSelection),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("方向键编辑与选词",
                                            "左右键移动拼音光标，上下键快速整页翻页与切换候选项。",
                                            arrowNavigation),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("候选翻页",
                                            "使用 PageUp / PageDown 和 + / = 键进行前后翻页。",
                                            pageNavigation),
                           FALSE, FALSE, 0);

        englishDefinition = gtk_switch_new();
        gtk_widget_set_tooltip_text(
            englishDefinition, "当候选栏第 1 位是英文单词时，在第 2 位显示其中文释义");
        setTarget(englishDefinition, "english-definition");
        gtk_box_pack_start(GTK_BOX(section),
                           createSettingRow("显示英文单词中文释义",
                                            "当候选栏第 1 位是英文单词时，在第 2 位插入其简明中文释义，直接选词即可上屏释义。",
                                            englishDefinition),
                           FALSE, FALSE, 0);

        for (auto *control : {numberSelection, arrowNavigation, pageNavigation, englishDefinition}) {
            g_signal_connect(control, "notify::active", G_CALLBACK(onSwitchChanged), this);
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

        // Live Preview Box
        previewBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        addStyleClass(previewBox, "modernime-preview-bar");
        auto *previewTitle = gtk_label_new("候选栏排版实时预览");
        addStyleClass(previewTitle, "modernime-setting-desc");
        gtk_widget_set_halign(previewTitle, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewBox), previewTitle, FALSE, FALSE, 0);

        previewLabel = gtk_label_new("");
        gtk_widget_set_halign(previewLabel, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(previewBox), previewLabel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), previewBox, FALSE, FALSE, 0);

        g_signal_connect(candidatePageSize, "value-changed", G_CALLBACK(onChanged), this);
        g_signal_connect(candidateFontSize, "value-changed", G_CALLBACK(onChanged), this);
    }

    void updatePreview(int pageSize, int fontSize) {
        if (previewLabel == nullptr) return;
        std::ostringstream ss;
        ss << "<span font='" << fontSize << "pt'>";
        for (int i = 1; i <= std::min(pageSize, 5); ++i) {
            if (i > 1) ss << "   ";
            ss << "<span alpha='65%'>" << i << ".</span> ";
            if (i == 1) ss << "你好";
            else if (i == 2) ss << "世界";
            else if (i == 3) ss << "现代";
            else if (i == 4) ss << "拼音";
            else ss << "输入法";
        }
        ss << "</span>";
        gtk_label_set_markup(GTK_LABEL(previewLabel), ss.str().c_str());
    }

    static void onToggleKeyClicked(GtkButton *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->refreshing) {
            return;
        }
        if (impl->isRecording) {
            impl->stopRecording(false);
        } else {
            impl->startRecording();
        }
    }

    void startRecording() {
        isRecording = true;
        addStyleClass(toggleKey, "recording");
        gtk_button_set_label(GTK_BUTTON(toggleKey), "请按快捷键... (Esc取消)");
        gtk_widget_set_tooltip_text(toggleKey, "请在键盘上按下目标快捷键组合，按 Esc 取消");
    }

    void stopRecording(bool applyNewValue, std::string newValue = {}) {
        if (!isRecording) {
            return;
        }
        isRecording = false;
        removeStyleClass(toggleKey, "recording");
        if (applyNewValue && !newValue.empty()) {
            toggleKeyValue = std::move(newValue);
            gtk_button_set_label(GTK_BUTTON(toggleKey), toggleKeyValue.c_str());
            onChanged(nullptr, this);
        } else {
            gtk_button_set_label(GTK_BUTTON(toggleKey),
                                 toggleKeyValue.empty() ? "点击设置快捷键"
                                                        : toggleKeyValue.c_str());
            updateState();
        }
    }

    static gboolean onToggleKeyPress(GtkWidget *, GdkEventKey *event, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (!impl->isRecording) {
            return GDK_EVENT_PROPAGATE;
        }

        if (event->keyval == GDK_KEY_Escape) {
            impl->stopRecording(false);
            return GDK_EVENT_STOP;
        }

        if ((event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_ISO_Left_Tab) &&
            (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_MOD4_MASK)) == 0) {
            impl->stopRecording(false);
            return GDK_EVENT_PROPAGATE;
        }

        if (isModifierKey(event->keyval)) {
            const auto prompt = formatModifierPrompt(event->keyval, event->state);
            gtk_button_set_label(GTK_BUTTON(impl->toggleKey), prompt.c_str());
            return GDK_EVENT_STOP;
        }

        const auto shortcut = buildShortcutString(event->keyval, event->state);
        if (!shortcut.empty()) {
            impl->stopRecording(true, shortcut);
        } else {
            impl->stopRecording(false);
        }
        return GDK_EVENT_STOP;
    }

    static gboolean onToggleKeyRelease(GtkWidget *, GdkEventKey *event, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (!impl->isRecording) {
            return GDK_EVENT_PROPAGATE;
        }

        if (isModifierKey(event->keyval)) {
            guint remainingState = event->state;
            if (event->keyval == GDK_KEY_Control_L || event->keyval == GDK_KEY_Control_R) {
                remainingState &= ~GDK_CONTROL_MASK;
            } else if (event->keyval == GDK_KEY_Alt_L || event->keyval == GDK_KEY_Alt_R) {
                remainingState &= ~GDK_MOD1_MASK;
            } else if (event->keyval == GDK_KEY_Super_L || event->keyval == GDK_KEY_Super_R ||
                       event->keyval == GDK_KEY_Meta_L || event->keyval == GDK_KEY_Meta_R) {
                remainingState &= ~(GDK_SUPER_MASK | GDK_MOD4_MASK);
            } else if (event->keyval == GDK_KEY_Shift_L || event->keyval == GDK_KEY_Shift_R) {
                remainingState &= ~GDK_SHIFT_MASK;
            }

            const auto prompt = formatModifierPrompt(0, remainingState);
            gtk_button_set_label(GTK_BUTTON(impl->toggleKey), prompt.c_str());
            return GDK_EVENT_STOP;
        }
        return GDK_EVENT_PROPAGATE;
    }

    static gboolean onToggleKeyFocusOut(GtkWidget *, GdkEventFocus *, gpointer data) {
        auto *impl = static_cast<Impl *>(data);
        if (impl->isRecording) {
            impl->stopRecording(false);
        }
        return GDK_EVENT_PROPAGATE;
    }

    static void onSwitchChanged(GObject *, GParamSpec *, gpointer data) {
        onChanged(nullptr, data);
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
        settings.toggleKey = impl->toggleKeyValue;
        settings.punctuationEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->punctuation));
        settings.numberSelection = gtk_switch_get_active(
            GTK_SWITCH(impl->numberSelection));
        settings.arrowNavigation = gtk_switch_get_active(
            GTK_SWITCH(impl->arrowNavigation));
        settings.pageNavigation = gtk_switch_get_active(
            GTK_SWITCH(impl->pageNavigation));
        settings.englishDefinitionEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->englishDefinition));
        settings.candidatePageSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidatePageSize));
        settings.candidateFontSize = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(impl->candidateFontSize));
        impl->model.setSettings(std::move(settings));
        impl->updateState();
        impl->updatePreview(settings.candidatePageSize, settings.candidateFontSize);
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
        toggleKeyValue = settings.toggleKey;
        if (isRecording) {
            isRecording = false;
            removeStyleClass(toggleKey, "recording");
        }
        gtk_button_set_label(GTK_BUTTON(toggleKey),
                             toggleKeyValue.empty() ? "点击设置快捷键"
                                                    : toggleKeyValue.c_str());
        gtk_switch_set_active(GTK_SWITCH(punctuation),
                              settings.punctuationEnabled);
        gtk_switch_set_active(GTK_SWITCH(numberSelection),
                              settings.numberSelection);
        gtk_switch_set_active(GTK_SWITCH(arrowNavigation),
                              settings.arrowNavigation);
        gtk_switch_set_active(GTK_SWITCH(pageNavigation),
                              settings.pageNavigation);
        gtk_switch_set_active(GTK_SWITCH(englishDefinition),
                              settings.englishDefinitionEnabled);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidatePageSize),
                                  settings.candidatePageSize);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(candidateFontSize),
                                  settings.candidateFontSize);
        updatePreview(settings.candidatePageSize, settings.candidateFontSize);
        refreshing = false;
        updateState();
    }

    void updateState() {
        const auto state = deriveInputPageState(model);
        if (!state.dependentControlsSensitive && isRecording) {
            stopRecording(false);
        }
        for (auto *control : {defaultMode, toggleKey, punctuation}) {
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive);
        }
        setWidgetError(toggleKey, !state.toggleKeyValid,
                       state.toggleKeyMessage);
        if (state.toggleKeyValid && !isRecording) {
            gtk_widget_set_tooltip_text(
                toggleKey, "点击以录制新的快捷键（例如 Ctrl+Space）");
        }
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
            std::pair{englishDefinition, static_cast<GtkWidget *>(nullptr)},
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
    std::string toggleKeyValue;
    bool isRecording = false;
    GtkWidget *punctuation = nullptr;
    GtkWidget *punctuationFallback = nullptr;
    GtkWidget *numberSelection = nullptr;
    GtkWidget *arrowNavigation = nullptr;
    GtkWidget *pageNavigation = nullptr;
    GtkWidget *englishDefinition = nullptr;
    GtkWidget *candidatePageSize = nullptr;
    GtkWidget *candidatePageSizeFallback = nullptr;
    GtkWidget *candidateFontSize = nullptr;
    GtkWidget *candidateFontSizeFallback = nullptr;
    GtkWidget *previewBox = nullptr;
    GtkWidget *previewLabel = nullptr;
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
