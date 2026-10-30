#include "modernime/fcitx5/engine.h"
#include "modernime/core/candidate_model.h"
#include "modernime/core/candidate_provider.h"

#if MODERNIME_HAS_FCITX5
#include "modernime/fcitx5/fcitx_engine.h"
#endif

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestEngineHost final : modernime::fcitx5::EngineHost {
    std::vector<modernime::core::CandidatePage> publishedPages;
    std::vector<std::string> committedTexts;

    void publishPage(const modernime::core::CandidatePage &page) override {
        publishedPages.push_back(page);
    }

    void commit(std::string_view text) override {
        committedTexts.emplace_back(text);
    }
};

modernime::core::CandidateItem makeItem(
    std::string text, std::string pinyin = {}, std::size_t consumed = 0,
    modernime::core::CommitCandidateRole role = modernime::core::CommitCandidateRole::Chinese,
    modernime::core::CandidateSource source = modernime::core::CandidateSource::Engine) {
    modernime::core::CandidateItem item;
    item.text = std::move(text);
    item.fullPinyin = std::move(pinyin);
    item.consumedInputBytes = consumed;
    item.commitRole = role;
    item.source = source;
    return item;
}

struct TestCandidateProvider final : modernime::core::CandidateProvider {
    modernime::core::CandidatePage currentPage;
    int appendCount = 0;
    int selectCount = 0;
    int resetCount = 0;
    int commitEventCount = 0;
    modernime::core::CommitEvent lastCommitEvent;

    bool append(std::string_view input) override {
        ++appendCount;
        currentPage.rawInput.append(input);
        currentPage.preedit = currentPage.rawInput;
        currentPage.generation++;
        if (currentPage.rawInput == "nihaoma") {
            currentPage.items = {
                makeItem("你好", "ni'hao", 5),
                makeItem("尼好", "ni'hao", 5)
            };
        } else if (currentPage.rawInput == "nihao") {
            currentPage.items = {
                makeItem("你好", "ni'hao", 5),
                makeItem("您好", "nin'hao", 5)
            };
        }
        return true;
    }

    bool eraseLast() override {
        if (currentPage.rawInput.empty()) {
            return false;
        }
        currentPage.rawInput.pop_back();
        currentPage.preedit = currentPage.rawInput;
        currentPage.generation++;
        return true;
    }

    bool replaceInput(std::string_view input) override {
        currentPage.rawInput = input;
        currentPage.preedit = input;
        currentPage.generation++;
        return true;
    }

    bool select(std::size_t index) override {
        ++selectCount;
        if (index >= currentPage.items.size()) {
            return false;
        }
        const auto &item = currentPage.items[index];
        const auto consumed = (item.consumedInputBytes == 0)
                                  ? currentPage.rawInput.size()
                                  : item.consumedInputBytes;
        if (consumed <= currentPage.rawInput.size()) {
            currentPage.rawInput.erase(0, consumed);
            currentPage.preedit = currentPage.rawInput;
        } else {
            currentPage.rawInput.clear();
            currentPage.preedit.clear();
        }
        currentPage.generation++;
        return true;
    }

    std::optional<modernime::core::CandidateSelectionOutcome> selectConfirmed(
        const modernime::core::CandidateSelectionRequest &request,
        std::uint64_t providerGeneration) override {
        if (providerGeneration != currentPage.generation) {
            return std::nullopt;
        }
        if (request.globalIndex >= currentPage.items.size()) {
            return std::nullopt;
        }
        if (currentPage.items[request.globalIndex].source == modernime::core::CandidateSource::None) {
            return std::nullopt;
        }
        const auto id = modernime::core::candidateIdentity(
            currentPage.items[request.globalIndex], currentPage);
        if (!(id == request.expected)) {
            return std::nullopt;
        }
        const auto action = (id.role == modernime::core::CommitCandidateRole::Command)
                                ? modernime::core::SelectionAction::OpenClipboard
                                : modernime::core::SelectionAction::CommitText;
        if (!select(request.globalIndex)) {
            return std::nullopt;
        }
        modernime::core::CandidateSelectionOutcome outcome;
        outcome.selected = id;
        outcome.action = action;
        outcome.remainingInput = currentPage.rawInput;
        return outcome;
    }

