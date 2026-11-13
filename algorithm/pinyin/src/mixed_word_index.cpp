#include "modernime/pinyin/mixed_word_index.h"

#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <cstdint>

namespace modernime::pinyin {

bool MixedWordIndex::isMixedWord(std::string_view text) {
    bool latin = false;
    bool han = false;
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 128) {
            if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
                latin = true;
            } else {
                return false;
            }
            ++index;
            continue;
        }
        const std::size_t length = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc2 ? 2 : 0;
        if (length == 0 || index + length > text.size()) return false;
        std::uint32_t codepoint = byte & ((1U << (7 - length)) - 1U);
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if (!((codepoint >= 0x3400 && codepoint <= 0x9fff) ||
              (codepoint >= 0x20000 && codepoint <= 0x2ebef))) return false;
        han = true;
        index += length;
    }
    return latin && han;
}

void MixedWordIndex::add(const UserDictionaryEntry &entry) {
    if (!isMixedWord(entry.phrase)) return;
    auto &values = entries_[core::PinyinMatchPolicy::canonical(entry.pinyin)];
    if (values.size() < 16 &&
        std::none_of(values.begin(), values.end(), [&](const auto &value) {
            return value.phrase == entry.phrase && value.pinyin == entry.pinyin;
        })) {
        values.push_back(entry);
    }
}

const std::vector<UserDictionaryEntry> &MixedWordIndex::lookup(std::string_view input) const {
    static const std::vector<UserDictionaryEntry> empty;
    const auto found = entries_.find(core::PinyinMatchPolicy::canonical(input));
    return found == entries_.end() ? empty : found->second;
}

} // namespace modernime::pinyin
