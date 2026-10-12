#pragma once

#include "modernime/core/commit_event.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modernime::core {

struct FormedPhrase final {
    std::string text;
    std::string fullPinyin;
    std::string rawKeystrokes;
    int repetitionCount = 0;
    bool isInitialFormation = false;
};

struct PhraseFormationStats final {
    struct SequenceStats {
        int count = 0;
        std::int64_t lastObservedMs = 0;
        bool formed = false;
    };

    std::unordered_map<std::string, SequenceStats> sequenceStats;
    mutable std::mutex mutex;

    void clear() {
        std::lock_guard lock(mutex);
        sequenceStats.clear();
    }
};

class PhraseFormationTracker final {
public:
    explicit PhraseFormationTracker(
        int repetitionThreshold = 3,
        std::shared_ptr<PhraseFormationStats> sharedStats = nullptr);

    // Intra-composition (分段选词):
    void recordPartialSelection(std::string_view text,
                                std::string_view fullPinyin,
                                std::string_view rawInput,
                                std::size_t consumedBytes);
    std::optional<FormedPhrase> recordFinalSelection(
        std::string_view text, std::string_view fullPinyin,
        std::string_view rawInput, std::size_t consumedBytes);
    void cancelComposition();

    // Inter-composition (跨组合重复提交):
    std::vector<FormedPhrase> recordCommit(const CommitEvent &event);

    // Boundary resets:
    void resetCommitHistory();
    void clearAllStats();

    int repetitionThreshold() const { return repetitionThreshold_; }
    void setRepetitionThreshold(int threshold) { repetitionThreshold_ = threshold; }

private:
    struct Segment {
        std::string text;
        std::string fullPinyin;
        std::string rawKeystrokes;
        std::size_t consumedBytes = 0;
        std::int64_t timestampMs = 0;
        std::string contextBefore;
    };

    static bool isAllChineseIdeographs(std::string_view text);
    static std::string joinPinyin(const std::vector<Segment> &segments);
    static std::string joinText(const std::vector<Segment> &segments);
    static std::string joinKeystrokes(const std::vector<Segment> &segments);

    int repetitionThreshold_ = 5;
    std::shared_ptr<PhraseFormationStats> sharedStats_;

    // Intra-composition buffer:
    std::vector<Segment> intraSegments_;
    std::string initialCompositionRawInput_;

    // Inter-composition buffer:
    std::deque<Segment> consecutiveCommits_;
    std::int64_t lastCommitTimestampMs_ = 0;
    std::string lastCommittedText_;
};

} // namespace modernime::core
