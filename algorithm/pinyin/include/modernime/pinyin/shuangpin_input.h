#pragma once

#include <libime/pinyin/pinyinencoder.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libime { class ShuangpinProfile; }

namespace modernime::pinyin {

// A per-request view of LibIME's code segmentation. Raw offsets never become
// learning keys; every reading remains ordinary, quote-separated full pinyin.
class ShuangpinInput final {
public:
    ShuangpinInput(std::string_view raw, const libime::ShuangpinProfile &profile,
                  libime::PinyinFuzzyFlags flags);

    struct Match { std::size_t consumed; int priority; };
    std::optional<Match> match(std::string_view fullPinyin) const;
    std::vector<std::string> recallInputs() const;
    std::vector<std::size_t> prefixEnds() const;
    const std::string &preedit() const { return preedit_; }

private:
    struct Segment {
        std::size_t end;
        libime::MatchedPinyinSyllables exact;
        libime::MatchedPinyinSyllables fuzzy;
    };
    std::size_t rawSize_ = 0;
    std::vector<Segment> segments_;
    std::string preedit_;
};

} // namespace modernime::pinyin
