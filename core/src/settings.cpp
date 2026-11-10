#include "modernime/core/settings.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/core/symbol_catalog.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace modernime::core {

SettingsFileStamp SettingsFileStamp::fromPath(const std::filesystem::path &path) {
    SettingsFileStamp stamp;
    if (path.empty()) {
        return stamp;
    }
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) {
        stamp.device = static_cast<std::uint64_t>(st.st_dev);
        stamp.inode = static_cast<std::uint64_t>(st.st_ino);
        stamp.size = static_cast<std::uint64_t>(st.st_size);
        stamp.mtimeSec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
        stamp.mtimeNsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
        stamp.ctimeSec = static_cast<std::int64_t>(st.st_ctim.tv_sec);
        stamp.ctimeNsec = static_cast<std::int64_t>(st.st_ctim.tv_nsec);
        stamp.exists = true;
    }
    return stamp;
}
namespace {

std::string fnv1a64Hex(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

bool parseBoolean(std::string_view value, bool &result) {
    if (value == "true") {
        result = true;
        return true;
    }
    if (value == "false") {
        result = false;
        return true;
    }
    return false;
}

bool parseInteger(std::string_view value, int &result) {
    if (value.empty()) {
        return false;
    }
    if (value.front() < '0' || value.front() > '9') {
        return false;
    }
    int parsed = 0;
    const auto conversion = std::from_chars(value.data(),
                                            value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} ||
        conversion.ptr != value.data() + value.size()) {
        return false;
    }
    result = parsed;
    return true;
}

bool validToggleKey(std::string_view value) {
    return value == "Ctrl+Space" || value == "Alt+Space" ||
           value == "Super+Space" || value == "Ctrl+Shift+Space" ||
           value == "Shift_L" || value == "Shift_R";
}

bool validAppId(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/' || c == '@';
        });
}

