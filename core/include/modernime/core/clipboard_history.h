#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

class ClipboardHistory final {
public:
    static constexpr std::size_t kMaxEntries = 30;
    static constexpr std::size_t kMaxEntryBytes = 1024 * 1024;

    // Records a non-empty clipboard value at the front of the history.
    // Existing equal values are moved to the front instead of duplicated.
    // Returns whether the visible history order changed.
    bool observe(std::string_view text);

    void clear();

    const std::vector<std::string> &entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
};

} // namespace modernime::core
