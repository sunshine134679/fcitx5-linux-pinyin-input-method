#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/learning_snapshot.h"

#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>

#include <cmath>
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace modernime::pinyin {
namespace {

std::string keyFor(std::string_view pinyin, std::string_view phrase) {
    std::string key = modernime::core::normalizePinyin(pinyin);
    key.push_back('\x1f');
    key.append(phrase);
    return key;
}

std::string segmentedPinyin(std::string_view pinyin) {
    if (pinyin.find('\'') != std::string_view::npos) {
        return std::string(pinyin);
    }
    try {
        auto graph = libime::PinyinEncoder::parseUserPinyin(
            std::string(pinyin), libime::PinyinFuzzyFlag::None);
        std::vector<std::size_t> bestPath;
        graph.dfs([&bestPath](const libime::SegmentGraphBase &,
                              const std::vector<std::size_t> &path) {
            if (path.size() > bestPath.size()) {
                bestPath = path;
            }
            return true;
        });
        if (bestPath.empty()) {
            return {};
        }
        std::string result;
        std::size_t previous = 0;
        for (const auto end : bestPath) {
            if (end <= previous || end > pinyin.size()) {
                return {};
            }
            if (!result.empty()) {
                result.push_back('\'');
            }
            result.append(pinyin.substr(previous, end - previous));
            previous = end;
        }
        return previous == pinyin.size() ? result : std::string();
    } catch (...) {
        return {};
    }
}

} // namespace

UserDictionary UserDictionary::loadText(
    const std::filesystem::path &path) {
    UserDictionary dictionary;
    std::ifstream input(path);
    if (!input) {
        return dictionary;
    }

    std::unordered_map<std::string, std::size_t> positions;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::string pinyin;
        std::string phrase;
        std::string weightText;
        std::string extra;
        std::istringstream fields(line);
        if (!std::getline(fields, pinyin, '\t') ||
            !std::getline(fields, phrase, '\t') ||
            !std::getline(fields, weightText, '\t') ||
            std::getline(fields, extra, '\t') ||
            pinyin.empty() || phrase.empty()) {
            continue;
        }
        try {
            std::size_t consumed = 0;
            const auto weight = std::stof(weightText, &consumed);
            if (!std::isfinite(weight) || weight < 0.0F) {
                continue;
            }
            if (consumed != weightText.size()) {
                continue;
            }
            const auto normalized = modernime::core::normalizePinyin(pinyin);
            if (normalized.empty()) {
                continue;
            }
            UserDictionaryEntry entry{normalized, std::move(phrase), weight};
            const auto key = keyFor(entry.pinyin, entry.phrase);
            const auto [position, inserted] = positions.emplace(
                key, dictionary.entries_.size());
            if (inserted) {
                dictionary.entries_.push_back(std::move(entry));
            } else {
                dictionary.entries_[position->second] = std::move(entry);
            }
        } catch (...) {
            continue;
        }
    }
    return dictionary;
}

bool UserDictionary::contains(std::string_view normalizedPinyin,
                              std::string_view phrase) const {
    const auto normalized = modernime::core::normalizePinyin(normalizedPinyin);
    for (const auto &entry : entries_) {
        if (entry.pinyin == normalized && entry.phrase == phrase) {
            return true;
        }
    }
    return false;
}

bool UserDictionary::remove(std::string_view normalizedPinyin,
                            std::string_view phrase) {
    const auto normalized = modernime::core::normalizePinyin(normalizedPinyin);
    const auto iterator = std::find_if(
        entries_.begin(), entries_.end(), [&normalized, phrase](const auto &entry) {
            return entry.pinyin == normalized && entry.phrase == phrase;
        });
    if (iterator == entries_.end()) {
        return false;
    }
    entries_.erase(iterator);
    return true;
}

bool UserDictionary::saveText(const std::filesystem::path &path) const {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
    }

    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        return false;
    }
    output << std::setprecision(9);
    for (const auto &entry : entries_) {
        output << entry.pinyin << '\t' << entry.phrase << '\t' << entry.weight
               << '\n';
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, error);
        return false;
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

void UserDictionary::addTo(libime::PinyinDictionary &dictionary,
                           std::size_t index) const {
    for (const auto &entry : entries_) {
        const auto segmented = segmentedPinyin(entry.pinyin);
        if (!segmented.empty()) {
            dictionary.addWord(index, segmented, entry.phrase, entry.weight);
        }
    }
}

bool UserDictionary::removeFrom(libime::PinyinDictionary &dictionary,
                                std::size_t index, std::string_view pinyin,
                                std::string_view phrase) const {
    const auto segmented = segmentedPinyin(
        modernime::core::normalizePinyin(pinyin));
    return !segmented.empty() &&
           dictionary.removeWord(index, segmented, phrase);
}

} // namespace modernime::pinyin
