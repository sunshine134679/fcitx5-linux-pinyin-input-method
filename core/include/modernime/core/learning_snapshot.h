#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

struct LearningEntry final {
    std::string phrase;
    std::string pinyin;
    std::string contextBefore;
    std::string contextAfter;
    std::int64_t frequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t negativeFeedback = 0;
    bool suppressed = false;
};

std::string normalizePinyin(std::string_view pinyin);

class LearningSnapshot final {
public:
    LearningSnapshot() = default;
    explicit LearningSnapshot(std::vector<LearningEntry> entries);

    const LearningEntry *entry(std::string_view phrase,
                               std::string_view pinyin,
                               std::string_view contextBefore,
                               std::string_view contextAfter) const;
    double boostAt(std::string_view phrase, std::string_view pinyin,
                   std::string_view contextBefore,
                   std::string_view contextAfter,
                   std::int64_t nowMs) const;
    bool isSuppressed(std::string_view phrase,
                      std::string_view pinyin) const;
    double contextBoost(std::string_view phrase, std::string_view pinyin,
                        std::string_view contextBefore,
                        std::string_view contextAfter) const;

    const std::vector<LearningEntry> &entries() const { return entries_; }

    void recordSelection(std::string_view phrase, std::string_view pinyin,
                         std::string_view contextBefore,
                         std::string_view contextAfter, std::int64_t nowMs);
    void recordNegativeFeedback(std::string_view phrase,
                                std::string_view pinyin);
    void recordSuppression(std::string_view phrase,
                           std::string_view pinyin);

private:
    LearningEntry *mutableEntry(std::string_view phrase,
                                std::string_view pinyin,
                                std::string_view contextBefore,
                                std::string_view contextAfter);

    std::vector<LearningEntry> entries_;
};

} // namespace modernime::core
