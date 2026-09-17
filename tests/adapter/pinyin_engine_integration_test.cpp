#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin engine integration test failed: " << message
                  << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct RecordingHost final : modernime::fcitx5::EngineHost {
    std::vector<modernime::core::CandidatePage> pages;
    std::vector<std::string> commits;

    void publishPage(const modernime::core::CandidatePage &page) override {
        pages.push_back(page);
    }

    void commit(std::string_view text) override { commits.emplace_back(text); }
};

std::size_t indexOf(const modernime::core::CandidatePage &page,
                    std::string_view text) {
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        if (page.items[index].text == text) {
            return index;
        }
    }
    return page.items.size();
}

} // namespace

int main() {
    modernime::pinyin::PinyinCandidateProvider provider;
    RecordingHost host;
    modernime::fcitx5::ModernIMEController controller(host, &provider);

    for (int iteration = 0; iteration < 100; ++iteration) {
        for (const char character : std::string_view("nihao")) {
            assertTrue(controller.handle({
                           modernime::fcitx5::KeyKind::Character, character, 0}),
                       "pinyin character is handled");
        }
        assertTrue(controller.page().preedit == "ni'hao",
                   "controller exposes segmented LibIME preedit");
        assertTrue(!controller.page().items.empty(),
                   "controller exposes LibIME candidates");
        assertTrue(controller.page().items.front().text == "你好",
                   "controller exposes ranked top candidate");
        assertTrue(controller.handle(
                       {modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits the top LibIME candidate");
        assertTrue(host.commits.back() == "你好",
                   "top LibIME candidate is committed");
        assertTrue(controller.page().preedit.empty(),
                   "commit clears the controller page");
    }

    for (const char character : std::string_view("nihao")) {
        assertTrue(controller.handle(
                       {modernime::fcitx5::KeyKind::Character, character, 0}),
                   "editable pinyin input is accepted");
    }
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}) &&
                   controller.handle(
                       {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}),
               "composition cursor moves inside segmented pinyin");
    assertTrue(controller.page().preedit == "ni'hao" &&
                   controller.page().preeditCursor == 4,
               "raw cursor maps across an automatically inserted separator");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Character, 'n', 0}),
               "LibIME composition accepts insertion in the middle");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Backspace, 0, 0}) &&
                   controller.page().preedit == "ni'hao" &&
                   controller.page().items.front().text == "你好",
               "middle backspace restores candidates without retyping the phrase");

    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "edited composition is committed before the partial test");
    for (const char character : std::string_view("nihaoalaodi")) {
        assertTrue(controller.handle(
                       {modernime::fcitx5::KeyKind::Character, character, 0}),
                   "long pinyin input is accepted");
    }
    const auto phraseIndex = indexOf(controller.page(), "你好啊");
    assertTrue(phraseIndex < controller.page().items.size(),
               "controller exposes the phrase-prefix candidate");
    assertTrue(controller.select(phraseIndex),
               "controller selects the phrase-prefix candidate");
    assertTrue(host.commits.back() == "你好啊",
               "partial selection commits only the selected phrase");
    assertTrue(controller.page().rawInput == "laodi" &&
                   controller.page().preedit == "lao'di",
               "controller keeps the remaining pinyin active");
    // Commit the remaining "laodi" from previous test
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "commit remaining partial pinyin");
    assertTrue(controller.page().preedit.empty(), "controller is clear");

    // Test English words: fact, good, apple, test commit directly with space
    for (const char* word : {"fact", "good", "apple", "test", "date"}) {
        for (const char character : std::string_view(word)) {
            assertTrue(controller.handle({
                           modernime::fcitx5::KeyKind::Character, character, 0}),
                       "character is handled");
        }
        if (controller.page().items.empty() || controller.page().items.front().text != word) {
            std::cerr << "FAILED on word: " << word << ", actual top: " 
                      << (controller.page().items.empty() ? "EMPTY" : controller.page().items.front().text) 
                      << " (source: " << (controller.page().items.empty() ? -1 : (int)controller.page().items.front().source) << ")" << std::endl;
        }
        assertTrue(!controller.page().items.empty() &&
                       controller.page().items.front().text == word,
                   "English word is the top candidate");
        assertTrue(controller.page().preedit == word,
                   "English preedit has no apostrophe segmentation");
        assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits the English word");
        assertTrue(host.commits.back() == word,
                   "the English word is committed");
    }

    // Test trusted abbreviation: bj commits 北京
    for (const char character : std::string_view("bj")) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "bj character is handled");
    }
    assertTrue(!controller.page().items.empty() &&
                   controller.page().items.front().text == "北京",
               "bj top candidate is 北京");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space commits 北京");
    assertTrue(host.commits.back() == "北京",
               "北京 is committed");

    // Test English prefix prediction: gara commits garage with Space
    for (const char character : std::string_view("gara")) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "gara character is handled");
    }
    assertTrue(!controller.page().items.empty() &&
                   controller.page().items.front().text == "garage",
               "gara top candidate is garage");
    assertTrue(controller.page().items.size() > 1 &&
                   controller.page().items[1].text == "gara",
               "gara second candidate is raw gara");
    assertTrue(controller.page().preedit == "gara",
               "gara preedit has no apostrophes");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space commits garage");
    assertTrue(host.commits.back() == "garage",
               "garage is committed");

    // Test English prefix raw selection: gara with 2 commits raw gara
    for (const char character : std::string_view("gara")) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "gara character is handled");
    }
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "digit 2 selects raw gara");
    assertTrue(host.commits.back() == "gara",
               "raw gara is committed");

    // Test English prefix prediction: appl commits apple with Space
    for (const char character : std::string_view("appl")) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "appl character is handled");
    }
    assertTrue(!controller.page().items.empty() &&
                   controller.page().items.front().text == "apple",
               "appl top candidate is apple");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space commits apple");
    assertTrue(host.commits.back() == "apple",
               "apple is committed");


    // Test Up/Down key page flipping in normal candidate mode
    for (const char character : std::string_view("hao")) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "hao character is handled");
    }
    assertTrue(!controller.page().items.empty(), "hao has candidates");
    const auto initialCursor = controller.page().cursor;
    assertTrue(initialCursor == 0, "initial cursor is at 0");
    assertTrue(controller.page().pageBoundaries.size() > 1,
               "hao has multiple candidate pages");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::NextClipboardItem, 0, 0}),
               "Down key moves to next candidate page");
    assertTrue(controller.page().cursor == controller.page().pageBoundaries[1].begin,
               "cursor advanced to next page boundary");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::PreviousClipboardItem, 0, 0}),
               "Up key moves back to previous candidate page");
    assertTrue(controller.page().cursor == 0,
               "cursor returned to first page boundary");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space commits top candidate");


    // Test repeated selection habit: selecting deep candidate elevates it to rank 0
    const auto repeatLearningDbPath = "/tmp/integration_repeat_learn.sqlite3";

    // Test dual-attribute word like adaptive elevation in controller
    const auto dualLearningDbPath = "/tmp/integration_dual_learn.sqlite3";
    std::filesystem::remove(dualLearningDbPath);
    std::filesystem::remove(std::string(dualLearningDbPath) + "-wal");
    std::filesystem::remove(std::string(dualLearningDbPath) + "-shm");
    {
        modernime::pinyin::PinyinDataPaths dualPaths;
        dualPaths.learningStore = dualLearningDbPath;
        modernime::pinyin::PinyinCandidateProvider dualProvider(dualPaths);
        RecordingHost dualHost;
        modernime::fcitx5::ModernIMEController dualController(dualHost, &dualProvider);

        // Initially typing like: candidate 0 is 立刻, candidate 1 is like
        for (char c : std::string_view("like")) {
            dualController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!dualController.page().items.empty() &&
                   dualController.page().items.front().text == "立刻",
                   "initially like defaults to 立刻");
        const auto likeIdx = indexOf(dualController.page(), "like");
        assertTrue(likeIdx == 1, "like is candidate 1");
        
        // Select candidate 1 (English like)
        dualController.select(1);
        assertTrue(dualHost.commits.back() == "like", "like is committed by select(1)");

        // Re-type like: now like is elevated to top candidate 0
        for (char c : std::string_view("like")) {
            dualController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!dualController.page().items.empty() &&
                   dualController.page().items.front().text == "like",
                   "after selection, like is elevated to top candidate (rank 0)");
        assertTrue(dualController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits like");
        assertTrue(dualHost.commits.back() == "like", "like is committed directly by space");
    }
    std::filesystem::remove(dualLearningDbPath);
    std::filesystem::remove(std::string(dualLearningDbPath) + "-wal");
    std::filesystem::remove(std::string(dualLearningDbPath) + "-shm");

    std::filesystem::remove(repeatLearningDbPath);
    std::filesystem::remove(std::string(repeatLearningDbPath) + "-wal");
    std::filesystem::remove(std::string(repeatLearningDbPath) + "-shm");
    {
        modernime::pinyin::PinyinDataPaths repeatPaths;
        repeatPaths.learningStore = repeatLearningDbPath;
        modernime::pinyin::PinyinCandidateProvider repeatProvider(repeatPaths);
        RecordingHost repeatHost;
        modernime::fcitx5::ModernIMEController repeatController(repeatHost, &repeatProvider);

        // Initially type zd: '中断' is around index 53
        for (char c : std::string_view("zd")) {
            repeatController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        std::size_t initialZhongduanIdx = indexOf(repeatController.page(), "中断");
        assertTrue(initialZhongduanIdx > 10, "initially 中断 is not on first two pages");

        // Repeat select 中断 10 times
        for (int cycle = 0; cycle < 10; ++cycle) {
            std::size_t idx = indexOf(repeatController.page(), "中断");
            if (idx < repeatController.page().items.size()) {
                repeatController.select(idx);
            }
            for (char c : std::string_view("zd")) {
                repeatController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
            }
        }
        assertTrue(!repeatController.page().items.empty() &&
                   repeatController.page().items.front().text == "中断",
                   "after 10 selections, 中断 is elevated to top candidate (rank 0)");
        assertTrue(repeatController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits 中断");
        assertTrue(repeatHost.commits.back() == "中断",
                   "中断 is committed directly by space");
    }
    std::filesystem::remove(repeatLearningDbPath);
    std::filesystem::remove(std::string(repeatLearningDbPath) + "-wal");
    std::filesystem::remove(std::string(repeatLearningDbPath) + "-shm");

    // Test typo transposition yuedign -> 约定 space commit
    {
        modernime::pinyin::PinyinDataPaths typoPaths;
        modernime::pinyin::PinyinCandidateProvider typoProvider(typoPaths);
        RecordingHost typoHost;
        modernime::fcitx5::ModernIMEController typoController(typoHost, &typoProvider);

        for (char c : std::string_view("yuedign")) {
            typoController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!typoController.page().items.empty(), "yuedign has candidates in engine");
        assertTrue(typoController.page().items.front().text == "约定",
                   "yuedign typo candidate ranks 约定 at rank 0");
        assertTrue(typoController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits 约定");
        assertTrue(typoHost.commits.back() == "约定",
                   "yuedign committed 约定 directly by space");
    }

    
    // Stress test: rapid typing and short/incomplete syllables
    {
        modernime::pinyin::PinyinDataPaths stressPaths;
        modernime::pinyin::PinyinCandidateProvider stressProvider(stressPaths);
        RecordingHost stressHost;
        modernime::fcitx5::ModernIMEController stressController(stressHost, &stressProvider);

        for (char c1 = 'a'; c1 <= 'z'; ++c1) {
            stressController.handle({modernime::fcitx5::KeyKind::Character, c1, 0});
            stressController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});
        }

        for (char c1 = 'a'; c1 <= 'f'; ++c1) {
            for (char c2 = 'a'; c2 <= 'f'; ++c2) {
                stressController.handle({modernime::fcitx5::KeyKind::Character, c1, 0});
                stressController.handle({modernime::fcitx5::KeyKind::Character, c2, 0});
                stressController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});
            }
        }
    }

    return EXIT_SUCCESS;
}
