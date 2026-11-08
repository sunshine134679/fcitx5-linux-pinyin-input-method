#include "modernime/core/clipboard_observation.h"

namespace modernime::core {

void ClipboardObservationTracker::setBaseline(std::string_view text) {
    // Oversize values can never enter history. Forgetting their bytes still
    // distinguishes them from every admissible later observation.
    if (text.size() > ClipboardHistory::kMaxEntryBytes) {
        lastObserved_.clear();
        hasObserved_ = false;
        return;
    }
    lastObserved_ = text;
    hasObserved_ = true;
}

bool ClipboardObservationTracker::accept(std::string_view text) {
    const bool changed = !hasObserved_ || text != lastObserved_;
    if (changed) setBaseline(text);
    return changed && !text.empty() && text.size() <= ClipboardHistory::kMaxEntryBytes;
}

void ClipboardObservationBatch::observe(std::string_view text) {
    if (text.empty() || text.size() > ClipboardHistory::kMaxEntryBytes) return;
    if (!first_) {
        first_ = text;
        return;
    }
    const auto &last = tail_.entries().empty() ? *first_ : tail_.entries().front();
    if (last != text) tail_.observe(text);
}

void ClipboardObservationBatch::append(const ClipboardObservationBatch &next) {
    if (next.empty()) return;
    if (this == &next) {
        const auto copy = next;
        append(copy);
        return;
    }
    observe(*next.first_);
    // Do not re-filter these transitions by their compressed predecessor:
    // each already had a distinct predecessor in the original sequence.
    for (auto it = next.tail_.entries().rbegin(); it != next.tail_.entries().rend(); ++it)
        tail_.observe(*it);
}

bool ClipboardObservationBatch::apply(ClipboardHistory &history,
                                      ClipboardHistory &pending) const {
    if (empty()) return false;
    const bool firstChanged = history.observe(*first_);
    if (firstChanged) pending.observe(*first_);
    for (auto it = tail_.entries().rbegin(); it != tail_.entries().rend(); ++it) {
        history.observe(*it);
        pending.observe(*it);
    }
    return firstChanged || !tail_.entries().empty();
}

} // namespace modernime::core
