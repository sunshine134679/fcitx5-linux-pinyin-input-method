#include "modernime/core/clipboard_history.h"

#include <algorithm>

namespace modernime::core {

bool ClipboardHistory::observe(std::string_view text) {
    if (text.empty() || text.size() > kMaxEntryBytes) {
        return false;
    }

    const auto existing = std::find(entries_.begin(), entries_.end(), text);
    if (existing != entries_.end()) {
        if (existing == entries_.begin()) {
            return false;
        }
        std::string value = std::move(*existing);
        entries_.erase(existing);
        entries_.insert(entries_.begin(), std::move(value));
        return true;
    }

    entries_.insert(entries_.begin(), std::string(text));
    if (entries_.size() > kMaxEntries) {
        entries_.pop_back();
    }
    return true;
}

void ClipboardHistory::clear() { entries_.clear(); }

} // namespace modernime::core
