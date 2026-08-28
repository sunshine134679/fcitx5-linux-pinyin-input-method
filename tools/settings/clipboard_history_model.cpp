#include "modernime/settings/clipboard_history_model.h"

#include "modernime/core/clipboard_history.h"

#include <utility>

namespace modernime::settings {

ClipboardHistoryModel::ClipboardHistoryModel(std::filesystem::path path)
    : path_(std::move(path)) {}

bool ClipboardHistoryModel::reload(std::string *error) {
    core::ClipboardHistory history;
    if (!history.load(path_, error)) {
        entries_.clear();
        return false;
    }
    entries_ = history.entries();
    return true;
}

} // namespace modernime::settings
