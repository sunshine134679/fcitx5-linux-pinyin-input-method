#include "modernime/core/candidate_model.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace modernime;

class ColdTerminologyRegression : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-cold-terminology-XXXXXX").string();
        const auto *created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.userDictionary = (root / "empty-user.dict").string();
        paths.learningStore = (root / "absent-learning.sqlite3").string();
        std::ofstream(paths.userDictionary).close();
        // Exercise the artifact from this build, independent of installed data.
        paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
        paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
        ASSERT_TRUE(std::filesystem::is_regular_file(paths.extensionDictionary));
        options.learningEnabled = false;
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
    pinyin::PinyinDataPaths paths;
    pinyin::PinyinProviderOptions options;
};

// Removing a reviewed term must fail at the compiled dictionary boundary,
// even when the full-spelling decoder can compose the same text from pieces.
TEST_F(ColdTerminologyRegression, AiTermsAreLexicalAtReviewedCost) {
    libime::PinyinDictionary dictionary;
    dictionary.load(0, paths.extensionDictionary.c_str(), libime::PinyinDictFormat::Binary);
    const std::vector<std::pair<std::string, std::string>> terms = {
        {"子代理", "zi'dai'li"}, {"主代理", "zhu'dai'li"},
        {"多代理", "duo'dai'li"},
        {"子智能体", "zi'zhi'neng'ti"}, {"多智能体", "duo'zhi'neng'ti"},
        {"工具调用", "gong'ju'diao'yong"},
        {"多模态模型", "duo'mo'tai'mo'xing"},
        {"代理编排", "dai'li'bian'pai"},
        {"模型上下文协议", "mo'xing'shang'xia'wen'xie'yi"},
    };
    for (const auto &[text, pinyin] : terms) {
        SCOPED_TRACE(text);
        const auto encoded = libime::PinyinEncoder::encodeFullPinyin(pinyin);
        bool found = false;
        dictionary.matchWords(encoded.data(), encoded.size(),
            [&](std::string_view, std::string_view phrase, float cost) {
                if (phrase == text) {
                    found = true;
                    EXPECT_FLOAT_EQ(cost, (text == "子代理" || text == "多代理") ? 3.0F : 0.0F);
                }
                return true;
            });
        EXPECT_TRUE(found) << "term must be stored as a whole word";
    }
}

// The whole-word coverage must reach the real final candidate/selection path,
// including initials and both representative mixed spellings, without training.
TEST_F(ColdTerminologyRegression, AgentTermsReachFinalCandidatesAndConsumeWholeInput) {
    pinyin::PinyinCandidateProvider provider(paths, options);
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"zidaili", "子代理"}, {"zi'dai'li", "子代理"},
        {"zdl", "子代理"}, {"zdaili", "子代理"}, {"zidli", "子代理"},
        {"zhudaili", "主代理"}, {"zhdl", "主代理"},
        {"zhdaili", "主代理"}, {"zhudli", "主代理"},
        {"duodaili", "多代理"}, {"ddl", "多代理"},
        {"ddaili", "多代理"}, {"duodli", "多代理"},
    };
    for (const auto &[raw, text] : cases) {
        SCOPED_TRACE(raw + "/" + text);
        provider.reset();
        provider.resetCommitHistory();
        ASSERT_TRUE(provider.append(raw));
        const auto &page = provider.page();
        std::size_t index = 0;
        while (index < page.items.size() && page.items[index].text != text) ++index;
        ASSERT_LT(index, page.items.size()) << "cold whole-word candidate missing";
        RecordProperty("rank_" + raw, static_cast<int>(index));
        const auto identity = core::candidateIdentity(page.items[index], page);
        EXPECT_EQ(identity.role, core::CommitCandidateRole::Chinese);
        EXPECT_EQ(identity.consumedInputBytes, raw.size());
        const core::CandidateSelectionRequest request{
            page.sessionId, page.displayGeneration, index, identity};
        const auto outcome = provider.selectConfirmed(request, page.generation);
        ASSERT_TRUE(outcome.has_value());
        EXPECT_EQ(outcome->selected.text, text);
        EXPECT_TRUE(outcome->remainingInput.empty());
        EXPECT_TRUE(provider.page().rawInput.empty());
    }
    EXPECT_FALSE(std::filesystem::exists(paths.learningStore));
}
} // namespace


namespace {
struct LongSentenceHost final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed += text; }
};

