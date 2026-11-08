#include "modernime/pinyin/shuangpin_input.h"
#include <libime/pinyin/shuangpinprofile.h>
#include <algorithm>
#include <stdexcept>

namespace modernime::pinyin {
namespace {
// Invalid final is LibIME's unfinished initial, not an invalid reading.
int matches(const libime::MatchedPinyinSyllables &readings,
            libime::PinyinInitial initial, libime::PinyinFinal final) {
    for (const auto &[i, finals] : readings) {
        if (i != initial || i == libime::PinyinInitial::Invalid) continue;
        for (const auto &[f, fuzzy] : finals) {
            if (f == final) return fuzzy ? 0 : 2;
            if (f == libime::PinyinFinal::Invalid) return 1;
        }
    }
    return -1;
}
}

ShuangpinInput::ShuangpinInput(std::string_view raw,
                             const libime::ShuangpinProfile &profile,
                             libime::PinyinFuzzyFlags flags)
    : rawSize_(raw.size()) {
    // PartialSp is not enabled: the native graph has one code segmentation.
    auto graph = libime::PinyinEncoder::parseUserShuangpin(std::string(raw), profile, flags);
    const auto *node = &graph.start();
    while (node->nextSize()) {
        const auto *next = &node->nexts().front();
        const auto code = graph.segment(*node, *next);
        if (!code.empty() && code.front() != '\'') {
            if (!preedit_.empty()) preedit_ += '\'';
            preedit_ += code;
            auto end = next->index();
            while (end < raw.size() && raw[end] == '\'') ++end;
            segments_.push_back({end,
                libime::PinyinEncoder::shuangpinToSyllables(code, profile, libime::PinyinFuzzyFlag::None),
                libime::PinyinEncoder::shuangpinToSyllables(code, profile, flags)});
        }
        node = next;
    }
    if (!raw.empty() && raw.back() == '\'') preedit_ += '\'';
}

std::optional<ShuangpinInput::Match> ShuangpinInput::match(std::string_view pinyin) const {
    std::vector<char> encoded;
    try { encoded = libime::PinyinEncoder::encodeFullPinyin(pinyin); }
    catch (const std::invalid_argument &) { return std::nullopt; }
    const auto count = encoded.size() / 2;
    if (count == 0 || count > segments_.size()) return std::nullopt;
    int priority = 2;
    for (std::size_t i = 0; i < count; ++i) {
        const auto initial = static_cast<libime::PinyinInitial>(encoded[2 * i]);
        const auto final = static_cast<libime::PinyinFinal>(encoded[2 * i + 1]);
        auto quality = matches(segments_[i].exact, initial, final);
        if (quality < 0) quality = matches(segments_[i].fuzzy, initial, final);
        if (quality < 0) return std::nullopt;
        priority = std::min(priority, quality);
    }
    const auto consumed = segments_[count - 1].end;
    return Match{consumed, consumed < rawSize_ ? -1 : priority};
}

std::vector<std::string> ShuangpinInput::recallInputs() const {
    std::vector<std::string> inputs{std::string{}};
    for (const auto &segment : segments_) {
        std::vector<std::string> next;
        for (const auto &[initial, finals] : segment.exact) {
            if (initial == libime::PinyinInitial::Invalid) continue;
            for (const auto &[final, fuzzy] : finals) {
                if (fuzzy) continue;
                const auto syllable = libime::PinyinSyllable(initial, final).toString();
                if (syllable.empty()) continue;
                for (const auto &prefix : inputs) {
                    const auto value = prefix.empty() ? syllable : prefix + "'" + syllable;
                    if (next.size() < 16 && std::find(next.begin(), next.end(), value) == next.end())
                        next.push_back(value);
                }
            }
        }
        inputs = std::move(next);
        if (inputs.empty()) break;
    }
    return inputs;
}

std::vector<std::size_t> ShuangpinInput::prefixEnds() const {
    std::vector<std::size_t> ends;
    for (const auto &segment : segments_)
        if (segment.end < rawSize_) ends.push_back(segment.end);
    return ends;
}
} // namespace modernime::pinyin
