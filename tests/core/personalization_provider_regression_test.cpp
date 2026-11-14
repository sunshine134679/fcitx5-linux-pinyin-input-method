#include "modernime/core/learning_store.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using modernime::core::CommitCandidateRole;
using modernime::core::CommitEvent;
using modernime::core::LearningStore;
using modernime::pinyin::PinyinCandidateProvider;
using modernime::pinyin::PinyinDataPaths;

class PersonalizationProviderRegression : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "modernime-provider-regression-XXXXXX").string();
        const auto *created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.learningStore = (root / "learning.sqlite3").string();
        paths.userDictionary = (root / "user.dict").string();
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    static std::size_t indexOf(const PinyinCandidateProvider &provider,
                               std::string_view text) {
        const auto &items = provider.page().items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].text == text) return i;
        }
        return items.size();
    }
    static void select(PinyinCandidateProvider &provider, std::string_view input,
                       std::string_view text) {
        ASSERT_TRUE(provider.append(input));
        const auto index = indexOf(provider, text);
        ASSERT_LT(index, provider.page().items.size()) << input << " / " << text;
        ASSERT_TRUE(provider.select(index));
    }
    static void nameRound(PinyinCandidateProvider &provider) {
        select(provider, "wang", "王");
        select(provider, "shao", "绍");
        select(provider, "long", "龙");
        provider.resetCommitHistory();
    }
    bool learnedName() {
        LearningStore store(paths.learningStore);
        EXPECT_TRUE(store.open());
        return store.snapshot()->hasPositiveFrequency("王绍龙", "wangshaolong");
    }
    std::filesystem::path root;
    PinyinDataPaths paths;
};

// Disabling learning must gate shared phrase statistics, not only disk writes.
TEST_F(PersonalizationProviderRegression, SensitiveRoundsDoNotContributeToSharedFormation) {
    auto shared = PinyinCandidateProvider::createSharedResources(paths, {});
    PinyinCandidateProvider sensitive(shared);
    sensitive.setLearningEnabled(false);
    nameRound(sensitive);
    nameRound(sensitive);
    PinyinCandidateProvider normal(shared);
    nameRound(normal);
    ASSERT_TRUE(normal.flush());
    EXPECT_FALSE(learnedName());
    nameRound(normal);
    ASSERT_TRUE(normal.flush());
    EXPECT_FALSE(learnedName());
    nameRound(normal);
    ASSERT_TRUE(normal.flush());
    EXPECT_TRUE(learnedName());
}

TEST_F(PersonalizationProviderRegression, LearningToggleClearsTransientSequenceButKeepsNormalStats) {
    PinyinCandidateProvider provider(paths);
    nameRound(provider);
    nameRound(provider);
    select(provider, "wang", "王");
    provider.setLearningEnabled(false);
    provider.setLearningEnabled(true);
    select(provider, "shao", "绍");
    select(provider, "long", "龙");
    provider.resetCommitHistory();
    ASSERT_TRUE(provider.flush());
    EXPECT_FALSE(learnedName());
    nameRound(provider);
    ASSERT_TRUE(provider.flush());
    EXPECT_TRUE(learnedName());
}

