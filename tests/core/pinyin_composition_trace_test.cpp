#include <gtest/gtest.h>

#include "modernime/core/candidate_model.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/candidate_composition.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace modernime::pinyin {
namespace {

struct TestHost final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed = text; }
};

class ArchitectureCompositionRegression : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "modernime-comp-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.learningStore = (root / "learning.sqlite3").string();
        paths.userDictionary = (root / "empty-user.dict").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    PinyinDataPaths paths;
};

// 1. FuzzyCandidatesPromotedToSecondSlot
TEST_F(ArchitectureCompositionRegression, FuzzyCandidatesPromotedToSecondSlot) {
    // Multi-syllable input: "cangku"
    // candidate 0: exact match "苍库" (priority 2), LibIME native rank sourceIndex = 5
    // candidate 1: other exact "仓库" (priority 2), sourceIndex = 6
    // candidate 2: fuzzy match "常客" (priority 0, chang'ke), LibIME native rank sourceIndex = 1
    std::vector<core::CandidateItem> fullCandidates;
    core::CandidateItem exact1;
    exact1.text = "苍库";
    exact1.fullPinyin = "cang'ku";
    exact1.source = core::CandidateSource::Engine;
    exact1.commitRole = core::CommitCandidateRole::Chinese;
    exact1.origins = {exact1.source};
    exact1.sourceIndex = 5;
    exact1.consumedInputBytes = 6;
    fullCandidates.push_back(exact1);

    core::CandidateItem exact2;
    exact2.text = "仓库";
    exact2.fullPinyin = "cang'ku";
    exact2.source = core::CandidateSource::Engine;
    exact2.commitRole = core::CommitCandidateRole::Chinese;
    exact2.origins = {exact2.source};
    exact2.sourceIndex = 6;
    exact2.consumedInputBytes = 6;
    fullCandidates.push_back(exact2);

    core::CandidateItem fuzzy;
    fuzzy.text = "常客";
    fuzzy.fullPinyin = "chang'ke";
    fuzzy.source = core::CandidateSource::Engine;
    fuzzy.commitRole = core::CommitCandidateRole::Chinese;
    fuzzy.origins = {fuzzy.source};
    fuzzy.sourceIndex = 1; // Higher native rank
    fuzzy.consumedInputBytes = 6;
    fullCandidates.push_back(fuzzy);

    interleaveHigherNativeRankFuzzyCandidate("cangku", fullCandidates);
    // Exact 1 remains at rank 0
    EXPECT_EQ(fullCandidates[0].text, "苍库");
    // Fuzzy item promoted to rank 1!
    EXPECT_EQ(fullCandidates[1].text, "常客");
    // Exact 2 pushed to rank 2
    EXPECT_EQ(fullCandidates[2].text, "仓库");

    // Single-syllable input: must NOT interleave fuzzy candidate into rank 1
    std::vector<core::CandidateItem> singleSyllableCandidates;
    core::CandidateItem singleExact;
    singleExact.text = "藏";
    singleExact.fullPinyin = "cang";
    singleExact.source = core::CandidateSource::Engine;
    singleExact.commitRole = core::CommitCandidateRole::Chinese;
    singleExact.sourceIndex = 5;
    singleExact.consumedInputBytes = 4;
    singleSyllableCandidates.push_back(singleExact);

    core::CandidateItem singleFuzzy;
    singleFuzzy.text = "常";
    singleFuzzy.fullPinyin = "chang";
    singleFuzzy.source = core::CandidateSource::Engine;
    singleFuzzy.commitRole = core::CommitCandidateRole::Chinese;
    singleFuzzy.sourceIndex = 1;
    singleFuzzy.consumedInputBytes = 4;
    singleSyllableCandidates.push_back(singleFuzzy);

    interleaveHigherNativeRankFuzzyCandidate("cang", singleSyllableCandidates);
    // Order strictly preserved for single-syllable inputs!
    EXPECT_EQ(singleSyllableCandidates[0].text, "藏");
    EXPECT_EQ(singleSyllableCandidates[1].text, "常");
}

