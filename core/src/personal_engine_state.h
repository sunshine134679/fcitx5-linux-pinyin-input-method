#pragma once

#include "modernime/core/personal_engine.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core::detail {

// Pure in-memory state representation for PersonalEngine staging.
// Destructor performs NO I/O, NO LMDB handle operations, and NO registry locks.
struct PersonalEngineState {
    using StoredVariant = PersonalEngine::StoredVariant;

    EngineConfig config;
    std::map<std::string, std::vector<StoredVariant>, std::less<>> db;
    std::map<std::string, std::vector<std::string>, std::less<>> compact_index;
    size_t total_entries = 0;
    int64_t reset_revision = 0;
    std::string active_context;
    uint32_t manual_clock_offset = 0;

    ~PersonalEngineState() = default;

    bool rebuildCompactIndex(std::string_view norm_key);
    void pruneContextVariants();
    void prune(uint32_t nowSeconds);
    bool recordUsage(std::string_view pinyin_key, std::string_view text, float initial_weight,
                     std::string_view context_before, std::string_view context_after,
                     uint32_t now_sec, int64_t now_ms);
    bool recordNegativeFeedback(std::string_view pinyin_key, std::string_view text);
    bool recordSuppression(std::string_view pinyin_key, std::string_view text);
};

} // namespace modernime::core::detail