TEST_F(PersonalizationProviderRegression, ClearDiscardsPreClearPartialPhraseButKeepsNewSelection) {
    for (const auto *backend : {"clear-partial.sqlite3", "clear-partial.lmdb"}) {
        SCOPED_TRACE(backend);
        paths.learningStore = (root / backend).string();
        modernime::pinyin::PinyinProviderOptions options;
        options.contextLearningEnabled = false;
        auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
        PinyinCandidateProvider provider(shared, options);

        ASSERT_TRUE(provider.append("nihao"));
        const auto first = indexOf(provider, "你");
        ASSERT_LT(first, provider.page().items.size());
        ASSERT_EQ(provider.page().items[first].consumedInputBytes, 2u);
        ASSERT_TRUE(provider.select(first));
        ASSERT_EQ(provider.page().rawInput, "hao");
        ASSERT_TRUE(provider.flush());

        LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.clear());
        ASSERT_TRUE(store.snapshot()->entries().empty());
        ASSERT_TRUE(PinyinCandidateProvider::reloadLearningStore(shared));
        ASSERT_EQ(provider.page().rawInput, "hao");

        const auto afterClear = indexOf(provider, "号");
        ASSERT_LT(afterClear, provider.page().items.size());
        ASSERT_EQ(provider.page().items[afterClear].consumedInputBytes, 3u);
        ASSERT_TRUE(provider.select(afterClear));
        EXPECT_TRUE(provider.page().rawInput.empty());
        ASSERT_TRUE(provider.flush());
        store.close();

        LearningStore reopened(paths.learningStore);
        ASSERT_TRUE(reopened.open());
        const auto snapshot = reopened.snapshot();
        ASSERT_NE(snapshot, nullptr);
        EXPECT_FALSE(snapshot->hasPositiveFrequency("你号", "nihao"));
        EXPECT_FALSE(snapshot->hasPositiveFrequency("你", "ni"));
        const auto *selected = snapshot->entry("号", "hao", "", "");
        ASSERT_NE(selected, nullptr);
        EXPECT_EQ(selected->frequency, 1);
    }
}

TEST_F(PersonalizationProviderRegression, SuppressedNativeCandidateStaysHiddenAcrossSharedAndRestartedProviders) {
    auto shared = PinyinCandidateProvider::createSharedResources(paths, {});
    {
        PinyinCandidateProvider provider(shared);
        ASSERT_TRUE(provider.append("nihao"));
        const auto index = indexOf(provider, "你好");
        ASSERT_LT(index, provider.page().items.size());
        ASSERT_TRUE(provider.remove(index));
        ASSERT_TRUE(provider.flush());
        EXPECT_EQ(indexOf(provider, "你好"), provider.page().items.size());
    }
    {
        PinyinCandidateProvider provider(shared);
        ASSERT_TRUE(provider.append("nihao"));
        EXPECT_EQ(indexOf(provider, "你好"), provider.page().items.size());
    }
    shared.reset();
    PinyinCandidateProvider restarted(paths);
    ASSERT_TRUE(restarted.append("nihao"));
    EXPECT_EQ(indexOf(restarted, "你好"), restarted.page().items.size());
    CommitEvent explicitCommit;
    explicitCommit.text = "你好";
    explicitCommit.fullPinyin = "ni'hao";
    explicitCommit.rawKeystrokes = "nihao";
    explicitCommit.role = CommitCandidateRole::Chinese;
    explicitCommit.timestampMs = 1700000000000LL;
    explicitCommit.isDeviation = true;
    restarted.onCommitEvent(explicitCommit);
    ASSERT_TRUE(restarted.flush());
    restarted.reset();
    ASSERT_TRUE(restarted.append("nihao"));
    EXPECT_LT(indexOf(restarted, "你好"), restarted.page().items.size());
}

TEST_F(PersonalizationProviderRegression, ExplicitSelectionInSharedContextRestoresSuppressedCandidateEverywhere) {
    auto shared = PinyinCandidateProvider::createSharedResources(paths, {});
    PinyinCandidateProvider first(shared);
    ASSERT_TRUE(first.append("nihao"));
    const auto index = indexOf(first, "你好");
    ASSERT_LT(index, first.page().items.size());
    ASSERT_TRUE(first.remove(index));
    PinyinCandidateProvider second(shared);
    CommitEvent selection;
    selection.text = "你好";
    selection.fullPinyin = "ni'hao";
    selection.rawKeystrokes = "nihao";
    selection.role = CommitCandidateRole::Chinese;
    selection.timestampMs = 1700000000000LL;
    selection.isDeviation = true;
    second.onCommitEvent(selection);
    ASSERT_TRUE(second.flush());
    first.reset();
    ASSERT_TRUE(first.append("nihao"));
    EXPECT_LT(indexOf(first, "你好"), first.page().items.size());
}

