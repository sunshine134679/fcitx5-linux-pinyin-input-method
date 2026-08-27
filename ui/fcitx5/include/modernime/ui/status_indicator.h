#pragma once

#include <string_view>

namespace modernime::ui {

struct StatusIndicator final {
    static constexpr std::string_view iconName() {
        return "input-keyboard-symbolic";
    }

    static constexpr std::string_view title() { return "ModernIME 拼音"; }
};

} // namespace modernime::ui
