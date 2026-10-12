#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_store.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "English arbitration test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testPath(const std::string &name) {
    return std::filesystem::temp_directory_path() / name;
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

void testDeveloperLearningDoesNotHijackDe() {
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-dev.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // 1. 模拟用户输入并提交 developer 5 次
        for (int i = 0; i < 5; ++i) {
            provider.reset();
            assertTrue(provider.append("developer"), "append developer");
            const auto devIdx = indexOf(provider.page(), "developer");
            assertTrue(devIdx < provider.page().items.size(), "developer found in candidates");
            assertTrue(provider.select(devIdx), "select developer");
        }
        provider.flush();

        // 2. 验证学习库中已成功召回 developer（以 de 为前缀）
        modernime::core::LearningStore store(learningPath);
        assertTrue(store.open(), "learning store opens");
        const auto snap = store.snapshot();
        const auto recalled = snap->recallEnglishWords("de", 5, 0);
        bool hasDeveloper = false;
        for (const auto &w : recalled) {
            if (w.text == "developer") {
                hasDeveloper = true;
                break;
            }
        }
        assertTrue(hasDeveloper, "developer is recalled from learning store with prefix de");

        // 3. 用户输入高频拼音 de：首选必须坚守中文“的”，严禁被 developer 篡位
        provider.reset();
        assertTrue(provider.append("de"), "append de");
        assertTrue(!provider.page().items.empty(), "de produces candidates");
        assertTrue(provider.page().items.front().text == "的",
                   "high-confidence Chinese '的' remains at rank 0 despite 5 developer learnings");

        // 验证 developer 作为次选项提供给用户（位于第 2 位），供需要英文的用户选择
        assertTrue(provider.page().items.size() >= 2, "at least 2 candidates for de");
        assertTrue(provider.page().items[1].text == "developer",
                   "developer is placed at index 1 as second candidate");

        // 4. 用户输入 devel：非标拼音前缀，正常置顶补全词 developer
        provider.reset();
        assertTrue(provider.append("devel"), "append devel");
        assertTrue(!provider.page().items.empty(), "devel produces candidates");
        assertTrue(provider.page().items.front().text == "developer",
                   "incomplete prefix devel recommends learned developer at rank 0");
        assertTrue(provider.page().items.size() >= 2 && provider.page().items[1].text == "devel",
                   "rawCandidate devel is at rank 1");

        // 5. 用户输入 dev：拼音无有效单字，置顶补全词 developer
        provider.reset();
        assertTrue(provider.append("dev"), "append dev");
        assertTrue(!provider.page().items.empty(), "dev produces candidates");
        assertTrue(provider.page().items.front().text == "developer",
                   "prefix dev recommends learned developer at rank 0");
    }

    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testGnueabihfRecall() {
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-gnuea.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // 学习长英文专有名词 gnueabihf
        for (int i = 0; i < 3; ++i) {
            provider.reset();
            assertTrue(provider.append("gnueabihf"), "append gnueabihf");
            const auto idx = indexOf(provider.page(), "gnueabihf");
            assertTrue(idx < provider.page().items.size(), "gnueabihf in candidates");
            assertTrue(provider.select(idx), "select gnueabihf");
        }
        provider.flush();

        // 输入长非标拼音前缀 gnuea：应正常召回 gnueabihf 并在首选推荐
        provider.reset();
        assertTrue(provider.append("gnuea"), "append gnuea");
        assertTrue(!provider.page().items.empty(), "gnuea produces candidates");
        assertTrue(provider.page().items.front().text == "gnueabihf",
                   "learned technical term gnueabihf is recalled at rank 0 for gnuea");
    }

    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testChineseAbbreviationProtection() {
    modernime::pinyin::PinyinDataPaths paths;
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;

    modernime::pinyin::PinyinCandidateProvider provider(paths, options);

    // sm 稳固保护中文“什么”
    assertTrue(provider.append("sm"), "append sm");
    assertTrue(!provider.page().items.empty(), "has candidates for sm");
    assertTrue(provider.page().items.front().text == "什么", "sm top candidate is 什么");

    // gn 稳固保护中文
    provider.reset();
    assertTrue(provider.append("gn"), "append gn");
    assertTrue(!provider.page().items.empty(), "has candidates for gn");
    assertTrue(provider.page().items.front().source != modernime::core::CandidateSource::Raw,
               "gn top candidate is Chinese, not raw English");

    // zw 稳固保护中文
    provider.reset();
    assertTrue(provider.append("zw"), "append zw");
    assertTrue(!provider.page().items.empty(), "has candidates for zw");
    assertTrue(provider.page().items.front().source != modernime::core::CandidateSource::Raw,
               "zw top candidate is Chinese, not raw English");
}