TEST_F(PersonalizationProviderRegression, StaticEnglishSuppressionAppliesImmediatelyAndAtOtherPrefixesAfterRestart) {
    {
        PinyinCandidateProvider provider(paths);
        ASSERT_TRUE(provider.append("gara"));
        const auto index = indexOf(provider, "garage");
        ASSERT_LT(index, provider.page().items.size());
        ASSERT_TRUE(provider.remove(index));
        EXPECT_EQ(indexOf(provider, "garage"), provider.page().items.size());
        ASSERT_TRUE(provider.flush());
    }
    PinyinCandidateProvider restarted(paths);
    ASSERT_TRUE(restarted.append("garag"));
    EXPECT_EQ(indexOf(restarted, "garage"), restarted.page().items.size());
}

TEST_F(PersonalizationProviderRegression, EnglishCompletionsLearnOneFullWordKeyAcrossPrefixes) {
    PinyinCandidateProvider provider(paths);
    ASSERT_TRUE(provider.append("gara"));
    const auto first = indexOf(provider, "garage");
    ASSERT_LT(first, provider.page().items.size());
    EXPECT_EQ(provider.page().items[first].consumedInputBytes, 4u);
    ASSERT_TRUE(provider.select(first));
    select(provider, "garag", "garage");
    ASSERT_TRUE(provider.flush());
    LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    const auto *entry = snapshot->entry("garage", "garage", "", "");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->frequency, 2);
    EXPECT_EQ(snapshot->entry("garage", "gara", "", ""), nullptr);
    EXPECT_EQ(snapshot->entry("garage", "garag", "", ""), nullptr);
    const auto recalled = snapshot->recallEnglishWords("gar");
    ASSERT_FALSE(recalled.empty());
    EXPECT_EQ(recalled.front().text, "garage");
}

struct LifecycleHost final : modernime::fcitx5::EngineHost {
    void publishPage(const modernime::core::CandidatePage &) override {}
    void commit(std::string_view text) override { commits.emplace_back(text); }
    std::vector<std::string> commits;
};