    bool remove(std::size_t index) override {
        if (index >= currentPage.items.size()) {
            return false;
        }
        if (currentPage.items[index].source == modernime::core::CandidateSource::None) {
            return false;
        }
        currentPage.items.erase(currentPage.items.begin() + index);
        currentPage.generation++;
        return true;
    }

    bool extractCharacter(std::size_t index, bool firstChar,
                          std::string *outText) override {
        if (index >= currentPage.items.size()) {
            return false;
        }
        if (currentPage.items[index].source == modernime::core::CandidateSource::None) {
            return false;
        }
        const auto &text = currentPage.items[index].text;
        if (text.size() < 3) {
            return false;
        }
        std::string ch = firstChar ? text.substr(0, 3) : text.substr(text.size() - 3);
        if (outText != nullptr) {
            *outText = ch;
        }
        currentPage.rawInput.clear();
        currentPage.preedit.clear();
        currentPage.generation++;
        return true;
    }

    void onCommitEvent(const modernime::core::CommitEvent &ev) override {
        ++commitEventCount;
        lastCommitEvent = ev;
    }

    void reset() override {
        ++resetCount;
        currentPage.clear();
    }

    const modernime::core::CandidatePage &page() const override {
        return currentPage;
    }
};

TEST(ArchitectureSelectionRegression, SameSizeReplacementRejectsOldWord) {
    TestEngineHost host;
    TestCandidateProvider provider;

    modernime::fcitx5::ModernIMEController controller(host, &provider);

    // List A has 2 candidates: "你好" (index 0), "拟好" (index 1)
    provider.currentPage.rawInput = "nihao";
    provider.currentPage.preedit = "nihao";
    provider.currentPage.generation = 1;
    provider.currentPage.items = {
        makeItem("你好", "ni'hao", 5),
        makeItem("拟好", "ni'hao", 5)
    };

    controller.handle({modernime::fcitx5::KeyKind::Character, 'n', 0});
    ASSERT_FALSE(host.publishedPages.empty());

    // Record request for List A, index 0
    modernime::core::CandidateSelectionRequest reqA;
    reqA.sessionId = controller.sessionId();
    reqA.displayGeneration = controller.displayGeneration();
    reqA.globalIndex = 0;
    reqA.expected = modernime::core::candidateIdentity(
        controller.page().items[0], controller.page());

#if MODERNIME_HAS_FCITX5
    auto wordA = std::make_unique<modernime::fcitx5::FcitxCandidateWord>(
        "你好", controller.endpoint(), reqA);
#endif

    // Replace with List B having the EXACT SAME SIZE (2 candidates): "喜欢", "西瓜"
    provider.currentPage.rawInput = "xihuan";
    provider.currentPage.preedit = "xihuan";
    provider.currentPage.generation = 2;
    provider.currentPage.items = {
        makeItem("喜欢", "xi'huan", 6),
        makeItem("西瓜", "xi'gua", 5)
    };

    controller.handle({modernime::fcitx5::KeyKind::Character, 'x', 0});
    ASSERT_EQ(controller.page().items.size(), 2u);

    // Calling old request for List A must be rejected
    const auto commitsBefore = host.committedTexts.size();
    const auto selectsBefore = provider.selectCount;
    const auto commitsEventBefore = provider.commitEventCount;

    EXPECT_FALSE(controller.select(reqA));
    EXPECT_EQ(host.committedTexts.size(), commitsBefore);
    EXPECT_EQ(provider.selectCount, selectsBefore);
    EXPECT_EQ(provider.commitEventCount, commitsEventBefore);

#if MODERNIME_HAS_FCITX5
    wordA->select(nullptr);
    EXPECT_EQ(host.committedTexts.size(), commitsBefore);
    EXPECT_EQ(provider.selectCount, selectsBefore);
    EXPECT_EQ(provider.commitEventCount, commitsEventBefore);
#endif
}

