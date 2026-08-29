#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

enum class InputMode { Chinese, English };

struct ModernIMESettings final {
    bool inputEnabled = true;
    InputMode defaultMode = InputMode::Chinese;
    std::string toggleKey = "Ctrl+Space";
    bool numberSelection = true;
    bool arrowNavigation = true;
    bool pageNavigation = true;
    bool punctuationEnabled = true;
    bool learningEnabled = true;
    bool contextLearningEnabled = true;
    bool clipboardEnabled = true;
    std::string clipboardTrigger = "V+2";

    bool operator==(const ModernIMESettings &) const = default;
};

struct SettingsValidationResult final {
    bool valid = true;
    std::vector<std::string> errors;
};

ModernIMESettings defaultSettings();
SettingsValidationResult validateSettings(const ModernIMESettings &settings);

struct SettingsPaths final {
    std::filesystem::path settingsFile;
    std::filesystem::path userDictionary;
    std::filesystem::path learningStore;
    std::filesystem::path clipboardHistory;

    static SettingsPaths fromEnvironment(std::string_view xdgConfigHome,
                                         std::string_view xdgDataHome,
                                         std::string_view home);
};

struct SettingsLoadResult final {
    ModernIMESettings settings = defaultSettings();
    std::vector<std::string> diagnostics;
};

class SettingsStore final {
public:
    static SettingsLoadResult load(const std::filesystem::path &path);
    static bool save(const std::filesystem::path &path,
                     const ModernIMESettings &settings,
                     std::string *error = nullptr);
    static bool reset(const std::filesystem::path &path,
                      std::string *error = nullptr);
};

} // namespace modernime::core