class PersonalizationLifecycleRegression
    : public PersonalizationProviderRegression,
      public ::testing::WithParamInterface<std::string> {
protected:
    using Controller = modernime::fcitx5::ModernIMEController;
    using Kind = modernime::fcitx5::KeyKind;

    void SetUp() override {
        PersonalizationProviderRegression::SetUp();
        paths.learningStore = (root / ("lifecycle." + GetParam())).string();
        paths.dictionaryPacks = (root / "empty-packs").string();
        paths.bundledDictionaryPacks = (root / "empty-bundled-packs").string();
        options.contextLearningEnabled = false;
    }
    static void type(Controller &controller, std::string_view input) {
        for (const char key : input)
            ASSERT_TRUE(controller.handle({Kind::Character, key}));
    }
    static void choose(Controller &controller, LifecycleHost &host,
                       std::string_view text, std::size_t consumed,
                       std::string_view remaining = {}) {
        const auto &page = controller.page();
        std::size_t index = 0;
        while (index < page.items.size() && page.items[index].text != text) ++index;
        ASSERT_LT(index, page.items.size()) << page.rawInput << " / " << text;
        EXPECT_EQ(page.items[index].consumedInputBytes, consumed);
        const auto previousCommits = host.commits.size();
        ASSERT_TRUE(controller.select(index));
        ASSERT_EQ(host.commits.size(), previousCommits + 1);
        EXPECT_EQ(host.commits.back(), text);
        EXPECT_EQ(controller.page().rawInput, remaining);
    }
    static void choose(PinyinCandidateProvider &provider, std::string_view text,
                       std::size_t consumed, std::string_view remaining = {}) {
        const auto index = indexOf(provider, text);
        ASSERT_LT(index, provider.page().items.size())
            << provider.page().rawInput << " / " << text;
        const auto &page = provider.page();
        modernime::core::CandidateSelectionRequest request;
        request.displayGeneration = page.generation;
        request.globalIndex = index;
        request.expected = modernime::core::candidateIdentity(page.items[index], page);
        EXPECT_EQ(request.expected.consumedInputBytes, consumed);
        const auto outcome = provider.selectConfirmed(request, page.generation);
        ASSERT_TRUE(outcome.has_value());
        EXPECT_EQ(outcome->selected, request.expected);
        EXPECT_EQ(outcome->selected.text, text);
        EXPECT_EQ(outcome->action, modernime::core::SelectionAction::CommitText);
        EXPECT_EQ(outcome->remainingInput, remaining);
        EXPECT_EQ(provider.page().rawInput, remaining);
    }
    static void commit(Controller &controller, LifecycleHost &host,
                       std::string_view input, std::string_view text) {
        ASSERT_TRUE(controller.page().rawInput.empty());
        ASSERT_NO_FATAL_FAILURE(type(controller, input));
        ASSERT_NO_FATAL_FAILURE(choose(controller, host, text, input.size()));
    }
    std::shared_ptr<const modernime::core::LearningSnapshot> snapshot() {
        LearningStore store(paths.learningStore);
        if (!store.openReadOnly()) {
            ADD_FAILURE() << "cannot reopen " << paths.learningStore;
            return {};
        }
        return store.snapshot();
    }
    static void frequency(const modernime::core::LearningSnapshot &state,
                          std::string_view text, std::string_view pinyin, int expected) {
        const auto *entry = state.entry(text, pinyin, "", "");
        ASSERT_NE(entry, nullptr) << text;
        EXPECT_EQ(entry->frequency, expected) << text;
        EXPECT_EQ(entry->negativeFeedback, 0) << text;
    }
    static void nameRound(PinyinCandidateProvider &provider, bool secondName) {
        for (const auto &[input, text] :
             std::array<std::pair<std::string_view, std::string_view>, 3>{{
                 {secondName ? "li" : "wang", secondName ? "李" : "王"},
                 {"shao", "绍"}, {"long", "龙"}}}) {
            ASSERT_TRUE(provider.append(input));
            ASSERT_NO_FATAL_FAILURE(choose(provider, text, input.size()));
        }
        provider.resetCommitHistory();
    }
    static void recallName(PinyinCandidateProvider &provider, bool secondName) {
        const std::string_view input = secondName ? "lsl" : "wsl";
        const std::string_view text = secondName ? "李绍龙" : "王绍龙";
        ASSERT_TRUE(provider.replaceInput(input));
        ASSERT_FALSE(provider.page().items.empty());
        EXPECT_EQ(provider.page().items.front().text, text);
        ASSERT_NO_FATAL_FAILURE(choose(provider, text, input.size()));
        provider.resetCommitHistory();
    }
    modernime::pinyin::PinyinProviderOptions options;
};

