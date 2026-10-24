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
    options.englishDefinitionEnabled = true;

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

    // wsl 稳固保护中文，不被生僻缩写词义（威塞尔斯布隆病毒）抢占候选位
    provider.reset();
    assertTrue(provider.append("wsl"), "append wsl");
    assertTrue(!provider.page().items.empty(), "has candidates for wsl");
    assertTrue(provider.page().items.front().source != modernime::core::CandidateSource::Raw,
               "wsl top candidate is Chinese, not raw wsl");
    assertTrue(provider.page().items.front().annotation.empty(),
               "wsl top candidate has no obscure English definition annotation");

    // 学习“王绍龙”（SQLite 落盘归一化为无撇号 wangshaolong）后，输入 wsl 与 wangshaolong 均在首位召回“王绍龙”
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-wsl.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
    {
        modernime::core::LearningStore store(learningPath);
        assertTrue(store.open(), "open wsl learning store");
        for (int i = 0; i < 5; ++i) {
            assertTrue(store.recordSelection("王绍龙", "wang'shao'long", {}, {}, 1000 + i),
                       "record 王绍龙 selection in SQLite store");
        }
        store.close();

        modernime::pinyin::PinyinDataPaths learnedPaths;
        learnedPaths.learningStore = learningPath.string();
        modernime::pinyin::PinyinProviderOptions learnedOptions;
        learnedOptions.learningEnabled = true;
        learnedOptions.englishDefinitionEnabled = true;
        modernime::pinyin::PinyinCandidateProvider learnedProvider(learnedPaths, learnedOptions);

        assertTrue(learnedProvider.append("wsl"), "append wsl with learned 王绍龙");
        assertTrue(!learnedProvider.page().items.empty(), "wsl produces candidates with learning");
        assertTrue(learnedProvider.page().items.front().text == "王绍龙",
                   "learned 王绍龙 is recalled at rank 0 for abbreviation wsl");
        assertTrue(learnedProvider.page().items.front().annotation.empty(),
                   "learned 王绍龙 has no English definition annotation");

        learnedProvider.reset();
        assertTrue(learnedProvider.append("wangshaolong"), "append wangshaolong with learned 王绍龙");
        assertTrue(!learnedProvider.page().items.empty(), "wangshaolong produces candidates");
        assertTrue(learnedProvider.page().items.front().text == "王绍龙",
                   "learned 王绍龙 is at rank 0 for full pinyin wangshaolong");
    }
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
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

void testChinesePersonalizationBeatsEnglishWord() {
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-panda.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    modernime::pinyin::PinyinCandidateProvider provider(paths, options);

    // 未训练时，panda 输入首选为英文单词 panda
    provider.reset();
    assertTrue(provider.append("panda"), "append panda");
    assertTrue(!provider.page().items.empty(), "panda produces candidates");
    assertTrue(provider.page().items.front().text == "panda",
               "unlearned panda has English word at rank 0");

    // 训练“潘达 / pan'da”15 次
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    modernime::core::CommitEvent custom;
    custom.text = "潘达";
    custom.fullPinyin = "pan'da";
    custom.rawKeystrokes = "panda";
    custom.role = modernime::core::CommitCandidateRole::Chinese;
    custom.timestampMs = now;
    custom.isDeviation = true;
    for (int i = 0; i < 15; ++i) {
        provider.onCommitEvent(custom);
    }
    provider.flush();

    // 再次输入 panda：中文“潘达”凭借 15 次学习积累的分数升至第一，仲裁层不应误读“盘大”的 0 分，
    // 首选必须为中文“潘达”而非英文 panda
    provider.reset();
    provider.resetCommitHistory();
    assertTrue(provider.append("panda"), "append panda after learning");
    assertTrue(!provider.page().items.empty(), "candidates not empty");
    assertTrue(provider.page().items.front().text == "潘达",
               "learned Chinese '潘达' beats English 'panda' at rank 0");

    // 类似地，测试“尚海 / shang'hai”15 次训练后排在首位，不会因 sourceIndex 错配读取“上海”
    custom.text = "尚海";
    custom.fullPinyin = "shang'hai";
    custom.rawKeystrokes = "shanghai";
    custom.isDeviation = true;
    for (int i = 0; i < 15; ++i) {
        provider.onCommitEvent(custom);
    }
    provider.flush();

    provider.reset();
    provider.resetCommitHistory();
    assertTrue(provider.append("shanghai"), "append shanghai after learning");
    assertTrue(!provider.page().items.empty(), "candidates not empty");
    assertTrue(provider.page().items.front().text == "尚海",
               "learned Chinese '尚海' reaches rank 0");
}

