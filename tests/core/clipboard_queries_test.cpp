#include "modernime/core/clipboard_queries.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "clipboard queries test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testSearch() {
    const std::vector<std::string> history{
        "Alpha 中文", "普通记录", "重复收藏"};
    const std::vector<std::string> favorites{"重复收藏", "Pinned alpha"};
    auto matches = modernime::core::searchClipboard(history, favorites, "");
    expect(matches.size() == 4 && matches[0].text == "重复收藏" &&
               matches[1].text == "Pinned alpha" &&
               matches[2].text == "Alpha 中文" && matches[3].text == "普通记录",
           "favorites lead and exact duplicate texts appear once");
    expect(matches[0].favorite && matches[0].historyIndex == 2 &&
               !matches[1].historyIndex && !matches[2].favorite &&
               matches[2].historyIndex == 0,
           "results retain favorite status and original history positions");
    expect(matches[0].text.data() == favorites[0].data() &&
               matches[2].text.data() == history[0].data(),
           "search results borrow full texts without copying");

    matches = modernime::core::searchClipboard(history, favorites, "aLPHa");
    expect(matches.size() == 2 && matches[0].text == "Pinned alpha" &&
               matches[1].text == "Alpha 中文",
           "ASCII search ignores case");
    matches = modernime::core::searchClipboard(history, favorites, "中文");
    expect(matches.size() == 1 && matches.front().historyIndex == 0,
           "Chinese substring search matches exact UTF-8 text");
    matches = modernime::core::searchClipboard(history, favorites, "", true);
    expect(matches.size() == 2 && matches[0].favorite && matches[1].favorite,
           "favorites-only search excludes history-only texts");
    matches = modernime::core::searchClipboard(history, favorites, "", false, 1);
    expect(matches.size() == 1 && matches.front().text == "重复收藏",
           "result limit preserves favorite priority");
    expect(history.front() == "Alpha 中文" && favorites.front() == "重复收藏",
           "search preserves input order and content");
}

void testIndependentPersistence(const std::filesystem::path &directory) {
    using modernime::core::ClipboardFavorites;
    using modernime::core::ClipboardHistory;
    const auto historyPath = directory / "history.bin";
    const auto favoritesPath = directory / "favorites.bin";
    ClipboardHistory history;
    history.observe("收藏文本");
    history.observe("历史文本");
    std::string error;
    expect(history.save(historyPath, &error), "history fixture saves");
    ClipboardFavorites favorites(favoritesPath);
    expect(favorites.load(&error) && favorites.entries().empty(),
           "missing favorites file loads as empty");
    expect(favorites.add("收藏文本", &error), "history text can be favorited");
    expect(history.remove(1) && history.save(historyPath, &error) &&
               favorites.entries().front() == "收藏文本",
           "removing history leaves its favorite intact");

    expect(favorites.add("历史文本", &error), "another history text is favorited");
    const auto favoriteOrder = favorites.entries();
    expect(favorites.add("收藏文本", &error) && error.empty() &&
               favorites.entries() == favoriteOrder,
           "duplicate favorite succeeds without reordering");
    expect(favorites.remove("历史文本", &error) &&
               history.entries() == std::vector<std::string>{"历史文本"},
           "removing a favorite leaves the history unchanged");
    expect(!favorites.remove("不存在", &error) && !error.empty() &&
               favorites.entries().front() == "收藏文本",
           "stale full-text removal reports a missing identity");

    history.clear();
    expect(history.save(historyPath, &error), "history clears independently");
    ClipboardFavorites reopened(favoritesPath);
    expect(reopened.load(&error) &&
               reopened.entries() == std::vector<std::string>{"收藏文本"},
           "favorite survives history clear and favorites reopen");
    history.observe("清空收藏后保留历史");
    expect(history.save(historyPath, &error) && reopened.clear(&error),
           "favorites can clear independently");
    ClipboardHistory reloadedHistory;
    ClipboardFavorites cleared(favoritesPath);
    expect(cleared.load(&error) && cleared.entries().empty() &&
               reloadedHistory.load(historyPath, &error) &&
               reloadedHistory.entries() == history.entries(),
           "favorites clear persists without touching the history file");
}

void testCapacityAndValidation(const std::filesystem::path &directory) {
    using modernime::core::ClipboardFavorites;
    using modernime::core::ClipboardHistory;
    const auto path = directory / "full-favorites.bin";
    ClipboardFavorites favorites(path);
    std::string error;
    expect(!favorites.add("", &error) && !error.empty(),
           "empty favorites follow the history validation rule");
    expect(!favorites.add(std::string(ClipboardHistory::kMaxEntryBytes + 1, 'x'),
                          &error) && !error.empty(),
           "favorites reuse the history text size limit");
    for (std::size_t i = 0; i < ClipboardHistory::kMaxEntries; ++i) {
        expect(favorites.add("favorite-" + std::to_string(i), &error),
               "favorites fill to the shared capacity");
    }
    const auto full = favorites.entries();
    expect(favorites.add("favorite-0", &error) && error.empty() &&
               favorites.entries() == full,
           "duplicate remains successful and stable at full capacity");
    expect(!favorites.add("one too many", &error) && !error.empty() &&
               favorites.entries() == full,
           "full favorites reject a new text without evicting anything");
    ClipboardFavorites reopened(path);
    expect(reopened.load(&error) && reopened.entries() == full,
           "capacity rejection also preserves the persisted entries");
}

void testFileFailureProtection(const std::filesystem::path &directory) {
    using modernime::core::ClipboardFavorites;
    const auto path = directory / "protected-favorites.bin";
    ClipboardFavorites favorites(path);
    std::string error;
    expect(favorites.add("保留的收藏", &error), "protected favorite saves");
    const auto visible = favorites.entries();
    std::filesystem::create_directory(path.string() + ".tmp");
    expect(!favorites.add("写入失败", &error) && !error.empty() &&
               favorites.entries() == visible,
           "failed atomic save preserves the visible favorites");
    ClipboardFavorites reopened(path);
    expect(reopened.load(&error) && reopened.entries() == visible,
           "failed atomic save preserves the previous file");
    std::filesystem::remove(path.string() + ".tmp");

    const std::string corrupt = "unreadable favorite data";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << corrupt;
    }
    expect(!favorites.load(&error) && !error.empty() &&
               favorites.entries() == visible,
           "failed read retains previously visible favorites");
    expect(!favorites.add("新内容", &error) && !error.empty() &&
               !favorites.remove("保留的收藏", &error) && !error.empty() &&
               !favorites.clear(&error) && !error.empty() &&
               favorites.entries() == visible,
           "mutations cannot replace a file that failed to load");
    std::ifstream input(path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    expect(after == corrupt, "failed file contents are preserved for recovery");
}

} // namespace

int main() {
    testSearch();
    char temporary[] = "/tmp/modernime-clipboard-queries-XXXXXX";
    const char *created = ::mkdtemp(temporary);
    expect(created != nullptr, "isolated test directory is created");
    const std::filesystem::path directory(created);
    testIndependentPersistence(directory);
    testCapacityAndValidation(directory);
    testFileFailureProtection(directory);
    std::filesystem::remove_all(directory);
    std::cout << "clipboard queries tests passed\n";
    return EXIT_SUCCESS;
}