TEST_P(PersonalizationLifecycleRegression, LearningAndSensitiveTransitionsKeepOnlyNormalSelections) {
    PinyinCandidateProvider provider(paths, options);
    LifecycleHost host;
    modernime::fcitx5::ControllerOptions controllerOptions;
    controllerOptions.contextLearningEnabled = false;
    Controller controller(host, &provider, controllerOptions);

    // Cross the gates in both orders: enabling learning while sensitive, then
    // leaving a sensitive field while the global learning switch stays off.
    for (int round = 0; round < 3; ++round) {
        SCOPED_TRACE(round);
        ASSERT_NO_FATAL_FAILURE(commit(controller, host, "shi", "试"));
        controllerOptions.learningEnabled = false;
        controller.setOptions(controllerOptions);
        ASSERT_NO_FATAL_FAILURE(commit(controller, host, "ma", "码"));
        controller.setSensitive(true);
        controllerOptions.learningEnabled = true;
        controller.setOptions(controllerOptions);
        ASSERT_NO_FATAL_FAILURE(commit(controller, host, "ni", "妮"));
        controller.setSensitive(false);
        ASSERT_NO_FATAL_FAILURE(commit(controller, host, "jian", "键"));

        controller.setSensitive(true);
        ASSERT_NO_FATAL_FAILURE(commit(controller, host, "ma", "码"));
        controllerOptions.learningEnabled = false;
        controller.setOptions(controllerOptions);
        controller.setSensitive(false);
        ASSERT_NO_FATAL_FAILURE(commit(controller, host, "ni", "妮"));
        controllerOptions.learningEnabled = true;
        controller.setOptions(controllerOptions);
        ASSERT_TRUE(provider.flush());
        const auto state = snapshot();
        ASSERT_NE(state, nullptr);
        ASSERT_NO_FATAL_FAILURE(frequency(*state, "试", "shi", round + 1));
        ASSERT_NO_FATAL_FAILURE(frequency(*state, "键", "jian", round + 1));
        EXPECT_FALSE(state->hasPositiveFrequency("码", "ma"));
        EXPECT_FALSE(state->hasPositiveFrequency("妮", "ni"));
    }

    PinyinCandidateProvider reopened(paths, options);
    for (const auto &[input, text] :
         std::array<std::pair<std::string_view, std::string_view>, 2>{{
             {"shi", "试"}, {"jian", "键"}}}) {
        ASSERT_TRUE(reopened.append(input));
        EXPECT_LT(indexOf(reopened, text), 5u);
        ASSERT_NO_FATAL_FAILURE(choose(reopened, text, input.size()));
        reopened.resetCommitHistory();
    }
    ASSERT_TRUE(reopened.flush());
    const auto state = snapshot();
    ASSERT_NE(state, nullptr);
    ASSERT_NO_FATAL_FAILURE(frequency(*state, "试", "shi", 4));
    ASSERT_NO_FATAL_FAILURE(frequency(*state, "键", "jian", 4));
    EXPECT_FALSE(state->hasPositiveFrequency("码", "ma"));
    EXPECT_FALSE(state->hasPositiveFrequency("妮", "ni"));
}

