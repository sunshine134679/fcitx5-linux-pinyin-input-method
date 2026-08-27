#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

enum class CandidateSource { Engine, UserDictionary, Learned, Raw };
enum class CandidatePageMode { Pinyin, Clipboard };

std::string candidateOrderKey(std::string_view text,
                              std::string_view fullPinyin);

struct CandidateItem final {
    std::string text;
    std::string fullPinyin;
    std::size_t sourceIndex = 0;
    CandidateSource source = CandidateSource::Engine;
};

struct CandidatePage final {
    std::string preedit;
    std::vector<CandidateItem> items;
    std::size_t cursor = 0;
    std::uint64_t generation = 0;
    CandidatePageMode mode = CandidatePageMode::Pinyin;

    bool select(std::size_t index);
    void clear();
};

} // namespace modernime::core