TEST_F(ColdTerminologyRegression, ConversationalFragmentsGeneralizeToUnlistedLongSentences) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"nanijixu", "那你继续"},
        {"na'ni'ji'xu", "那你继续"},
        {"nanijixushuoba", "那你继续说吧"},
        {"nanijixushuo", "那你继续说"},
        {"nanixianjixu", "那你先继续"},
        {"nawomenjixu", "那我们继续"},
        {"nijixushuo", "你继续说"},
        {"wojixuxie", "我继续写"},
        {"nanixianmang", "那你先忙"},
        {"nanixianshuo", "那你先说"},
        {"nanihaohaoxiuxi", "那你好好休息"},
        {"naniquba", "那你去吧"},
        {"nanikankan", "那你看看"},
        {"qingjixu", "请继续"},
        {"qingnijixushuo", "请你继续说"},
        {"zhegewentizenmejiejue", "这个问题怎么解决"},
        {"qingbangwodakaiwenjian", "请帮我打开文件"},
        {"womenmingtianjixutaolun", "我们明天继续讨论"},
        {"haodenanijixu", "好的那你继续"},
        {"nanijixuwomendengni", "那你继续我们等你"},
        {"nani", "那你"},
        {"nanijixushuobawohuirenzhentingde", "那你继续说吧我会认真听的"},
        {"qingbangwojianchayixiazhegewentizenmejiejue", "请帮我检查一下这个问题怎么解决"},
        {"womenmingtianxiawujixutaolunzhegewenti", "我们明天下午继续讨论这个问题"},
        {"zhegeshurufaxianzaizhongyukeyizhengchangshiyongle", "这个输入法现在终于可以正常使用了"},
        {"qingnijixubashengxiadedaimaxiewan", "请你继续把剩下的代码写完"},
        {"rangzidailibangwojianchayixiadaima", "让子代理帮我检查一下代码"},
        {"rangduodailixietonggongzuo", "让多代理协同工作"},
        {"rangzhudailibangwojianchayixiadaima", "让主代理帮我检查一下代码"},
    };
    for (const auto &[raw, target] : cases) {
        SCOPED_TRACE(raw);
        pinyin::PinyinCandidateProvider provider(paths, options);
        LongSentenceHost host;
        fcitx5::ModernIMEController controller(host, &provider);
        for (char key : raw) ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
        ASSERT_FALSE(controller.page().items.empty());
        EXPECT_EQ(controller.page().items.front().text, target);
        EXPECT_EQ(controller.page().items.front().consumedInputBytes, raw.size());
        ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Space}));
        EXPECT_EQ(host.committed, target);
        EXPECT_TRUE(controller.page().rawInput.empty());
    }
    EXPECT_FALSE(std::filesystem::exists(paths.learningStore));
}

TEST_F(ColdTerminologyRegression, LongSentencePartialSelectionsPreserveEveryRemainingSyllable) {
    for (const bool separated : {false, true}) {
        pinyin::PinyinCandidateProvider provider(paths, options);
        LongSentenceHost host;
        fcitx5::ModernIMEController controller(host, &provider);
        const std::string raw = separated ? "na'ni'ji'xu'shuo'ba" : "nanijixushuoba";
        for (char key : raw) ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
        const auto choose = [&](std::string_view text, std::size_t consumed, std::string_view rest) {
            const auto &page = controller.page();
            std::size_t index = 0;
            while (index < page.items.size() && page.items[index].text != text) ++index;
            ASSERT_LT(index, page.items.size());
            const auto id = core::candidateIdentity(page.items[index], page);
            EXPECT_EQ(id.consumedInputBytes, consumed);
            const core::CandidateSelectionRequest request{
                controller.sessionId(), controller.displayGeneration(), index, id};
            ASSERT_TRUE(controller.select(request));
            EXPECT_EQ(controller.page().rawInput, rest);
        };
        choose("那你", separated ? 6 : 4, separated ? "ji'xu'shuo'ba" : "jixushuoba");
        choose("继续", separated ? 6 : 4, separated ? "shuo'ba" : "shuoba");
        choose("说吧", separated ? 7 : 6, "");
        EXPECT_EQ(host.committed, "那你继续说吧");
    }
}

TEST_F(ColdTerminologyRegression, ConversationalCalibrationKeepsIntentionalAlternativeSelectable) {
    pinyin::PinyinCandidateProvider provider(paths, options);
    ASSERT_TRUE(provider.append("nani"));
    ASSERT_FALSE(provider.page().items.empty());
    EXPECT_EQ(provider.page().items.front().text, "那你");
    const auto &page = provider.page();
    std::size_t index = 0;
    while (index < page.items.size() && page.items[index].text != "纳尼") ++index;
    ASSERT_LT(index, page.items.size()) << "calibration must not blacklist an intentional word";
    ASSERT_TRUE(provider.select(index));
    EXPECT_TRUE(provider.page().rawInput.empty());
}
} // namespace

namespace {
TEST_F(ColdTerminologyRegression, ReviewedTermsDoNotDisplaceCommonAbbreviationDefault) {
    pinyin::PinyinCandidateProvider provider(paths, options);
    ASSERT_TRUE(provider.append("zdl"));
    ASSERT_FALSE(provider.page().items.empty());
    EXPECT_EQ(provider.page().items.front().text, "战斗力");
}
} // namespace