TEST(ArchitectureSelectionRegression, OldListAfterControllerDestruction) {
    auto host = std::make_unique<TestEngineHost>();
    auto provider = std::make_unique<TestCandidateProvider>();

    auto controller = std::make_unique<modernime::fcitx5::ModernIMEController>(*host, provider.get());
    for (char c : std::string_view("nihao")) {
        controller->handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    ASSERT_FALSE(host->publishedPages.empty());

    auto ep = controller->endpoint();
    auto sharedEp = ep.lock();
    ASSERT_NE(sharedEp, nullptr);

    modernime::core::CandidateSelectionRequest req;
    req.sessionId = controller->sessionId();
    req.displayGeneration = controller->displayGeneration();
    req.globalIndex = 0;
    req.expected = modernime::core::candidateIdentity(
        controller->page().items[0], controller->page());

#if MODERNIME_HAS_FCITX5
    auto word = std::make_unique<modernime::fcitx5::FcitxCandidateWord>("你好", ep, req);
    auto list = std::make_unique<modernime::fcitx5::FcitxCandidateList>(ep, req.displayGeneration, 1);
#endif

    // Destroy the controller
    controller.reset();

    // Endpoints and old candidates must safely return false with zero UAF/crashes
    EXPECT_FALSE(sharedEp->select(req));
    EXPECT_FALSE(sharedEp->setPageBoundaries(1, 1, {{0, 1}}));
    EXPECT_FALSE(sharedEp->moveCursor(1));
    EXPECT_FALSE(sharedEp->movePage(1));
    EXPECT_EQ(sharedEp->currentPageIndex(), 0u);

#if MODERNIME_HAS_FCITX5
    word->select(nullptr);
    list->prev();
    list->next();
    list->setPage(1);
    list->prevCandidate();
    list->nextCandidate();
    list->setPageBoundaries({{0, 1}});
#endif

    sharedEp.reset();
    EXPECT_EQ(ep.lock(), nullptr);
    EXPECT_TRUE(host->committedTexts.empty());
}

TEST(ArchitectureSelectionRegression, StaleBoundaryAndOldFontRejected) {
    TestEngineHost host;
    TestCandidateProvider provider;

    provider.currentPage.rawInput = "test";
    provider.currentPage.preedit = "test";
    provider.currentPage.generation = 1;
    for (int i = 0; i < 5; ++i) {
        provider.currentPage.items.push_back(makeItem(std::to_string(i), {}, 4));
    }

    modernime::fcitx5::ModernIMEController controller(host, &provider);
    controller.handle({modernime::fcitx5::KeyKind::Character, 't', 0});
    ASSERT_EQ(controller.page().items.size(), 5u);

    const auto displayGen = controller.displayGeneration();
    const auto layoutGen = controller.layoutGeneration();

    // 1. Stale displayGeneration rejected
    EXPECT_FALSE(controller.setPageBoundaries(displayGen - 1, layoutGen, {{0, 5}}));

    // 2. Accept layoutGeneration = 2
    EXPECT_TRUE(controller.setPageBoundaries(displayGen, layoutGen + 1, {{0, 3}, {3, 5}}));
    EXPECT_EQ(controller.layoutGeneration(), layoutGen + 1);

    // 3. Stale layoutGeneration (< 2) rejected
    EXPECT_FALSE(controller.setPageBoundaries(displayGen, layoutGen, {{0, 5}}));

    // 4. Incomplete boundaries rejected (covers only 3 of 5)
    EXPECT_FALSE(controller.setPageBoundaries(displayGen, layoutGen + 2, {{0, 3}}));

    // 5. Non-continuous boundaries rejected (gap between 2 and 3)
    EXPECT_FALSE(controller.setPageBoundaries(displayGen, layoutGen + 2, {{0, 2}, {3, 5}}));

    // 6. Out-of-bounds boundary rejected
    EXPECT_FALSE(controller.setPageBoundaries(displayGen, layoutGen + 2, {{0, 6}}));

    // 7. Valid full coverage accepted
    EXPECT_TRUE(controller.setPageBoundaries(displayGen, layoutGen + 2, {{0, 2}, {2, 5}}));
}

TEST(ArchitectureSelectionRegression, ClipboardDisabledDuringComposition) {
    TestEngineHost host;
    TestCandidateProvider provider;

    modernime::fcitx5::ControllerOptions options;
    options.clipboardEnabled = false;
    options.clipboardTrigger = "V+2";

    modernime::fcitx5::ModernIMEController controller(host, &provider, options);

    provider.currentPage.rawInput = "v2";
    provider.currentPage.preedit = "v2";
    provider.currentPage.generation = 1;
    modernime::core::CandidateItem cmdItem;
    cmdItem.text = "(剪贴板)";
    cmdItem.source = modernime::core::CandidateSource::Command;
    cmdItem.commitRole = modernime::core::CommitCandidateRole::Command;
    cmdItem.consumedInputBytes = 2;
    provider.currentPage.items.push_back(cmdItem);

    controller.handle({modernime::fcitx5::KeyKind::Character, 'v', 0});

    // KeyKind::OpenClipboard must be rejected
    EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::OpenClipboard}));
    EXPECT_FALSE(controller.clipboardMode());

    // Selecting a Command candidate when clipboard is disabled must be rejected
    modernime::core::CandidateSelectionRequest req;
    req.sessionId = controller.sessionId();
    req.displayGeneration = controller.displayGeneration();
    req.globalIndex = 0;
    req.expected = modernime::core::candidateIdentity(
        controller.page().items[0], controller.page());

    EXPECT_FALSE(controller.select(req));
    EXPECT_FALSE(controller.clipboardMode());
    EXPECT_TRUE(host.committedTexts.empty());
}

