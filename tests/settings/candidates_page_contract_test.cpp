#include "modernime/settings/pages/candidates_page.h"
#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_shell_state.h"

#include <cassert>
#include <string_view>
#include <type_traits>

int main() {
    using namespace modernime::settings;

    // 1. Static type contracts
    static_assert(CandidatesPage::pageId == SettingsPageId::Candidates);
    static_assert(!std::is_copy_constructible_v<CandidatesPage>);
    static_assert(!std::is_copy_assignable_v<CandidatesPage>);

    // 2. Definition check
    const auto pages = settingsPageDefinitions();
    bool foundCandidatesPage = false;
    for (const auto &def : pages) {
        if (def.id == SettingsPageId::Candidates) {
            assert(def.name == "candidates");
            assert(def.title == "候选与外观");
            foundCandidatesPage = true;
            break;
        }
    }
    assert(foundCandidatesPage);

    // 3. Focus route for candidate validation issues
    const auto pageSizeRoute = settingsFocusRouteForIssue("candidate.page_size");
    assert(pageSizeRoute.has_value());
    assert(pageSizeRoute->page == SettingsPageId::Candidates);
    assert(pageSizeRoute->target == "candidate-page-size");

    const auto fontSizeRoute = settingsFocusRouteForIssue("candidate.font_size");
    assert(fontSizeRoute.has_value());
    assert(fontSizeRoute->page == SettingsPageId::Candidates);
    assert(fontSizeRoute->target == "candidate-font-size");

    // 4. Search integration for candidate items
    const auto searchPageSize = searchSettings("每页候选词数");
    assert(!searchPageSize.empty());
    assert(searchPageSize.front().page == SettingsPageId::Candidates);
    assert(searchPageSize.front().target == "candidate-page-size");

    const auto searchFontSize = searchSettings("字号");
    assert(!searchFontSize.empty());
    bool foundFontSize = false;
    for (const auto &item : searchFontSize) {
        if (item.page == SettingsPageId::Candidates && item.target == "candidate-font-size") {
            foundFontSize = true;
            break;
        }
    }
    assert(foundFontSize);

    // English definition moved to Candidates page
    const auto searchDefinition = searchSettings("英文释义");
    assert(!searchDefinition.empty());
    bool foundDef = false;
    for (const auto &item : searchDefinition) {
        if (item.page == SettingsPageId::Candidates && item.target == "english-definition") {
            foundDef = true;
            break;
        }
    }
    assert(foundDef);

    // Character extraction remains in Input page
    const auto searchCharExtraction = searchSettings("以词定字");
    assert(!searchCharExtraction.empty());
    assert(searchCharExtraction.front().page == SettingsPageId::Input);
    assert(searchCharExtraction.front().target == "character-extraction");

    return 0;
}
