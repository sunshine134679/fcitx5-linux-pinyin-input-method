#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::settings {
struct QuickPhraseEntry {
    std::string abbreviation;
    std::string text;
    bool operator==(const QuickPhraseEntry &) const = default;
};
struct QuickPhraseSnapshot {
    std::vector<QuickPhraseEntry> entries;
    // 内容与 exists 共同构成保存基线；必须原样传回，不以界面草稿替代。
    std::string content;
    std::string error;
    bool exists = false;
};
enum class QuickPhraseSaveStatus { Saved, Invalid, Conflict, IoError };
struct QuickPhraseSaveResult {
    QuickPhraseSaveStatus status = QuickPhraseSaveStatus::Invalid;
    std::string error;
    bool saved() const { return status == QuickPhraseSaveStatus::Saved; }
};
struct QuickPhraseReloadResult {
    bool applied = false;
    std::string error;
};
std::filesystem::path quickPhrasePath();
// 空字符串表示有效；缩写 1–30 个小写英文字母，文本为非空 UTF-8 单行。
std::string quickPhraseValidation(const QuickPhraseEntry &entry);
// 纯内存搜索，按缩写/内容匹配；返回原始 entries 下标，保留原序。
std::vector<std::size_t> filterQuickPhrases(std::span<const QuickPhraseEntry> entries,
                                         std::string_view query);
// 以下接口有阻塞 I/O，客户端应放在已有 Worker 中调用。
QuickPhraseSnapshot loadQuickPhrases(const std::filesystem::path &path);
// 编辑 entries 后整体保存可完成增删改；基线过期或读取失败时不覆盖文件。
QuickPhraseSaveResult saveQuickPhrases(const std::filesystem::path &path,
                                      const QuickPhraseSnapshot &baseline,
                                      const std::vector<QuickPhraseEntry> &entries);
// 与保存结果独立；只重载 QuickPhrase，不重启输入法、不改写全局配置。
QuickPhraseReloadResult reloadQuickPhrases();
} // namespace modernime::settings
