#include "modernime/settings/dictionary_pronunciation.h"

#include <glib.h>
#include <libime/pinyin/pinyindictionary.h>
#include <algorithm>
#include <exception>
#include <sstream>
#include <unordered_set>

namespace modernime::settings {
namespace {

std::string_view wordAt(const std::string &text, std::size_t offset) {
    return std::string_view(text).substr(offset, text.find('\t', offset) - offset);
}

bool isHan(gunichar character) {
    return (character >= 0x3400 && character <= 0x9fff) ||
           (character >= 0xf900 && character <= 0xfaff) ||
           (character >= 0x20000 && character <= 0x323af);
}

} // namespace

DictionaryPronunciation DictionaryPronunciation::load(
    const std::vector<std::filesystem::path> &paths,
    const std::function<bool()> &cancelled) {
    DictionaryPronunciation result;
    std::unordered_map<gunichar, std::unordered_map<std::string, std::size_t>> counts;
    std::unordered_set<std::string> loaded;
    const auto stopped = [&] { return cancelled && cancelled(); };
    for (const auto &path : paths) {
        if (stopped()) return {};
        std::error_code error;
        if (!loaded.insert(path.string()).second ||
            !std::filesystem::is_regular_file(path, error)) continue;
        std::string text;
        try {
            libime::PinyinDictionary dictionary;
            dictionary.load(0, path.c_str(), libime::PinyinDictFormat::Binary);
            std::ostringstream output;
            dictionary.save(0, output, libime::PinyinDictFormat::Text);
            text = std::move(output).str();
        } catch (const std::exception &) {
            continue;
        }
        // Use LibIME's public text export, rather than its private trie encoding.
        std::size_t start = 0;
        std::size_t row = 0;
        while (start < text.size()) {
            if ((row++ % 1024) == 0 && stopped()) return {};
            const auto end = text.find('\n', start);
            const auto line = std::string_view(text).substr(
                start, (end == std::string::npos ? text.size() : end) - start);
            start = end == std::string::npos ? text.size() : end + 1;
            const auto first = line.find(' ');
            if (first == std::string_view::npos) continue;
            const auto second = line.find(' ', first + 1);
            if (second == std::string_view::npos) continue;
            const auto word = line.substr(0, first);
            const auto reading = line.substr(first + 1, second - first - 1);
            if (word.empty() || reading.empty() ||
                !g_utf8_validate(word.data(), static_cast<gssize>(word.size()), nullptr)) continue;
            result.offsets_.push_back(result.records_.size());
            result.records_.append(word).append("\t").append(reading).append("\n");

            // Only aligned all-Han words contribute character pronunciation hints.
            std::vector<gunichar> characters;
            for (auto *p = word.data(); p < word.data() + word.size(); p = g_utf8_next_char(p)) {
                const auto character = g_utf8_get_char(p);
                if (!isHan(character)) { characters.clear(); break; }
                characters.push_back(character);
            }
            std::vector<std::string> syllables;
            std::size_t position = 0;
            while (position < reading.size()) {
                const auto quote = reading.find('\'', position);
                syllables.emplace_back(reading.substr(position,
                    (quote == std::string_view::npos ? reading.size() : quote) - position));
                position = quote == std::string_view::npos ? reading.size() : quote + 1;
            }
            if (characters.size() != syllables.size()) continue;
            for (std::size_t index = 0; index < characters.size(); ++index) {
                ++counts[characters[index]][syllables[index]];
            }
        }
    }
    if (stopped()) return {};
    std::sort(result.offsets_.begin(), result.offsets_.end(), [&](auto left, auto right) {
        return wordAt(result.records_, left) < wordAt(result.records_, right);
    });
    for (const auto &[character, readings] : counts) {
        std::string best;
        std::size_t frequency = 0;
        for (const auto &[reading, count] : readings) {
            if (count > frequency || (count == frequency && reading < best)) {
                best = reading;
                frequency = count;
            }
        }
        result.characters_.emplace(character, std::move(best));
    }
    return stopped() ? DictionaryPronunciation{} : std::move(result);
}

PronunciationSuggestion DictionaryPronunciation::suggest(std::string_view phrase) const {
    PronunciationSuggestion result;
    if (phrase.empty() || phrase.size() > 1024 ||
        !g_utf8_validate(phrase.data(), static_cast<gssize>(phrase.size()), nullptr)) return result;
    auto row = std::lower_bound(offsets_.begin(), offsets_.end(), phrase,
        [&](auto offset, auto word) { return wordAt(records_, offset) < word; });
    for (; row != offsets_.end() && wordAt(records_, *row) == phrase; ++row) {
        const auto start = records_.find('\t', *row) + 1;
        result.readings.emplace_back(records_.substr(start, records_.find('\n', start) - start));
    }
    if (!result.readings.empty()) {
        std::sort(result.readings.begin(), result.readings.end());
        result.readings.erase(std::unique(result.readings.begin(), result.readings.end()), result.readings.end());
        return result;
    }
    std::string reading;
    for (auto *p = phrase.data(); p < phrase.data() + phrase.size(); p = g_utf8_next_char(p)) {
        const auto found = characters_.find(g_utf8_get_char(p));
        if (found == characters_.end()) return {};
        if (!reading.empty()) reading += '\'';
        reading += found->second;
    }
    result.readings.push_back(std::move(reading));
    result.approximate = true;
    return result;
}

} // namespace modernime::settings
