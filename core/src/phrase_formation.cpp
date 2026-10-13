#include "modernime/core/phrase_formation.h"

#include <algorithm>
#include <chrono>

namespace modernime::core {
namespace {

std::size_t countUtf8CodePoints(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

} // namespace

PhraseFormationTracker::PhraseFormationTracker(
    int repetitionThreshold,
    std::shared_ptr<PhraseFormationStats> sharedStats)
    : repetitionThreshold_(repetitionThreshold),
      sharedStats_(sharedStats ? std::move(sharedStats)
                               : std::make_shared<PhraseFormationStats>()) {}

bool PhraseFormationTracker::isAllChineseIdeographs(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unsigned char c0 = static_cast<unsigned char>(text[offset]);
        char32_t cp = 0;
        std::size_t charLen = 1;
        if (c0 < 0x80) {
            return false;
        } else if ((c0 & 0xE0) == 0xC0 && offset + 2 <= text.size()) {
            cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(text[offset + 1]) & 0x3F);
            charLen = 2;
        } else if ((c0 & 0xF0) == 0xE0 && offset + 3 <= text.size()) {
            cp = ((c0 & 0x0F) << 12) |
                 ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[offset + 2]) & 0x3F);
            charLen = 3;
        } else if ((c0 & 0xF8) == 0xF0 && offset + 4 <= text.size()) {
            cp = ((c0 & 0x07) << 18) |
                 ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(text[offset + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[offset + 3]) & 0x3F);
            charLen = 4;
        } else {
            return false;
        }

        const bool isIdeograph =
            (cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x20000 && cp <= 0x2EBEF);
        if (!isIdeograph) {
            return false;
        }
        offset += charLen;
    }
    return true;
}

std::string PhraseFormationTracker::joinPinyin(
    const std::vector<Segment> &segments) {
    std::string joined;
    for (const auto &seg : segments) {
        if (seg.fullPinyin.empty()) {
            return {};
        }
        std::string_view p = seg.fullPinyin;
        while (!p.empty() && p.front() == '\'') {
            p.remove_prefix(1);
        }
        while (!p.empty() && p.back() == '\'') {
            p.remove_suffix(1);
        }
        if (p.empty()) {
            return {};
        }
        if (!joined.empty()) {
            joined.push_back('\'');
        }
        joined.append(p);
    }
    return joined;
}

std::string PhraseFormationTracker::joinText(
    const std::vector<Segment> &segments) {
    std::string text;
    for (const auto &seg : segments) {
        text.append(seg.text);
    }
    return text;
}

std::string PhraseFormationTracker::joinKeystrokes(
    const std::vector<Segment> &segments) {
    std::string keys;
    for (const auto &seg : segments) {
        keys.append(seg.rawKeystrokes);
    }
    return keys;
}

void PhraseFormationTracker::recordPartialSelection(
    std::string_view text, std::string_view fullPinyin,
    std::string_view rawInput, std::size_t consumedBytes) {
    if (intraSegments_.empty()) {
        initialCompositionRawInput_ = std::string(rawInput);
    }
    Segment seg;
    seg.text = std::string(text);
    seg.fullPinyin = fullPinyin.empty()
                         ? std::string(rawInput.substr(0, consumedBytes))
                         : std::string(fullPinyin);
    seg.rawKeystrokes = std::string(rawInput.substr(0, consumedBytes));
    seg.consumedBytes = consumedBytes;
    intraSegments_.push_back(std::move(seg));
}

std::optional<FormedPhrase> PhraseFormationTracker::recordFinalSelection(
    std::string_view text, std::string_view fullPinyin,
    std::string_view rawInput, std::size_t consumedBytes) {
    if (intraSegments_.empty()) {
        return std::nullopt;
    }
    Segment seg;
    seg.text = std::string(text);
    seg.fullPinyin = fullPinyin.empty()
                         ? std::string(rawInput.substr(0, consumedBytes))
                         : std::string(fullPinyin);
    seg.rawKeystrokes = std::string(rawInput.substr(0, consumedBytes));
    seg.consumedBytes = consumedBytes;
    intraSegments_.push_back(std::move(seg));

    std::vector<Segment> segs = std::move(intraSegments_);
    intraSegments_.clear();

    const auto phraseText = joinText(segs);
    const auto phrasePinyin = joinPinyin(segs);
    if (!isAllChineseIdeographs(phraseText)) {
        return std::nullopt;
    }
    const auto charCount = countUtf8CodePoints(phraseText);
    if (charCount < 2 || charCount > 6) {
        return std::nullopt;
    }

    FormedPhrase fp;
    fp.text = phraseText;
    fp.fullPinyin = phrasePinyin;
    fp.rawKeystrokes = initialCompositionRawInput_.empty()
                           ? joinKeystrokes(segs)
                           : initialCompositionRawInput_;
    fp.repetitionCount = 1;
    fp.isInitialFormation = true;
    initialCompositionRawInput_.clear();
    return fp;
}

