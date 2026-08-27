#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace libime {
class PinyinDictionary;
}

namespace modernime::pinyin {

struct UserDictionaryEntry final {
    std::string pinyin;
    std::string phrase;
    float weight = 0.0F;
};

class UserDictionary final {
public:
    static UserDictionary loadText(const std::filesystem::path &path);

    const std::vector<UserDictionaryEntry> &entries() const {
        return entries_;
    }
    bool contains(std::string_view normalizedPinyin,
                  std::string_view phrase) const;
    void addTo(libime::PinyinDictionary &dictionary, std::size_t index) const;
    bool removeFrom(libime::PinyinDictionary &dictionary, std::size_t index,
                    std::string_view pinyin, std::string_view phrase) const;

private:
    std::vector<UserDictionaryEntry> entries_;
};

} // namespace modernime::pinyin
