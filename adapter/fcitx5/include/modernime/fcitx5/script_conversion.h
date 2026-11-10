#pragma once

#include <fcitx-config/rawconfig.h>
#include <string_view>
#include <vector>
#include <string>
#include <algorithm>

namespace modernime::fcitx5 {
inline constexpr std::string_view kChineseScriptConfigUri = "fcitx://config/addon/chttrans";

// The Fcitx addon is the sole owner of conversion state. These helpers edit a
// freshly fetched RawConfig; clients submit it through Controller1.SetConfig.
inline bool traditionalOutput(const fcitx::RawConfig &config) {
    const auto enabled = config.get("EnabledIM");
    if (!enabled) return false;
    for (const auto &key : enabled->subItems())
        if (enabled->get(key)->value() == "modernime") return true;
    return false;
}

inline void setTraditionalOutput(fcitx::RawConfig &config, bool traditional) {
    std::vector<std::string> methods;
    if (const auto enabled = config.get("EnabledIM"))
        for (const auto &key : enabled->subItems()) {
            const auto &value = enabled->get(key)->value();
            if (value != "modernime") methods.push_back(value);
        }
    if (traditional) methods.emplace_back("modernime");
    config.remove("EnabledIM");
    auto &enabled = config["EnabledIM"];
    for (std::size_t i = 0; i < methods.size(); ++i) enabled[std::to_string(i)] = methods[i];
}
} // namespace modernime::fcitx5
