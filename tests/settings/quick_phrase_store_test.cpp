#include "modernime/settings/quick_phrase_store.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace modernime::settings;
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("modernime-phrases-" + std::to_string(getpid()));
    const auto path = root / "quickphrase.d/modernime.mb";
    auto snapshot = loadQuickPhrases(path);
    assert(snapshot.error.empty() && !snapshot.exists);
    const std::vector<QuickPhraseEntry> entries{{"yx", "hello@example.test"}, {"dz", " 地址  A栋 \\ 2楼 \"前台\" "}, {"hf", "收到，谢谢！"}};
    assert(saveQuickPhrases(path, snapshot, entries).saved());
    snapshot = loadQuickPhrases(path);
    assert(snapshot.error.empty() && snapshot.entries == entries);
    assert(filterQuickPhrases(snapshot.entries, "EXAMPLE") == std::vector<std::size_t>{0});
    assert(filterQuickPhrases(snapshot.entries, "地址") == std::vector<std::size_t>{1});
    assert(filterQuickPhrases(snapshot.entries, {}).size() == entries.size());
    auto changed = entries;
    changed[0].text = "new@example.test";
    assert(saveQuickPhrases(path, snapshot, changed).saved());
    assert(saveQuickPhrases(path, snapshot, {}).status == QuickPhraseSaveStatus::Conflict); // 保留外部更新
    snapshot = loadQuickPhrases(path);
    changed.erase(changed.begin() + 1);
    assert(saveQuickPhrases(path, snapshot, changed).saved());
    assert(loadQuickPhrases(path).entries == changed);
    assert(!quickPhraseValidation({"bad1", "文本"}).empty());
    assert(!quickPhraseValidation({"hf", "第一行\n第二行"}).empty());
    std::filesystem::remove_all(root);
}
