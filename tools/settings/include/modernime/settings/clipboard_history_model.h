#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace modernime::settings {

class ClipboardHistoryModel final {
public:
    explicit ClipboardHistoryModel(std::filesystem::path path);

    bool reload(std::string *error = nullptr);

    const std::vector<std::string> &entries() const { return entries_; }

private:
    std::filesystem::path path_;
    std::vector<std::string> entries_;
};

} // namespace modernime::settings
