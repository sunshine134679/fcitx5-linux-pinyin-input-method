#pragma once

#include <array>
#include <string_view>

namespace modernime::settings {

struct SettingsPageDefinition final {
    std::string_view name;
    std::string_view title;
    std::string_view subtitle;
};

constexpr auto settingsPageDefinitions() {
    return std::array<SettingsPageDefinition, 6>{
        SettingsPageDefinition{"basic", "基本设置", "配置输入法的启用状态和基础快捷键"},
        SettingsPageDefinition{"candidate", "候选设置", "配置候选选择和翻页方式"},
        SettingsPageDefinition{"clipboard", "剪贴板", "管理 V+2 剪贴板功能和历史内容"},
        SettingsPageDefinition{"learning", "智能学习", "管理用户习惯学习和上下文排序"},
        SettingsPageDefinition{"dictionary", "用户词典", "维护个人词条、专业词和导入文件"},
        SettingsPageDefinition{"status", "输入法状态", "查看 Fcitx5 状态并重新加载 ModernIME"},
    };
}

constexpr auto settingsActionLabels() {
    return std::array<std::string_view, 4>{"应用", "保存并关闭", "恢复修改",
                                          "恢复默认"};
}

constexpr auto settingsClipboardActionLabels() {
    return std::array<std::string_view, 3>{"复制选中", "删除选中", "清空历史"};
}

constexpr auto settingsRuntimeStatusLabels() {
    return std::array<std::string_view, 4>{"fcitx5-remote", "Fcitx5 服务",
                                          "当前输入法", "ModernIME"};
}

} // namespace modernime::settings