// 2. StageOrderingAndMoveTracing
TEST_F(ArchitectureCompositionRegression, StageOrderingAndMoveTracing) {
    CandidateCompositionInput input;
    input.rawInput = "t";
    input.segmentedInput = "t";
    input.macroCandidates = {"2026年10月16日"};
    input.recordMoves = true;

    core::CandidateItem item;
    item.text = "他";
    item.fullPinyin = "ta";
    item.source = core::CandidateSource::Engine;
    item.commitRole = core::CommitCandidateRole::Chinese;
    item.origins = {item.source};
    item.sourceIndex = 0;
    item.consumedInputBytes = 1;
    input.fullCandidates.push_back(item);

    auto result = composeCandidates(input);
    ASSERT_FALSE(result.moves.empty());
    EXPECT_LE(result.moves.size(), 256u);

    // Verify move stages are ordered logically
    int lastStageInt = -1;
    for (const auto &m : result.moves) {
        int currentStageInt = static_cast<int>(m.stage);
        EXPECT_GE(currentStageInt, lastStageInt);
        lastStageInt = currentStageInt;
    }

    // Disabled by default: recordMoves = false produces empty moves
    input.recordMoves = false;
    auto noMovesResult = composeCandidates(input);
    EXPECT_TRUE(noMovesResult.moves.empty());
}

// 3. MacroPlacementFixedPositions
TEST_F(ArchitectureCompositionRegression, MacroPlacementFixedPositions) {
    // Normal input: macro placed at position 1 (behind top candidate)
    CandidateCompositionInput inputNormal;
    inputNormal.rawInput = "sj";
    inputNormal.segmentedInput = "s'j";
    inputNormal.prefixEnds = {1, 2};

    core::CandidateItem topItem;
    topItem.text = "时间";
    topItem.fullPinyin = "shi'jian";
    topItem.source = core::CandidateSource::Engine;
    topItem.commitRole = core::CommitCandidateRole::Chinese;
    topItem.origins = {topItem.source};
    topItem.sourceIndex = 0;
    topItem.consumedInputBytes = 2;
    inputNormal.fullCandidates.push_back(topItem);
    inputNormal.macroCandidates = {"11:30:00"};

    auto resNormal = composeCandidates(inputNormal);
    ASSERT_GE(resNormal.items.size(), 2u);
    EXPECT_EQ(resNormal.items[0].text, "时间");
    EXPECT_EQ(resNormal.items[1].text, "11:30:00");

    // 'v' input: macro placed at position 0
    CandidateCompositionInput inputV;
    inputV.rawInput = "v123";
    inputV.segmentedInput = "v123";
    inputV.macroCandidates = {"壹佰贰拾叁元整"};

    auto resV = composeCandidates(inputV);
    ASSERT_FALSE(resV.items.empty());
    EXPECT_EQ(resV.items[0].text, "壹佰贰拾叁元整");
}

// 4. ClipboardCommandAndSingleTriggerV
TEST_F(ArchitectureCompositionRegression, ClipboardCommandAndSingleTriggerV) {
    CandidateCompositionInput input;
    input.rawInput = "v";
    input.segmentedInput = "v";
    input.clipboardEnabled = true;
    input.clipboardTrigger = "V+2";
    input.recordMoves = true;

    auto result = composeCandidates(input);
    ASSERT_EQ(result.items.size(), 2u);
    EXPECT_EQ(result.items[0].text, "v");
    EXPECT_EQ(result.items[0].source, core::CandidateSource::Raw);
    EXPECT_EQ(result.items[0].commitRole, core::CommitCandidateRole::Raw);

    EXPECT_EQ(result.items[1].text, "剪贴板 (V+2)");
    EXPECT_EQ(result.items[1].source, core::CandidateSource::Command);
    EXPECT_EQ(result.items[1].commitRole, core::CommitCandidateRole::Command);

    EXPECT_EQ(result.preedit, "v");

    bool hasCommandMove = false;
    for (const auto &m : result.moves) {
        if (m.stage == CompositionStage::CommandPlacement &&
            m.reason == MoveReason::CommandSingleTrigger) {
            hasCommandMove = true;
        }
    }
    EXPECT_TRUE(hasCommandMove);
}

