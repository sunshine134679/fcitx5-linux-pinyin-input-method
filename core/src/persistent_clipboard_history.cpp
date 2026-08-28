#include "modernime/core/persistent_clipboard_history.h"

#include <utility>

namespace modernime::core {
namespace {

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

} // namespace

PersistentClipboardHistory::PersistentClipboardHistory(
    std::filesystem::path path)
    : path_(std::move(path)) {}

bool PersistentClipboardHistory::load(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    const bool result = history_.load(path_, error);
    loaded_ = result;
    dirty_ = false;
    return result;
}

bool PersistentClipboardHistory::observe(std::string_view text,
                                         std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!loaded_) {
        setError(error, "clipboard history is not loaded");
        return false;
    }

    const auto changed = history_.observe(text);
    if (changed) {
        dirty_ = true;
    }
    if (!dirty_) {
        return changed;
    }

    if (!persist(error)) {
        return changed;
    }
    return changed;
}

bool PersistentClipboardHistory::flush(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!loaded_) {
        setError(error, "clipboard history is not loaded");
        return false;
    }
    return persist(error);
}

bool PersistentClipboardHistory::persist(std::string *error) {
    if (!history_.save(path_, error)) {
        return false;
    }
    dirty_ = false;
    return true;
}

} // namespace modernime::core
