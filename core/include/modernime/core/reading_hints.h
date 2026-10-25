#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace modernime::core {

struct ReadingHintEntry {
    const char *word;
    std::uint16_t afterCharIndex;  // item.text 内第几个 UTF-8 码点之后插入
    const char *canonicalToneless; // 该字在本词中的规范无声调读音，如 "qia"
    const char *reading;           // 带声调展示串，如 "qiǎ"
};

#include "modernime/core/reading_hints.inc"

// 易错音内联读音提示词表查询：命中返回词表条目，未收录返回 nullptr。
// 表按词的 UTF-8 字节序排序，二分查找。
inline const ReadingHintEntry *readingHintFor(std::string_view word) {
    int low = 0;
    int high = static_cast<int>(sizeof(kReadingHintEntries) /
                                sizeof(kReadingHintEntries[0]));
    while (low < high) {
        const int mid = (low + high) / 2;
        const int cmp = std::strncmp(
            kReadingHintEntries[mid].word, word.data(), word.size());
        if (cmp == 0 &&
            kReadingHintEntries[mid].word[word.size()] == '\0') {
            return &kReadingHintEntries[mid];
        }
        if (cmp < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return nullptr;
}

} // namespace modernime::core