void testDoubleLetterFaultTolerance() {
    modernime::pinyin::PinyinDataPaths paths;
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;

    modernime::pinyin::PinyinCandidateProvider provider(paths, options);

    // 1. dee 容错为中文首选“的”，全量消费 3 字节，上屏后无残余 'e'
    assertTrue(provider.append("dee"), "append dee");
    assertTrue(!provider.page().items.empty(), "dee produces candidates");
    assertTrue(provider.page().items.front().text == "的", "dee rank 0 is 的");
    assertTrue(provider.page().items.front().consumedInputBytes == 3, "dee consumes all 3 bytes");
    assertTrue(provider.select(0), "select 的 for dee");
    assertTrue(provider.page().items.empty(), "preedit and candidate list cleared after select 的 for dee");

    // 2. shii 容错为中文首选“是”，全量消费 4 字节，不带多余 i
    provider.reset();
    assertTrue(provider.append("shii"), "append shii");
    assertTrue(!provider.page().items.empty(), "shii produces candidates");
    assertTrue(provider.page().items.front().text == "是", "shii rank 0 is 是");
    assertTrue(provider.page().items.front().consumedInputBytes == 4, "shii consumes all 4 bytes");
    assertTrue(provider.select(0), "select 是 for shii");
    assertTrue(provider.page().items.empty(), "preedit and candidate list cleared after select 是 for shii");

    // 3. nii 容错为中文首选“你”，全量消费 3 字节，不带多余 i
    provider.reset();
    assertTrue(provider.append("nii"), "append nii");
    assertTrue(!provider.page().items.empty(), "nii produces candidates");
    assertTrue(provider.page().items.front().text == "你", "nii rank 0 is 你");
    assertTrue(provider.page().items.front().consumedInputBytes == 3, "nii consumes all 3 bytes");
    assertTrue(provider.select(0), "select 你 for nii");
    assertTrue(provider.page().items.empty(), "preedit and candidate list cleared after select 你 for nii");

    // 4. see/fee/bee/app 等高频合法英文不被无条件折叠，首选英文
    provider.reset();
    assertTrue(provider.append("see"), "append see");
    assertTrue(!provider.page().items.empty(), "see produces candidates");
    assertTrue(provider.page().items.front().text == "see", "see rank 0 is English word see");

    provider.reset();
    assertTrue(provider.append("fee"), "append fee");
    assertTrue(!provider.page().items.empty(), "fee produces candidates");
    assertTrue(provider.page().items.front().text == "fee", "fee rank 0 is English word fee");

    provider.reset();
    assertTrue(provider.append("bee"), "append bee");
    assertTrue(!provider.page().items.empty(), "bee produces candidates");
    assertTrue(provider.page().items.front().text == "bee", "bee rank 0 is English word bee");

    provider.reset();
    assertTrue(provider.append("app"), "append app");
    assertTrue(!provider.page().items.empty(), "app produces candidates");
    assertTrue(provider.page().items.front().text == "app", "app rank 0 is English word app");

    // 5. 用户明确输入分隔符 de'e 保留多音节意图，不折叠为单音节“的”
    provider.reset();
    assertTrue(provider.append("de'e"), "append de'e");
    assertTrue(!provider.page().items.empty(), "de'e produces candidates");
    assertTrue(provider.page().items.front().text != "的", "de'e does not collapse across explicit separator");
}

} // namespace

int main() {
    testDeveloperLearningDoesNotHijackDe();
    testGnueabihfRecall();
    testChineseAbbreviationProtection();
    testDoubleLetterFaultTolerance();
    return EXIT_SUCCESS;
}