TEST_P(PersonalizationLifecycleRegression, CanceledSegmentsDoNotJoinTheNextComposition) {
    PinyinCandidateProvider provider(paths, options);
    LifecycleHost host;
    modernime::fcitx5::ControllerOptions controllerOptions;
    controllerOptions.contextLearningEnabled = false;
    Controller controller(host, &provider, controllerOptions);
    struct Phrase {
        std::string_view input, first, suffix, last, phrase;
        std::size_t consumed;
    };
    const std::array phrases{
        Phrase{"nihao", "你", "hao", "号", "你号", 2},
        Phrase{"shijie", "试", "jie", "界", "试界", 3},
    };
    for (int boundary = 0; boundary < 3; ++boundary) {
        for (const auto &phrase : phrases) {
            SCOPED_TRACE(std::string(phrase.phrase) + "/" + std::to_string(boundary));
            ASSERT_NO_FATAL_FAILURE(type(controller, phrase.input));
            ASSERT_NO_FATAL_FAILURE(choose(controller, host, phrase.first,
                                           phrase.consumed, phrase.suffix));
            if (boundary == 0) {
                ASSERT_TRUE(controller.handle({Kind::Escape}));
            } else if (boundary == 1) {
                for (std::size_t i = 0; i < phrase.suffix.size(); ++i)
                    ASSERT_TRUE(controller.handle({Kind::Backspace}));
            } else {
                controllerOptions.inputEnabled = false;
                controller.setOptions(controllerOptions);
                controllerOptions.inputEnabled = true;
                controller.setOptions(controllerOptions);
            }
            ASSERT_TRUE(controller.page().rawInput.empty());
            ASSERT_NO_FATAL_FAILURE(commit(controller, host, phrase.suffix, phrase.last));
            controller.reset();
            ASSERT_TRUE(provider.flush());
            const auto state = snapshot();
            ASSERT_NE(state, nullptr);
            EXPECT_FALSE(state->hasPositiveFrequency(phrase.phrase, phrase.input));
            // Only the confirmed prefix and the later independent suffix exist.
            ASSERT_NO_FATAL_FAILURE(frequency(*state, phrase.first,
                                               phrase.input.substr(0, phrase.consumed), boundary + 1));
            ASSERT_NO_FATAL_FAILURE(frequency(*state, phrase.last, phrase.suffix, boundary + 1));
            EXPECT_EQ(state->entry(phrase.first, phrase.input, "", ""), nullptr);
        }
    }

    // Completing a fresh composition still forms the intended phrase once.
    for (const auto &phrase : phrases) {
        ASSERT_NO_FATAL_FAILURE(type(controller, phrase.input));
        ASSERT_NO_FATAL_FAILURE(choose(controller, host, phrase.first,
                                       phrase.consumed, phrase.suffix));
        ASSERT_NO_FATAL_FAILURE(choose(controller, host, phrase.last, phrase.suffix.size()));
        controller.reset();
    }
    ASSERT_TRUE(provider.flush());
    const auto state = snapshot();
    ASSERT_NE(state, nullptr);
    for (const auto &phrase : phrases)
        ASSERT_NO_FATAL_FAILURE(frequency(*state, phrase.phrase, phrase.input, 1));

    PinyinCandidateProvider reopened(paths, options);
    for (const auto &phrase : phrases) {
        ASSERT_TRUE(reopened.append(phrase.input));
        ASSERT_LT(indexOf(reopened, phrase.phrase), reopened.page().items.size());
        ASSERT_NO_FATAL_FAILURE(choose(reopened, phrase.phrase, phrase.input.size()));
        reopened.resetCommitHistory();
    }
    ASSERT_TRUE(reopened.flush());
}

