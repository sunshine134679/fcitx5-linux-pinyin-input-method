#include "modernime/core/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "settings test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testDirectory() {
    const auto path = std::filesystem::temp_directory_path() /
                      "modernime-settings-test";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    assertTrue(!error, "test directory is created");
    return path;
}

void testPaths() {
    const auto paths = modernime::core::SettingsPaths::fromEnvironment(
        "/tmp/cfg", "/tmp/data", "/tmp/home");
    assertTrue(paths.settingsFile == "/tmp/cfg/modernime/settings.conf",
               "settings path uses config home");
    assertTrue(paths.userDictionary ==
                   "/tmp/data/modernime/user-dictionary.txt",
               "dictionary path uses data home");
    assertTrue(paths.learningStore == "/tmp/data/modernime/learning.sqlite3",
               "learning path uses data home");
    assertTrue(paths.clipboardHistory ==
                   "/tmp/data/modernime/clipboard-history.bin",
               "clipboard history path uses data home");
}

void testDefaultsAndRoundTrip() {
    const auto directory = testDirectory();
    const auto path = directory / "settings.conf";
    const auto missing = modernime::core::SettingsStore::load(path);
    const auto defaults = modernime::core::defaultSettings();
    assertTrue(missing.settings.inputEnabled == defaults.inputEnabled,
               "missing file uses input default");
    assertTrue(missing.settings.defaultMode == defaults.defaultMode,
               "missing file uses mode default");
    assertTrue(missing.settings.toggleKey == defaults.toggleKey,
               "missing file uses toggle default");
    assertTrue(missing.settings.clipboardEnabled == defaults.clipboardEnabled,
               "missing file uses clipboard enabled default");
    assertTrue(missing.settings.clipboardTrigger == defaults.clipboardTrigger,
               "missing file uses clipboard trigger default");

    auto expected = defaults;
    expected.inputEnabled = false;
    expected.defaultMode = modernime::core::InputMode::English;
    expected.toggleKey = "Alt+Space";
    expected.numberSelection = false;
    expected.arrowNavigation = false;
    expected.pageNavigation = false;
    expected.learningEnabled = false;
    expected.contextLearningEnabled = false;
    expected.clipboardEnabled = false;
    expected.clipboardTrigger = "B+7";
    std::string error;
    assertTrue(modernime::core::SettingsStore::save(path, expected, &error),
               "settings save succeeds: " + error);

    const auto loaded = modernime::core::SettingsStore::load(path);
    assertTrue(loaded.settings.inputEnabled == expected.inputEnabled,
               "input flag round-trips");
    assertTrue(loaded.settings.defaultMode == expected.defaultMode,
               "mode round-trips");
    assertTrue(loaded.settings.toggleKey == expected.toggleKey,
               "toggle key round-trips");
    assertTrue(loaded.settings.numberSelection == expected.numberSelection &&
                   loaded.settings.arrowNavigation == expected.arrowNavigation &&
                   loaded.settings.pageNavigation == expected.pageNavigation,
               "candidate flags round-trip");
    assertTrue(loaded.settings.learningEnabled == expected.learningEnabled &&
                   loaded.settings.contextLearningEnabled ==
                       expected.contextLearningEnabled,
               "learning flags round-trip");
    assertTrue(loaded.settings.clipboardEnabled == expected.clipboardEnabled &&
                   loaded.settings.clipboardTrigger ==
                       expected.clipboardTrigger,
               "clipboard settings round-trip");
}

