#pragma once

#include <string_view>

namespace modernime::ui {

struct StatusIndicator final {
    static constexpr std::string_view iconName() {
        return "input-keyboard-symbolic";
    }

    static constexpr std::string_view title() { return "ModernIME 拼音"; }

    static constexpr std::string_view labelForInputMethod(
        std::string_view inputMethod) {
        return inputMethod == "modernime" ? "中" : "英";
    }

    static constexpr std::string_view titleForInputMethod(
        std::string_view inputMethod) {
        return inputMethod == "modernime" ? "中文输入法" : "英文键盘";
    }

    static constexpr std::string_view promptForInputMethod(
        std::string_view inputMethod) {
        return inputMethod == "modernime" ? "中文" : "英文";
    }
};

} // namespace modernime::ui
