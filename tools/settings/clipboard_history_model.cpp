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

bool ClipboardHistoryModel::remove(std::size_t index, std::string *error) {
    core::ClipboardHistory history;
    if (!history.load(path_, error)) {
        return false;
    }
    if (!history.remove(index)) {
        if (error != nullptr) {
            *error = "剪贴板历史序号无效";
        }
        return false;
    }
    if (!history.save(path_, error)) {
        return false;
    }
    entries_ = history.entries();
    return true;
}

bool ClipboardHistoryModel::clear(std::string *error) {
    core::ClipboardHistory history;
    if (!history.load(path_, error)) {
        return false;
    }
    history.clear();
    if (!history.save(path_, error)) {
        return false;
    }
    entries_.clear();
    return true;
}

} // namespace modernime::settings
