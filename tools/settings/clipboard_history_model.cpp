#include "modernime/settings/clipboard_history_model.h"

#include "modernime/core/clipboard_history.h"

#include <algorithm>
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
    if (index >= entries_.size()) {
        if (error != nullptr) {
            *error = "剪贴板历史序号无效";
        }
        return false;
    }
    const auto selectedText = entries_[index];
    core::ClipboardHistory history;
    if (!history.load(path_, error)) {
        return false;
    }
    const auto selected = std::find(history.entries().begin(),
                                    history.entries().end(), selectedText);
    if (selected == history.entries().end()) {
        entries_ = history.entries();
        if (error != nullptr) {
            *error = "选中的剪贴板历史已被删除";
        }
        return false;
    }
    if (!history.remove(static_cast<std::size_t>(
            selected - history.entries().begin()))) {
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
