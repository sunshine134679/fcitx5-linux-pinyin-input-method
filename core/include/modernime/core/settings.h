#pragma once

#include "modernime/core/pinyin_options.h"
#include "modernime/core/candidate_pin.h"

#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

enum class InputMode { Chinese, English };
enum class UnderscoreStyle { Smart, Ascii, Chinese };

enum class CandidateTheme { Light, Dark, ClassicBlue, Jade, Amber, Rose, Lavender, Midnight, HighContrast };
enum class CandidateShape { Rounded, Square, Capsule };
enum class CandidateSpacing { Compact, Standard, Spacious };
struct ClientThemeOption { std::string_view key; std::string_view label; };
inline constexpr std::array<ClientThemeOption, 7> kClientThemes{{
    ClientThemeOption{"blue", "晴空蓝"}, {"jade", "青竹绿"}, {"night", "深海夜"},
    {"amber", "暖砂橙"}, {"rose", "玫瑰粉"}, {"lavender", "雾紫"}, {"contrast", "高对比"}}};
struct CandidateThemeOption { CandidateTheme value; std::string_view key; std::string_view label; };
struct CandidateShapeOption { CandidateShape value; std::string_view key; std::string_view label; };
struct CandidateSpacingOption { CandidateSpacing value; std::string_view key; std::string_view label; };
inline constexpr std::array kCandidateThemes{
    CandidateThemeOption{CandidateTheme::Light, "light", "浅色"},
    CandidateThemeOption{CandidateTheme::Dark, "dark", "深色"},
    CandidateThemeOption{CandidateTheme::ClassicBlue, "classic-blue", "经典蓝"},
    CandidateThemeOption{CandidateTheme::Jade, "jade", "青竹绿"},
    CandidateThemeOption{CandidateTheme::Amber, "amber", "暖砂橙"},
    CandidateThemeOption{CandidateTheme::Rose, "rose", "玫瑰粉"},
    CandidateThemeOption{CandidateTheme::Lavender, "lavender", "雾紫"},
    CandidateThemeOption{CandidateTheme::Midnight, "midnight", "深海夜"},
    CandidateThemeOption{CandidateTheme::HighContrast, "high-contrast", "高对比"}};
inline constexpr std::array kCandidateShapes{
    CandidateShapeOption{CandidateShape::Rounded, "rounded", "圆润"},
    CandidateShapeOption{CandidateShape::Square, "square", "方正"},
    CandidateShapeOption{CandidateShape::Capsule, "capsule", "胶囊"}};
inline constexpr std::array kCandidateSpacings{
    CandidateSpacingOption{CandidateSpacing::Compact, "compact", "紧凑"},
    CandidateSpacingOption{CandidateSpacing::Standard, "standard", "标准"},
    CandidateSpacingOption{CandidateSpacing::Spacious, "spacious", "宽松"}};

struct ModernIMESettings final {
    bool inputEnabled = true;
    InputMode defaultMode = InputMode::Chinese;
    std::string toggleKey = "Ctrl+Shift+Space";
    int candidatePageSize = 9;
    int candidateFontSize = 20;
    std::string candidateFontFamily = "Noto Sans CJK SC";
    CandidateTheme candidateTheme = CandidateTheme::Light;
    CandidateShape candidateShape = CandidateShape::Rounded;
    CandidateSpacing candidateSpacing = CandidateSpacing::Standard;
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
    std::string clientTheme = "blue";
    FuzzyPinyinOptions fuzzyPinyin;
    PinyinScheme pinyinScheme = PinyinScheme::Full;
    bool autoPairPunctuation = true;
    bool smartPunctuation = true;
    UnderscoreStyle underscoreStyle = UnderscoreStyle::Smart;
    // Printable ASCII punctuation key -> single-line literal UTF-8 text.
    std::map<char, std::string> punctuationOverrides;
    bool rememberModePerApp = false;
    std::map<std::string, InputMode> appInputModes;
    bool undoCommitEnabled = true;
    bool temporaryEnglishEnabled = true;
    bool componentLookupEnabled = true;
    bool calculatorEnabled = true;
    std::vector<CandidatePin> candidatePins;
    bool symbolSearchEnabled = true;
    std::set<std::string> symbolFavorites;

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

enum class SettingsSaveStatus {
    Saved,
    Conflict,
    Invalid,
    IoError,
};

struct SettingsSaveResult final {
    SettingsSaveStatus status = SettingsSaveStatus::Invalid;
    ModernIMESettings settings = defaultSettings();
    std::string contentHash;
    std::vector<std::string> rawLines;
    SettingsFileStamp stamp;
    std::string error;

    std::string currentDiskHash;
    ModernIMESettings currentDiskSettings = defaultSettings();
    std::vector<std::string> currentDiskRawLines;
};

enum class SettingsBaselineKind {
    Unchecked,
    ExpectedMissing,
    ExpectedContentHash
};

struct SettingsExpectedBaseline final {
    SettingsBaselineKind kind = SettingsBaselineKind::Unchecked;
    std::string contentHash;

    SettingsExpectedBaseline() = default;
    SettingsExpectedBaseline(SettingsBaselineKind k, std::string hash = {})
        : kind(k), contentHash(std::move(hash)) {}

    static SettingsExpectedBaseline unchecked() {
        return SettingsExpectedBaseline{SettingsBaselineKind::Unchecked, ""};
    }
    static SettingsExpectedBaseline expectedMissing() {
        return SettingsExpectedBaseline{SettingsBaselineKind::ExpectedMissing, ""};
    }
    static SettingsExpectedBaseline expectedContent(std::string hash) {
        if (hash.empty()) {
            return expectedMissing();
        }
        return SettingsExpectedBaseline{SettingsBaselineKind::ExpectedContentHash, std::move(hash)};
    }
    static SettingsExpectedBaseline fromLoadResult(const SettingsLoadResult &loaded) {
        if (loaded.isMissing()) {
            return expectedMissing();
        }
        if (loaded.isSuccess()) {
            return expectedContent(loaded.contentHash);
        }
        return unchecked();
    }

    SettingsExpectedBaseline(std::string_view hash) {
        if (hash.empty()) {
            kind = SettingsBaselineKind::ExpectedMissing;
        } else {
            kind = SettingsBaselineKind::ExpectedContentHash;
            contentHash = std::string(hash);
        }
    }
    SettingsExpectedBaseline(const char *hash)
        : SettingsExpectedBaseline(hash ? std::string_view(hash) : std::string_view{}) {}
    SettingsExpectedBaseline(const std::string &hash)
        : SettingsExpectedBaseline(std::string_view(hash)) {}
};

class SettingsStore final {
public:
    static SettingsLoadResult load(const std::filesystem::path &path);
    static SettingsSaveResult saveWithResult(const std::filesystem::path &path,
                                             const ModernIMESettings &settings,
                                             SettingsExpectedBaseline expectedBaseline = SettingsExpectedBaseline::unchecked(),
                                             const std::vector<std::string> *rawLinesToPreserve = nullptr);
    static bool save(const std::filesystem::path &path,
                     const ModernIMESettings &settings,
                     std::string *error = nullptr,
                     SettingsExpectedBaseline expectedBaseline = SettingsExpectedBaseline::unchecked(),
                     const std::vector<std::string> *rawLinesToPreserve = nullptr);
    static bool reset(const std::filesystem::path &path,
                      std::string *error = nullptr);
};

} // namespace modernime::core