void PhraseFormationTracker::cancelComposition() {
    intraSegments_.clear();
    initialCompositionRawInput_.clear();
}

std::vector<FormedPhrase> PhraseFormationTracker::recordCommit(
    const CommitEvent &event) {
    std::vector<FormedPhrase> formed;
    if (event.isPartial || event.role != CommitCandidateRole::Chinese) {
        resetCommitHistory();
        return formed;
    }
    if (!isAllChineseIdeographs(event.text)) {
        resetCommitHistory();
        return formed;
    }

    // Check idle timeout (15 seconds between commits breaks transient sequence)
    if (lastCommitTimestampMs_ > 0 && event.timestampMs > 0 &&
        event.timestampMs - lastCommitTimestampMs_ > 15000) {
        consecutiveCommits_.clear();
    }

    // Check surrounding context continuity if available
    if (!event.contextBefore.empty() && !lastCommittedText_.empty()) {
        if (!event.contextBefore.ends_with(lastCommittedText_)) {
            consecutiveCommits_.clear();
        }
    }

    lastCommitTimestampMs_ = event.timestampMs;
    lastCommittedText_ = event.text;

    Segment seg;
    seg.text = event.text;
    seg.fullPinyin = event.fullPinyin.empty() ? event.rawKeystrokes : event.fullPinyin;
    seg.rawKeystrokes = event.rawKeystrokes;
    seg.consumedBytes = event.consumedInputBytes;
    seg.timestampMs = event.timestampMs;
    seg.contextBefore = event.contextBefore;

    consecutiveCommits_.push_back(std::move(seg));
    if (consecutiveCommits_.size() > 4) {
        consecutiveCommits_.pop_front();
    }

    const std::size_t n = consecutiveCommits_.size();
    std::lock_guard lock(sharedStats_->mutex);
    for (std::size_t k = 2; k <= n && k <= 4; ++k) {
        std::vector<Segment> window(consecutiveCommits_.end() - k,
                                    consecutiveCommits_.end());
        const auto phraseText = joinText(window);
        const auto phrasePinyin = joinPinyin(window);
        if (phraseText.empty() || phrasePinyin.empty()) {
            continue;
        }
        const auto charCount = countUtf8CodePoints(phraseText);
        if (charCount < 2 || charCount > 6) {
            continue;
        }

        const auto key = phraseText + "\x1f" + phrasePinyin;
        auto &stats = sharedStats_->sequenceStats[key];
        stats.count++;
        stats.lastObservedMs = event.timestampMs;

        if (stats.count == repetitionThreshold_) {
            stats.formed = true;
            formed.push_back({phraseText, phrasePinyin, joinKeystrokes(window),
                              stats.count, true});
        } else if (stats.formed && stats.count > repetitionThreshold_) {
            formed.push_back({phraseText, phrasePinyin, joinKeystrokes(window),
                              stats.count, false});
        }
    }

    // Bounded stats capacity
    if (sharedStats_->sequenceStats.size() > 500) {
        // Formed phrases already live in the learning store. Keep this
        // transient repetition cache recent, so 500 old formed sequences
        // cannot permanently evict every newly observed phrase.
        std::vector<std::string> oldest;
        oldest.reserve(sharedStats_->sequenceStats.size());
        for (const auto &[key, stats] : sharedStats_->sequenceStats) {
            (void)stats;
            oldest.push_back(key);
        }
        const auto eraseCount = oldest.size() - 400;
        std::partial_sort(oldest.begin(), oldest.begin() +
                              static_cast<std::ptrdiff_t>(eraseCount),
                          oldest.end(), [&](const auto &left, const auto &right) {
            const auto &a = sharedStats_->sequenceStats.at(left);
            const auto &b = sharedStats_->sequenceStats.at(right);
            if (a.lastObservedMs != b.lastObservedMs) {
                return a.lastObservedMs < b.lastObservedMs;
            }
            if (a.formed != b.formed) {
                return !a.formed;
            }
            return left < right;
        });
        for (std::size_t index = 0; index < eraseCount; ++index) {
            sharedStats_->sequenceStats.erase(oldest[index]);
        }
    }

    return formed;
}

void PhraseFormationTracker::resetCommitHistory() {
    consecutiveCommits_.clear();
    lastCommitTimestampMs_ = 0;
    lastCommittedText_.clear();
}

void PhraseFormationTracker::clearAllStats() {
    resetCommitHistory();
    cancelComposition();
    if (sharedStats_ != nullptr) {
        sharedStats_->clear();
    }
}

} // namespace modernime::core