TEST(ArchitectureSelectionRegression, SelectionEquivalenceAndPartialConsumption) {
    TestEngineHost host;
    TestCandidateProvider provider;

    modernime::fcitx5::ModernIMEController controller(host, &provider);

    // 1. Partial consumption: type "nihaoma" -> "你好" (5 bytes) leaving "ma"
    for (char c : std::string_view("nihaoma")) {
        controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    ASSERT_EQ(controller.page().items.size(), 2u);

    // Press Space to select first candidate ("你好")
    EXPECT_TRUE(controller.handle({modernime::fcitx5::KeyKind::Space}));
    ASSERT_EQ(host.committedTexts.size(), 1u);
    EXPECT_EQ(host.committedTexts.back(), "你好");
    EXPECT_EQ(controller.page().rawInput, "ma");

    // Clear remainder
    controller.reset();
    host.committedTexts.clear();

    // 2. Digit selection: type "nihao" and Digit '2' selects index 1 ("您好")
    for (char c : std::string_view("nihao")) {
        controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    ASSERT_EQ(controller.page().items.size(), 2u);
    EXPECT_TRUE(controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '2'}));
    ASSERT_EQ(host.committedTexts.size(), 1u);
    EXPECT_EQ(host.committedTexts.back(), "您好");

    // 3. Character extraction: '[' extracts first character, ']' extracts last
    host.committedTexts.clear();
    for (char c : std::string_view("nihao")) {
        controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    EXPECT_TRUE(controller.handle({modernime::fcitx5::KeyKind::CommitFirstCharacter}));
    ASSERT_EQ(host.committedTexts.size(), 1u);
    EXPECT_EQ(host.committedTexts.back(), "你");

    host.committedTexts.clear();
    for (char c : std::string_view("nihao")) {
        controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    EXPECT_TRUE(controller.handle({modernime::fcitx5::KeyKind::CommitLastCharacter}));
    ASSERT_EQ(host.committedTexts.size(), 1u);
    EXPECT_EQ(host.committedTexts.back(), "好");
}

TEST(ArchitectureSelectionRegression, ResetDoesNotReuseGeneration) {
    TestEngineHost host;
    TestCandidateProvider provider;

    modernime::fcitx5::ModernIMEController controller(host, &provider);

    const auto gen1 = controller.displayGeneration();
    controller.reset();
    const auto gen2 = controller.displayGeneration();
    EXPECT_GT(gen2, gen1);

    controller.reset();
    const auto gen3 = controller.displayGeneration();
    EXPECT_GT(gen3, gen2);
}

TEST(ArchitectureSelectionRegression, T11_A05_StalePageRejectsSelectionExtractionRemoval) {
    TestEngineHost host;
    TestCandidateProvider provider;

    modernime::fcitx5::ModernIMEController controller(host, &provider);

    provider.currentPage.rawInput = "nihao";
    provider.currentPage.preedit = "nihao";
    provider.currentPage.generation = 1;
    provider.currentPage.items = {
        makeItem("你好", "ni'hao", 5),
        makeItem("拟好", "ni'hao", 5)
    };

    controller.handle({modernime::fcitx5::KeyKind::Character, 'n', 0});
    ASSERT_EQ(controller.page().items.size(), 2u);

    modernime::core::CandidateSelectionRequest req;
    req.sessionId = controller.sessionId();
    req.displayGeneration = controller.displayGeneration();
    req.globalIndex = 0;
    req.expected = modernime::core::candidateIdentity(controller.page().items[0], controller.page());

    provider.currentPage.generation = 2;
    provider.currentPage.items = {
        makeItem("喜欢", "xi'huan", 6),
        makeItem("西瓜", "xi'gua", 5)
    };

    EXPECT_FALSE(controller.select(req));
    EXPECT_TRUE(host.committedTexts.empty());
    EXPECT_EQ(provider.selectCount, 0);

    EXPECT_FALSE(controller.select(0));
    EXPECT_TRUE(host.committedTexts.empty());
    EXPECT_EQ(provider.selectCount, 0);
    EXPECT_EQ(provider.commitEventCount, 0);
}

TEST(ArchitectureSelectionRegression, T11_A06_NoneSourceFullActionRejection) {
    TestEngineHost host;
    TestCandidateProvider provider;

    modernime::fcitx5::ModernIMEController controller(host, &provider);

    provider.currentPage.rawInput = "";
    provider.currentPage.preedit = "";
    provider.currentPage.generation = 1;
    modernime::core::CandidateItem noneItem = makeItem(
        "...", {}, 0, modernime::core::CommitCandidateRole::Chinese,
        modernime::core::CandidateSource::None);
    provider.currentPage.items = { noneItem };

    for (char c : std::string_view("test")) {
        controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    ASSERT_EQ(controller.page().items.size(), 1u);
    EXPECT_EQ(controller.page().items[0].source, modernime::core::CandidateSource::None);

    // 1. Direct index select rejected
    EXPECT_FALSE(controller.select(0));
    EXPECT_TRUE(host.committedTexts.empty());

    // 2. Space key rejected (does not commit None placeholder)
    EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::Space}));
    EXPECT_TRUE(host.committedTexts.empty());

    // 3. Digit selection rejected
    EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '1'}));
    EXPECT_TRUE(host.committedTexts.empty());

    // 4. Character extraction rejected
    EXPECT_FALSE(controller.extractCharacter(true));
    EXPECT_FALSE(controller.extractCharacter(false));
    EXPECT_TRUE(host.committedTexts.empty());

    // 5. Remove current rejected
    EXPECT_FALSE(controller.removeCurrent());
    EXPECT_TRUE(host.committedTexts.empty());

    // 6. Punctuation under None: zero implicit commit, composition & cursor preserved
    EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::Punctuation, ','}));
    EXPECT_TRUE(host.committedTexts.empty());
    EXPECT_EQ(controller.page().preedit, "test");

    // Test quote under None: zero implicit commit, composition preserved
    EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::Punctuation, '"'}));
    EXPECT_TRUE(host.committedTexts.empty());
    EXPECT_EQ(controller.page().preedit, "test");

    // Test bracket under None: zero implicit commit, composition preserved
    EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::Punctuation, '('}));
    EXPECT_TRUE(host.committedTexts.empty());
    EXPECT_EQ(controller.page().preedit, "test");

    // 7. Enter commits raw preedit (contract preserved)
    host.committedTexts.clear();
    controller.reset();
    provider.currentPage.rawInput = "hello";
    provider.currentPage.preedit = "hello";
    provider.currentPage.generation = 2;
    provider.currentPage.items = { noneItem };
    for (char c : std::string_view("hello")) {
        controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
    }
    EXPECT_TRUE(controller.handle({modernime::fcitx5::KeyKind::Enter}));
    ASSERT_EQ(host.committedTexts.size(), 1u);
    EXPECT_EQ(host.committedTexts[0], "hello");
}

