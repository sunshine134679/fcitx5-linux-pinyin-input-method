#include "modernime/core/clipboard_history.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/settings.h"
#include "modernime/pinyin/user_dictionary.h"
#include "modernime/settings/overview_model.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "overview model test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testDirectory(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("modernime-overview-model-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    assertTrue(!error, "test directory is created");
    return path;
}

void testCollectsOverviewFromPersistedData() {
    const auto directory = testDirectory("snapshot");
    modernime::core::SettingsPaths paths;
    paths.settingsFile = directory / "settings.conf";
    paths.userDictionary = directory / "user-dictionary.txt";
    paths.learningStore = directory / "learning.sqlite3";
    paths.clipboardHistory = directory / "clipboard-history.bin";

    modernime::pinyin::UserDictionary dictionary;
    assertTrue(dictionary.upsert("ni'hao", "你好", 100.0F),
               "first dictionary fixture entry is valid");
    assertTrue(dictionary.upsert("shi'jie", "世界", 90.0F),
               "second dictionary fixture entry is valid");
    assertTrue(dictionary.saveText(paths.userDictionary),
               "dictionary fixture saves");

    modernime::core::ClipboardHistory clipboard;
    assertTrue(clipboard.observe("clipboard fixture"),
               "clipboard fixture is observed");
    std::string error;
    assertTrue(clipboard.save(paths.clipboardHistory, &error),
               "clipboard fixture saves: " + error);

    modernime::core::LearningStore learning(paths.learningStore);
    assertTrue(learning.open(), "learning fixture opens");
    assertTrue(learning.recordSelection("你好", "nihao", {}, {}, 1000),
               "learning fixture records a selection");
    learning.close();

    modernime::settings::RuntimeStatus runtime;
    runtime.available = true;
    runtime.running = true;
    runtime.modernimeAvailable = true;
    runtime.modernimeActive = true;

    const auto snapshot = modernime::settings::collectOverviewSnapshot(
        paths, modernime::core::defaultSettings(), runtime);
    assertTrue(snapshot.runtimeSummary == "ModernIME 正在运行",
               "active runtime summary is reported");
    assertTrue(snapshot.defaultMode == "中文",
               "default input mode is localized");
    assertTrue(snapshot.toggleKey == "Ctrl+Space",
               "toggle shortcut is copied from settings");
    assertTrue(snapshot.dictionaryEntries == 2,
               "dictionary entry count is collected");
    assertTrue(snapshot.clipboardEntries == 1,
               "clipboard entry count is collected");
    assertTrue(snapshot.learningEntries == 1,
               "learning entry count is collected");
    assertTrue(snapshot.notices.empty(),
               "healthy persisted data produces no notices");
}

void testUnreadablePathsProduceDiagnosticsNotice() {
    const auto directory = testDirectory("unreadable");
    modernime::core::SettingsPaths paths;
    paths.userDictionary = directory;
    paths.learningStore = directory;
    paths.clipboardHistory = directory;

    modernime::settings::RuntimeStatus runtime;
    runtime.available = true;
    runtime.running = true;
    runtime.modernimeAvailable = true;
    runtime.modernimeActive = true;

    const auto snapshot = modernime::settings::collectOverviewSnapshot(
        paths, modernime::core::defaultSettings(), runtime);
    assertTrue(!snapshot.notices.empty(),
               "unreadable data paths produce a notice");
    bool hasDiagnosticsNotice = false;
    for (const auto &notice : snapshot.notices) {
        if (notice.destination ==
            modernime::settings::SettingsPageId::Diagnostics) {
            hasDiagnosticsNotice = true;
            break;
        }
    }
    assertTrue(hasDiagnosticsNotice,
               "unreadable data notice links to diagnostics");
}

} // namespace

int main() {
    testCollectsOverviewFromPersistedData();
    testUnreadablePathsProduceDiagnosticsNotice();
    return EXIT_SUCCESS;
}
