#include "modernime/core/clipboard_queries.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace modernime::core {
namespace {

void setError(std::string *error, std::string_view message) {
    if (error != nullptr) {
        *error = message;
    }
}

unsigned char foldAscii(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

bool contains(std::string_view text, std::string_view query) {
    return query.empty() ||
           std::search(text.begin(), text.end(), query.begin(), query.end(),
                       [](char lhs, char rhs) {
                           return foldAscii(lhs) == foldAscii(rhs);
                       }) != text.end();
}

} // namespace

ClipboardFavorites::ClipboardFavorites(std::filesystem::path path)
    : path_(std::move(path)) {}

bool ClipboardFavorites::load(std::string *error) {
    return history_.load(path_, error);
}

bool ClipboardFavorites::update(
    const std::function<bool(ClipboardHistory &)> &mutation, std::string *error) {
    ClipboardHistory committed;
    if (!ClipboardHistory::updateFile(path_, mutation, &committed, error)) {
        return false;
    }
    history_ = std::move(committed);
    return true;
}

bool ClipboardFavorites::add(std::string_view text, std::string *error) {
    return update([&](ClipboardHistory &current) {
        const auto &entries = current.entries();
        if (std::find(entries.begin(), entries.end(), text) != entries.end()) {
            return true;
        }
        if (text.empty() || text.size() > ClipboardHistory::kMaxEntryBytes) {
            setError(error, "收藏内容须为1至1048576字节");
            return false;
        }
        if (entries.size() >= ClipboardHistory::kMaxEntries) {
            setError(error, "收藏已满（最多30条）");
            return false;
        }
        return current.observe(text);
    }, error);
}

bool ClipboardFavorites::remove(std::string_view expectedText,
                                std::string *error) {
    return update([&](ClipboardHistory &current) {
        const auto &entries = current.entries();
        const auto found = std::find(entries.begin(), entries.end(), expectedText);
        if (found == entries.end()) {
            setError(error, "该收藏已不存在，请刷新后重试");
            return false;
        }
        return current.remove(static_cast<std::size_t>(found - entries.begin()));
    }, error);
}

bool ClipboardFavorites::clear(std::string *error) {
    return update([](ClipboardHistory &current) {
        current.clear();
        return true;
    }, error);
}

std::vector<ClipboardSearchMatch> searchClipboard(
    const std::vector<std::string> &history,
    const std::vector<std::string> &favorites, std::string_view query,
    bool favoritesOnly, std::size_t limit) {
    std::vector<ClipboardSearchMatch> matches;
    if (limit == 0) {
        return matches;
    }
    matches.reserve(std::min(limit, ClipboardHistory::kMaxEntries * 2));

    std::unordered_map<std::string_view, std::size_t> historyIndices;
    historyIndices.reserve(history.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
        historyIndices.try_emplace(history[i], i);
    }
    std::unordered_set<std::string_view> seen;
    const auto append = [&](std::string_view text, bool favorite,
                            std::optional<std::size_t> historyIndex) {
        if (seen.insert(text).second && contains(text, query)) {
            matches.push_back({text, favorite, historyIndex});
        }
    };
    for (const auto &text : favorites) {
        const auto found = historyIndices.find(text);
        append(text, true, found == historyIndices.end()
                               ? std::nullopt
                               : std::optional<std::size_t>(found->second));
        if (matches.size() == limit) {
            return matches;
        }
    }
    if (!favoritesOnly) {
        for (std::size_t i = 0; i < history.size(); ++i) {
            append(history[i], false, i);
            if (matches.size() == limit) {
                break;
            }
        }
    }
    return matches;
}

} // namespace modernime::core
