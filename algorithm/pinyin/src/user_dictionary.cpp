#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/learning_snapshot.h"

#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace modernime::pinyin {
namespace {

std::atomic<std::uint64_t> gTempCounter{1};

std::string fnv1a64Hex(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return ss.str();
}

std::string keyFor(std::string_view pinyin, std::string_view phrase) {
    std::string key = modernime::core::normalizePinyin(pinyin);
    key.push_back('\x1f');
    key.append(phrase);
    return key;
}

bool validUserPinyin(std::string_view pinyin) {
    if (pinyin.empty()) {
        return false;
    }
    bool hasLetter = false;
    bool separator = false;
    for (const char character : pinyin) {
        const bool asciiLetter =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        if (asciiLetter) {
            hasLetter = true;
            separator = false;
            continue;
        }
        if (character != '\'' || !hasLetter || separator) {
            return false;
        }
        separator = true;
    }
    return hasLetter && !separator;
}

bool validUtf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuationCount = 1;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuationCount = 2;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuationCount = 3;
        } else {
            return false;
        }
        if (index + continuationCount >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
        }
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xa0U) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

std::string segmentedPinyin(std::string_view pinyin) {
    if (pinyin.find('\'') != std::string_view::npos) {
        return std::string(pinyin);
    }
    try {
        auto graph = libime::PinyinEncoder::parseUserPinyin(
            std::string(pinyin), libime::PinyinFuzzyFlag::None);
        // Edges always advance the input offset. Compute the longest suffix
        // once per node instead of enumerating exponentially many complete
        // paths (e.g. repeated "changan"). Retain the first edge on ties to
        // preserve the original DFS's first deepest segmentation.
        struct Suffix {
            std::size_t length = 0;
            const libime::SegmentGraphNode *next = nullptr;
        };
        std::unordered_map<const libime::SegmentGraphNode *, Suffix> suffixes;
        suffixes.emplace(&graph.end(), Suffix{});
        for (std::size_t offset = graph.size(); offset-- > 0;) {
            for (const auto &node : graph.nodes(offset)) {
                Suffix best;
                for (const auto &next : node.nexts()) {
                    const auto found = suffixes.find(&next);
                    if (found != suffixes.end() &&
                        found->second.length + 1 > best.length) {
                        best = {found->second.length + 1, &next};
                    }
                }
                if (best.next != nullptr) {
                    suffixes.emplace(&node, best);
                }
            }
        }
        const auto start = suffixes.find(&graph.start());
        if (start == suffixes.end() || start->second.next == nullptr) {
            return {};
        }
        std::vector<std::size_t> bestPath;
        bestPath.reserve(start->second.length);
        for (auto *node = &graph.start(); node != &graph.end();) {
            node = suffixes.at(node).next;
            bestPath.push_back(node->index());
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

UserDictionarySnapshot UserDictionary::loadSnapshot(
    const std::filesystem::path &path) {
    UserDictionarySnapshot snapshot;
    if (path.empty()) {
        snapshot.error = "词典路径为空";
        return snapshot;
    }

    std::error_code ec;
    snapshot.exists = std::filesystem::exists(path, ec);
    if (!snapshot.exists) {
        return snapshot;
    }

    if (std::filesystem::is_directory(path, ec)) {
        snapshot.readable = false;
        snapshot.error = "路径是一个目录，无法作为用户词典";
        return snapshot;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        snapshot.readable = false;
        snapshot.error = "无法打开词典文件读取";
        return snapshot;
    }

    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    if (input.bad()) {
        snapshot.readable = false;
        snapshot.error = "读取词典文件中断";
        return snapshot;
    }
    snapshot.readable = true;
    snapshot.contentHash = fnv1a64Hex(content);

    std::unordered_map<std::string, std::size_t> positions;
    std::istringstream stream(content);
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
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
            std::getline(fields, extra, '\t')) {
            snapshot.diagnostics.push_back(
                {lineNumber, line, "列数错误（需要拼音、词组、权重三列，以Tab分隔）"});
            continue;
        }
        if (pinyin.empty()) {
            snapshot.diagnostics.push_back({lineNumber, line, "拼音为空"});
            continue;
        }
        if (!validUserPinyin(pinyin)) {
            snapshot.diagnostics.push_back({lineNumber, line, "无效的拼音格式"});
            continue;
        }
        if (phrase.empty()) {
            snapshot.diagnostics.push_back({lineNumber, line, "词组为空"});
            continue;
        }
        if (!validUtf8(phrase)) {
            snapshot.diagnostics.push_back({lineNumber, line, "词组包含非法的UTF-8字符"});
            continue;
        }
        try {
            std::size_t consumed = 0;
            const auto weight = std::stof(weightText, &consumed);
            if (!std::isfinite(weight) || weight < 0.0F || consumed != weightText.size()) {
                snapshot.diagnostics.push_back({lineNumber, line, "无效的权重数值"});
                continue;
            }
            const auto normalized = modernime::core::normalizePinyin(pinyin);
            if (normalized.empty()) {
                snapshot.diagnostics.push_back({lineNumber, line, "拼音规范化失败"});
                continue;
            }
            UserDictionaryEntry entry{normalized, std::move(phrase), weight};
            const auto key = keyFor(entry.pinyin, entry.phrase);
            const auto [position, inserted] = positions.emplace(
                key, snapshot.entries.size());
            if (inserted) {
                snapshot.entries.push_back(std::move(entry));
            } else {
                snapshot.entries[position->second] = std::move(entry);
            }
        } catch (...) {
            snapshot.diagnostics.push_back({lineNumber, line, "权重数值解析异常"});
            continue;
        }
    }
    return snapshot;
}

UserDictionary UserDictionary::loadText(
    const std::filesystem::path &path) {
    auto snapshot = loadSnapshot(path);
    UserDictionary dictionary;
    std::unordered_map<std::string, std::size_t> positions;
    for (auto &entry : snapshot.entries) {
        const auto key = keyFor(entry.pinyin, entry.phrase);
        const auto [position, inserted] = positions.emplace(
            key, dictionary.entries_.size());
        if (inserted) {
            dictionary.entries_.push_back(std::move(entry));
        } else {
            dictionary.entries_[position->second] = std::move(entry);
        }
    }
    dictionary.entryIndex_ = std::move(positions);
    return dictionary;
}

bool UserDictionary::upsert(std::string_view pinyin, std::string_view phrase,
                            float weight) {
    if (!validUserPinyin(pinyin) || phrase.empty() || !validUtf8(phrase) ||
        !std::isfinite(weight) || weight < 0.0F) {
        return false;
    }
    const auto normalized = modernime::core::normalizePinyin(pinyin);
    if (normalized.empty()) {
        return false;
    }
    const auto key = keyFor(normalized, phrase);
    const auto iterator = entryIndex_.find(key);
    if (iterator == entryIndex_.end()) {
        entryIndex_.emplace(key, entries_.size());
        entries_.push_back({normalized, std::string(phrase), weight});
    } else {
        entries_[iterator->second].weight = weight;
    }
    return true;
}

bool UserDictionary::contains(std::string_view normalizedPinyin,
                              std::string_view phrase) const {
    const auto key = keyFor(normalizedPinyin, phrase);
    return entryIndex_.find(key) != entryIndex_.end();
}

bool UserDictionary::remove(std::string_view normalizedPinyin,
                            std::string_view phrase) {
    const auto key = keyFor(normalizedPinyin, phrase);
    const auto iterator = entryIndex_.find(key);
    if (iterator == entryIndex_.end()) {
        return false;
    }
    const auto index = iterator->second;
    entryIndex_.erase(iterator);
    entries_.erase(entries_.begin() + index);
    for (std::size_t i = index; i < entries_.size(); ++i) {
        entryIndex_[keyFor(entries_[i].pinyin, entries_[i].phrase)] = i;
    }
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

    const auto temporary = path.string() + ".tmp." +
                           std::to_string(getpid()) + "." +
                           std::to_string(gTempCounter.fetch_add(1));
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
            try {
                dictionary.addWord(index, segmented, entry.phrase, entry.weight);
            } catch (const std::invalid_argument &) {
                // Alphabetic input may still contain invalid or incomplete
                // syllables. One imported row must not abort the engine's
                // startup or escape the live dictionary reload callback.
                continue;
            }
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
