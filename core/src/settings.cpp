#include "modernime/core/settings.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
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

ModernIMESettings defaultSettings() { return {}; }

SettingsValidationResult validateSettings(const ModernIMESettings &settings) {
    SettingsValidationResult result;
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
    if (!validClipboardTrigger(settings.clipboardTrigger)) {
        result.issues.push_back(
            {"clipboard.trigger", "剪贴板触发键必须符合字母+数字格式，例如 V+2"});
    }
    result.valid = result.issues.empty();
    return result;
}

std::string computeSettingsHash(const ModernIMESettings &settings) {
    std::ostringstream output;
    output << "input.enabled=" << (settings.inputEnabled ? "true" : "false")
           << '\n'
           << "input.default_mode="
           << (settings.defaultMode == InputMode::Chinese ? "chinese"
                                                            : "english")
           << '\n'
           << "input.toggle_key=" << settings.toggleKey << '\n'
           << "candidate.page_size=" << settings.candidatePageSize << '\n'
           << "candidate.font_size=" << settings.candidateFontSize << '\n'
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
            if (key == "input.enabled") {
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
            } else if (key == "candidate.number_selection") {
                parsed = parseBoolean(value, result.settings.numberSelection);
            } else if (key == "candidate.arrow_navigation") {
                parsed = parseBoolean(value, result.settings.arrowNavigation);
            } else if (key == "candidate.page_navigation") {
                parsed = parseBoolean(value, result.settings.pageNavigation);
            } else if (key == "punctuation.enabled") {
                parsed = parseBoolean(value, result.settings.punctuationEnabled);
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
    knownSettings["input.enabled"] = settings.inputEnabled ? "true" : "false";
    knownSettings["input.default_mode"] =
        settings.defaultMode == InputMode::Chinese ? "chinese" : "english";
    knownSettings["input.toggle_key"] = settings.toggleKey;
    knownSettings["candidate.page_size"] =
        std::to_string(settings.candidatePageSize);
    knownSettings["candidate.font_size"] =
        std::to_string(settings.candidateFontSize);
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
            } else {
                output << rawLine << '\n';
                writtenLines.push_back(rawLine);
            }
        }
    }

    const std::array<const char *, 15> keyOrder{
        "input.enabled", "input.default_mode", "input.toggle_key",
        "candidate.page_size", "candidate.font_size",
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
