#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

enum class CandidateSource { Engine, UserDictionary, Learned, Raw };
enum class CandidatePageMode { Pinyin, Clipboard };

struct PageBoundary final {
    std::size_t begin = 0;
    std::size_t end = 0;

    bool operator==(const PageBoundary &) const = default;
};

std::string candidateOrderKey(std::string_view text,
                              std::string_view fullPinyin);

struct CandidateItem final {
    std::string text;
    std::string fullPinyin;
    std::size_t sourceIndex = 0;
    CandidateSource source = CandidateSource::Engine;
    // Number of bytes consumed from CandidatePage::rawInput. Zero keeps the
    // legacy meaning of consuming the complete composition.
    std::size_t consumedInputBytes = 0;
};

struct CandidatePage final {
    static constexpr std::size_t kCursorAtEnd =
        std::numeric_limits<std::size_t>::max();
    std::string rawInput;
    std::string preedit;
    std::size_t preeditCursor = kCursorAtEnd;
    std::vector<CandidateItem> items;
    std::vector<PageBoundary> pageBoundaries;
    std::size_t cursor = 0;
    std::uint64_t generation = 0;
    CandidatePageMode mode = CandidatePageMode::Pinyin;

    bool select(std::size_t index);
    void clear();
};

} // namespace modernime::core
