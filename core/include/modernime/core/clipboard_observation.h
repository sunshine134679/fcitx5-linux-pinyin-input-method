#pragma once

#include "modernime/core/clipboard_history.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace modernime::core {

// Filters observation identity. Baselines do not create history operations.
class ClipboardObservationTracker final {
public:
    void setBaseline(std::string_view text);
    bool accept(std::string_view text);
private:
    std::string lastObserved_;
    bool hasObserved_ = false;
};

// A bounded transformation of an as-yet unknown history, not a history copy.
// Its first value is conditional on the base's top entry. Every later distinct
// transition is eligible for pending persistence, including after compaction.
class ClipboardObservationBatch final {
public:
    void observe(std::string_view text);
    void append(const ClipboardObservationBatch &next);
    bool apply(ClipboardHistory &history, ClipboardHistory &pending) const;
    bool empty() const { return !first_; }
    std::size_t retainedSize() const { return (first_ ? 1 : 0) + tail_.entries().size(); }
private:
    std::optional<std::string> first_;
    ClipboardHistory tail_;
};

} // namespace modernime::core
