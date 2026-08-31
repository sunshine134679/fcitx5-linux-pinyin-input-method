#include "modernime/settings/page_registry.h"

#include <array>
#include <string>

namespace modernime::settings {
namespace {

constexpr std::array kPages{
    SettingsPageDefinition{SettingsPageId::Overview, "overview", "概览",
                           "查看 ModernIME 设置和本地数据概况", "主页"},
    SettingsPageDefinition{SettingsPageId::Input, "input", "输入体验",
                           "配置输入状态、快捷键、标点和候选行为", "输入"},
    SettingsPageDefinition{SettingsPageId::Dictionary, "dictionary", "个人词典",
                           "维护个人词条和专业名词", "数据与工具"},
    SettingsPageDefinition{SettingsPageId::Clipboard, "clipboard", "剪贴板",
                           "管理 V+2 剪贴板功能和历史内容", "数据与工具"},
    SettingsPageDefinition{SettingsPageId::Learning, "learning", "智能学习",
                           "管理用户习惯学习和上下文排序", "数据与工具"},
    SettingsPageDefinition{SettingsPageId::Diagnostics, "diagnostics",
                           "系统与诊断", "检查 Fcitx5 和 ModernIME 运行状态",
                           "数据与工具"},
};

constexpr std::array kSearchEntries{
    SettingsSearchEntry{SettingsPageId::Overview, "", "概览",
                        "查看 ModernIME 设置和本地数据概况", "overview home"},
    SettingsSearchEntry{SettingsPageId::Input, "", "输入体验",
                        "配置输入状态、快捷键、标点和候选行为", "input"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "", "个人词典",
                        "维护个人词条和专业名词", "dictionary"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "", "剪贴板",
                        "管理 V+2 剪贴板功能和历史内容", "clipboard"},
    SettingsSearchEntry{SettingsPageId::Learning, "", "智能学习",
                        "管理用户习惯学习和上下文排序", "learning"},
    SettingsSearchEntry{SettingsPageId::Diagnostics, "", "系统与诊断",
                        "检查 Fcitx5 和 ModernIME 运行状态", "diagnostics system"},
    SettingsSearchEntry{SettingsPageId::Input, "input-enabled", "启用 ModernIME",
                        "控制 ModernIME 是否接收键盘输入", "enable input"},
    SettingsSearchEntry{SettingsPageId::Input, "default-mode", "默认输入状态",
                        "选择启动时默认使用中文或英文", "default mode chinese english"},
    SettingsSearchEntry{SettingsPageId::Input, "toggle-key", "中英文切换快捷键",
                        "设置中英文输入状态之间的切换按键", "shortcut hotkey toggle ctrl space"},
    SettingsSearchEntry{SettingsPageId::Input, "punctuation", "中文标点",
                        "在中文状态下使用全角标点", "punctuation full width"},
    SettingsSearchEntry{SettingsPageId::Input, "number-selection", "数字键选择候选",
                        "使用数字键选择当前候选项", "number selection candidate"},
    SettingsSearchEntry{SettingsPageId::Input, "arrow-navigation", "左右方向键切换候选",
                        "使用左右方向键切换候选项", "arrow navigation candidate"},
    SettingsSearchEntry{SettingsPageId::Input, "page-navigation", "候选翻页",
                        "使用上下方向键和 + / = 翻页", "page navigation candidate"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-add", "添加词条",
                        "向个人词典添加一条词条", "dictionary add entry"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-edit", "编辑词条",
                        "编辑选中的个人词典词条", "dictionary edit entry"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-delete", "删除词条",
                        "删除选中的个人词典词条", "dictionary delete entry"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-import", "导入词典",
                        "从文本文件导入个人词典词条", "dictionary import"},
    SettingsSearchEntry{SettingsPageId::Dictionary, "dictionary-export", "导出词典",
                        "将当前个人词典导出为文本文件", "dictionary export"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-enabled", "启用 V+2 剪贴板",
                        "开启本地剪贴板历史功能", "clipboard enable"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-trigger", "剪贴板触发键",
                        "设置打开剪贴板历史的触发键", "clipboard trigger shortcut"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-copy", "复制选中",
                        "复制选中的剪贴板历史内容", "clipboard copy"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-delete", "删除选中",
                        "删除选中的剪贴板历史内容", "clipboard delete"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-clear", "清空历史",
                        "清空全部本地剪贴板历史内容", "clipboard clear history"},
    SettingsSearchEntry{SettingsPageId::Clipboard, "clipboard-refresh", "刷新历史",
                        "重新读取磁盘上的剪贴板历史", "clipboard refresh"},
    SettingsSearchEntry{SettingsPageId::Learning, "learning-enabled", "记忆用户候选选择",
                        "记录主动选择的候选词", "learning remember candidate"},
    SettingsSearchEntry{SettingsPageId::Learning, "context-learning", "上下文学习",
                        "根据光标前后文调整候选排序", "context learning"},
    SettingsSearchEntry{SettingsPageId::Learning, "learning-clear", "清空学习记录",
                        "备份后清空本地学习排序", "learning clear reset"},
    SettingsSearchEntry{SettingsPageId::Diagnostics, "diagnostics-refresh", "刷新状态",
                        "重新查询 Fcitx5 和 ModernIME 状态", "fcitx fcitx5 diagnostics refresh"},
    SettingsSearchEntry{SettingsPageId::Diagnostics, "diagnostics-reload", "重新加载 ModernIME",
                        "保存配置后重新加载 ModernIME", "fcitx fcitx5 reload restart"},
};

bool isAsciiWhitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r' || character == '\f' || character == '\v';
}

std::string trimAndFoldAscii(std::string_view value) {
    while (!value.empty() && isAsciiWhitespace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isAsciiWhitespace(value.back())) {
        value.remove_suffix(1);
    }

    std::string result(value);
    for (auto &character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return result;
}

bool contains(std::string_view value, const std::string &query) {
    return trimAndFoldAscii(value).find(query) != std::string::npos;
}

} // namespace

std::span<const SettingsPageDefinition> settingsPageDefinitions() {
    return kPages;
}

std::span<const SettingsSearchEntry> settingsSearchEntries() {
    return kSearchEntries;
}

std::vector<SettingsSearchEntry> searchSettings(std::string_view query) {
    const auto normalizedQuery = trimAndFoldAscii(query);
    if (normalizedQuery.empty()) {
        return {};
    }

    std::vector<SettingsSearchEntry> exactTitle;
    std::vector<SettingsSearchEntry> titleSubstring;
    std::vector<SettingsSearchEntry> descriptionOrKeywordSubstring;
    for (const auto &entry : kSearchEntries) {
        const auto normalizedTitle = trimAndFoldAscii(entry.title);
        if (normalizedTitle == normalizedQuery) {
            exactTitle.push_back(entry);
        } else if (normalizedTitle.find(normalizedQuery) != std::string::npos) {
            titleSubstring.push_back(entry);
        } else if (contains(entry.description, normalizedQuery) ||
                   contains(entry.keywords, normalizedQuery)) {
            descriptionOrKeywordSubstring.push_back(entry);
        }
    }

    exactTitle.insert(exactTitle.end(), titleSubstring.begin(),
                      titleSubstring.end());
    exactTitle.insert(exactTitle.end(), descriptionOrKeywordSubstring.begin(),
                      descriptionOrKeywordSubstring.end());
    return exactTitle;
}

} // namespace modernime::settings
