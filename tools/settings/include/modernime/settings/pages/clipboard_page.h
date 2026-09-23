#pragma once

#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_model.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

struct ClipboardItemMeta {
    std::string typeBadge;
    std::string badgeClass;
    std::string preview;
    std::string sizeLabel;
    std::size_t lineCount = 0;
    std::size_t charCount = 0;
    bool isLong = false;
};

inline std::size_t countUtf8Characters(std::string_view text) {
    std::size_t count = 0;
    for (char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

inline std::size_t countLines(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    std::size_t lines = 1;
    for (char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    return lines;
}

inline std::string makeCollapsedPreview(std::string_view text, std::size_t maxLines = 3, std::size_t maxChars = 120) {
    std::string result;
    std::size_t lines = 0;
    std::size_t lineStart = 0;
    bool truncatedByLine = false;

    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string_view line = text.substr(lineStart, i - lineStart);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (lines > 0) {
                result.push_back('\n');
            }
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
                line.remove_prefix(1);
            }
            result.append(line);
            ++lines;
            lineStart = i + 1;
            if (lines >= maxLines && lineStart < text.size()) {
                truncatedByLine = true;
                break;
            }
        }
    }

    if (truncatedByLine || countUtf8Characters(result) > maxChars) {
        if (countUtf8Characters(result) > maxChars) {
            std::size_t chars = 0;
            std::size_t byteIdx = 0;
            while (byteIdx < result.size() && chars < maxChars) {
                unsigned char byte = static_cast<unsigned char>(result[byteIdx]);
                std::size_t charLen = 1;
                if ((byte & 0xE0) == 0xC0) charLen = 2;
                else if ((byte & 0xF0) == 0xE0) charLen = 3;
                else if ((byte & 0xF8) == 0xF0) charLen = 4;
                if (byteIdx + charLen > result.size()) break;
                byteIdx += charLen;
                ++chars;
            }
            result.resize(byteIdx);
        }
        result += " …";
    }
    return result;
}

inline ClipboardItemMeta analyzeClipboardContent(std::string_view text) {
    const auto lines = countLines(text);
    const auto chars = countUtf8Characters(text);
    const bool isLong = (lines > 2) || (chars > 120);

    std::string badge;
    std::string badgeClass;
    if (text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0 ||
        text.rfind("ftp://", 0) == 0 || text.rfind("file://", 0) == 0) {
        badge = "🔗 链接";
        badgeClass = "modernime-badge-url";
    } else if (lines > 1) {
        static const std::array<std::string_view, 14> codeKeywords = {
            "#include", "import ", "def ", "class ", "func ", "function",
            "void ", "int ", "const ", "var ", "let ", ":=", "$(", "all:"
        };
        bool isCode = false;
        for (const auto &kw : codeKeywords) {
            if (text.find(kw) != std::string_view::npos) {
                isCode = true;
                break;
            }
        }
        if (!isCode && (text.find('{') != std::string_view::npos && text.find('}') != std::string_view::npos)) {
            isCode = true;
        }
        if (!isCode && (text.find(" = ") != std::string_view::npos && text.find('\t') != std::string_view::npos)) {
            isCode = true;
        }
        if (isCode) {
            badge = "💻 代码 (" + std::to_string(lines) + "行)";
            badgeClass = "modernime-badge-code";
        } else {
            badge = "📄 多行 (" + std::to_string(lines) + "行)";
            badgeClass = "modernime-badge-multiline";
        }
    } else {
        badge = "📝 文本";
        badgeClass = "modernime-badge-text";
    }

    std::string preview = isLong ? makeCollapsedPreview(text, 3, 120) : std::string(text);

    return ClipboardItemMeta{
        std::move(badge),
        std::move(badgeClass),
        std::move(preview),
        std::to_string(chars) + " 字",
        lines,
        chars,
        isLong
    };
}

class ClipboardPage final {
public:
    static constexpr auto pageId = SettingsPageId::Clipboard;

    ClipboardPage(SettingsWindowModel &settings,
                  std::filesystem::path historyPath,
                  std::function<void()> settingsChanged,
                  std::function<void(std::string)> notify);
    ~ClipboardPage();

    ClipboardPage(const ClipboardPage &) = delete;
    ClipboardPage &operator=(const ClipboardPage &) = delete;

    GtkWidget *widget() const;
    void refresh(bool notify);
    void refreshSettings();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