TEST(ArchitectureSelectionRegression, T11_A06_PunctuationOptionsMatrixUnderNonePreservesComposition) {
    TestEngineHost host;
    TestCandidateProvider provider;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    modernime::core::CandidateItem noneItem = makeItem(
        "正在更新候选", {}, 0, modernime::core::CommitCandidateRole::Chinese,
        modernime::core::CandidateSource::None);

    const std::vector<char> testPunctuations = {',', '.', '!', '?', ':', ';', '\'', '"', '(', ')'};
    for (bool punctEnabled : {true, false}) {
        for (char p : testPunctuations) {
            host.committedTexts.clear();
            controller.reset();

            modernime::fcitx5::ControllerOptions opts;
            opts.punctuationEnabled = punctEnabled;
            controller.setOptions(opts);

            provider.currentPage.rawInput = "";
            provider.currentPage.preedit = "";
            provider.currentPage.generation = 10;
            provider.currentPage.items = { noneItem };

            for (char c : std::string_view("pinyin")) {
                controller.handle({modernime::fcitx5::KeyKind::Character, c, 0});
            }
            EXPECT_EQ(controller.page().preedit, "pinyin");

            // Press punctuation under None: must be rejected, zero host commit, preedit intact
            EXPECT_FALSE(controller.handle({modernime::fcitx5::KeyKind::Punctuation, p}));
            EXPECT_TRUE(host.committedTexts.empty());
            EXPECT_EQ(controller.page().preedit, "pinyin");

            // Explicit enter still works to commit raw preedit
            EXPECT_TRUE(controller.handle({modernime::fcitx5::KeyKind::Enter}));
            ASSERT_EQ(host.committedTexts.size(), 1u);
            EXPECT_EQ(host.committedTexts[0], "pinyin");
        }
    }
}

