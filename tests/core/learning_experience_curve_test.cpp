#include "modernime/core/learning_store.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"
#if defined(MODERNIME_HAS_FCITX5)
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_pagination.h"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <unistd.h>

#if defined(MODERNIME_HAS_FCITX5)
namespace {
using namespace modernime;
using Controller = fcitx5::ModernIMEController;
using Kind = fcitx5::KeyKind;

struct CurveHost final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed = text; }
};

class LearningExperienceCurve : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-learning-curve-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    pinyin::PinyinDataPaths paths(std::string_view name, bool lmdb) const {
        const auto directory = root / name / (lmdb ? "lmdb" : "sqlite");
        std::filesystem::create_directories(directory);
        pinyin::PinyinDataPaths result;
        result.learningStore = (directory / (lmdb ? "learning.lmdb" : "learning.sqlite3")).string();
        result.userDictionary = (directory / "empty-user.dict").string();
        result.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
        result.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
        return result;
    }
    static void type(Controller &controller, std::string_view input) {
        controller.setContext("", "");
        for (const char key : input)
            ASSERT_TRUE(controller.handle({Kind::Character, key}));
        ASSERT_EQ(controller.page().rawInput, input);
    }
    static std::size_t rank(const core::CandidatePage &page, std::string_view text) {
        for (std::size_t i = 0; i < page.items.size(); ++i)
            if (page.items[i].text == text) return i;
        return page.items.size();
    }
    static bool checkpoint(int count) {
        return count == 1 || count == 2 || count == 3 || count == 5 ||
               count == 10 || count == 20 || count == 50;
    }
    static ui::CandidateBarMetrics metrics() {
        auto result = ui::CandidateBarMetrics::reference(20, 9);
        result.panelWidth = 610;
        return result;
    }
    static double textWidth(std::string_view text) {
        double width = 0;
        for (const unsigned char ch : text)
            if ((ch & 0xc0) != 0x80) width += ch < 128 ? 11 : 20;
        return width;
    }
    static std::pair<std::size_t, std::size_t> position(
        const core::CandidatePage &page, std::size_t index) {
        const auto boundaries = ui::CandidatePagination::partition(page.items, metrics(), textWidth);
        for (std::size_t p = 0; p < boundaries.size(); ++p)
            if (index >= boundaries[p].begin && index < boundaries[p].end)
                return {p, index - boundaries[p].begin};
        ADD_FAILURE() << "candidate omitted by physical pagination";
        return {boundaries.size(), 0};
    }
    static void choose(Controller &controller, std::size_t index, bool digit) {
        const auto boundaries = ui::CandidatePagination::partition(
            controller.page().items, metrics(), textWidth);
        controller.setPageBoundaries(boundaries);
        const auto [physicalPage, slot] = position(controller.page(), index);
        ASSERT_LT(physicalPage, boundaries.size());
        for (std::size_t p = 0; p < physicalPage; ++p)
            ASSERT_TRUE(controller.handle({Kind::NextPage}));
        ASSERT_EQ(controller.currentPageIndex(), physicalPage);
        if (digit) {
            ASSERT_LT(slot, 9u);
            ASSERT_TRUE(controller.handle({Kind::Digit, 0, static_cast<char>('1' + slot)}));
        } else {
            const auto boundary = boundaries[physicalPage];
            core::CandidatePage visible = controller.page();
            visible.items.assign(controller.page().items.begin() + boundary.begin,
                                 controller.page().items.begin() + boundary.end);
            const auto layout = ui::CandidateBarLayout::measure(visible, metrics(), textWidth);
            ASSERT_LT(slot, layout.candidates.size());
            const auto &bounds = layout.candidates[slot].bounds;
            const auto hit = ui::hitTestCandidate(layout, metrics(),
                                                 bounds.x + bounds.width / 2,
                                                 bounds.y + bounds.height / 2);
            ASSERT_EQ(hit, static_cast<int>(slot));
            ASSERT_TRUE(controller.select(boundary.begin + static_cast<std::size_t>(hit)));
        }
    }
    static void persistedFrequency(const pinyin::PinyinDataPaths &data,
                                   std::string_view text, std::string_view pinyin,
                                   std::int64_t frequency) {
        core::LearningStore reader(data.learningStore);
        ASSERT_TRUE(reader.openReadOnly());
        const auto snapshot = reader.snapshot();
        const auto *entry = snapshot->entry(text, pinyin, {}, {});
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->frequency, frequency);
    }
    static void emit(std::string_view backend, std::string_view curve, int count,
                     const core::CandidatePage &page, std::string_view target,
                     std::string_view committed, std::size_t committedBytes,
                     std::int64_t frequency) {
        const auto index = rank(page, target);
        std::cout << "LEARNING_CURVE\t" << backend << '\t' << curve << '\t'
                  << count << '\t' << page.rawInput << '\t' << target;
        if (index == page.items.size()) {
            std::cout << "\tabsent\tabsent\tabsent\t0";
        } else {
            const auto [physicalPage, slot] = position(page, index);
            std::cout << '\t' << index << '\t' << physicalPage << '\t' << slot
                      << '\t' << page.items[index].consumedInputBytes;
        }
        std::cout << '\t' << committed << '\t' << committedBytes
                  << "\t<empty>\t" << frequency << '\n';
    }
    std::filesystem::path root;
};

