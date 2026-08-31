#include "modernime/settings/page_registry.h"

#include <cassert>
#include <array>
#include <string_view>

int main() {
    using namespace modernime::settings;

    const auto pages = settingsPageDefinitions();
    assert(pages.size() == 6);
    assert(pages.front().id == SettingsPageId::Overview);
    assert(pages[1].id == SettingsPageId::Input);
    assert(pages.back().id == SettingsPageId::Diagnostics);

    const auto shortcut = searchSettings("快捷键");
    assert(!shortcut.empty());
    assert(shortcut.front().page == SettingsPageId::Input);
    assert(shortcut.front().target == "toggle-key");

    const auto alias = searchSettings("fcitx");
    assert(!alias.empty());
    assert(alias.front().page == SettingsPageId::Diagnostics);
    assert(searchSettings("   ").empty());

    constexpr std::array pageTitles{
        std::string_view("概览"), std::string_view("输入体验"),
        std::string_view("个人词典"), std::string_view("剪贴板"),
        std::string_view("智能学习"), std::string_view("系统与诊断")};
    for (std::size_t index = 0; index < pageTitles.size(); ++index) {
        const auto matches = searchSettings(pageTitles[index]);
        assert(!matches.empty());
        assert(matches.front().page == pages[index].id);
        assert(matches.front().title == pageTitles[index]);
    }
}
