#pragma once

#include "modernime/core/clipboard_history.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

// Owns the lifecycle of a clipboard history file. A changed history is
// persisted immediately, and flush() is used before the input method reloads
// or exits.
class PersistentClipboardHistory final {
public:
    explicit PersistentClipboardHistory(std::filesystem::path path);

    bool load(std::string *error = nullptr);
    bool observe(std::string_view text, std::string *error = nullptr);
    bool flush(std::string *error = nullptr);

    const std::vector<std::string> &entries() const {
        return history_.entries();
    }

private:
    bool persist(std::string *error);

    std::filesystem::path path_;
    ClipboardHistory history_;
    bool loaded_ = false;
    bool dirty_ = false;
};

} // namespace modernime::core
