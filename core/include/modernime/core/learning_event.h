#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace modernime::core {

struct LearningEvent final {
    enum class Kind { Selection, NegativeFeedback, Suppression };

    Kind kind = Kind::Selection;
    std::string phrase;
    std::string pinyin;
    std::string contextBefore;
    std::string contextAfter;
    std::int64_t nowMs = 0;
    // Queued events from before a settings clear must not recreate deleted rows.
    std::optional<std::int64_t> resetRevision = std::nullopt;
};

} // namespace modernime::core
