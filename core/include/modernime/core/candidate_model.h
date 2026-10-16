#pragma once

#include "modernime/core/commit_event.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

enum class CandidateSource { Engine, UserDictionary, Learned, Raw, EnglishDefinition, Command };
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
    std::string annotation = {};
    double learningBoost = 0.0;
    bool isLexical = false;
    double recentChoiceBonus = 0.0;
    std::optional<CommitCandidateRole> commitRole;
};

struct CandidateIdentity {
    std::string text;
    std::string fullPinyin;
    CommitCandidateRole role = CommitCandidateRole::Chinese;
    std::size_t consumedInputBytes = 0;
    bool operator==(const CandidateIdentity &) const = default;
};

struct CandidateSelectionRequest {
    std::uint64_t sessionId = 0;
    std::uint64_t displayGeneration = 0;
    std::size_t globalIndex = 0;
    CandidateIdentity expected;
};

enum class SelectionAction { CommitText, OpenClipboard, NoCommit };

struct CandidateSelectionOutcome {
    CandidateIdentity selected;
    SelectionAction action = SelectionAction::CommitText;
    std::string remainingInput;
};

class SelectionEndpoint {
public:
    virtual ~SelectionEndpoint() = default;
    virtual bool select(const CandidateSelectionRequest &request) = 0;
    virtual bool setPageBoundaries(std::uint64_t displayGeneration,
                                   std::uint64_t layoutGeneration,
                                   std::vector<PageBoundary> boundaries) = 0;
    virtual bool moveCursor(std::ptrdiff_t delta) { (void)delta; return false; }
    virtual bool movePage(std::ptrdiff_t delta) { (void)delta; return false; }
    virtual std::size_t currentPageIndex() const { return 0; }
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
    std::uint64_t sessionId = 0;
    std::uint64_t displayGeneration = 0;
    CandidatePageMode mode = CandidatePageMode::Pinyin;

    bool select(std::size_t index);
    void clear();
};

CandidateIdentity candidateIdentity(const CandidateItem &item,
                                    const CandidatePage &page);

} // namespace modernime::core
