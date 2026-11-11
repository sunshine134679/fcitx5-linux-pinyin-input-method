#include "modernime/settings/detail/release_notes.h"

#include <glib.h>

#include <string>
#include <string_view>

namespace modernime::settings::detail {
namespace {

std::string escaped(std::string_view text) {
    auto *value = g_markup_escape_text(text.data(), static_cast<gssize>(text.size()));
    std::string result(value);
    g_free(value);
    return result;
}

std::string_view trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r");
    if (start == text.npos) return {};
    return text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
}

std::string webLink(std::string_view target, std::string_view version) {
    if (target.starts_with("https://") || target.starts_with("http://"))
        return std::string(target);
    // Resolve the relative documentation links used in our release .md files
    // against the immutable release tag, without opening local paths or schemes.
    if (!target.starts_with("./") && !target.starts_with("../")) return {};
    constexpr auto versionCharacters =
        "0123456789.-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (version.empty() || version.find_first_not_of(versionCharacters) != version.npos)
        return {};
    std::string base = "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/blob/v";
    base += version;
    base += "/docs/releases/";
    auto *uri = g_uri_resolve_relative(base.c_str(), std::string(target).c_str(),
                                       G_URI_FLAGS_NONE, nullptr);
    if (!uri) return {};
    std::string result(uri);
    g_free(uri);
    return result;
}

std::string inlineMarkup(std::string_view text, std::string_view version,
                          unsigned depth = 0) {
    if (depth >= 8) return escaped(text);
    std::string result;
    std::size_t plain = 0;
    const auto flush = [&](std::size_t end) {
        result += escaped(text.substr(plain, end - plain));
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size() &&
            g_ascii_ispunct(text[i + 1])) {
            flush(i);
            result += escaped(text.substr(i + 1, 1));
            i += 2;
            plain = i;
            continue;
        }
        if (text[i] == '`') {
            auto count = text.find_first_not_of('`', i);
            if (count == text.npos) count = text.size();
            const auto delimiter = text.substr(i, count - i);
            const auto end = text.find(delimiter, count);
            if (end != text.npos) {
                flush(i);
                result += "<tt>" + escaped(text.substr(count, end - count)) + "</tt>";
                i = end + delimiter.size();
                plain = i;
                continue;
            }
        }
        if (text[i] == '[') {
            const auto labelEnd = text.find("](", i + 1);
            if (labelEnd != text.npos) {
                auto end = labelEnd + 2;
                unsigned nesting = 1;
                for (; end < text.size(); ++end) {
                    if (text[end] == '(') ++nesting;
                    if (text[end] == ')' && --nesting == 0) break;
                }
                if (end < text.size()) {
                    flush(i);
                    const auto label = inlineMarkup(
                        text.substr(i + 1, labelEnd - i - 1), version, depth + 1);
                    const auto url = webLink(
                        trim(text.substr(labelEnd + 2, end - labelEnd - 2)), version);
                    result += url.empty() ? label : "<a href=\"" + escaped(url) + "\">" + label + "</a>";
                    i = end + 1;
                    plain = i;
                    continue;
                }
            }
        }
        if (text[i] == '*' || text[i] == '_' || text[i] == '~') {
            const char marker = text[i];
            const bool doubled = i + 1 < text.size() && text[i + 1] == marker;
            const std::size_t count = doubled ? 2 : 1;
            const auto delimiter = text.substr(i, count);
            const auto end = text.find(delimiter, i + count);
            // Underscores within identifiers (foo_bar) are ordinary text.
            const bool wordUnderscore = marker == '_' && i > 0 &&
                g_ascii_isalnum(text[i - 1]);
            if (!wordUnderscore && (marker != '~' || doubled) &&
                end != text.npos && end > i + count &&
                !g_ascii_isspace(text[i + count]) && !g_ascii_isspace(text[end - 1])) {
                flush(i);
                const std::string tag = marker == '~' ? "s" : doubled ? "b" : "i";
                result += "<" + tag + ">" + inlineMarkup(
                    text.substr(i + count, end - i - count), version, depth + 1) +
                    "</" + tag + ">";
                i = end + count;
                plain = i;
                continue;
            }
        }
        ++i;
    }
    flush(text.size());
    return result;
}

} // namespace

