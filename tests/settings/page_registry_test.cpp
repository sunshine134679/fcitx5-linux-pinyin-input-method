#include "modernime/settings/page_registry.h"

#include <cassert>
#include <array>
#include <string_view>

int main() {
    using namespace modernime::settings;

    const auto pages = settingsPageDefinitions();
    assert(pages.size() == 8);
    assert(pages.front().id == SettingsPageId::Overview);
    assert(pages[1].id == SettingsPageId::Input);
    assert(pages[2].id == SettingsPageId::Candidates);
    assert(pages[6].id == SettingsPageId::Updates);
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
        std::string_view("概览"), std::string_view("输入与按键"),
        std::string_view("候选与外观"),
        std::string_view("个人词典"), std::string_view("剪贴板"),
        std::string_view("智能学习"), std::string_view("更新与安装"),
        std::string_view("系统与诊断")};
    for (std::size_t index = 0; index < pageTitles.size(); ++index) {
        const auto matches = searchSettings(pageTitles[index]);
        assert(!matches.empty());
        assert(matches.front().page == pages[index].id);
        assert(matches.front().title == pageTitles[index]);
    }

    for (const auto *query : {"英文", "释义", "翻译"}) {
        const auto matches = searchSettings(query);
        assert(!matches.empty());
        bool found = false;
        for (const auto &item : matches) {
            if (item.page == SettingsPageId::Candidates && item.target == "english-definition") {
                found = true;
                break;
            }
        }
        assert(found);
    }

    for (const auto *query : {"检查更新", "apt", "dnf"}) {
        const auto matches = searchSettings(query);
        assert(!matches.empty());
        bool found = false;
        for (const auto &item : matches) {
            if (item.page == SettingsPageId::Updates &&
                item.target == "updates-check") {
                found = true;
                break;
            }
        }
        assert(found);
    }

    // Verify parseSettingsPage for all 8 canonical page names
    assert(parseSettingsPage("overview") == SettingsPageId::Overview);
    assert(parseSettingsPage("input") == SettingsPageId::Input);
    assert(parseSettingsPage("candidates") == SettingsPageId::Candidates);
    assert(parseSettingsPage("dictionary") == SettingsPageId::Dictionary);
    assert(parseSettingsPage("clipboard") == SettingsPageId::Clipboard);
    assert(parseSettingsPage("learning") == SettingsPageId::Learning);
    assert(parseSettingsPage("updates") == SettingsPageId::Updates);
    assert(parseSettingsPage("diagnostics") == SettingsPageId::Diagnostics);

    // Verify CLI and historical aliases
    assert(parseSettingsPage("home") == SettingsPageId::Overview);
    assert(parseSettingsPage("basic") == SettingsPageId::Input);
    assert(parseSettingsPage("candidate") == SettingsPageId::Candidates);
    assert(parseSettingsPage("appearance") == SettingsPageId::Candidates);
    assert(parseSettingsPage("dict") == SettingsPageId::Dictionary);
    assert(parseSettingsPage("clip") == SettingsPageId::Clipboard);
    assert(parseSettingsPage("update") == SettingsPageId::Updates);
    assert(parseSettingsPage("upgrade") == SettingsPageId::Updates);
    assert(parseSettingsPage("diagnostic") == SettingsPageId::Diagnostics);
    assert(parseSettingsPage("status") == SettingsPageId::Diagnostics);
    assert(parseSettingsPage("diag") == SettingsPageId::Diagnostics);
    assert(parseSettingsPage("system") == SettingsPageId::Diagnostics);

    // Verify case insensitivity and whitespace trimming
    assert(parseSettingsPage("  Overview  ") == SettingsPageId::Overview);
    assert(parseSettingsPage("INPUT") == SettingsPageId::Input);
    assert(parseSettingsPage("Candidates") == SettingsPageId::Candidates);
    assert(parseSettingsPage("  CANDIDATE  ") == SettingsPageId::Candidates);
    assert(parseSettingsPage("\tdict\n") == SettingsPageId::Dictionary);
    assert(parseSettingsPage("  CLIPBOARD  ") == SettingsPageId::Clipboard);
    assert(parseSettingsPage("Learning") == SettingsPageId::Learning);
    assert(parseSettingsPage("  UpDaTeS  ") == SettingsPageId::Updates);
    assert(parseSettingsPage("STATUS") == SettingsPageId::Diagnostics);

    // Verify all 8 canonical Chinese page titles
    assert(parseSettingsPage("概览") == SettingsPageId::Overview);
    assert(parseSettingsPage("输入与按键") == SettingsPageId::Input);
    assert(parseSettingsPage("候选与外观") == SettingsPageId::Candidates);
    assert(parseSettingsPage("个人词典") == SettingsPageId::Dictionary);
    assert(parseSettingsPage("剪贴板") == SettingsPageId::Clipboard);
    assert(parseSettingsPage("智能学习") == SettingsPageId::Learning);
    assert(parseSettingsPage("更新与安装") == SettingsPageId::Updates);
    assert(parseSettingsPage("系统与诊断") == SettingsPageId::Diagnostics);
    assert(parseSettingsPage("  系统与诊断  ") == SettingsPageId::Diagnostics);

    // Verify unknown and invalid page names return std::nullopt (explicit diagnostic)
    assert(!parseSettingsPage("").has_value());
    assert(!parseSettingsPage("   ").has_value());
    assert(!parseSettingsPage("unknown").has_value());
    assert(!parseSettingsPage("nonexistent").has_value());
    assert(!parseSettingsPage("settings").has_value());
    assert(!parseSettingsPage("window").has_value());
    assert(!parseSettingsPage("foo_bar").has_value());
}
