#include "modernime/settings/clipboard_history_model.h"

#include "modernime/core/clipboard_history.h"

#include <algorithm>
#include <utility>

namespace modernime::settings {

ClipboardHistoryModel::ClipboardHistoryModel(std::filesystem::path path)
    : path_(std::move(path)) {}

ClipboardHistoryModel::ClipboardHistoryModel(std::filesystem::path path, std::vector<std::string> entries)
    : path_(std::move(path)), entries_(std::move(entries)) {}

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
    return remove(index, std::string_view{}, error);
}

bool ClipboardHistoryModel::remove(std::size_t index, std::string_view expectedText, std::string *error) {
    if (entries_.empty()) {
        reload(nullptr);
    }
    std::string targetText;
    if (!expectedText.empty()) {
        targetText = std::string(expectedText);
        if (index < entries_.size() && entries_[index] == expectedText) {
            // Index matches expected text
        } else {
            auto it = std::find(entries_.begin(), entries_.end(), targetText);
            if (it == entries_.end()) {
                if (error != nullptr) {
                    *error = "选中的剪贴板历史已被删除";
                }
                return false;
            }
            index = static_cast<std::size_t>(it - entries_.begin());
        }
    } else {
        if (index >= entries_.size()) {
            if (error != nullptr) {
                *error = "剪贴板历史序号无效";
            }
            return false;
        }
        targetText = entries_[index];
    }

    core::ClipboardHistory committed;
    bool itemExisted = true;
    const bool ok = core::ClipboardHistory::updateFile(
        path_,
        [&](core::ClipboardHistory &history) {
            const auto &entries = history.entries();
            const auto selected = std::find(entries.begin(), entries.end(), targetText);
            if (selected == entries.end()) {
                itemExisted = false;
                return false;
            }
            return history.remove(static_cast<std::size_t>(selected - entries.begin()));
        },
        &committed,
        error);

    if (!ok) {
        if (!itemExisted) {
            reload(nullptr);
            if (error != nullptr) {
                *error = "选中的剪贴板历史已被删除";
            }
        }
        return false;
    }
    entries_ = committed.entries();
    return true;
}

bool ClipboardHistoryModel::clear(std::string *error) {
    core::ClipboardHistory committed;
    const bool ok = core::ClipboardHistory::updateFile(
        path_,
        [](core::ClipboardHistory &history) {
            history.clear();
            return true;
        },
        &committed,
        error);

    if (!ok) {
        return false;
    }
    entries_.clear();
    return true;
}

} // namespace modernime::settings
