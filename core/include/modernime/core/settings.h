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
    std::string toggleKey = "Ctrl+Shift+Space";
    int candidatePageSize = 9;
    int candidateFontSize = 20;
    bool numberSelection = true;
    bool arrowNavigation = true;
    bool pageNavigation = true;
    bool punctuationEnabled = true;
    bool learningEnabled = true;
    bool contextLearningEnabled = true;
    bool clipboardEnabled = true;
    std::string clipboardTrigger = "V+2";
    bool englishDefinitionEnabled = false;
    bool characterExtractionEnabled = true;

    bool operator==(const ModernIMESettings &) const = default;
};

struct SettingsFileStamp final {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtimeSec = 0;
    std::int64_t mtimeNsec = 0;
    std::int64_t ctimeSec = 0;
    std::int64_t ctimeNsec = 0;
    bool exists = false;

    static SettingsFileStamp fromPath(const std::filesystem::path &path);
    bool operator==(const SettingsFileStamp &) const = default;
};

struct SettingsApplicationVersion final {
    std::string contentHash;
    std::uint64_t generation = 0;

    bool operator==(const SettingsApplicationVersion &) const = default;
};

struct SettingsValidationIssue final {
    std::string key;
    std::string message;
};

struct SettingsValidationResult final {
    bool valid = true;
    std::vector<SettingsValidationIssue> issues;
};

inline constexpr int kMinimumCandidatePageSize = 3;
inline constexpr int kMaximumCandidatePageSize = 9;
inline constexpr int kDefaultCandidatePageSize = 9;

inline constexpr int kMinimumCandidateFontSize = 14;
inline constexpr int kMaximumCandidateFontSize = 28;
inline constexpr int kDefaultCandidateFontSize = 20;

ModernIMESettings defaultSettings();
SettingsValidationResult validateSettings(const ModernIMESettings &settings);
std::string computeSettingsHash(const ModernIMESettings &settings);
std::string computeFileContentHash(const std::filesystem::path &path);

struct SettingsPaths final {
    std::filesystem::path settingsFile;
    std::filesystem::path userDictionary;
    std::filesystem::path learningStore;
    std::filesystem::path clipboardHistory;
    std::filesystem::path runtimeState;

    static SettingsPaths fromEnvironment(std::string_view xdgConfigHome,
                                         std::string_view xdgDataHome,
                                         std::string_view home);
};

enum class SettingsLoadStatus {
    Success,
    Missing,
    IoError
};

struct SettingsLoadResult final {
    ModernIMESettings settings = defaultSettings();
    std::vector<std::string> diagnostics;
    std::string contentHash;
    std::vector<std::string> rawLines;
    SettingsFileStamp stamp;
    SettingsLoadStatus status = SettingsLoadStatus::Missing;

    bool isSuccess() const noexcept { return status == SettingsLoadStatus::Success; }
    bool isMissing() const noexcept { return status == SettingsLoadStatus::Missing; }
    bool isIoError() const noexcept { return status == SettingsLoadStatus::IoError; }
};

class SettingsStore final {
public:
    static SettingsLoadResult load(const std::filesystem::path &path);
    static bool save(const std::filesystem::path &path,
                     const ModernIMESettings &settings,
                     std::string *error = nullptr,
                     std::string_view expectedContentHash = {},
                     const std::vector<std::string> *rawLinesToPreserve = nullptr);
    static bool reset(const std::filesystem::path &path,
                      std::string *error = nullptr);
};

} // namespace modernime::core