TEST_P(PersonalizationLifecycleRegression, SharedNamesReopenAndRestartFormationAfterConsecutiveClears) {
    {
        auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
        PinyinCandidateProvider first(shared, options);
        PinyinCandidateProvider second(shared, options);
        for (int round = 0; round < 3; ++round) {
            SCOPED_TRACE(round);
            ASSERT_NO_FATAL_FAILURE(nameRound(round % 2 == 0 ? first : second, false));
            ASSERT_NO_FATAL_FAILURE(nameRound(round % 2 == 0 ? second : first, true));
            ASSERT_TRUE(first.flush());
            const auto state = snapshot();
            ASSERT_NE(state, nullptr);
            if (round < 2) {
                EXPECT_FALSE(state->hasPositiveFrequency("王绍龙", "wangshaolong"));
                EXPECT_FALSE(state->hasPositiveFrequency("李绍龙", "lishaolong"));
            } else {
                ASSERT_NO_FATAL_FAILURE(frequency(*state, "王绍龙", "wangshaolong", 3));
                ASSERT_NO_FATAL_FAILURE(frequency(*state, "李绍龙", "lishaolong", 3));
            }
        }
        ASSERT_NO_FATAL_FAILURE(recallName(second, false));
        ASSERT_NO_FATAL_FAILURE(recallName(first, true));
        ASSERT_TRUE(first.flush());
    }
    {
        auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
        PinyinCandidateProvider first(shared, options);
        PinyinCandidateProvider second(shared, options);
        ASSERT_NO_FATAL_FAILURE(recallName(first, false));
        ASSERT_NO_FATAL_FAILURE(recallName(second, true));
        ASSERT_TRUE(first.append("nihao"));
        ASSERT_NO_FATAL_FAILURE(choose(first, "你", 2, "hao"));
        ASSERT_TRUE(second.append("shijie"));
        ASSERT_NO_FATAL_FAILURE(choose(second, "试", 3, "jie"));
        ASSERT_TRUE(first.flush());

        LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.clear());
        ASSERT_TRUE(PinyinCandidateProvider::reloadLearningStore(shared));
        ASSERT_EQ(first.page().rawInput, "hao");
        ASSERT_NO_FATAL_FAILURE(choose(first, "号", 3));
        ASSERT_TRUE(first.flush());
        auto state = snapshot();
        ASSERT_NE(state, nullptr);
        ASSERT_NO_FATAL_FAILURE(frequency(*state, "号", "hao", 1));
        EXPECT_FALSE(state->hasPositiveFrequency("你号", "nihao"));

        // The second window observes both clear generations at once, after
        // the first has already accepted a new-generation selection.
        ASSERT_TRUE(store.clear());
        ASSERT_TRUE(PinyinCandidateProvider::reloadLearningStore(shared));
        ASSERT_EQ(second.page().rawInput, "jie");
        ASSERT_NO_FATAL_FAILURE(choose(second, "界", 3));
        ASSERT_TRUE(second.flush());
        state = snapshot();
        ASSERT_NE(state, nullptr);
        ASSERT_NO_FATAL_FAILURE(frequency(*state, "界", "jie", 1));
        for (const auto &[text, pinyin] :
             std::array<std::pair<std::string_view, std::string_view>, 5>{{
                 {"王绍龙", "wangshaolong"}, {"李绍龙", "lishaolong"},
                 {"你", "ni"}, {"号", "hao"}, {"试界", "shijie"}}})
            EXPECT_FALSE(state->hasPositiveFrequency(text, pinyin)) << text;

        first.resetCommitHistory();
        second.resetCommitHistory();
        for (int round = 0; round < 3; ++round) {
            SCOPED_TRACE(round);
            ASSERT_NO_FATAL_FAILURE(nameRound(round % 2 == 0 ? second : first, true));
            ASSERT_NO_FATAL_FAILURE(nameRound(round % 2 == 0 ? first : second, false));
            ASSERT_TRUE(first.flush());
            state = snapshot();
            ASSERT_NE(state, nullptr);
            if (round < 2) {
                EXPECT_FALSE(state->hasPositiveFrequency("王绍龙", "wangshaolong"));
                EXPECT_FALSE(state->hasPositiveFrequency("李绍龙", "lishaolong"));
            } else {
                ASSERT_NO_FATAL_FAILURE(frequency(*state, "王绍龙", "wangshaolong", 3));
                ASSERT_NO_FATAL_FAILURE(frequency(*state, "李绍龙", "lishaolong", 3));
            }
        }
    }
    PinyinCandidateProvider reopened(paths, options);
    const auto state = snapshot();
    ASSERT_NE(state, nullptr);
    ASSERT_NO_FATAL_FAILURE(frequency(*state, "界", "jie", 1));
    ASSERT_NO_FATAL_FAILURE(frequency(*state, "王绍龙", "wangshaolong", 3));
    ASSERT_NO_FATAL_FAILURE(frequency(*state, "李绍龙", "lishaolong", 3));
    ASSERT_NO_FATAL_FAILURE(recallName(reopened, false));
    ASSERT_NO_FATAL_FAILURE(recallName(reopened, true));
    ASSERT_TRUE(reopened.flush());
}

INSTANTIATE_TEST_SUITE_P(
    BothBackends, PersonalizationLifecycleRegression,
    ::testing::Values(std::string("sqlite3"), std::string("lmdb")),
    [](const ::testing::TestParamInfo<std::string> &backend) {
        return backend.param == "lmdb" ? "Lmdb" : "Sqlite";
    });
} // namespace
