#include "modernime/settings/pages/dictionary_page.h"
#include "modernime/settings/data_controller.h"
#include "modernime/settings/page_registry.h"

#include <cassert>
#include <type_traits>

int main() {
    using namespace modernime::settings;

    // 1. Static type contracts
    static_assert(DictionaryPage::pageId == SettingsPageId::Dictionary);
    static_assert(!std::is_copy_constructible_v<DictionaryPage>);
    static_assert(!std::is_copy_assignable_v<DictionaryPage>);

    // 2. Policy enum coverage
    static_assert(DictionaryConflictPolicy::KeepExisting != DictionaryConflictPolicy::OverwriteWithImport);

    // 3. Search registry entries
    for (const auto *query : {"个人词典", "添加词条", "编辑词条", "删除词条", "导入词典", "导出词典"}) {
        const auto matches = searchSettings(query);
        assert(!matches.empty());
        bool found = false;
        for (const auto &item : matches) {
            if (item.page == SettingsPageId::Dictionary) {
                found = true;
                break;
            }
        }
        assert(found);
    }

    const auto importMatch = searchSettings("导入词典");
    assert(!importMatch.empty());
    assert(importMatch.front().page == SettingsPageId::Dictionary);
    assert(importMatch.front().target == "dictionary-import");

    const auto exportMatch = searchSettings("导出词典");
    assert(!exportMatch.empty());
    assert(exportMatch.front().page == SettingsPageId::Dictionary);
    assert(exportMatch.front().target == "dictionary-export");

    return 0;
}
