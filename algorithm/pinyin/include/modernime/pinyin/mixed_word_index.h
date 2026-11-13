#pragma once
#include "modernime/pinyin/user_dictionary.h"
#include <string_view>
#include <unordered_map>
#include <vector>
namespace modernime::pinyin {
// Literal Latin runs and Han characters share one lexical identity. The input
// key remains alphabetic, e.g. docker'rong'qi; display casing is preserved.
class MixedWordIndex final {
public:
    static bool isMixedWord(std::string_view text);
    void add(const UserDictionaryEntry &entry);
    const std::vector<UserDictionaryEntry> &lookup(std::string_view input) const;
private:
    std::unordered_map<std::string,std::vector<UserDictionaryEntry>> entries_;
};
}
