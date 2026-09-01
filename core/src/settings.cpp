#include "modernime/core/settings.h"

#include <fstream>
#include <utility>

namespace modernime::core {
namespace {

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

bool validToggleKey(std::string_view value) {
    return value == "Ctrl+Space" || value == "Alt+Space" ||
           value == "Super+Space" || value == "Ctrl+Shift+Space";
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
             "中英文切换快捷键仅支持 Ctrl+Space、Alt+Space、Super+Space 或 "
             "Ctrl+Shift+Space"});
    }
    if (!validClipboardTrigger(settings.clipboardTrigger)) {
        result.issues.push_back(
            {"clipboard.trigger", "剪贴板触发键必须符合字母+数字格式，例如 V+2"});
    }
    result.valid = result.issues.empty();
    return result;
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
            modernimeData / "clipboard-history.bin"};
}

SettingsLoadResult SettingsStore::load(const std::filesystem::path &path) {
    SettingsLoadResult result;
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            result.diagnostics.push_back("unable to read settings file");
        }
        return result;
    }

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
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
        const auto value = trim(
            std::string_view(content).substr(separator + 1));
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
            parsed = parseBoolean(value,
                                  result.settings.contextLearningEnabled);
        } else if (key == "clipboard.enabled") {
            parsed = parseBoolean(value, result.settings.clipboardEnabled);
        } else if (key == "clipboard.trigger") {
            if (validClipboardTrigger(value)) {
                result.settings.clipboardTrigger = value;
                parsed = true;
            }
        } else {
            diagnostic(result, lineNumber, "unknown key '" + key + "'");
            continue;
        }
        if (!parsed) {
            diagnostic(result, lineNumber, "invalid value for '" + key + "'");
        }
    }
    if (!input.eof() && input.fail()) {
        result.diagnostics.push_back("unable to finish reading settings file");
    }
    return result;
}

bool SettingsStore::save(const std::filesystem::path &path,
                         const ModernIMESettings &settings,
                         std::string *error) {
    const auto setError = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
    };
    if (path.empty()) {
        setError("settings path is empty");
        return false;
    }
    const auto validation = validateSettings(settings);
    if (!validation.valid) {
        setError(validation.issues.front().message);
        return false;
    }
    std::error_code filesystemError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(),
                                            filesystemError);
        if (filesystemError) {
            setError("unable to create settings directory: " +
                     filesystemError.message());
            return false;
        }
    }

    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        setError("unable to open temporary settings file");
        return false;
    }
    output << "input.enabled=" << (settings.inputEnabled ? "true" : "false")
           << '\n'
           << "input.default_mode="
           << (settings.defaultMode == InputMode::Chinese ? "chinese"
                                                            : "english")
           << '\n'
           << "input.toggle_key=" << settings.toggleKey << '\n'
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
           << "clipboard.trigger=" << settings.clipboardTrigger << '\n';
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, filesystemError);
        setError("unable to write temporary settings file");
        return false;
    }

    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(temporary, filesystemError);
        setError("unable to replace settings file: " +
                 filesystemError.message());
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool SettingsStore::reset(const std::filesystem::path &path,
                          std::string *error) {
    return save(path, defaultSettings(), error);
}

} // namespace modernime::core
