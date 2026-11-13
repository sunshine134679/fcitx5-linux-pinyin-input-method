#pragma once
#include "modernime/pinyin/user_dictionary.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace modernime::pinyin {
struct DictionaryPackManifest final {
    std::string id, name, source, license, version;
    std::size_t entryCount = 0;
    bool enabled = true;
};
struct DictionaryPack final {
    DictionaryPackManifest manifest;
    std::vector<UserDictionaryEntry> entries;
};
// Explicit management operations perform I/O. Candidate requests only read the
// immutable indexes published by PinyinSharedResources, never this directory.
class DictionaryPackStore final {
public:
    explicit DictionaryPackStore(std::filesystem::path root);
    const std::filesystem::path &root() const noexcept { return root_; }
    std::optional<DictionaryPack> validate(const std::filesystem::path &directory,
                                           std::string *error = nullptr) const;
    std::vector<DictionaryPackManifest> list() const;
    std::vector<DictionaryPack> loadEnabled(std::string *error = nullptr) const;
    bool importPack(const std::filesystem::path &directory, bool replace = false,
                    std::string *error = nullptr) const;
    bool setEnabled(std::string_view id, bool enabled, std::string *error = nullptr) const;
    bool remove(std::string_view id, std::string *error = nullptr) const;
private:
    std::filesystem::path root_;
};
}