TEST_F(LearningExperienceCurve, UnknownEnglishObservationAdmissionAndCompletionCurve) {
    std::cout << "LEARNING_CURVE_COLUMNS\tbackend\tcurve\tcount\tinput\ttarget"
                 "\tglobal_zero_based\tphysical_page_zero_based\tslot_zero_based"
                 "\tcandidate_consumed_bytes\tlast_actual_commit"
                 "\tlast_commit_consumed_bytes\tlast_commit_remaining\tpersisted_frequency\n";
    for (const bool lmdb : {false, true}) {
        SCOPED_TRACE(lmdb ? "LMDB" : "SQLite");
        const auto data = paths("unknown-english", lmdb);
        const std::string word = "gnueabihf", prefix = "gnue";
        {
            pinyin::PinyinCandidateProvider provider(data);
            CurveHost host;
            Controller controller(host, &provider);
            for (int count = 1; count <= 20; ++count) {
                SCOPED_TRACE(count);
                if (count <= 2) {
                    type(controller, word);
                    ASSERT_TRUE(controller.handle({Kind::Enter}));
                } else {
                    type(controller, prefix);
                    const auto index = rank(controller.page(), word);
                    ASSERT_LT(index, controller.page().items.size());
                    EXPECT_EQ(controller.page().items[index].fullPinyin, word);
                    EXPECT_EQ(controller.page().items[index].consumedInputBytes, prefix.size());
                    choose(controller, index, count % 2 == 0);
                }
                ASSERT_EQ(host.committed, word);
                ASSERT_TRUE(controller.page().rawInput.empty());
                if (!checkpoint(count)) continue;
                ASSERT_TRUE(provider.flush());
                persistedFrequency(data, word, word, count);
                type(controller, prefix);
                const auto index = rank(controller.page(), word);
                if (count == 1) {
                    EXPECT_EQ(index, controller.page().items.size());
                } else {
                    ASSERT_LT(index, controller.page().items.size());
                    EXPECT_EQ(index, 0u);
                    EXPECT_EQ(position(controller.page(), index),
                              (std::pair<std::size_t, std::size_t>{0, 0}));
                    EXPECT_EQ(controller.page().items[index].consumedInputBytes, prefix.size());
                }
                emit(lmdb ? "lmdb" : "sqlite", "unknown-english", count,
                     controller.page(), word, host.committed,
                     count <= 2 ? word.size() : prefix.size(), count);
                controller.reset();
            }
        }
        pinyin::PinyinCandidateProvider reopened(data);
        CurveHost host;
        Controller controller(host, &reopened);
        type(controller, prefix);
        ASSERT_EQ(rank(controller.page(), word), 0u);
        ASSERT_TRUE(controller.handle({Kind::Space}));
        EXPECT_EQ(host.committed, word);
        EXPECT_TRUE(controller.page().rawInput.empty());
    }
}