std::string releaseNotesMarkup(std::string_view markdown, std::string_view version) {
    std::string result;
    std::string code;
    std::string fence;
    unsigned pendingBreaks = 0;
    bool previousList = false;
    bool previousParagraph = false;
    const auto append = [&](const std::string &block, bool list) {
        if (!result.empty()) {
            // Keep paragraph breaks selectable while making the empty line
            // smaller than body text in the compact native reading pane.
            result += pendingBreaks == 0 && list && previousList
                ? "\n" : "\n<span size=\"xx-small\">\n</span>";
        }
        result += block;
        pendingBreaks = 0;
        previousList = list;
        previousParagraph = false;
    };
    for (std::size_t offset = 0; offset < markdown.size();) {
        const auto end = markdown.find('\n', offset);
        auto line = markdown.substr(
            offset, end == markdown.npos ? markdown.size() - offset : end - offset);
        offset = end == markdown.npos ? markdown.size() : end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto stripped = trim(line);
        if (!fence.empty()) {
            if (stripped.starts_with(fence) && trim(stripped.substr(fence.size())).empty()) {
                if (!code.empty()) code.pop_back();
                append("<tt>" + escaped(code) + "</tt>", false);
                fence.clear();
                code.clear();
            } else {
                code += line;
                code += '\n';
            }
            continue;
        }
        if (stripped.starts_with("```") || stripped.starts_with("~~~")) {
            fence = stripped.substr(0, stripped.find_first_not_of(stripped[0]));
            continue;
        }
        if (stripped.empty()) {
            pendingBreaks = 1;
            continue;
        }
        std::size_t heading = 0;
        while (heading < stripped.size() && stripped[heading] == '#') ++heading;
        if (heading >= 1 && heading <= 6 && heading < stripped.size() && stripped[heading] == ' ') {
            const auto body = inlineMarkup(trim(stripped.substr(heading)), version);
            const auto *size = heading == 1 ? "x-large" : heading == 2 ? "large" : "medium";
            append(std::string("<span weight=\"bold\" size=\"") + size + "\">" +
                       body + "</span>", false);
            continue;
        }
        if (stripped.size() > 2 && (stripped[0] == '-' || stripped[0] == '+' ||
            stripped[0] == '*') && stripped[1] == ' ') {
            // Preserve list nesting; the label wraps naturally in the page width.
            const auto indent = line.size() - stripped.size();
            append(std::string(indent, ' ') + "• " +
                       inlineMarkup(stripped.substr(2), version), true);
            continue;
        }
        auto digitEnd = stripped.find_first_not_of("0123456789");
        if (digitEnd > 0 && digitEnd != stripped.npos && digitEnd + 1 < stripped.size() &&
            (stripped[digitEnd] == '.' || stripped[digitEnd] == ')') && stripped[digitEnd + 1] == ' ') {
            append(escaped(stripped.substr(0, digitEnd)) + ". " +
                       inlineMarkup(stripped.substr(digitEnd + 2), version), true);
            continue;
        }
        if (stripped.starts_with("> ")) {
            append("<i>" + inlineMarkup(stripped.substr(2), version) + "</i>", false);
            continue;
        }
        auto body = inlineMarkup(stripped, version);
        if (!result.empty() && pendingBreaks == 0 && previousParagraph) {
            // Markdown soft line breaks stay within their paragraph.
            result += " " + body;
        } else {
            append(body, false);
        }
        previousParagraph = true;
    }
    if (!fence.empty()) {
        if (!code.empty()) code.pop_back();
        append("<tt>" + escaped(code) + "</tt>", false);
    }
    return result;
}

} // namespace modernime::settings::detail