void testDiagnosticsAndPerKeyFallback() {
    const auto directory = testDirectory();
    const auto path = directory / "settings.conf";
    std::ofstream output(path);
    output << "input.enabled=not-a-bool\n"
           << "input.default_mode=english\n"
           << "input.toggle_key=\n"
           << "learning.enabled=false\n"
           << "clipboard.enabled=false\n"
           << "clipboard.trigger=bad-trigger\n"
           << "unknown.option=true\n";
    output.close();

    const auto loaded = modernime::core::SettingsStore::load(path);
    const auto defaults = modernime::core::defaultSettings();
    assertTrue(loaded.settings.inputEnabled == defaults.inputEnabled,
               "invalid boolean falls back independently");
    assertTrue(loaded.settings.defaultMode ==
                   modernime::core::InputMode::English,
               "valid mode remains applied");
    assertTrue(loaded.settings.toggleKey == defaults.toggleKey,
               "invalid key falls back independently");
    assertTrue(!loaded.settings.learningEnabled,
               "valid learning value remains applied");
    assertTrue(!loaded.settings.clipboardEnabled,
               "valid clipboard flag remains applied");
    assertTrue(loaded.settings.clipboardTrigger == defaults.clipboardTrigger,
               "invalid clipboard trigger falls back independently");
    assertTrue(loaded.diagnostics.size() >= 3,
               "invalid and unknown lines produce diagnostics");
}

void testFailedSavePreservesExistingTarget() {
    const auto directory = testDirectory();
    const auto blockedTarget = directory / "settings.conf";
    std::filesystem::create_directory(blockedTarget);
    const auto marker = blockedTarget / "marker";
    std::ofstream(marker) << "preserve";

    std::string error;
    assertTrue(!modernime::core::SettingsStore::save(
                   blockedTarget, modernime::core::defaultSettings(), &error),
               "save reports replacement failure");
    std::ifstream input(marker);
    std::string content;
    std::getline(input, content);
    assertTrue(content == "preserve", "failed save preserves target data");
}

void testSharedValidationAcceptsOnlyRuntimeToggleKeys() {
    for (const auto *toggleKey : {"Ctrl+Space", "Alt+Space", "Super+Space",
                                  "Ctrl+Shift+Space"}) {
        auto settings = modernime::core::defaultSettings();
        settings.toggleKey = toggleKey;
        const auto validation = modernime::core::validateSettings(settings);
        assertTrue(validation.valid,
                   std::string("runtime toggle key is accepted: ") + toggleKey);
    }

    auto settings = modernime::core::defaultSettings();
    settings.toggleKey = "Ctrl+A";
    const auto unsupported = modernime::core::validateSettings(settings);
    assertTrue(!unsupported.valid, "unsupported runtime toggle key is rejected");
    assertTrue(unsupported.issues.size() == 1,
               "unsupported toggle key produces one validation issue");
    assertTrue(unsupported.issues.front().key == "input.toggle_key",
               "unsupported toggle key identifies its settings field");
    assertTrue(unsupported.issues.front().message.find("仅支持") !=
                   std::string::npos,
               "unsupported toggle key explains the supported choices");
}

void testSharedValidationRejectsInvalidValues() {
    auto settings = modernime::core::defaultSettings();
    settings.toggleKey = "Ctrl Space";
    const auto invalidToggle =
        modernime::core::validateSettings(settings);
    assertTrue(!invalidToggle.valid, "invalid toggle key is rejected");
    assertTrue(invalidToggle.issues.size() == 1,
               "toggle key validation explains the error");
    assertTrue(invalidToggle.issues.front().key == "input.toggle_key",
               "toggle key validation identifies its settings field");

    settings = modernime::core::defaultSettings();
    settings.clipboardTrigger = "bad-trigger";
    const auto invalidClipboard =
        modernime::core::validateSettings(settings);
    assertTrue(!invalidClipboard.valid,
               "invalid clipboard trigger is rejected");
    assertTrue(invalidClipboard.issues.size() == 1,
               "clipboard validation explains the error");
    assertTrue(invalidClipboard.issues.front().key == "clipboard.trigger",
               "clipboard validation identifies its settings field");
}

} // namespace

int main() {
    testPaths();
    testDefaultsAndRoundTrip();
    testDiagnosticsAndPerKeyFallback();
    testFailedSavePreservesExistingTarget();
    testSharedValidationAcceptsOnlyRuntimeToggleKeys();
    testSharedValidationRejectsInvalidValues();
    return EXIT_SUCCESS;
}