TEST_F(LearningExperienceCurve, ForgetStartsTheCurrentWindowAtTheFreshPreferenceCurve) {
    for (const bool lmdb : {false, true}) {
        for (const bool anotherWindowRelearnsFirst : {false, true}) {
            SCOPED_TRACE(lmdb ? "LMDB" : "SQLite");
            SCOPED_TRACE(anotherWindowRelearnsFirst);
            const auto suffix = anotherWindowRelearnsFirst ? "-shared" : "-direct";
            const auto oldData = paths(std::string("forgotten") + suffix, lmdb);
            const auto cleanData = paths(std::string("clean") + suffix, lmdb);
            pinyin::PinyinProviderOptions options;
            options.contextLearningEnabled = false;
            pinyin::PinyinCandidateProvider old(oldData, options), clean(cleanData, options);
            CurveHost oldHost, cleanHost;
            Controller oldController(oldHost, &old), cleanController(cleanHost, &clean);
            for (int i = 0; i < 3; ++i) {
                type(oldController, "shi");
                const auto index = rank(oldController.page(), "试");
                ASSERT_LT(index, oldController.page().items.size());
                choose(oldController, index, true);
                ASSERT_EQ(oldHost.committed, "试");
            }
            ASSERT_TRUE(old.flush());
            ASSERT_TRUE(old.sharedResources()->learning->enqueueForget("试", "shi"));
            ASSERT_TRUE(old.flush());
            if (anotherWindowRelearnsFirst) {
                // The original Provider never sees an empty learning record.
                // Invalidating only when frequency == 0 would miss this case.
                for (auto *provider : {&old, &clean}) {
                    pinyin::PinyinCandidateProvider other(provider->sharedResources(), options);
                    CurveHost host;
                    Controller controller(host, &other);
                    type(controller, "shi");
                    const auto index = rank(controller.page(), "试");
                    ASSERT_LT(index, controller.page().items.size());
                    choose(controller, index, true);
                    ASSERT_EQ(host.committed, "试");
                    ASSERT_TRUE(other.flush());
                }
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (auto *provider : {&old, &clean}) {
                for (int i = 0; i < 200; ++i)
                    ASSERT_TRUE(provider->sharedResources()->learning->enqueueSelection(
                        "是", "shi", {}, {}, now));
                ASSERT_TRUE(provider->flush());
            }
            type(oldController, "shi");
            type(cleanController, "shi");
            for (int count = 1; count <= 4; ++count) {
                SCOPED_TRACE(count);
                for (auto *controller : {&oldController, &cleanController}) {
                    const auto index = rank(controller->page(), "试");
                    ASSERT_LT(index, controller->page().items.size());
                    EXPECT_EQ(controller->page().items[index].consumedInputBytes, 3u);
                    choose(*controller, index, true);
                    EXPECT_TRUE(controller->page().rawInput.empty());
                    type(*controller, "shi");
                }
                EXPECT_EQ(oldHost.committed, "试");
                EXPECT_EQ(cleanHost.committed, "试");
                const auto &a = oldController.page();
                const auto &b = cleanController.page();
                const auto oldIndex = rank(a, "试"), cleanIndex = rank(b, "试");
                ASSERT_LT(oldIndex, a.items.size());
                ASSERT_LT(cleanIndex, b.items.size());
                EXPECT_EQ(a.items[oldIndex].recentChoiceBonus, b.items[cleanIndex].recentChoiceBonus);
                EXPECT_EQ(oldIndex, cleanIndex);
                EXPECT_EQ(position(a, oldIndex), position(b, cleanIndex));
                EXPECT_EQ(a.items.front().text, b.items.front().text);
            }
            ASSERT_TRUE(old.flush());
            ASSERT_TRUE(clean.flush());
            persistedFrequency(oldData, "试", "shi", anotherWindowRelearnsFirst ? 5 : 4);
            persistedFrequency(cleanData, "试", "shi", anotherWindowRelearnsFirst ? 5 : 4);
            persistedFrequency(oldData, "是", "shi", 200);
        }
    }
}

TEST_F(LearningExperienceCurve, MaturePreferencesKeepOriginalThresholdsInBothDirections) {
    // This adds the missing intermediate observations to the established
    // contracts: one click cannot replace 200 uses, Chinese switches within
    // ten choices, English within fifty, and fifty survives reopening.
    const std::vector<std::tuple<std::string, std::string, std::string, bool>> curves{
        {"compu", "computer", "compute", true},
        {"compu", "compute", "computer", true},
        {"shi", "试", "是", false}, {"shi", "是", "试", false}};
    for (const bool lmdb : {false, true}) {
        for (const auto &[input, target, incumbent, english] : curves) {
            const auto name = incumbent + "-to-" + target;
            SCOPED_TRACE(name + (lmdb ? " LMDB" : " SQLite"));
            const auto data = paths(name, lmdb);
            const auto seedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() - 60'000;
            {
                core::LearningStore seed(data.learningStore);
                ASSERT_TRUE(seed.open());
                ASSERT_TRUE(seed.recordBatch(std::vector<core::LearningEvent>(200,
                    {core::LearningEvent::Kind::Selection, incumbent,
                     english ? incumbent : input, {}, {}, seedTime})));
            }
            {
                pinyin::PinyinCandidateProvider provider(data);
                CurveHost host;
                Controller controller(host, &provider);
                type(controller, input);
                for (int count = 1; count <= 50; ++count) {
                    SCOPED_TRACE(count);
                    const auto index = rank(controller.page(), target);
                    ASSERT_LT(index, controller.page().items.size());
                    EXPECT_EQ(controller.page().items[index].fullPinyin,
                              english ? target : input);
                    EXPECT_EQ(controller.page().items[index].consumedInputBytes, input.size());
                    choose(controller, index, count % 2 == 0);
                    ASSERT_EQ(host.committed, target);
                    ASSERT_TRUE(controller.page().rawInput.empty());
                    type(controller, input);
                    if (!checkpoint(count)) continue;
                    ASSERT_TRUE(provider.flush());
                    persistedFrequency(data, target, english ? target : input, count);
                    if (count == 1) {
                        EXPECT_LT(rank(controller.page(), incumbent), rank(controller.page(), target));
                    }
                    if (!english && count >= 10) {
                        EXPECT_EQ(controller.page().items.front().text, target);
                    }
                    if (english && count == 50) {
                        EXPECT_LT(rank(controller.page(), target), rank(controller.page(), incumbent));
                    }
                    emit(lmdb ? "lmdb" : "sqlite", name, count, controller.page(),
                         target, host.committed, input.size(), count);
                }
            }
            pinyin::PinyinCandidateProvider reopened(data);
            CurveHost host;
            Controller controller(host, &reopened);
            type(controller, input);
            if (english) {
                EXPECT_LT(rank(controller.page(), target), rank(controller.page(), incumbent));
            } else {
                EXPECT_EQ(controller.page().items.front().text, target);
            }
            const auto index = rank(controller.page(), target);
            ASSERT_LT(index, controller.page().items.size());
            choose(controller, index, true);
            EXPECT_EQ(host.committed, target);
            EXPECT_TRUE(controller.page().rawInput.empty());
        }
    }
}
} // namespace
#endif