#if MODERNIME_HAS_FCITX5
TEST(ArchitectureSelectionRegression, T11_A07_ReadyNotificationRefreshesController) {
    TestEngineHost host;
    TestCandidateProvider provider;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    // Initial state: reloading / none placeholder
    provider.currentPage.rawInput = "zhong";
    provider.currentPage.preedit = "zhong";
    provider.currentPage.generation = 1;
    modernime::core::CandidateItem noneItem = makeItem(
        "正在更新候选", {}, 0, modernime::core::CommitCandidateRole::Chinese,
        modernime::core::CandidateSource::None);
    provider.currentPage.items = { noneItem };
    controller.handle({modernime::fcitx5::KeyKind::Character, 'z', 0});

    const auto initialGen = controller.displayGeneration();
    const auto preedit = controller.page().preedit;
    ASSERT_EQ(controller.page().items.size(), 1u);
    EXPECT_EQ(controller.page().items[0].source, modernime::core::CandidateSource::None);

    // Set up real Fcitx5 event loop and event dispatcher
    fcitx::EventLoop loop;
    fcitx::EventDispatcher dispatcher;
    dispatcher.attach(&loop);

    auto endpoint = std::make_shared<modernime::fcitx5::ReadyNotificationEndpoint>();
    endpoint->active = true;
    endpoint->dispatcher = &dispatcher;
    std::atomic<bool> refreshed{false};
    endpoint->onRefreshCallbackForTest = [&controller, &refreshed]() {
        controller.refreshPage();
        refreshed = true;
    };

    std::thread loopThread([&loop]() {
        loop.exec();
    });

    // When background worker finishes, real candidates become available
    provider.currentPage.generation = 2;
    provider.currentPage.items = {
        makeItem("中国", "zhong'guo", 5),
        makeItem("中", "zhong", 5)
    };

    // Worker thread invokes production endpoint->notify()
    std::thread workerThread([endpoint]() {
        endpoint->notify();
    });
    workerThread.join();

    for (int i = 0; i < 2000 && !refreshed.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(refreshed.load());

    dispatcher.schedule([&loop]() {
        loop.exit();
    });
    loopThread.join();
    dispatcher.detach();

    // Verify: without any new keys or manual refresh, event loop dispatched and updated controller
    EXPECT_GT(controller.displayGeneration(), initialGen);
    EXPECT_EQ(controller.page().preedit, preedit);
    ASSERT_EQ(controller.page().items.size(), 2u);
    EXPECT_EQ(controller.page().items[0].text, "中国");
}

TEST(ArchitectureSelectionRegression, T11_A08_ReadyNotificationSafeAgainstDestroyBlurAndClear) {
    TestEngineHost host;
    TestCandidateProvider provider;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    // 1. Old selection rejected across reset
    const auto oldGen = controller.displayGeneration();
    controller.reset();
    EXPECT_GT(controller.displayGeneration(), oldGen);

    modernime::core::CandidateSelectionRequest oldReq;
    oldReq.sessionId = controller.sessionId();
    oldReq.displayGeneration = oldGen;
    oldReq.globalIndex = 0;
    EXPECT_FALSE(controller.select(oldReq));

    fcitx::EventLoop loop;
    fcitx::EventDispatcher dispatcher;
    dispatcher.attach(&loop);

    std::thread loopThread([&loop]() {
        loop.exec();
    });

    // 2. Cancellation at Barrier 1: after acquiring dispatcher, before schedule()
    {
        auto endpoint = std::make_shared<modernime::fcitx5::ReadyNotificationEndpoint>();
        endpoint->active = true;
        endpoint->dispatcher = &dispatcher;

        std::atomic<bool> workerAtBarrier1{false};
        std::atomic<bool> mainProceed{false};

        endpoint->onBeforeScheduleForTest = [&workerAtBarrier1, &mainProceed]() {
            workerAtBarrier1 = true;
            while (!mainProceed.load()) {
                std::this_thread::yield();
            }
        };

        std::thread worker([endpoint]() {
            endpoint->notify();
        });

        // Wait until worker is paused at Barrier 1
        while (!workerAtBarrier1.load()) {
            std::this_thread::yield();
        }

        // Destructor thread calls deactivateAndDrain while worker holds an in-flight schedule
        std::thread destructorThread([endpoint, &mainProceed]() {
            mainProceed = true;
            endpoint->deactivateAndDrain();
        });

        worker.join();
        destructorThread.join();

        EXPECT_TRUE(workerAtBarrier1.load());
        EXPECT_FALSE(endpoint->active);
        EXPECT_EQ(endpoint->dispatcher, nullptr);
    }

    // 3. Cancellation at Barrier 2: after schedule(), before dispatch
    {
        auto endpoint = std::make_shared<modernime::fcitx5::ReadyNotificationEndpoint>();
        endpoint->active = true;
        endpoint->dispatcher = &dispatcher;

        std::atomic<bool> dispatchBarrierHit{false};
        std::atomic<bool> refreshRan{false};

        endpoint->onBeforeDispatchForTest = [&endpoint, &dispatchBarrierHit]() {
            dispatchBarrierHit = true;
            endpoint->deactivateAndDrain();
        };
        endpoint->onRefreshCallbackForTest = [&refreshRan]() {
            refreshRan = true;
        };

        std::thread worker([endpoint]() {
            endpoint->notify();
        });
        worker.join();

        for (int i = 0; i < 2000 && !dispatchBarrierHit.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        EXPECT_TRUE(dispatchBarrierHit.load());
        EXPECT_FALSE(refreshRan.load());
    }

    // 4. Stale generation across second clear / reset
    {
        auto endpoint = std::make_shared<modernime::fcitx5::ReadyNotificationEndpoint>();
        endpoint->active = true;
        endpoint->dispatcher = &dispatcher;

        std::atomic<bool> dispatchBarrierHit{false};
        std::atomic<bool> staleRefreshRan{false};

        endpoint->onBeforeDispatchForTest = [&endpoint, &dispatchBarrierHit]() {
            dispatchBarrierHit = true;
            std::lock_guard lock(endpoint->mutex);
            ++endpoint->notificationGeneration; // Second clear / reset before dispatch
        };
        endpoint->onRefreshCallbackForTest = [&staleRefreshRan]() {
            staleRefreshRan = true;
        };

        std::thread worker([endpoint]() {
            endpoint->notify();
        });
        worker.join();

        for (int i = 0; i < 2000 && !dispatchBarrierHit.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        EXPECT_TRUE(dispatchBarrierHit.load());
        EXPECT_FALSE(staleRefreshRan.load());
    }

    dispatcher.schedule([&loop]() {
        loop.exit();
    });
    loopThread.join();
    dispatcher.detach();

    // 5. Lost focus and sensitive mode filtering
    {
        auto endpoint = std::make_shared<modernime::fcitx5::ReadyNotificationEndpoint>();
        endpoint->active = true;
        endpoint->dispatcher = &dispatcher;

        bool hasFocus = false;
        bool isSensitive = false;
        bool dispatched = false;

        endpoint->onDispatch = [&hasFocus, &isSensitive, &dispatched]() {
            if (!hasFocus || isSensitive) {
                return;
            }
            dispatched = true;
        };

        // Lost focus -> dropped
        endpoint->dispatchLocked();
        EXPECT_FALSE(dispatched);

        // Sensitive -> dropped
        hasFocus = true;
        isSensitive = true;
        endpoint->dispatchLocked();
        EXPECT_FALSE(dispatched);

        // Focused non-sensitive -> dispatched
        isSensitive = false;
        endpoint->dispatchLocked();
        EXPECT_TRUE(dispatched);
    }
}
#endif

TEST(ArchitectureSelectionRegression, AR03_IdenticalContextDoesNotInvalidateVisibleCandidates) {
    TestEngineHost host;
    TestCandidateProvider provider;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    controller.setContext("", "", modernime::fcitx5::ContextAvailability::Valid);
    provider.currentPage.rawInput = "nihao";
    provider.currentPage.preedit = "nihao";
    provider.currentPage.generation = 1;
    provider.currentPage.items = {
        makeItem("你好", "ni'hao", 5),
        makeItem("您好", "nin'hao", 5)
    };
    controller.handle({modernime::fcitx5::KeyKind::Character, 'n', 0});
    ASSERT_FALSE(controller.page().items.empty());

    const auto initialGen = controller.displayGeneration();
    const auto initialPublishes = host.publishedPages.size();
    modernime::core::CandidateSelectionRequest req;
    req.sessionId = controller.sessionId();
    req.displayGeneration = initialGen;
    req.globalIndex = 0;
    req.expected = modernime::core::candidateIdentity(controller.page().items.front(), controller.page());

    // Repeating identical context notification must not bump displayGeneration nor republish
    controller.setContext("", "", modernime::fcitx5::ContextAvailability::Valid);
    EXPECT_EQ(controller.displayGeneration(), initialGen);
    EXPECT_EQ(host.publishedPages.size(), initialPublishes);

    // Old visible candidate request (e.g. mouse click) must still be accepted
    EXPECT_TRUE(controller.select(req));
    EXPECT_FALSE(host.committedTexts.empty());
    EXPECT_EQ(host.committedTexts.back(), "你好");
}

TEST(ArchitectureSelectionRegression, AR03_ActualContextChangeRepublishesAndRejectsOldRequest) {
    TestEngineHost host;
    TestCandidateProvider provider;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    controller.setContext("", "", modernime::fcitx5::ContextAvailability::Valid);
    provider.currentPage.rawInput = "nihao";
    provider.currentPage.preedit = "nihao";
    provider.currentPage.generation = 1;
    provider.currentPage.items = {
        makeItem("你好", "ni'hao", 5),
        makeItem("您好", "nin'hao", 5)
    };
    controller.handle({modernime::fcitx5::KeyKind::Character, 'n', 0});
    ASSERT_FALSE(controller.page().items.empty());

    const auto oldGen = controller.displayGeneration();
    const auto oldPublishes = host.publishedPages.size();
    modernime::core::CandidateSelectionRequest oldReq;
    oldReq.sessionId = controller.sessionId();
    oldReq.displayGeneration = oldGen;
    oldReq.globalIndex = 0;
    oldReq.expected = modernime::core::candidateIdentity(controller.page().items.front(), controller.page());

    // Actual context change occurs
    controller.setContext("new_context", "", modernime::fcitx5::ContextAvailability::Valid);
    EXPECT_GT(controller.displayGeneration(), oldGen);
    EXPECT_GT(host.publishedPages.size(), oldPublishes);

    // Old request holding stale displayGeneration must be rejected
    EXPECT_FALSE(controller.select(oldReq));

    // New request holding fresh displayGeneration must be accepted
    modernime::core::CandidateSelectionRequest freshReq;
    freshReq.sessionId = controller.sessionId();
    freshReq.displayGeneration = controller.displayGeneration();
    freshReq.globalIndex = 0;
    freshReq.expected = modernime::core::candidateIdentity(controller.page().items.front(), controller.page());
    EXPECT_TRUE(controller.select(freshReq));
}

TEST(ArchitectureSelectionRegression, AR03_SensitivityChangeRepublishesAndRejectsOldRequest) {
    TestEngineHost host;
    TestCandidateProvider provider;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    provider.currentPage.rawInput = "nihao";
    provider.currentPage.preedit = "nihao";
    provider.currentPage.generation = 1;
    provider.currentPage.items = {
        makeItem("你好", "ni'hao", 5),
        makeItem("您好", "nin'hao", 5)
    };
    controller.handle({modernime::fcitx5::KeyKind::Character, 'n', 0});
    ASSERT_FALSE(controller.page().items.empty());

    const auto oldGen = controller.displayGeneration();
    modernime::core::CandidateSelectionRequest oldReq;
    oldReq.sessionId = controller.sessionId();
    oldReq.displayGeneration = oldGen;
    oldReq.globalIndex = 0;
    oldReq.expected = modernime::core::candidateIdentity(controller.page().items.front(), controller.page());

    // Sensitive mode entered
    controller.setSensitive(true);
    EXPECT_GT(controller.displayGeneration(), oldGen);
    EXPECT_FALSE(controller.select(oldReq));
}

} // namespace
