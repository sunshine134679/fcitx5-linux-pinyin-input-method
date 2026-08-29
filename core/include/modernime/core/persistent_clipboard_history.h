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
    // Reloads from disk when another process (e.g. the settings client)
    // modified the file since our last load or save; returns true when a
    // reload happened.
    bool reloadIfExternallyChanged(std::string *error = nullptr);

    const std::vector<std::string> &entries() const {
        return history_.entries();
    }

private:
    bool persist(std::string *error);

    std::filesystem::path path_;
    ClipboardHistory history_;
    bool loaded_ = false;
    bool dirty_ = false;
    std::filesystem::file_time_type knownMtime_{};
    bool knownMtimeValid_ = false;
};

} // namespace modernime::core