void testRawPinyinCommitDoesNotPolluteEnglishPrediction() {
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-raw-pinyin.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // 1. 模拟用户打拼音 jianpan 并按回车上屏（Raw commit）2 次
        for (int i = 0; i < 2; ++i) {
            modernime::core::CommitEvent rawCommit;
            rawCommit.text = "jianpan";
            rawCommit.fullPinyin = "jianpan";
            rawCommit.rawKeystrokes = "jianpan";
            rawCommit.role = modernime::core::CommitCandidateRole::Raw;
            rawCommit.isPartial = false;
            rawCommit.consumedInputBytes = 7;
            provider.onCommitEvent(rawCommit);
        }
        assertTrue(provider.flush(), "flush raw pinyin commit");

        // 2. 验证学习库中没有将 jianpan 记录为英文单词
        modernime::core::LearningStore store(learningPath);
        assertTrue(store.open(), "learning store opens");
        const auto snap = store.snapshot();
        const auto recalled = snap->recallEnglishWords("jianp", 5, 0);
        bool hasJianpan = false;
        for (const auto &w : recalled) {
            if (w.text == "jianpan") {
                hasJianpan = true;
                break;
            }
        }
        assertTrue(!hasJianpan, "raw pinyin jianpan is NOT recalled as English word");

        // 3. 用户输入 jianp：首选必须为中文“键盘”，绝不允许被 jianpan 挤占
        provider.reset();
        provider.resetCommitHistory();
        assertTrue(provider.append("jianp"), "append jianp");
        assertTrue(!provider.page().items.empty(), "jianp produces candidates");
        assertTrue(provider.page().items.front().text == "键盘",
                   "top candidate for jianp must be Chinese '键盘'");

        // 4. 验证差量偏差自学习：直接敲空格选择 Rank 0 核心中文词（无上下文），频次保持为 0
        provider.reset();
        provider.resetCommitHistory();
        assertTrue(provider.append("shi"), "append shi");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "是",
                   "top candidate for shi is '是'");
        assertTrue(provider.select(0), "select rank 0 candidate '是'");
        provider.flush();

        // 验证数据库中“是”频次依然为 0（未写入无条件冗余频次）
        const auto snap2 = store.snapshot();
        const auto shiEntry = snap2->entry("是", "shi", "", "");
        assertTrue(shiEntry == nullptr, "unconditioned rank 0 selection of '是' has 0 frequency in learning store");
    }
    std::filesystem::remove(learningPath, error);
}

void testMultiItemEnglishCompletions() {
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-compu.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // 1. 测试 compu：3 个英文补全词 compute, computed, computer 均必须出现在候选列表中
        provider.reset();
        assertTrue(provider.append("compu"), "append compu");
        const auto &pageCompu = provider.page();
        assertTrue(!pageCompu.items.empty(), "compu has candidates");

        const auto idxCompute = indexOf(pageCompu, "compute");
        const auto idxComputed = indexOf(pageCompu, "computed");
        const auto idxComputer = indexOf(pageCompu, "computer");
        assertTrue(idxCompute < pageCompu.items.size(), "compu contains 'compute'");
        assertTrue(idxComputed < pageCompu.items.size(), "compu contains 'computed'");
        assertTrue(idxComputer < pageCompu.items.size(), "compu contains 'computer'");

        // 验证结构契约：前缀优先时，结构应为 [topPrediction (compute), rawCandidate (compu), nextPredictions (computed, computer)...]
        assertTrue(pageCompu.items[0].text == "compute", "compu rank 0 is compute");
        assertTrue(pageCompu.items[1].text == "compu", "compu rank 1 is raw compu");
        assertTrue(indexOf(pageCompu, "computed") < 5, "computed is within top 5");
        assertTrue(indexOf(pageCompu, "computer") < 5, "computer is within top 5");

        // 2. 选择 computer：验证上屏消耗按键 5 字节
        assertTrue(pageCompu.items[idxComputer].consumedInputBytes == 5,
                   "computer on compu consumedInputBytes == 5");
        assertTrue(provider.select(idxComputer), "select computer for compu");

        // 3. 测试 comput：3 个英文补全词 compute, computed, computer 均必须出现
        provider.reset();
        assertTrue(provider.append("comput"), "append comput");
        const auto &pageComput = provider.page();
        assertTrue(!pageComput.items.empty(), "comput has candidates");

        const auto idxCompute2 = indexOf(pageComput, "compute");
        const auto idxComputed2 = indexOf(pageComput, "computed");
        const auto idxComputer2 = indexOf(pageComput, "computer");
        assertTrue(idxCompute2 < pageComput.items.size(), "comput contains 'compute'");
        assertTrue(idxComputed2 < pageComput.items.size(), "comput contains 'computed'");
        assertTrue(idxComputer2 < pageComput.items.size(), "comput contains 'computer'");
        assertTrue(pageComput.items[idxComputer2].consumedInputBytes == 6,
                   "computer on comput consumedInputBytes == 6");
        assertTrue(provider.select(idxComputer2), "select computer for comput");

        // 4. 短拼音中文保护与双属性词规则
        // de: 的 #1, 得 #2
        provider.reset();
        assertTrue(provider.append("de"), "append de");
        assertTrue(provider.page().items[0].text == "的", "de rank 0 is '的'");
        assertTrue(provider.page().items[1].text == "得", "de rank 1 is '得'");

        // shi: 是 #1
        provider.reset();
        assertTrue(provider.append("shi"), "append shi");
        assertTrue(provider.page().items[0].text == "是", "shi rank 0 is '是'");

        // ni: 你 #1
        provider.reset();
        assertTrue(provider.append("ni"), "append ni");
        assertTrue(provider.page().items[0].text == "你", "ni rank 0 is '你'");

        // qu: 去 #1
        provider.reset();
        assertTrue(provider.append("qu"), "append qu");
        assertTrue(provider.page().items[0].text == "去", "qu rank 0 is '去'");

        // can: 中文优先，次选为英文 can
        provider.reset();
        assertTrue(provider.append("can"), "append can");
        assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().source != modernime::core::CandidateSource::Raw,
                   "can rank 0 is Chinese");
        assertTrue(provider.page().items.size() > 1 && provider.page().items[1].text == "can",
                   "can rank 1 is English word can");
    }
    std::filesystem::remove(learningPath, error);
}

