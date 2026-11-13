#include "modernime/settings/pages/input_page.h"

#include "modernime/settings/detail/gtk_raii.h"
#include "modernime/settings/input_rule_dialog.h"
#include "modernime/settings/input_assist.h"
#include "modernime/settings/script_conversion_control.h"
#include "modernime/settings/settings_disclosure.h"
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
            "输入与按键", ""));
        page = pageGuard.get();
        buildInputStatusSection();
        buildFuzzySection();
        buildShortcutSection();
        script = std::make_unique<ScriptConversionControl>();
        gtk_box_pack_start(GTK_BOX(page), script->widget(), FALSE, FALSE, 0);
        assist = std::make_unique<InputAssist>(model, changed);
        gtk_box_pack_start(GTK_BOX(page), assist->widget(), FALSE, FALSE, 0);
        buildPunctuationSection();
        setSettingsFocusChain(page, {inputEnabled, defaultMode, rememberPerApp, appRules, pinyinScheme, fuzzyExpander,
            fuzzyControls[0], fuzzyControls[1], fuzzyControls[2],
            fuzzyControls[3], fuzzyControls[4], fuzzyControls[5],
            toggleKey, characterExtraction, undoCommit, script->widget(), assist->widget(), punctuation, autoPair, smartPunctuation,
            punctuationAdvanced, underscore, punctuationRules});
        refresh();
        pageGuard.release();
    }

    ~Impl() {
        if (activeRuleDialog) gtk_widget_destroy(activeRuleDialog);
    }

    GtkWidget *addEditingRow(GtkWidget *section, const char *label, const char *description,
                            GtkWidget *control, const char *name) {
        setTarget(control, name);
        setAccessibleWidgetText(control, label, description);
        auto *row = createSettingRow(label, description, control);
        gtk_box_pack_start(GTK_BOX(section), row, FALSE, FALSE, 0);
        editingControls.emplace_back(control, row);
        return row;
    }

    void manageRules(InputRuleKind kind) {
        if (activeRuleDialog) { gtk_window_present(GTK_WINDOW(activeRuleDialog)); return; }
        auto *top = gtk_widget_get_toplevel(page);
        activeRuleDialog = createInputRuleDialog(GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
            model.settings(), kind, [this, kind](core::ModernIMESettings draft) {
                auto settings = model.settings();
                if (kind == InputRuleKind::Application) settings.appInputModes = std::move(draft.appInputModes);
                else settings.punctuationOverrides = std::move(draft.punctuationOverrides);
                model.setSettings(std::move(settings));
                updateState();
                changed();
            });
        g_signal_connect(activeRuleDialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer p) {
            static_cast<Impl *>(p)->activeRuleDialog = nullptr;
        }), this);
        gtk_widget_show_all(activeRuleDialog);
    }

    void buildInputStatusSection() {
        auto *section = createSectionCard(
            "输入状态", "");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);

        inputEnabled = gtk_switch_new();
        setTarget(inputEnabled, "input-enabled");
        gtk_box_pack_start(GTK_BOX(section),
                            createSettingRow("启用 ModernIME",
                                             "控制 ModernIME 是否接收键盘输入并弹出候选框。",
                                             inputEnabled),
                            FALSE, FALSE, 0);

        defaultMode = createSettingsChoice();
        appendSettingsChoice(defaultMode, "中文");
        appendSettingsChoice(defaultMode, "英文");
        gtk_widget_set_hexpand(defaultMode, FALSE);
        setTarget(defaultMode, "default-mode");
        defaultModeFallback = createSettingRow(
            "默认输入状态", "未记住应用状态、也未设置应用规则时，默认使用中文或英文。", defaultMode);
        gtk_box_pack_start(GTK_BOX(section), defaultModeFallback, FALSE, FALSE,
                            0);

        rememberPerApp = gtk_switch_new();
        addEditingRow(section, "按应用记住中英状态",
            "在本次 Fcitx 运行期间，优先记住每个应用切换后的中英状态；重启后不保留。",
            rememberPerApp, "input.remember_per_app");
        appRules = gtk_button_new_with_label("管理规则");
        addEditingRow(section, "应用默认输入状态",
            "为编辑器等应用指定中文或英文；未设置的应用沿用全局默认，已记住的状态优先。",
            appRules, "input.app");
        g_signal_connect(rememberPerApp, "notify::active", G_CALLBACK(onSwitchChanged), this);
        g_signal_connect(appRules, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
            static_cast<Impl *>(p)->manageRules(InputRuleKind::Application);
        }), this);

        g_signal_connect(inputEnabled, "notify::active", G_CALLBACK(onSwitchChanged), this);
        g_signal_connect(defaultMode, "changed", G_CALLBACK(onChanged), this);
    }

    void buildFuzzySection() {
        auto *schemeSection = createSectionCard("拼音方案", "");
        gtk_box_pack_start(GTK_BOX(page), schemeSection, FALSE, FALSE, 0);
        pinyinScheme = createSettingsChoice();
        for (const auto &scheme : core::kPinyinSchemes)
            appendSettingsChoice(pinyinScheme, scheme.label);
        setTarget(pinyinScheme, "pinyin.scheme");
        gtk_box_pack_start(GTK_BOX(schemeSection), createSettingRow(
            "输入方案", "全拼兼容简拼；双拼支持小鹤、自然码。个人词典与学习记录共用。", pinyinScheme), FALSE, FALSE, 0);
        g_signal_connect(pinyinScheme, "changed", G_CALLBACK(onChanged), this);
        fuzzyExpander = gtk_expander_new("模糊音");
        addStyleClass(fuzzyExpander, "modernime-fuzzy-expander");
        setAccessibleWidgetText(fuzzyExpander, "模糊音设置",
            "展开后按发音习惯选择需要混用的音，保存后生效。");
        auto *disclosure = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto *revealer = gtk_revealer_new();
        auto *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(revealer), section);
        gtk_box_pack_start(GTK_BOX(disclosure), fuzzyExpander, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(disclosure), revealer, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(schemeSection), disclosure, FALSE, FALSE, 0);
        bindSettingsDisclosure(fuzzyExpander, revealer);
        for (std::size_t i = 0; i < core::kFuzzyPinyinOptions.size(); ++i) {
            const auto &option = core::kFuzzyPinyinOptions[i];
            auto *control = gtk_switch_new();
            fuzzyControls[i] = control;
            setTarget(control, option.key);
            gtk_box_pack_start(GTK_BOX(section),
                createSettingRow(option.label, "", control),
                FALSE, FALSE, 0);
            g_signal_connect(control, "notify::active", G_CALLBACK(onSwitchChanged), this);
        }
    }

    void buildShortcutSection() {
        auto *section = createSectionCard(
            "快捷键", "");
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

        undoCommit = gtk_switch_new();
        addEditingRow(section, "撤销上屏并重新选词",
            "Ctrl+Shift+BackSpace：15 秒内撤回最近一次候选上屏，恢复原始拼音；需应用支持。",
            undoCommit, "input.undo_commit");
        gtk_widget_set_tooltip_text(undoCommit,
            "只支持最近一次普通候选提交，不含标点、剪贴板等。新输入、光标移动或切换应用后失效；Ctrl+BackSpace 仍留给应用删词。");
        g_signal_connect(undoCommit, "notify::active", G_CALLBACK(onSwitchChanged), this);

        g_signal_connect(toggleKey, "clicked", G_CALLBACK(onToggleKeyClicked), this);
        g_signal_connect(toggleKey, "key-press-event", G_CALLBACK(onToggleKeyPress), this);
        g_signal_connect(toggleKey, "key-release-event", G_CALLBACK(onToggleKeyRelease), this);
        g_signal_connect(toggleKey, "focus-out-event", G_CALLBACK(onToggleKeyFocusOut), this);
        g_signal_connect(characterExtraction, "notify::active", G_CALLBACK(onSwitchChanged), this);
    }

    void buildPunctuationSection() {
        auto *section = createSectionCard(
            "标点", "");
        gtk_box_pack_start(GTK_BOX(page), section, FALSE, FALSE, 0);
        punctuation = gtk_switch_new();
        setTarget(punctuation, "punctuation");
        punctuationFallback = createSettingRow(
            "默认中文标点", "在中文状态下使用全角标点符号（，。！？）。", punctuation);
        gtk_box_pack_start(GTK_BOX(section), punctuationFallback, FALSE, FALSE,
                            0);
        g_signal_connect(punctuation, "notify::active", G_CALLBACK(onSwitchChanged), this);

        autoPair = gtk_switch_new();
        addEditingRow(section, "自动补全成对标点",
            "补全（）、【】、《》和中文引号，光标留在中间；终端及不支持的应用使用普通标点。",
            autoPair, "punctuation.auto_pair");
        smartPunctuation = gtk_switch_new();
        addEditingRow(section, "智能保留英文标点",
            "网址、路径和连续英文标识符内保留半角，如 https://、snake__。",
            smartPunctuation, "punctuation.smart");
        for (auto *control : {autoPair, smartPunctuation})
            g_signal_connect(control, "notify::active", G_CALLBACK(onSwitchChanged), this);

        punctuationAdvanced = gtk_expander_new("更多标点设置");
        setTarget(punctuationAdvanced, "punctuation-advanced");
        auto *disclosure = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto *revealer = gtk_revealer_new();
        auto *advanced = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(revealer), advanced);
        gtk_box_pack_start(GTK_BOX(disclosure), punctuationAdvanced, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(disclosure), revealer, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(section), disclosure, FALSE, FALSE, 0);
        bindSettingsDisclosure(punctuationAdvanced, revealer);
        underscore = createSettingsChoice();
        for (const auto *label : {"智能", "始终 _", "始终 ——"}) appendSettingsChoice(underscore, label);
        addEditingRow(advanced, "下划线输出",
            "智能模式单独输入为 ——，英文标识符内为 _；逐键自定义优先。",
            underscore, "punctuation.underscore");
        punctuationRules = gtk_button_new_with_label("自定义标点");
        addEditingRow(advanced, "逐键自定义",
            "让 /、_ 等按键始终输出指定文本，覆盖该键的智能转换与配对。",
            punctuationRules, "punctuation.map");
        g_signal_connect(underscore, "changed", G_CALLBACK(onChanged), this);
        g_signal_connect(punctuationRules, "clicked", G_CALLBACK(+[](GtkButton *, gpointer p) {
            static_cast<Impl *>(p)->manageRules(InputRuleKind::Punctuation);
        }), this);
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
        settings.inputEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->inputEnabled));
        settings.defaultMode = settingsChoiceActive(impl->defaultMode) == 1
                                   ? core::InputMode::English
                                   : core::InputMode::Chinese;
        settings.toggleKey = impl->toggleKeyValue;
        settings.punctuationEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->punctuation));
        settings.characterExtractionEnabled = gtk_switch_get_active(
            GTK_SWITCH(impl->characterExtraction));
        settings.autoPairPunctuation = gtk_switch_get_active(GTK_SWITCH(impl->autoPair));
        settings.smartPunctuation = gtk_switch_get_active(GTK_SWITCH(impl->smartPunctuation));
        settings.rememberModePerApp = gtk_switch_get_active(GTK_SWITCH(impl->rememberPerApp));
        settings.undoCommitEnabled = gtk_switch_get_active(GTK_SWITCH(impl->undoCommit));
        const std::array underscoreStyles{core::UnderscoreStyle::Smart, core::UnderscoreStyle::Ascii, core::UnderscoreStyle::Chinese};
        const auto underscoreIndex = settingsChoiceActive(impl->underscore);
        if (underscoreIndex >= 0 && underscoreIndex < static_cast<int>(underscoreStyles.size()))
            settings.underscoreStyle = underscoreStyles[underscoreIndex];
        const auto schemeIndex = settingsChoiceActive(impl->pinyinScheme);
        if (schemeIndex >= 0 && static_cast<std::size_t>(schemeIndex) < core::kPinyinSchemes.size())
            settings.pinyinScheme = core::kPinyinSchemes[schemeIndex].scheme;
        for (std::size_t i = 0; i < core::kFuzzyPinyinOptions.size(); ++i) {
            settings.fuzzyPinyin.*core::kFuzzyPinyinOptions[i].member =
                gtk_switch_get_active(GTK_SWITCH(impl->fuzzyControls[i]));
        }
        impl->model.setSettings(std::move(settings));
        impl->updateState();
        impl->changed();
    }

    void refresh() {
        refreshing = true;
        const auto &settings = model.settings();
        gtk_switch_set_active(GTK_SWITCH(inputEnabled), settings.inputEnabled);
        setSettingsChoiceActive(defaultMode,
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
        gtk_switch_set_active(GTK_SWITCH(autoPair), settings.autoPairPunctuation);
        gtk_switch_set_active(GTK_SWITCH(smartPunctuation), settings.smartPunctuation);
        gtk_switch_set_active(GTK_SWITCH(rememberPerApp), settings.rememberModePerApp);
        gtk_switch_set_active(GTK_SWITCH(undoCommit), settings.undoCommitEnabled);
        setSettingsChoiceActive(underscore, settings.underscoreStyle == core::UnderscoreStyle::Ascii ? 1
            : settings.underscoreStyle == core::UnderscoreStyle::Chinese ? 2 : 0);
        for (std::size_t i = 0; i < core::kPinyinSchemes.size(); ++i) {
            if (core::kPinyinSchemes[i].scheme == settings.pinyinScheme)
                setSettingsChoiceActive(pinyinScheme, static_cast<int>(i));
        }
        for (std::size_t i = 0; i < core::kFuzzyPinyinOptions.size(); ++i) {
            gtk_switch_set_active(GTK_SWITCH(fuzzyControls[i]),
                settings.fuzzyPinyin.*core::kFuzzyPinyinOptions[i].member);
        }
        refreshing = false;
        updateState();
    }

    void updateState() {
        assist->refresh();
        const auto state = deriveInputPageState(model);
        if (!state.dependentControlsSensitive && isRecording) {
            stopRecording(false);
        }
        for (auto *control : {defaultMode, pinyinScheme, toggleKey, characterExtraction, punctuation}) {
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive);
        }
        for (auto *control : {rememberPerApp, appRules, undoCommit})
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive);
        for (auto *control : {autoPair, smartPunctuation, underscore, punctuationRules})
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive && model.settings().punctuationEnabled);
        const auto applicationSummary = "管理规则 · " + std::to_string(model.settings().appInputModes.size()) + " 条";
        gtk_button_set_label(GTK_BUTTON(appRules), applicationSummary.c_str());
        const auto punctuationSummary = "自定义 · " + std::to_string(model.settings().punctuationOverrides.size()) + " 项";
        gtk_button_set_label(GTK_BUTTON(punctuationRules), punctuationSummary.c_str());
        for (auto *control : fuzzyControls) {
            gtk_widget_set_sensitive(control, state.dependentControlsSensitive);
        }
        const auto &fuzzy = model.settings().fuzzyPinyin;
        const auto enabled = std::count_if(core::kFuzzyPinyinOptions.begin(),
            core::kFuzzyPinyinOptions.end(), [&fuzzy](const auto &option) {
                return fuzzy.*option.member;
            });
        const auto fuzzyLabel = "模糊音 · 已开启 " + std::to_string(enabled) + " 组";
        gtk_expander_set_label(GTK_EXPANDER(fuzzyExpander), fuzzyLabel.c_str());
        setWidgetError(toggleKey, !state.toggleKeyValid,
                       state.toggleKeyMessage);
        if (state.toggleKeyValid && !isRecording) {
            gtk_widget_set_tooltip_text(
                toggleKey, "点击后按下快捷键，支持 Shift 单键");
        }
    }

    bool focusTarget(std::string_view target) {
        if (script->focusTarget(target)) return true;
        if (assist->focusTarget(target)) return true;
        for (const auto &[control, fallback] : editingControls) {
            const auto *id = static_cast<const char *>(g_object_get_data(G_OBJECT(control), "modernime-settings-target"));
            if (id && target == id) {
                if (control == underscore || control == punctuationRules)
                    setSettingsDisclosureExpanded(punctuationAdvanced, true, false);
                return focusWidgetOrFallback(control, fallback);
            }
        }
        if (target == "pinyin.scheme") return focusWidgetOrFallback(pinyinScheme, nullptr);
        for (std::size_t i = 0; i < core::kFuzzyPinyinOptions.size(); ++i) {
            if (target == core::kFuzzyPinyinOptions[i].key) {
                setSettingsDisclosureExpanded(fuzzyExpander, true, false);
                return focusWidgetOrFallback(fuzzyControls[i], fuzzyExpander);
            }
        }
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
    GtkWidget *pinyinScheme = nullptr;
    GtkWidget *fuzzyExpander = nullptr;
    GtkWidget *defaultModeFallback = nullptr;
    std::array<GtkWidget *, core::kFuzzyPinyinOptions.size()> fuzzyControls{};
    GtkWidget *toggleKey = nullptr;
    GtkWidget *toggleKeyFallback = nullptr;
    std::string toggleKeyValue;
    bool isRecording = false;
    GtkWidget *characterExtraction = nullptr;
    GtkWidget *characterExtractionFallback = nullptr;
    GtkWidget *punctuation = nullptr;
    GtkWidget *punctuationFallback = nullptr;
    GtkWidget *rememberPerApp = nullptr, *appRules = nullptr, *undoCommit = nullptr;
    GtkWidget *autoPair = nullptr, *smartPunctuation = nullptr, *underscore = nullptr;
    GtkWidget *punctuationAdvanced = nullptr, *punctuationRules = nullptr, *activeRuleDialog = nullptr;
    std::vector<std::pair<GtkWidget *, GtkWidget *>> editingControls;
    std::unique_ptr<InputAssist> assist;
    std::unique_ptr<ScriptConversionControl> script;
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
