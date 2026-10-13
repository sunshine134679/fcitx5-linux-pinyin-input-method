#include "modernime/settings/pages/learning_page.h"
#include "modernime/settings/data_controller.h"
#include "modernime/settings/page_registry.h"

#include <cassert>
#include <filesystem>
#include <type_traits>

int main() {
    using namespace modernime::settings;

    // 1. Static contracts
    static_assert(LearningPage::pageId == SettingsPageId::Learning);
    static_assert(!std::is_copy_constructible_v<LearningPage>);
    static_assert(!std::is_copy_assignable_v<LearningPage>);

    // 2. Active backend detection
    const auto sqliteBackend = DataController::detectActiveLearningBackend("learning.sqlite3");
    assert(sqliteBackend.kind == LearningBackendKind::Sqlite);
    assert(sqliteBackend.name == "SQLite WAL");
    assert(!sqliteBackend.description.empty());

    const auto lmdbBackend = DataController::detectActiveLearningBackend("learning.lmdb");
    assert(lmdbBackend.kind == LearningBackendKind::Lmdb);
    assert(lmdbBackend.name == "LMDB");
    assert(!lmdbBackend.description.empty());

    // 3. Search registry entries
    for (const auto *query : {"智能学习", "记忆用户候选选择", "上下文学习", "清空学习记录"}) {
        const auto matches = searchSettings(query);
        assert(!matches.empty());
        bool found = false;
        for (const auto &item : matches) {
            if (item.page == SettingsPageId::Learning) {
                found = true;
                break;
            }
        }
        assert(found);
    }

    const auto clearMatch = searchSettings("清空学习记录");
    assert(!clearMatch.empty());
    assert(clearMatch.front().page == SettingsPageId::Learning);
    assert(clearMatch.front().target == "learning-clear");

    // 4. Safe read-only entry count on nonexistent path
    std::string error;
    const std::filesystem::path missingPath = "/tmp/modernime-nonexistent-learning-test.sqlite3";
    std::error_code ec;
    std::filesystem::remove(missingPath, ec);
    assert(DataController::learningEntryCount(missingPath, &error) == 0);
    assert(!std::filesystem::exists(missingPath));

    return 0;
}

