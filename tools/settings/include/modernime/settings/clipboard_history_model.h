#pragma once

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

namespace modernime::settings {

class ClipboardHistoryModel final {
public:
    explicit ClipboardHistoryModel(std::filesystem::path path);
    ClipboardHistoryModel(std::filesystem::path path, std::vector<std::string> entries);

    bool reload(std::string *error = nullptr);
    bool remove(std::size_t index, std::string *error = nullptr);
    bool remove(std::size_t index, std::string_view expectedText, std::string *error = nullptr);
    bool clear(std::string *error = nullptr);

    const std::vector<std::string> &entries() const { return entries_; }
    void setEntries(std::vector<std::string> entries) { entries_ = std::move(entries); }

private:
    std::filesystem::path path_;
    std::vector<std::string> entries_;
};

} // namespace modernime::settings
