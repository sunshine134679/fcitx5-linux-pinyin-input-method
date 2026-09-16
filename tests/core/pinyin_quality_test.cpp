#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/core/pinyin_match.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin quality test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::size_t indexOf(const modernime::core::CandidatePage &page,
                    std::string_view text) {
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        if (page.items[index].text == text) {
            return index;
        }
    }
    return page.items.size();
}

bool hasUserDictionaryText(const modernime::core::CandidatePage &page,
                           std::string_view text) {
    for (const auto &item : page.items) {
        if (item.text == text &&
            item.source == modernime::core::CandidateSource::UserDictionary) {
            return true;
        }
    }
    return false;
}

class QualityTestEnvironment final {
public:
    QualityTestEnvironment()
        : directory_(createDirectory()),
          isolatedDictionary_(directory_ / "absent-user-dictionary.txt") {
        if (const auto *value = std::getenv("XDG_DATA_HOME")) {
            previousDataHome_ = value;
        }
        std::error_code error;
        std::filesystem::create_directories(directory_ / "modernime", error);
        assertTrue(!error, "isolated quality directory is created");
        std::ofstream dictionary(directory_ / "modernime" /
                                 "user-dictionary.txt");
        dictionary << "ce'shi'zhang'hu'ci'dian\t测试账户词典\t999999\n";
        dictionary.close();
        assertTrue(dictionary.good(),
                   "controlled account dictionary is written");
        assertTrue(::setenv("XDG_DATA_HOME", directory_.c_str(), 1) == 0,
                   "controlled account data home is installed");
        assertTrue(!std::filesystem::exists(isolatedDictionary_),
                   "isolated quality dictionary starts absent");
    }

    ~QualityTestEnvironment() {
        if (previousDataHome_) {
            ::setenv("XDG_DATA_HOME", previousDataHome_->c_str(), 1);
        } else {
            ::unsetenv("XDG_DATA_HOME");
        }
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    const std::filesystem::path &isolatedDictionary() const {
        return isolatedDictionary_;
    }

private:
    static std::filesystem::path createDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-pinyin-quality-XXXXXX")
                           .string();
        std::vector<char> writablePattern(pattern.begin(), pattern.end());
        writablePattern.push_back('\0');
        const auto *created = ::mkdtemp(writablePattern.data());
        assertTrue(created != nullptr,
                   "unique isolated quality directory is created");
        return created;
    }

    std::filesystem::path directory_;
    std::filesystem::path isolatedDictionary_;
    std::optional<std::string> previousDataHome_;
};

modernime::pinyin::PinyinCandidateProvider
makeProvider(const std::filesystem::path &isolatedDictionary) {
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.userDictionary = isolatedDictionary.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    return modernime::pinyin::PinyinCandidateProvider(paths, options);
}

void testQualityProviderIgnoresAccountDictionary(
    const std::filesystem::path &isolatedDictionary) {
    auto provider = makeProvider(isolatedDictionary);
    assertTrue(provider.append("ceshizhanghucidian"),
               "controlled account-dictionary input is accepted");
    assertTrue(!hasUserDictionaryText(provider.page(), "测试账户词典"),
               "quality provider ignores account dictionary data");
}

void testFrequentSentencesAndPartialSelection(
    const std::filesystem::path &isolatedDictionary) {
    auto provider = makeProvider(isolatedDictionary);

    assertTrue(provider.append("jintiantianqihenhao"),
               "weather sentence input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "今天天气很好",
               "weather sentence is the first candidate");

    provider.reset();
    assertTrue(provider.append("qingbangwodakaiwenjian"),
               "file-opening request input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "请帮我打开文件",
               "file-opening request is the first candidate");
    const auto phraseIndex = indexOf(provider.page(), "请帮我");
    assertTrue(phraseIndex < provider.page().items.size() && phraseIndex < 5,
               "trusted request prefix is in the top five");
    assertTrue(provider.select(phraseIndex),
               "trusted request prefix can be selected");
    assertTrue(provider.page().rawInput == "dakaiwenjian",
               "partial selection preserves the file-opening suffix");

    provider.reset();
    assertTrue(provider.append("nengbunengbangwokanxia"),
               "ambiguous request input is accepted");
    assertTrue(indexOf(provider.page(), "能不能帮我看下") < 3,
               "semantic ambiguous request is in the top three");
}

void testOfflineFuzzyTypoAndAbbreviationRecovery(
    const std::filesystem::path &isolatedDictionary) {
    auto provider = makeProvider(isolatedDictionary);

    assertTrue(provider.append("zongguo"),
               "fuzzy initial input is accepted");
    assertTrue(indexOf(provider.page(), "中国") < 3,
               "z/zh fuzzy recovery is in the top three");

    provider.reset();
    assertTrue(provider.append("wsm"),
               "common short abbreviation input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "为什么",
               "common short abbreviation is the first candidate");

    provider.reset();
    assertTrue(provider.append("bj"),
               "common initialism bj is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "北京",
               "common initialism bj yields 北京 as first candidate");

    provider.reset();
    assertTrue(provider.append("dl"),
               "common initialism dl is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "登录",
               "common initialism dl yields 登录 as first candidate");

    provider.reset();
    assertTrue(provider.append("who"), "ordinary English input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "who",
               "ordinary English remains the first candidate");

    provider.reset();
    assertTrue(provider.append("xign"),
               "common transposition input is accepted");
    bool foundCorrectedXing = false;
    for (const auto &candidate : provider.page().items) {
        if (candidate.source != modernime::core::CandidateSource::Raw &&
            modernime::core::PinyinMatchPolicy::canonical(
                candidate.fullPinyin) == "xing") {
            foundCorrectedXing = true;
            break;
        }
    }
    assertTrue(foundCorrectedXing,
               "transposed input exposes a corrected xing candidate");
}

} // namespace

int main() {
    QualityTestEnvironment environment;
    testQualityProviderIgnoresAccountDictionary(
        environment.isolatedDictionary());
    testFrequentSentencesAndPartialSelection(environment.isolatedDictionary());
    testOfflineFuzzyTypoAndAbbreviationRecovery(
        environment.isolatedDictionary());
    return EXIT_SUCCESS;
}
