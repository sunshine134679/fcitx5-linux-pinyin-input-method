#include "modernime/settings/pages/input_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/settings_widgets.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
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

} // namespace

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

    const bool hasModifiers = !parts.empty();
    if (!hasModifiers) {
        return "";
    }

    bool isPureModifier = false;
    for (const auto &p : parts) {
        if (p == keyName) {
            isPureModifier = true;
            break;
        }
    }

    if (isPureModifier && parts.size() == 1) {
        return parts.front();
    }

    std::string result;
    for (const auto &p : parts) {
        if (!result.empty()) result += "+";
        result += p;
    }
    if (!isPureModifier) {
        if (!result.empty()) result += "+";
        result += keyName;
    }
    return result;
}

namespace {

void setWidgetError(GtkWidget *widget, bool hasError, std::string_view message) {
    if (widget == nullptr) {
        return;
    }
    auto *context = gtk_widget_get_style_context(widget);
    if (hasError) {
        gtk_style_context_add_class(context, "error");
        if (!message.empty()) {
            gtk_widget_set_tooltip_text(widget, std::string(message).c_str());
        }
    } else {
        gtk_style_context_remove_class(context, "error");
    }
}

} // namespace

class InputPage::Impl final {
public:
    Impl(SettingsWindowModel &modelRef, std::function<void()> changedCallback)
        : model(modelRef), changed(std::move(changedCallback)) {
        detail::GtkWidgetGuard pageGuard(createPageShell(
            "输入与按键", "调整默认输入状态、中英文切换键与以词定字。"));
        page = pageGuard.get();
        buildInputStatusSection();
        buildShortcutSection();
        buildPunctuationSection();
        setSettingsFocusChain(
            page, {inputEnabled, defaultMode, toggleKey, characterExtraction,
                   punctuation});
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
            "快捷键", "设置在中文和英文输入状态之间切换的按键及以词定字。");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        toggleKey = gtk_button_new_with_label("");
        addStyleClass(toggleKey, kSettingsShortcutButtonClass);
        gtk_widget_set_hexpand(toggleKey, FALSE);
        setTarget(toggleKey, "toggle-key");
        setAccessibleWidgetText(toggleKey, "中英文切换快捷键",
                                "点击后按下键盘按键以设置中英文切换快捷键");
        toggleKeyFallback = createSettingRow(
            "中英文切换快捷键",
            "支持 Shift 单键、Ctrl+Space、Alt+Space、Super+Space 或 Ctrl+Shift+Space。",
            toggleKey);
        gtk_box_pack_start(GTK_BOX(section), toggleKeyFallback, FALSE, FALSE,
                            0);

        characterExtraction = gtk_switch_new();
        gtk_widget_set_tooltip_text(characterExtraction,
                                    "输入拼音时使用 [ 键输入词组首字，使用 ] 键输入词组尾字");
        setTarget(characterExtraction, "character-extraction");
        characterExtractionFallback = createSettingRow(
            "以词定字快捷键",
            "输入拼音时使用 [ 键输入词组首字，使用 ] 键输入词组尾字。",
            characterExtraction);
        gtk_box_pack_start(GTK_BOX(section), characterExtractionFallback, FALSE,
                            FALSE, 0);

        g_signal_connect(toggleKey, "clicked", G_CALLBACK(onToggleKeyClicked), this);
        g_signal_connect(toggleKey, "key-press-event", G_CALLBACK(onToggleKeyPress), this);
        g_signal_connect(toggleKey, "key-release-event", G_CALLBACK(onToggleKeyRelease), this);
        g_signal_connect(toggleKey, "focus-out-event", G_CALLBACK(onToggleKeyFocusOut), this);
        g_signal_connect(characterExtraction, "notify::active", G_CALLBACK(onSwitchChanged), this);
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

    static const char *shortcutLabel(const std::string &value) {
        return value == "Shift_L" || value == "Shift_R" ? "Shift" : value.c_str();
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
        gtk_button_set_label(GTK_BUTTON(toggleKey), "请按快捷键…");
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
            gtk_button_set_label(GTK_BUTTON(toggleKey), shortcutLabel(toggleKeyValue));
            onChanged(nullptr, this);
        } else {
            gtk_button_set_label(GTK_BUTTON(toggleKey),
                                 toggleKeyValue.empty() ? "点击设置快捷键"
                                                        : shortcutLabel(toggleKeyValue));
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
            std::string modName;
            switch (event->keyval) {
            case GDK_KEY_Shift_L: modName = "Shift_L"; break;
            case GDK_KEY_Shift_R: modName = "Shift_R"; break;
            case GDK_KEY_Control_L: modName = "Control_L"; break;
            case GDK_KEY_Control_R: modName = "Control_R"; break;
            default: break;
            }
            if (!modName.empty()) {
                impl->stopRecording(true, modName);
                return GDK_EVENT_STOP;
            }
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
        settings.characterExtractionEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->characterExtraction));
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
        toggleKeyValue = settings.toggleKey;
        if (isRecording) {
            isRecording = false;
            removeStyleClass(toggleKey, "recording");
        }
        gtk_button_set_label(GTK_BUTTON(toggleKey),
                             toggleKeyValue.empty() ? "点击设置快捷键"
                                                    : shortcutLabel(toggleKeyValue));
        gtk_switch_set_active(GTK_SWITCH(characterExtraction),
                              settings.characterExtractionEnabled);
        gtk_switch_set_active(GTK_SWITCH(punctuation),
                              settings.punctuationEnabled);
        refreshing = false;
        updateState();
    }

    void updateState() {
        const auto state = deriveInputPageState(model);
        if (!state.dependentControlsSensitive && isRecording) {
            stopRecording(false);
        }
        for (auto *control : {defaultMode, toggleKey, characterExtraction, punctuation}) {
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive);
        }
        setWidgetError(toggleKey, !state.toggleKeyValid,
                       state.toggleKeyMessage);
        if (state.toggleKeyValid && !isRecording) {
            gtk_widget_set_tooltip_text(
                toggleKey, "点击后按下快捷键，支持 Shift 单键");
        }
    }

    bool focusTarget(std::string_view target) {
        if (target == "toggle-key-shift") {
            return focusWidgetOrFallback(toggleKey, toggleKeyFallback);
        }
        const std::array controls{
            std::pair{inputEnabled, static_cast<GtkWidget *>(nullptr)},
            std::pair{defaultMode, defaultModeFallback},
            std::pair{toggleKey, toggleKeyFallback},
            std::pair{characterExtraction, characterExtractionFallback},
            std::pair{punctuation, punctuationFallback},
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
    GtkWidget *characterExtraction = nullptr;
    GtkWidget *characterExtractionFallback = nullptr;
    GtkWidget *punctuation = nullptr;
    GtkWidget *punctuationFallback = nullptr;
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