// 5. EnglishPriorityAndDualAttributeCan
TEST_F(ArchitectureCompositionRegression, EnglishPriorityAndDualAttributeCan) {
    // "can" has dual attribute exemption: even though single syllable, raw is placed at index 1
    CandidateCompositionInput inputCan;
    inputCan.rawInput = "can";
    inputCan.segmentedInput = "can";
    inputCan.isEnglishWord = true;

    core::CandidateItem topChinese;
    topChinese.text = "餐";
    topChinese.fullPinyin = "can";
    topChinese.source = core::CandidateSource::Engine;
    topChinese.commitRole = core::CommitCandidateRole::Chinese;
    topChinese.origins = {topChinese.source};
    topChinese.sourceIndex = 0;
    topChinese.consumedInputBytes = 3;
    topChinese.isLexical = true;
    inputCan.fullCandidates.push_back(topChinese);

    auto resCan = composeCandidates(inputCan);
    ASSERT_GE(resCan.items.size(), 2u);
    EXPECT_EQ(resCan.items[0].text, "餐");
    EXPECT_EQ(resCan.items[1].text, "can");

    // Prefer English when predicted words present and no exact full pinyin match
    CandidateCompositionInput inputEnglish;
    inputEnglish.rawInput = "gara";
    inputEnglish.segmentedInput = "gara";
    inputEnglish.englishPredictions = {"garage"};

    core::CandidateItem nonExactChinese;
    nonExactChinese.text = "嘎啦";
    nonExactChinese.fullPinyin = "ga'la";
    nonExactChinese.source = core::CandidateSource::Engine;
    nonExactChinese.commitRole = core::CommitCandidateRole::Chinese;
    nonExactChinese.origins = {nonExactChinese.source};
    nonExactChinese.consumedInputBytes = 4;
    inputEnglish.fullCandidates.push_back(nonExactChinese);

    auto resEng = composeCandidates(inputEnglish);
    ASSERT_GE(resEng.items.size(), 2u);
    EXPECT_EQ(resEng.items[0].text, "garage");
    EXPECT_EQ(resEng.items[1].text, "gara");
}

// 6. FinalSuppressionUnifiedBoundary
TEST_F(ArchitectureCompositionRegression, FinalSuppressionUnifiedBoundary) {
    CandidateCompositionInput input;
    input.rawInput = "test";
    input.segmentedInput = "test";

    core::CandidateItem goodItem;
    goodItem.text = "特斯特";
    goodItem.fullPinyin = "te'si'te";
    goodItem.source = core::CandidateSource::Engine;
    goodItem.commitRole = core::CommitCandidateRole::Chinese;
    goodItem.origins = {goodItem.source};
    goodItem.consumedInputBytes = 4;
    input.fullCandidates.push_back(goodItem);

    core::CandidateItem badItem;
    badItem.text = "坏词";
    badItem.fullPinyin = "huai'ci";
    badItem.source = core::CandidateSource::Learned;
    badItem.commitRole = core::CommitCandidateRole::Chinese;
    badItem.origins = {badItem.source};
    badItem.consumedInputBytes = 4;
    input.fullCandidates.push_back(badItem);

    input.isSuppressed = [](std::string_view text, std::string_view) {
        return text == "坏词";
    };
    input.recordMoves = true;

    auto res = composeCandidates(input);
    bool foundBad = false;
    for (const auto &item : res.items) {
        if (item.text == "坏词") foundBad = true;
    }
    EXPECT_FALSE(foundBad);

    bool suppressedMove = false;
    for (const auto &m : res.moves) {
        if (m.stage == CompositionStage::FinalSuppression &&
            m.reason == MoveReason::SuppressedCandidateDropped) {
            suppressedMove = true;
        }
    }
    EXPECT_TRUE(suppressedMove);
}

// 7. MovesTraceCappedAt256AndDisabledByDefault
TEST_F(ArchitectureCompositionRegression, MovesTraceCappedAt256AndDisabledByDefault) {
    CandidateCompositionInput input;
    input.rawInput = "test";
    input.segmentedInput = "test";
    input.recordMoves = true;

    // Generate 300 candidates to verify move cap
    for (int i = 0; i < 300; ++i) {
        core::CandidateItem item;
        item.text = "词" + std::to_string(i);
        item.fullPinyin = "ci";
        item.source = core::CandidateSource::Engine;
        item.commitRole = core::CommitCandidateRole::Chinese;
        item.origins = {item.source};
        item.consumedInputBytes = 4;
        input.fullCandidates.push_back(item);
    }

    auto res = composeCandidates(input);
    EXPECT_LE(res.moves.size(), 256u);
}

// 8. ProviderCallsSingleCompositionBoundary
TEST_F(ArchitectureCompositionRegression, ProviderCallsSingleCompositionBoundary) {
    PinyinCandidateProvider provider(paths);
    TestHost host;
    fcitx5::ModernIMEController c(host, &provider);

    c.setContext("", "");
    for (char ch : std::string_view("ceshi")) {
        ASSERT_TRUE(c.handle({fcitx5::KeyKind::Character, ch}));
    }

    const auto &page = c.page();
    ASSERT_FALSE(page.items.empty());
    EXPECT_EQ(page.rawInput, "ceshi");

    // All items must have valid commitRole, origins, and unique identities
    std::unordered_set<core::CandidateIdentity> seen;
    for (const auto &item : page.items) {
        EXPECT_TRUE(item.commitRole.has_value());
        EXPECT_FALSE(item.origins.empty());
        const auto id = core::candidateIdentity(item, page);
        EXPECT_TRUE(seen.insert(id).second);
    }
}

} // namespace
} // namespace modernime::pinyin