void testSingleSyllablePinyinSuppressesStaticEnglishPredictions() {
    std::error_code error;
    const auto learningPath = testPath("english-arbitration-syllable.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // you：既是合法单音节拼音（有/又/友…）又是英文单词。
        // 单音节契约要求前排全部为地道中文：静态英文补全（your/young/yours）
        // 不得出现，原始英文 you 仍按既有规则沉底可及。
        provider.reset();
        assertTrue(provider.append("you"), "append you");
        const auto &pageYou = provider.page();
        assertTrue(!pageYou.items.empty() && pageYou.items.front().text == "有",
                   "you rank 0 is '有'");
        assertTrue(indexOf(pageYou, "your") == pageYou.items.size(),
                   "static completion 'your' must not appear for single-syllable pinyin input");
        assertTrue(indexOf(pageYou, "young") == pageYou.items.size(),
                   "static completion 'young' must not appear for single-syllable pinyin input");
        assertTrue(indexOf(pageYou, "yours") == pageYou.items.size(),
                   "static completion 'yours' must not appear for single-syllable pinyin input");
        assertTrue(indexOf(pageYou, "又") < 10, "'又' back within top 10 for you");

        // can：双属性豁免只保留英文原词次选；补全（cannot/canal/candy）不得
        // 把一级常用中文挤出前排（参 回到前 5）。
        provider.reset();
        assertTrue(provider.append("can"), "append can");
        const auto &pageCan = provider.page();
        assertTrue(!pageCan.items.empty() &&
                       pageCan.items.front().source != modernime::core::CandidateSource::Raw,
                   "can rank 0 is Chinese");
        assertTrue(indexOf(pageCan, "cannot") == pageCan.items.size(),
                   "static completion 'cannot' must not appear for single-syllable pinyin input");
        assertTrue(indexOf(pageCan, "canal") == pageCan.items.size(),
                   "static completion 'canal' must not appear for single-syllable pinyin input");
        assertTrue(indexOf(pageCan, "can") == 1, "raw English 'can' keeps rank 1 exemption");
        assertTrue(indexOf(pageCan, "参") < 5, "'参' back within top 5 for can");

        // app 不是合法拼音串：v0.1.18 的 3 字母英文补全放宽必须继续生效。
        provider.reset();
        assertTrue(provider.append("app"), "append app");
        const auto &pageApp = provider.page();
        assertTrue(indexOf(pageApp, "apple") < pageApp.items.size(),
                   "app still predicts 'apple'");
        assertTrue(indexOf(pageApp, "application") < pageApp.items.size(),
                   "app still predicts 'application'");
    }
    std::filesystem::remove(learningPath, error);
}

} // namespace

int main() {
    testDeveloperLearningDoesNotHijackDe();
    testGnueabihfRecall();
    testChineseAbbreviationProtection();
    testDoubleLetterFaultTolerance();
    testChinesePersonalizationBeatsEnglishWord();
    testRawPinyinCommitDoesNotPolluteEnglishPrediction();
    testMultiItemEnglishCompletions();
    testSingleSyllablePinyinSuppressesStaticEnglishPredictions();
    return EXIT_SUCCESS;
}
