#include "modernime/settings/page_registry.h"

#include <cassert>

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
}
