#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace modernime::core {

enum class CommitCandidateRole {
    Chinese,        // 中文词汇 / 单字
    UserDictionary, // 用户词典词条
    English,        // 英文单词 / 英文术语
    Raw,            // 原始输入原文回车
    Command         // 命令项（剪贴板等）
};

struct CommitEvent final {
    std::string text;                   // 提交上屏的文本
    std::string fullPinyin;             // 规范完整拼音
    std::string rawKeystrokes;          // 原始输入击键
    CommitCandidateRole role = CommitCandidateRole::Chinese;
    bool isPartial = false;             // 是否为分段部分选词
    std::size_t consumedInputBytes = 0; // 消耗的输入字节数
    std::string contextBefore;          // 光标前上下文
    std::string contextAfter;           // 光标后上下文
    std::int64_t timestampMs = 0;       // 提交时间戳 (毫秒)

    bool operator==(const CommitEvent &) const = default;
};

} // namespace modernime::core