bool punctuationKey(int key) {
    return key >= 33 && key <= 126 && !((key >= 'a' && key <= 'z') ||
        (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'));
}

bool validPunctuationText(std::string_view text, std::size_t maxBytes = 32) {
    if (text.empty() || text.size() > maxBytes || trim(text) != text) return false;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        int count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
        if (count < 0 || i + static_cast<std::size_t>(count) > text.size()) return false;
        const auto next = static_cast<unsigned char>(text[i]);
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) return false;
        while (count--) if ((static_cast<unsigned char>(text[i++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

std::string_view underscoreName(UnderscoreStyle style) {
    switch (style) {
    case UnderscoreStyle::Smart: return "smart";
    case UnderscoreStyle::Ascii: return "ascii";
    case UnderscoreStyle::Chinese: return "chinese";
    }
    return "invalid";
}

std::string pinKey(const CandidatePin &pin) {
    for (const auto &scheme : kPinyinSchemes)
        if (scheme.scheme == pin.scheme) return "candidate.pin." + std::string(scheme.key) + "." + PinyinMatchPolicy::canonical(pin.input);
    return {};
}

bool validPin(const CandidatePin &pin) {
    return !pinKey(pin).empty() && !pin.input.empty() && pin.input.size() <= 128 &&
        PinyinMatchPolicy::validComposition(pin.input) &&
        pin.input == PinyinMatchPolicy::canonical(pin.input) &&
        validPunctuationText(pin.text, 128) && pin.text.find('|') == std::string::npos &&
        pin.fullPinyin.size() <= 128 && PinyinMatchPolicy::isValidPinyinSequence(pin.fullPinyin);
}

std::string symbolFavoriteKey(std::string_view id) {
    constexpr char hex[] = "0123456789abcdef";
    std::string key = "symbols.favorite.";
    for (const unsigned char c : id) { key += hex[c >> 4]; key += hex[c & 15]; }
    return key;
}
std::string decodeSymbolId(std::string_view key) {
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (key.empty() || key.size() > 128 || key.size() % 2) return {};
    std::string id;
    for (std::size_t i = 0; i < key.size(); i += 2) {
        const int a = digit(key[i]), b = digit(key[i + 1]);
        if (a < 0 || b < 0) return {};
        id += static_cast<char>((a << 4) | b);
    }
    return id;
}

bool validClipboardTrigger(std::string_view value) {
    if (value.size() != 3) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value[0]);
    const auto second = static_cast<unsigned char>(value[2]);
    const bool letter = (first >= 'A' && first <= 'Z') ||
                        (first >= 'a' && first <= 'z');
    return letter && value[1] == '+' && second >= '1' && second <= '9';
}

void diagnostic(SettingsLoadResult &result, std::size_t line,
                std::string message) {
    result.diagnostics.push_back("line " + std::to_string(line) + ": " +
                                 std::move(message));
}

std::filesystem::path homePath(std::string_view home) {
    return home.empty() ? std::filesystem::path{}
                        : std::filesystem::path(std::string(home));
}

} // namespace

namespace {
bool validCandidateFontFamily(std::string_view value) {
    return !value.empty() && value.size() <= 128 && trim(value) == value &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
}

ModernIMESettings defaultSettings() { return {}; }

SettingsValidationResult validateSettings(const ModernIMESettings &settings) {
    SettingsValidationResult result;
    if (settings.symbolFavorites.size() > 128 || std::any_of(settings.symbolFavorites.begin(), settings.symbolFavorites.end(),
            [](const auto &id) { return SymbolCatalog::find(id) == nullptr; }))
        result.issues.push_back({"symbols.favorites", "收藏最多128项，须选择现有符号目录条目"});
    std::unordered_set<std::string> pinKeys;
    if (settings.candidatePins.size() > 128)
        result.issues.push_back({"candidate.pin", "候选置顶规则最多128条"});
    for (const auto &pin : settings.candidatePins)
        if (!validPin(pin) || !pinKeys.insert(pinKey(pin)).second)
            result.issues.push_back({"candidate.pin", "置顶规则需包含有效方案、输入、文本和完整拼音，同一输入只允许一条"});
    if (underscoreName(settings.underscoreStyle) == "invalid")
        result.issues.push_back({"punctuation.underscore", "请选择 smart、ascii 或 chinese"});
    for (const auto &[key, text] : settings.punctuationOverrides)
        if (!punctuationKey(key) || !validPunctuationText(text))
            result.issues.push_back({"punctuation.map." + std::to_string(static_cast<unsigned char>(key)), "映射须为 ASCII 标点到 1–32 字节单行 UTF-8 文本"});
    if (settings.appInputModes.size() > 128)
        result.issues.push_back({"input.app", "应用规则最多 128 条"});
    for (const auto &[app, mode] : settings.appInputModes)
        if (!validAppId(app) || (mode != InputMode::Chinese && mode != InputMode::English))
            result.issues.push_back({"input.app." + app, "请输入有效的应用标识和中英文模式"});
    if (std::none_of(kClientThemes.begin(), kClientThemes.end(), [&](const auto &option) { return option.key == settings.clientTheme; })) {
        result.issues.push_back({"appearance.client_theme", "请选择支持的客户端主题"});
    }
    if (!validToggleKey(settings.toggleKey)) {
        result.issues.push_back(
            {"input.toggle_key",
             "中英文切换快捷键仅支持 Shift 单键、Ctrl+Space、Alt+Space、Super+Space 或 "
             "Ctrl+Shift+Space"});
    }
    if (settings.candidatePageSize < kMinimumCandidatePageSize ||
        settings.candidatePageSize > kMaximumCandidatePageSize) {
        result.issues.push_back(
            {"candidate.page_size", "候选栏的单字容量仅支持 3 到 9 之间的整数"});
    }
    if (settings.candidateFontSize < kMinimumCandidateFontSize ||
        settings.candidateFontSize > kMaximumCandidateFontSize) {
        result.issues.push_back(
            {"candidate.font_size", "候选框字号仅支持 14 到 28 之间的整数"});
    }
    if (!validCandidateFontFamily(settings.candidateFontFamily))
        result.issues.push_back({"candidate.font_family", "请选择有效的候选字体"});
    if (std::none_of(kCandidateThemes.begin(), kCandidateThemes.end(), [&](const auto &option) { return option.value == settings.candidateTheme; }))
        result.issues.push_back({"candidate.theme", "请选择支持的候选配色"});
    if (std::none_of(kCandidateShapes.begin(), kCandidateShapes.end(), [&](const auto &option) { return option.value == settings.candidateShape; }))
        result.issues.push_back({"candidate.shape", "请选择圆润、方正或胶囊轮廓"});
    if (std::none_of(kCandidateSpacings.begin(), kCandidateSpacings.end(), [&](const auto &option) { return option.value == settings.candidateSpacing; }))
        result.issues.push_back({"candidate.spacing", "请选择紧凑、标准或宽松间距"});
    if (!validClipboardTrigger(settings.clipboardTrigger)) {
        result.issues.push_back(
            {"clipboard.trigger", "剪贴板触发键必须符合字母+数字格式，例如 V+2"});
    }
    result.valid = result.issues.empty();
    return result;
}

std::string computeSettingsHash(const ModernIMESettings &settings) {
    std::ostringstream output;
    for (const auto &pin : settings.candidatePins)
        output << pinKey(pin) << '=' << pin.text << '|' << pin.fullPinyin << '\n';
    output << "punctuation.auto_pair=" << settings.autoPairPunctuation << '\n'
           << "punctuation.smart=" << settings.smartPunctuation << '\n'
           << "punctuation.underscore=" << underscoreName(settings.underscoreStyle) << '\n'
           << "input.remember_per_app=" << settings.rememberModePerApp << '\n'
           << "input.undo_commit=" << settings.undoCommitEnabled << '\n';
    output << "symbols.enabled=" << settings.symbolSearchEnabled << '\n';
    for (const auto &id : settings.symbolFavorites) output << symbolFavoriteKey(id) << "=true\n";
    output << "input.calculator=" << settings.calculatorEnabled << '\n';
    output << "input.component_lookup=" << settings.componentLookupEnabled << '\n';
    output << "input.temporary_english=" << settings.temporaryEnglishEnabled << '\n';
    for (const auto &[key, text] : settings.punctuationOverrides)
        output << "punctuation.map." << static_cast<int>(key) << '=' << text << '\n';
    for (const auto &[app, mode] : settings.appInputModes)
        output << "input.app." << app << '=' << (mode == InputMode::Chinese ? "chinese" : "english") << '\n';
    output << "input.enabled=" << (settings.inputEnabled ? "true" : "false")
           << '\n'
           << "input.default_mode="
           << (settings.defaultMode == InputMode::Chinese ? "chinese"
                                                            : "english")
           << '\n'
           << "input.toggle_key=" << settings.toggleKey << '\n'
           << "appearance.client_theme=" << settings.clientTheme << '\n'
           << "candidate.page_size=" << settings.candidatePageSize << '\n'
           << "candidate.font_size=" << settings.candidateFontSize << '\n'
           << "candidate.font_family=" << settings.candidateFontFamily << '\n'
           << "candidate.number_selection="
           << (settings.numberSelection ? "true" : "false") << '\n'
           << "candidate.arrow_navigation="
           << (settings.arrowNavigation ? "true" : "false") << '\n'
           << "candidate.page_navigation="
           << (settings.pageNavigation ? "true" : "false") << '\n'
           << "punctuation.enabled="
           << (settings.punctuationEnabled ? "true" : "false") << '\n'
           << "learning.enabled="
           << (settings.learningEnabled ? "true" : "false") << '\n'
           << "learning.context_enabled="
           << (settings.contextLearningEnabled ? "true" : "false") << '\n'
           << "clipboard.enabled="
           << (settings.clipboardEnabled ? "true" : "false") << '\n'
           << "clipboard.trigger=" << settings.clipboardTrigger << '\n'
           << "candidate.english_definition="
           << (settings.englishDefinitionEnabled ? "true" : "false") << '\n'
           << "candidate.character_extraction="
           << (settings.characterExtractionEnabled ? "true" : "false") << '\n';
    for (const auto &option : kCandidateThemes)
        if (option.value == settings.candidateTheme) output << "candidate.theme=" << option.key << '\n';
    for (const auto &option : kCandidateShapes)
        if (option.value == settings.candidateShape) output << "candidate.shape=" << option.key << '\n';
    for (const auto &option : kCandidateSpacings)
        if (option.value == settings.candidateSpacing) output << "candidate.spacing=" << option.key << '\n';
    for (const auto &option : kFuzzyPinyinOptions) {
        output << option.key << '=' << (settings.fuzzyPinyin.*option.member ? "true" : "false") << '\n';
    }
    for (const auto &option : kPinyinSchemes) {
        if (option.scheme == settings.pinyinScheme) output << "pinyin.scheme=" << option.key << '\n';
    }
    return fnv1a64Hex(output.str());
}

std::string computeFileContentHash(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream ss;
    ss << input.rdbuf();
    return fnv1a64Hex(ss.str());
}

SettingsPaths SettingsPaths::fromEnvironment(std::string_view xdgConfigHome,
                                             std::string_view xdgDataHome,
                                             std::string_view home) {
    const auto homeDirectory = homePath(home);
    const auto configDirectory = xdgConfigHome.empty()
                                     ? homeDirectory / ".config"
                                     : std::filesystem::path(
                                           std::string(xdgConfigHome));
    const auto dataDirectory = xdgDataHome.empty()
                                   ? homeDirectory / ".local" / "share"
                                   : std::filesystem::path(
                                         std::string(xdgDataHome));
    const auto modernimeConfig = configDirectory / "modernime";
    const auto modernimeData = dataDirectory / "modernime";
    return {modernimeConfig / "settings.conf",
            modernimeData / "user-dictionary.txt",
            modernimeData / "learning.sqlite3",
            modernimeData / "clipboard-history.bin",
            modernimeConfig / "runtime.state"};
}

SettingsLoadResult SettingsStore::load(const std::filesystem::path &path) {
    SettingsLoadResult result;
    if (path.empty()) {
        result.status = SettingsLoadStatus::Missing;
        return result;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (fp == nullptr) {
            const int err = errno;
            if (err == ENOENT) {
                result.status = SettingsLoadStatus::Missing;
                result.stamp = SettingsFileStamp::fromPath(path);
                result.settings = defaultSettings();
                result.contentHash.clear();
                return result;
            }
            result.status = SettingsLoadStatus::IoError;
            result.stamp = SettingsFileStamp::fromPath(path);
            result.diagnostics.push_back("unable to read settings file");
            return result;
        }

        const int fd = fileno(fp);
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            std::fclose(fp);
            result.status = SettingsLoadStatus::IoError;
            result.diagnostics.push_back("unable to stat open settings file");
            return result;
        }

        std::string buffer;
        if (st.st_size > 0) {
            buffer.resize(static_cast<std::size_t>(st.st_size));
            const std::size_t bytesRead = std::fread(buffer.data(), 1, buffer.size(), fp);
            if (bytesRead != buffer.size()) {
                buffer.resize(bytesRead);
                if (std::ferror(fp)) {
                    std::fclose(fp);
                    result.status = SettingsLoadStatus::IoError;
                    result.diagnostics.push_back("unable to finish reading settings file");
                    return result;
                }
            }
        } else {
            char chunk[4096];
            while (const std::size_t n = std::fread(chunk, 1, sizeof(chunk), fp)) {
                buffer.append(chunk, n);
            }
            if (std::ferror(fp)) {
                std::fclose(fp);
                result.status = SettingsLoadStatus::IoError;
                result.diagnostics.push_back("unable to finish reading settings file");
                return result;
            }
        }
        std::fclose(fp);

        struct stat diskSt {};
        if (::stat(path.c_str(), &diskSt) == 0 &&
            (diskSt.st_ino != st.st_ino || diskSt.st_dev != st.st_dev) &&
            attempt < 2) {
            continue;
        }

        result.stamp.device = static_cast<std::uint64_t>(st.st_dev);
        result.stamp.inode = static_cast<std::uint64_t>(st.st_ino);
        result.stamp.size = static_cast<std::uint64_t>(st.st_size);
        result.stamp.mtimeSec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
        result.stamp.mtimeNsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
        result.stamp.ctimeSec = static_cast<std::int64_t>(st.st_ctim.tv_sec);
        result.stamp.ctimeNsec = static_cast<std::int64_t>(st.st_ctim.tv_nsec);
        result.stamp.exists = true;

        result.contentHash = fnv1a64Hex(buffer);
        result.status = SettingsLoadStatus::Success;

        std::istringstream input(buffer);
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            result.rawLines.push_back(line);
            const auto content = trim(line);
            if (content.empty() || content.front() == '#') {
                continue;
            }
            const auto separator = content.find('=');
            if (separator == std::string::npos) {
                diagnostic(result, lineNumber, "missing '='");
                continue;
            }
            const auto key = trim(std::string_view(content).substr(0, separator));
            const auto value = trim(std::string_view(content).substr(separator + 1));
            bool parsed = false;
            const auto fuzzy = std::find_if(kFuzzyPinyinOptions.begin(), kFuzzyPinyinOptions.end(),
                [&](const auto &option) { return key == option.key; });
            if (key == "pinyin.scheme") {
                for (const auto &option : kPinyinSchemes) {
                    if (value == option.key) {
                        result.settings.pinyinScheme = option.scheme;
                        parsed = true;
                        break;
                    }
                }
            } else if (fuzzy != kFuzzyPinyinOptions.end()) {
                parsed = parseBoolean(value, result.settings.fuzzyPinyin.*fuzzy->member);
            } else if (key == "appearance.client_theme") {
                if (std::any_of(kClientThemes.begin(), kClientThemes.end(), [&](const auto &option) { return option.key == value; })) {
                    result.settings.clientTheme = value;
                    parsed = true;
                }
            } else if (key == "input.enabled") {
                parsed = parseBoolean(value, result.settings.inputEnabled);
            } else if (key == "input.default_mode") {
                if (value == "chinese") {
                    result.settings.defaultMode = InputMode::Chinese;
                    parsed = true;
                } else if (value == "english") {
                    result.settings.defaultMode = InputMode::English;
                    parsed = true;
                }
            } else if (key == "input.toggle_key") {
                if (validToggleKey(value)) {
                    result.settings.toggleKey = value;
                    parsed = true;
                }
            } else if (key == "candidate.page_size") {
                int val = 0;
                if (parseInteger(value, val) && val >= kMinimumCandidatePageSize &&
                    val <= kMaximumCandidatePageSize) {
                    result.settings.candidatePageSize = val;
                    parsed = true;
                }
            } else if (key == "candidate.font_size") {
                int val = 0;
                if (parseInteger(value, val) && val >= kMinimumCandidateFontSize &&
                    val <= kMaximumCandidateFontSize) {
                    result.settings.candidateFontSize = val;
                    parsed = true;
                }
            } else if (key == "candidate.font_family") {
                if (validCandidateFontFamily(value)) {
                    result.settings.candidateFontFamily = value;
                    parsed = true;
                }
            } else if (key == "candidate.theme") {
                for (const auto &option : kCandidateThemes) if (option.key == value) {
                    result.settings.candidateTheme = option.value;
                    parsed = true;
                }
            } else if (key == "candidate.shape") {
                for (const auto &option : kCandidateShapes) if (option.key == value) {
                    result.settings.candidateShape = option.value;
                    parsed = true;
                }
            } else if (key == "candidate.spacing") {
                for (const auto &option : kCandidateSpacings) if (option.key == value) {
                    result.settings.candidateSpacing = option.value;
                    parsed = true;
                }
            } else if (key == "candidate.number_selection") {
                parsed = parseBoolean(value, result.settings.numberSelection);
            } else if (key == "candidate.arrow_navigation") {
                parsed = parseBoolean(value, result.settings.arrowNavigation);
            } else if (key == "candidate.page_navigation") {
                parsed = parseBoolean(value, result.settings.pageNavigation);
            } else if (key == "punctuation.enabled") {
                parsed = parseBoolean(value, result.settings.punctuationEnabled);
            } else if (key == "punctuation.auto_pair") {
                parsed = parseBoolean(value, result.settings.autoPairPunctuation);
            } else if (key == "punctuation.smart") {
                parsed = parseBoolean(value, result.settings.smartPunctuation);
            } else if (key == "input.undo_commit") {
                parsed = parseBoolean(value, result.settings.undoCommitEnabled);
            } else if (key == "symbols.enabled") {
                parsed = parseBoolean(value, result.settings.symbolSearchEnabled);
            } else if (key.starts_with("symbols.favorite.")) {
                const auto id = decodeSymbolId(std::string_view(key).substr(17));
                bool favorite = false;
                if (SymbolCatalog::find(id) && parseBoolean(value, favorite)) {
                    auto &favorites = result.settings.symbolFavorites;
                    if (!favorite) { favorites.erase(id); parsed = true; }
                    else if (favorites.size() < 128 || favorites.contains(id)) { favorites.insert(id); parsed = true; }
                }
            } else if (key == "input.calculator") {
                parsed = parseBoolean(value, result.settings.calculatorEnabled);
            } else if (key == "input.component_lookup") {
                parsed = parseBoolean(value, result.settings.componentLookupEnabled);
            } else if (key == "input.temporary_english") {
                parsed = parseBoolean(value, result.settings.temporaryEnglishEnabled);
            } else if (key.starts_with("candidate.pin.")) {
                const auto schemeEnd = key.find('.', 14);
                const auto pinSeparator = value.find('|');
                if (schemeEnd != std::string::npos && pinSeparator != std::string::npos) {
                    for (const auto &scheme : kPinyinSchemes) {
                        if (key.substr(14, schemeEnd - 14) != scheme.key) continue;
                        CandidatePin pin{scheme.scheme, PinyinMatchPolicy::canonical(key.substr(schemeEnd + 1)), value.substr(0, pinSeparator), value.substr(pinSeparator + 1)};
                        if (!validPin(pin)) break;
                        auto &pins = result.settings.candidatePins;
                        auto existing = std::find_if(pins.begin(), pins.end(), [&](const auto &p) { return pinKey(p) == pinKey(pin); });
                        if (existing != pins.end()) { *existing = std::move(pin); parsed = true; }
                        else if (pins.size() < 128) { pins.push_back(std::move(pin)); parsed = true; }
                        break;
                    }
                }
            } else if (key == "input.remember_per_app") {
                parsed = parseBoolean(value, result.settings.rememberModePerApp);
            } else if (key == "punctuation.underscore") {
                if (value == "smart" || value == "ascii" || value == "chinese") {
                    result.settings.underscoreStyle = value == "ascii" ? UnderscoreStyle::Ascii : value == "chinese" ? UnderscoreStyle::Chinese : UnderscoreStyle::Smart;
                    parsed = true;
                }
            } else if (key.starts_with("punctuation.map.")) {
                int code = 0;
                if (parseInteger(std::string_view(key).substr(16), code) &&
                    punctuationKey(code) && validPunctuationText(value)) {
                    result.settings.punctuationOverrides[static_cast<char>(code)] = value;
                    parsed = true;
                }
            } else if (key.starts_with("input.app.")) {
                const auto app = key.substr(10);
                if (validAppId(app) && (value == "chinese" || value == "english") &&
                    (result.settings.appInputModes.contains(app) || result.settings.appInputModes.size() < 128)) {
                    result.settings.appInputModes[app] = value == "chinese" ? InputMode::Chinese : InputMode::English;
                    parsed = true;
                }
            } else if (key == "learning.enabled") {
                parsed = parseBoolean(value, result.settings.learningEnabled);
            } else if (key == "learning.context_enabled") {
                parsed = parseBoolean(value, result.settings.contextLearningEnabled);
            } else if (key == "clipboard.enabled") {
                parsed = parseBoolean(value, result.settings.clipboardEnabled);
            } else if (key == "clipboard.trigger") {
                if (validClipboardTrigger(value)) {
                    result.settings.clipboardTrigger = value;
                    parsed = true;
                }
            } else if (key == "candidate.english_definition" || key == "input.english_definition") {
                parsed = parseBoolean(value, result.settings.englishDefinitionEnabled);
            } else if (key == "candidate.character_extraction" || key == "candidate.extract_character") {
                parsed = parseBoolean(value, result.settings.characterExtractionEnabled);
            } else {
                diagnostic(result, lineNumber, "unknown key '" + key + "'");
                continue;
            }
            if (!parsed) {
                diagnostic(result, lineNumber, "invalid value for '" + key + "'");
            }
        }
        return result;
    }
    return result;
}

SettingsSaveResult SettingsStore::saveWithResult(
    const std::filesystem::path &path,
    const ModernIMESettings &settings,
    SettingsExpectedBaseline expectedBaseline,
    const std::vector<std::string> *rawLinesToPreserve) {
    SettingsSaveResult result;
    if (path.empty()) {
        result.status = SettingsSaveStatus::IoError;
        result.error = "settings path is empty";
        return result;
    }
    const auto validation = validateSettings(settings);
    if (!validation.valid) {
        result.status = SettingsSaveStatus::Invalid;
        result.error = validation.issues.front().message;
        return result;
    }

    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystemError);
        if (filesystemError) {
            result.status = SettingsSaveStatus::IoError;
            result.error = "unable to create settings directory: " + filesystemError.message();
            return result;
        }
    }

    const auto lockPath = path.string() + ".lock";
    int lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (lockFd < 0) {
        result.status = SettingsSaveStatus::IoError;
        result.error = "无法获取配置文件锁: " + std::string(std::strerror(errno));
        return result;
    }
    struct LockGuard {
        int fd = -1;
        ~LockGuard() {
            if (fd >= 0) {
                ::flock(fd, LOCK_UN);
                ::close(fd);
            }
        }
    } guard{lockFd};
    if (::flock(lockFd, LOCK_EX) != 0) {
        result.status = SettingsSaveStatus::IoError;
        result.error = "无法锁定配置文件: " + std::string(std::strerror(errno));
        return result;
    }

    if (expectedBaseline.kind != SettingsBaselineKind::Unchecked) {
        const bool fileExists = std::filesystem::exists(path, filesystemError);
        if (expectedBaseline.kind == SettingsBaselineKind::ExpectedMissing) {
            if (fileExists) {
                result.status = SettingsSaveStatus::Conflict;
                result.error = "配置文件已被外部创建，请重新加载后再保存";
                result.currentDiskHash = computeFileContentHash(path);
                const auto currentLoaded = SettingsStore::load(path);
                result.currentDiskSettings = currentLoaded.settings;
                result.currentDiskRawLines = currentLoaded.rawLines;
                return result;
            }
        } else if (expectedBaseline.kind == SettingsBaselineKind::ExpectedContentHash) {
            const auto currentHash = fileExists ? computeFileContentHash(path) : std::string();
            if (currentHash != expectedBaseline.contentHash) {
                result.status = SettingsSaveStatus::Conflict;
                result.error = fileExists
                    ? "配置文件已被外部修改，请重新加载后再保存"
                    : "配置文件已被外部删除，请重新加载后再保存";
                result.currentDiskHash = currentHash;
                if (fileExists) {
                    const auto currentLoaded = SettingsStore::load(path);
                    result.currentDiskSettings = currentLoaded.settings;
                    result.currentDiskRawLines = currentLoaded.rawLines;
                }
                return result;
            }
        }
    }

    std::unordered_map<std::string, std::string> knownSettings;
    for (const auto &option : kPinyinSchemes) {
        if (option.scheme == settings.pinyinScheme) knownSettings["pinyin.scheme"] = option.key;
    }
    for (const auto &option : kFuzzyPinyinOptions) {
        knownSettings[option.key] = settings.fuzzyPinyin.*option.member ? "true" : "false";
    }
    knownSettings["input.enabled"] = settings.inputEnabled ? "true" : "false";
    knownSettings["input.default_mode"] =
        settings.defaultMode == InputMode::Chinese ? "chinese" : "english";
    knownSettings["input.toggle_key"] = settings.toggleKey;
    knownSettings["input.remember_per_app"] = settings.rememberModePerApp ? "true" : "false";
    knownSettings["input.undo_commit"] = settings.undoCommitEnabled ? "true" : "false";
    knownSettings["symbols.enabled"] = settings.symbolSearchEnabled ? "true" : "false";
    knownSettings["input.calculator"] = settings.calculatorEnabled ? "true" : "false";
    knownSettings["input.component_lookup"] = settings.componentLookupEnabled ? "true" : "false";
    knownSettings["input.temporary_english"] = settings.temporaryEnglishEnabled ? "true" : "false";
    knownSettings["punctuation.auto_pair"] = settings.autoPairPunctuation ? "true" : "false";
    knownSettings["punctuation.smart"] = settings.smartPunctuation ? "true" : "false";
    knownSettings["punctuation.underscore"] = underscoreName(settings.underscoreStyle);
    std::vector<std::string> dynamicKeys;
    for (const auto &id : settings.symbolFavorites) {
        const auto key = symbolFavoriteKey(id); knownSettings[key] = "true"; dynamicKeys.push_back(key);
    }

    for (const auto &pin : settings.candidatePins) {
        auto key = pinKey(pin);
        knownSettings[key] = pin.text + "|" + pin.fullPinyin;
        dynamicKeys.push_back(std::move(key));
    }
    for (const auto &[key, text] : settings.punctuationOverrides) {
        const auto name = "punctuation.map." + std::to_string(static_cast<unsigned char>(key));
        knownSettings[name] = text; dynamicKeys.push_back(name);
    }
    for (const auto &[app, mode] : settings.appInputModes) {
        const auto name = "input.app." + app;
        knownSettings[name] = mode == InputMode::Chinese ? "chinese" : "english";
        dynamicKeys.push_back(name);
    }
    knownSettings["appearance.client_theme"] = settings.clientTheme;
    knownSettings["candidate.page_size"] =
        std::to_string(settings.candidatePageSize);
    knownSettings["candidate.font_size"] =
        std::to_string(settings.candidateFontSize);
    knownSettings["candidate.font_family"] = settings.candidateFontFamily;
    for (const auto &option : kCandidateThemes)
        if (option.value == settings.candidateTheme) knownSettings["candidate.theme"] = option.key;
    for (const auto &option : kCandidateShapes)
        if (option.value == settings.candidateShape) knownSettings["candidate.shape"] = option.key;
    for (const auto &option : kCandidateSpacings)
        if (option.value == settings.candidateSpacing) knownSettings["candidate.spacing"] = option.key;
    knownSettings["candidate.number_selection"] =
        settings.numberSelection ? "true" : "false";
    knownSettings["candidate.arrow_navigation"] =
        settings.arrowNavigation ? "true" : "false";
    knownSettings["candidate.page_navigation"] =
        settings.pageNavigation ? "true" : "false";
    knownSettings["punctuation.enabled"] =
        settings.punctuationEnabled ? "true" : "false";
    knownSettings["learning.enabled"] =
        settings.learningEnabled ? "true" : "false";
    knownSettings["learning.context_enabled"] =
        settings.contextLearningEnabled ? "true" : "false";
    knownSettings["clipboard.enabled"] =
        settings.clipboardEnabled ? "true" : "false";
    knownSettings["clipboard.trigger"] = settings.clipboardTrigger;
    knownSettings["candidate.english_definition"] =
        settings.englishDefinitionEnabled ? "true" : "false";
    knownSettings["candidate.character_extraction"] =
        settings.characterExtractionEnabled ? "true" : "false";

    std::unordered_set<std::string> writtenKeys;
    const auto canonicalKey = [](std::string_view k) -> std::string {
        if (k == "input.english_definition") return "candidate.english_definition";
        if (k == "candidate.extract_character") return "candidate.character_extraction";
        return std::string(k);
    };

    std::vector<std::string> writtenLines;
    std::ostringstream output;

    if (rawLinesToPreserve != nullptr && !rawLinesToPreserve->empty()) {
        for (const auto &rawLine : *rawLinesToPreserve) {
            const auto content = trim(rawLine);
            if (content.empty() || content.front() == '#') {
                output << rawLine << '\n';
                writtenLines.push_back(rawLine);
                continue;
            }
            const auto separator = content.find('=');
            if (separator == std::string::npos) {
                output << rawLine << '\n';
                writtenLines.push_back(rawLine);
                continue;
            }
            const auto key = canonicalKey(trim(std::string_view(content).substr(0, separator)));
            auto it = knownSettings.find(key);
            if (it != knownSettings.end()) {
                std::string line = key + "=" + it->second;
                output << line << '\n';
                writtenLines.push_back(std::move(line));
                writtenKeys.insert(key);
            } else if (!key.starts_with("punctuation.map.") && !key.starts_with("input.app.") && !key.starts_with("candidate.pin.") && !key.starts_with("symbols.favorite.")) {
                output << rawLine << '\n';
                writtenLines.push_back(rawLine);
            }
        }
    }

    const std::array keyOrder{
        "input.enabled", "input.default_mode", "input.toggle_key",
        "input.remember_per_app", "input.undo_commit",
        "symbols.enabled",
        "input.calculator",
        "input.component_lookup",
        "input.temporary_english",
        "punctuation.auto_pair", "punctuation.smart", "punctuation.underscore",
        "pinyin.scheme",
        "pinyin.fuzzy_z_zh", "pinyin.fuzzy_c_ch", "pinyin.fuzzy_s_sh",
        "pinyin.fuzzy_l_n", "pinyin.fuzzy_en_eng", "pinyin.fuzzy_in_ing",
        "appearance.client_theme", "candidate.page_size", "candidate.font_size",
        "candidate.font_family", "candidate.theme", "candidate.shape", "candidate.spacing",
        "candidate.number_selection", "candidate.arrow_navigation",
        "candidate.page_navigation", "punctuation.enabled",
        "learning.enabled", "learning.context_enabled",
        "clipboard.enabled", "clipboard.trigger",
        "candidate.english_definition", "candidate.character_extraction"};

    for (const char *k : keyOrder) {
        if (writtenKeys.find(k) == writtenKeys.end()) {
            std::string line = std::string(k) + "=" + knownSettings[k];
            output << line << '\n';
            writtenLines.push_back(std::move(line));
        }
    }
    for (const auto &key : dynamicKeys) {
        if (!writtenKeys.contains(key)) {
            std::string line = key + "=" + knownSettings[key];
            output << line << '\n'; writtenLines.push_back(std::move(line));
        }
    }

    const std::string writtenContent = output.str();
    const std::string contentHash = fnv1a64Hex(writtenContent);

    static std::atomic<std::uint64_t> s_tempCounter{0};
    const auto tempPath = path.string() + ".tmp." + std::to_string(::getpid()) + "." +
                          std::to_string(++s_tempCounter);

    int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        result.status = SettingsSaveStatus::IoError;
        result.error = "unable to open temporary settings file";
        return result;
    }
    std::size_t written = 0;
    while (written < writtenContent.size()) {
        ssize_t n = ::write(fd, writtenContent.data() + written, writtenContent.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tempPath.c_str());
            result.status = SettingsSaveStatus::IoError;
            result.error = "unable to write temporary settings file";
            return result;
        }
        written += static_cast<std::size_t>(n);
    }
    ::fsync(fd);

    std::filesystem::rename(tempPath, path, filesystemError);
    if (filesystemError) {
        ::close(fd);
        std::filesystem::remove(tempPath, filesystemError);
        result.status = SettingsSaveStatus::IoError;
        result.error = "unable to replace settings file: " + filesystemError.message();
        return result;
    }

    struct stat committedStat {};
    if (::fstat(fd, &committedStat) != 0) {
        ::close(fd);
        result.status = SettingsSaveStatus::IoError;
        result.error = "unable to stat committed settings file: " + std::string(std::strerror(errno));
        result.stamp = SettingsFileStamp{};
        return result;
    }
    ::close(fd);

    result.status = SettingsSaveStatus::Saved;
    result.settings = settings;
    result.contentHash = contentHash;
    result.rawLines = std::move(writtenLines);
    result.stamp.device = static_cast<std::uint64_t>(committedStat.st_dev);
    result.stamp.inode = static_cast<std::uint64_t>(committedStat.st_ino);
    result.stamp.size = static_cast<std::uint64_t>(committedStat.st_size);
    result.stamp.mtimeSec = static_cast<std::int64_t>(committedStat.st_mtim.tv_sec);
    result.stamp.mtimeNsec = static_cast<std::int64_t>(committedStat.st_mtim.tv_nsec);
    result.stamp.ctimeSec = static_cast<std::int64_t>(committedStat.st_ctim.tv_sec);
    result.stamp.ctimeNsec = static_cast<std::int64_t>(committedStat.st_ctim.tv_nsec);
    result.stamp.exists = true;
    result.error.clear();
    return result;
}

bool SettingsStore::save(const std::filesystem::path &path,
                         const ModernIMESettings &settings,
                         std::string *error,
                         SettingsExpectedBaseline expectedBaseline,
                         const std::vector<std::string> *rawLinesToPreserve) {
    const auto res = saveWithResult(path, settings, expectedBaseline, rawLinesToPreserve);
    if (res.status == SettingsSaveStatus::Saved) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    if (error != nullptr) {
        *error = res.error;
    }
    return false;
}

bool SettingsStore::reset(const std::filesystem::path &path,
                          std::string *error) {
    return save(path, defaultSettings(), error);
}

} // namespace modernime::core
