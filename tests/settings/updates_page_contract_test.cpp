#include "modernime/settings/pages/updates_page.h"
#include "modernime/settings/page_registry.h"

#include <cassert>
#include <type_traits>

int main() {
    using namespace modernime::settings;

    // 1. Static type contracts
    static_assert(UpdatesPage::pageId == SettingsPageId::Updates);
    static_assert(!std::is_copy_constructible_v<UpdatesPage>);
    static_assert(!std::is_copy_assignable_v<UpdatesPage>);

    // 2. Definition check
    const auto pages = settingsPageDefinitions();
    bool foundUpdatesPage = false;
    for (const auto &def : pages) {
        if (def.id == SettingsPageId::Updates) {
            assert(def.name == "updates");
            assert(def.title == "更新与安装");
            assert(def.group == "系统与支持");
            foundUpdatesPage = true;
            break;
        }
    }
    assert(foundUpdatesPage);

    // 3. Search routes to Updates page
    for (const auto *query : {"检查更新", "apt", "dnf", "升级"}) {
        const auto matches = searchSettings(query);
        assert(!matches.empty());
        bool found = false;
        for (const auto &item : matches) {
            if (item.page == SettingsPageId::Updates) {
                found = true;
                break;
            }
        }
        assert(found);
    }

    const auto checkMatches = searchSettings("检查更新");
    assert(!checkMatches.empty());
    assert(checkMatches.front().page == SettingsPageId::Updates);
    assert(checkMatches.front().target == "updates-check");

    const auto cacheMatches = searchSettings("刷新软件源缓存");
    assert(!cacheMatches.empty());
    assert(cacheMatches.front().page == SettingsPageId::Updates);
    assert(cacheMatches.front().target == "updates-refresh-cache");

    const auto cmdMatches = searchSettings("复制升级命令");
    assert(!cmdMatches.empty());
    assert(cmdMatches.front().page == SettingsPageId::Updates);
    assert(cmdMatches.front().target == "updates-copy-command");

    return 0;
}
