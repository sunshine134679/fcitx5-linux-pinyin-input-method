#include "modernime/settings/quick_phrase_store.h"
#include <fcitx-utils/stringutils.h>
#include <gio/gio.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <optional>
#include <unordered_set>
#include <utility>
#include <sys/file.h>
#include <unistd.h>

namespace modernime::settings {
namespace {
struct FileLock {
    int fd;
    ~FileLock() { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
};
}
std::filesystem::path quickPhrasePath() {
    const char *data = std::getenv("XDG_DATA_HOME");
    const char *home = std::getenv("HOME");
    const auto root = data && *data ? std::filesystem::path(data)
        : (home && *home ? std::filesystem::path(home) / ".local/share" : std::filesystem::path{});
    return root.empty() ? root : root / "fcitx5/data/quickphrase.d/modernime.mb";
}
std::string quickPhraseValidation(const QuickPhraseEntry &entry) {
#ifdef MODERNIME_LEGACY_QUICK_PHRASE
    return "快捷短语管理需要 Fcitx5 5.0.16 或更新版本。";
#endif
    if (entry.abbreviation.empty() || entry.abbreviation.size() > 30 ||
        !std::all_of(entry.abbreviation.begin(), entry.abbreviation.end(),
                     [](char c) { return c >= 'a' && c <= 'z'; })) {
        return "缩写请使用 1–30 个小写英文字母，例如 yx、dz、gs。";
    }
    if (entry.text.empty() || !g_utf8_validate(entry.text.data(), static_cast<gssize>(entry.text.size()), nullptr) ||
        std::any_of(entry.text.begin(), entry.text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        return "请填写单行固定文本，不包含换行、制表符或控制字符。";
    }
    return {};
}
std::vector<std::size_t> filterQuickPhrases(std::span<const QuickPhraseEntry> entries,
                                         std::string_view query) {
    const auto fold = [](std::string_view value) {
        if (value.empty()) return std::string{};
        auto *text = g_utf8_casefold(value.data(), static_cast<gssize>(value.size()));
        std::string result(text ? text : "");
        g_free(text);
        return result;
    };
    const auto needle = fold(query);
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (needle.empty() || fold(entries[i].abbreviation).find(needle) != std::string::npos ||
            fold(entries[i].text).find(needle) != std::string::npos) result.push_back(i);
    }
    return result;
}
QuickPhraseSnapshot loadQuickPhrases(const std::filesystem::path &path) {
    QuickPhraseSnapshot result;
#ifdef MODERNIME_LEGACY_QUICK_PHRASE
    result.error = "快捷短语管理需要 Fcitx5 5.0.16 或更新版本，原有个人词典可继续使用。";
    return result;
#endif
    if (path.empty()) { result.error = "无法确定快捷短语保存位置"; return result; }
    gchar *content = nullptr;
    gsize length = 0;
    GError *error = nullptr;
    if (!g_file_get_contents(path.c_str(), &content, &length, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            result.error = "读取快捷短语失败：" + std::string(error->message);
        g_clear_error(&error);
        return result;
    }
    result.exists = true;
    result.content.assign(content, length);
    g_free(content);
    std::istringstream input(result.content);
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto split = line.find_first_of(" \t");
        const auto word = split == std::string::npos ? split : line.find_first_not_of(" \t", split);
#ifndef MODERNIME_LEGACY_QUICK_PHRASE
        const auto text = word == std::string::npos ? std::optional<std::string>{}
            : fcitx::stringutils::unescapeForValue(std::string_view(line).substr(word));
#else
        const auto text = std::optional<std::string>{};
        (void)word;
#endif
        QuickPhraseEntry entry{line.substr(0, split), text.value_or("")};
        if (!text || !quickPhraseValidation(entry).empty()) {
            result.error = "快捷短语第 " + std::to_string(number) + " 行格式不支持，请先备份并修复文件。";
            return result;
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}
QuickPhraseSaveResult saveQuickPhrases(const std::filesystem::path &path,
                                      const QuickPhraseSnapshot &baseline,
                                      const std::vector<QuickPhraseEntry> &entries) {
    if (path.empty() || !baseline.error.empty()) return {QuickPhraseSaveStatus::Invalid, "无法保存：快捷短语尚未成功读取"};
    std::string content;
    std::unordered_set<std::string> identities;
    for (const auto &entry : entries) {
        if (auto error = quickPhraseValidation(entry); !error.empty()) return {QuickPhraseSaveStatus::Invalid, std::move(error)};
        if (!identities.insert(entry.abbreviation + "\x1f" + entry.text).second)
            return {QuickPhraseSaveStatus::Invalid, "相同缩写和文本已存在"};
#ifndef MODERNIME_LEGACY_QUICK_PHRASE
        content += entry.abbreviation + "\t" + fcitx::stringutils::escapeForValue(entry.text) + "\n";
#endif
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return {QuickPhraseSaveStatus::IoError, "无法创建快捷短语目录：" + ec.message()};
    FileLock lock{open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
    if (lock.fd < 0 || flock(lock.fd, LOCK_EX) != 0) return {QuickPhraseSaveStatus::IoError, "无法锁定快捷短语文件：" + std::string(strerror(errno))};
    const auto current = loadQuickPhrases(path);
    if (!current.error.empty()) return {QuickPhraseSaveStatus::IoError, current.error};
    if (current.exists != baseline.exists || current.content != baseline.content)
        return {QuickPhraseSaveStatus::Conflict, "快捷短语已被其他程序修改，请重新读取后合并草稿；本次未覆盖文件"};
    GError *error = nullptr;
    const bool saved = g_file_set_contents_full(path.c_str(), content.data(), static_cast<gssize>(content.size()),
        static_cast<GFileSetContentsFlags>(G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0600, &error);
    std::string message;
    if (!saved) message = "保存快捷短语失败：" + std::string(error->message);
    g_clear_error(&error);
    return {saved ? QuickPhraseSaveStatus::Saved : QuickPhraseSaveStatus::IoError, std::move(message)};
}
QuickPhraseReloadResult reloadQuickPhrases() {
    GError *error = nullptr;
    auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    GVariant *reply = nullptr;
    if (bus) {
        // editor 子配置只读取短语文件，不保存/更改 Fcitx5 的触发键。
        reply = g_dbus_connection_call_sync(bus, "org.fcitx.Fcitx5", "/controller",
            "org.fcitx.Fcitx.Controller1", "SetConfig",
            g_variant_new("(sv)", "fcitx://config/addon/quickphrase/editor",
                          g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
            nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, &error);
        g_object_unref(bus);
    }
    const bool applied = reply != nullptr;
    if (reply) g_variant_unref(reply);
    std::string message;
    if (!applied) message = "暂未应用，请启动 Fcitx5 并启用“快速输入”插件后重试：" +
        std::string(error ? error->message : "未收到模块响应");
    g_clear_error(&error);
    return {applied, std::move(message)};
}
} // namespace modernime::settings
