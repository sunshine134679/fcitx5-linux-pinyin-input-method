#include "modernime/fcitx5/engine.h"
#include "modernime/core/learning_store.h"
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

        // Re-type like: 常用高频词汇（如 立刻、那么）坚守中文优先原则，中文词语仍稳居首位（rank 0）
        for (char c : std::string_view("like")) {
            dualController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!dualController.page().items.empty() &&
                   dualController.page().items.front().text == "立刻",
                   "high-frequency Chinese word 立刻 remains at rank 0 after selecting like");
        assertTrue(indexOf(dualController.page(), "like") == 1,
                   "like remains candidate 1");
        assertTrue(dualController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits 立刻");
        assertTrue(dualHost.commits.back() == "立刻", "立刻 is committed directly by space");

        // 验证 name -> 那么：无论是否偶选过英文 name，输入 name 空格上屏依旧是“那么”
        for (char c : std::string_view("name")) {
            dualController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!dualController.page().items.empty() &&
                   dualController.page().items.front().text == "那么",
                   "initially name defaults to 那么");
        const auto nameIdx = indexOf(dualController.page(), "name");
        assertTrue(nameIdx == 1, "name is candidate 1");
        dualController.select(1); // 偶选英文 name
        assertTrue(dualHost.commits.back() == "name", "name is committed by select(1)");

        // 再次输入 name：常用高频词汇 那么 仍稳居首位，空格直接上屏 那么
        for (char c : std::string_view("name")) {
            dualController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!dualController.page().items.empty() &&
                   dualController.page().items.front().text == "那么",
                   "high-frequency Chinese word 那么 remains at rank 0 after selecting name");
        assertTrue(dualController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "space commits 那么");
        assertTrue(dualHost.commits.back() == "那么", "那么 is committed directly by space");
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

    
    // Settings clears in WAL must reset ranking in shared, already-active providers.
    {
        const auto path = std::filesystem::temp_directory_path() /
                          "modernime-ranking-wal-clear.sqlite3";
        std::filesystem::remove(path);
        modernime::pinyin::PinyinDataPaths paths;
        paths.learningStore = path;
        auto shared = modernime::pinyin::PinyinCandidateProvider::createSharedResources(paths, {});
        modernime::pinyin::PinyinCandidateProvider active(shared);
        for (int cycle = 0; cycle < 10; ++cycle) {
            active.append("zd");
            const auto index = indexOf(active.page(), "中断");
            assertTrue(index < active.page().items.size() && active.select(index),
                       "active provider records repeated deep-candidate selections");
        }
        assertTrue(modernime::pinyin::PinyinCandidateProvider::reloadLearningStore(shared),
                   "learning is durable before the settings clear");
        active.append("zd");
        assertTrue(active.page().items.front().text == "中断",
                   "learned ranking is visible before external clear");
        active.reset();
        modernime::core::LearningStore settings(path);
        assertTrue(settings.open(), "settings opens the active engine database");
        const auto before = std::filesystem::last_write_time(path);
        assertTrue(settings.clear() && std::filesystem::last_write_time(path) == before,
                   "real settings clear stays in WAL while engine connection is open");
        assertTrue(modernime::pinyin::PinyinCandidateProvider::refreshLearningStore(shared),
                   "engine polling API observes the external WAL reset");
        active.append("zd");
        assertTrue(indexOf(active.page(), "中断") > 10,
                   "an existing provider no longer ranks the cleared learned candidate first");
        assertTrue(!modernime::pinyin::PinyinCandidateProvider::refreshLearningStore(shared),
                   "unchanged learning does not trigger another reload");
        assertTrue(settings.clear(), "settings clears again before the next timer tick");
        const auto selected = indexOf(active.page(), "中断");
        assertTrue(selected < active.page().items.size() && active.select(selected),
                   "active provider accepts its first new selection before polling");
        assertTrue(modernime::pinyin::PinyinCandidateProvider::reloadLearningStore(shared),
                   "first post-clear selection is flushed through the real provider");
        const auto postClear = settings.snapshot();
        assertTrue(postClear->entries().size() == 1 &&
                       postClear->entries().front().phrase == "中断" &&
                       postClear->entries().front().frequency == 1,
                   "first post-clear provider selection persists without old learned frequency");
        settings.close();
        active.reset();
        std::filesystem::remove(path);
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

    // Test context association in controller without SurroundingText:
    {
        modernime::pinyin::PinyinDataPaths ctxPaths;
        const auto ctxLearningPath = std::filesystem::temp_directory_path() / "modernime-engine-ctx.sqlite3";
        std::error_code ec;
        std::filesystem::remove(ctxLearningPath, ec);
        ctxPaths.learningStore = ctxLearningPath.string();
        modernime::pinyin::PinyinProviderOptions ctxOptions;
        ctxOptions.learningEnabled = true;
        ctxOptions.contextLearningEnabled = true;
        modernime::pinyin::PinyinCandidateProvider ctxProvider(ctxPaths, ctxOptions);
        RecordingHost ctxHost;
        modernime::fcitx5::ModernIMEController ctxController(ctxHost, &ctxProvider);

        // Train: commit 王 (no surrounding text, setContext("", "") called)
        ctxController.setContext("", "");
        for (char c : std::string_view("wang")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        ctxController.select(indexOf(ctxController.page(), "王"));

        // Train: commit 绍
        ctxController.setContext("", "");
        for (char c : std::string_view("shao")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        ctxController.select(indexOf(ctxController.page(), "绍"));

        // Train: commit 龙
        ctxController.setContext("", "");
        for (char c : std::string_view("long")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        ctxController.select(indexOf(ctxController.page(), "龙"));

        ctxProvider.flush();

        // Now test sequence: reset and start fresh
        ctxController.reset();

        // 1. Commit 王
        ctxController.setContext("", "");
        for (char c : std::string_view("wang")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        ctxController.select(indexOf(ctxController.page(), "王"));

        // 2. Type shao without surrounding text:
        ctxController.setContext("", "");
        for (char c : std::string_view("shao")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!ctxController.page().items.empty() &&
                   ctxController.page().items.front().text == "绍",
                   "controller maintains fallback context '王' and boosts 绍 to rank 0");
        ctxController.select(0);

        // 3. Type long without surrounding text:
        ctxController.setContext("", "");
        for (char c : std::string_view("long")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!ctxController.page().items.empty() &&
                   ctxController.page().items.front().text == "龙",
                   "controller maintains fallback context '王绍' and boosts 龙 to rank 0");
        ctxController.select(0);

        // 4. Punctuation resets fallback context:
        ctxController.handle({modernime::fcitx5::KeyKind::Punctuation, ',', 0});
        ctxController.setContext("", "");
        for (char c : std::string_view("shao")) {
            ctxController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!ctxController.page().items.empty() &&
                   ctxController.page().items.front().text != "绍",
                   "punctuation resets fallback context so 绍 is not rank 0");
        ctxController.reset();

        std::filesystem::remove(ctxLearningPath, ec);
        std::filesystem::remove(ctxLearningPath.string() + "-wal", ec);
        std::filesystem::remove(ctxLearningPath.string() + "-shm", ec);
    }

    // Test custom English term learning via Enter and prefix completion:
    {
        modernime::pinyin::PinyinDataPaths engPaths;
        const auto engLearningPath = std::filesystem::temp_directory_path() / "modernime-engine-eng.sqlite3";
        std::error_code ec;
        std::filesystem::remove(engLearningPath, ec);
        engPaths.learningStore = engLearningPath.string();
        modernime::pinyin::PinyinProviderOptions engOptions;
        engOptions.learningEnabled = true;
        modernime::pinyin::PinyinCandidateProvider engProvider(engPaths, engOptions);
        RecordingHost engHost;
        modernime::fcitx5::ModernIMEController engController(engHost, &engProvider);

        // Commit gnueabihf 5 times via Enter
        for (int i = 0; i < 5; ++i) {
            for (char c : std::string_view("gnueabihf")) {
                engController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
            }
            assertTrue(engController.handle({modernime::fcitx5::KeyKind::Enter, 0, 0}),
                       "Enter commits raw gnueabihf");
            assertTrue(engHost.commits.back() == "gnueabihf", "host committed gnueabihf");
        }
        engProvider.flush();

        // 1. gnu -> top candidate is gnueabihf, space commits it
        for (char c : std::string_view("gnu")) {
            engController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!engController.page().items.empty() &&
                   engController.page().items.front().text == "gnueabihf",
                   "gnu predicts gnueabihf at rank 0");
        assertTrue(engController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
                   "Space commits gnueabihf");
        assertTrue(engHost.commits.back() == "gnueabihf", "host committed gnueabihf via Space");

        // 2. gnue -> top candidate is gnueabihf
        for (char c : std::string_view("gnue")) {
            engController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!engController.page().items.empty() &&
                   engController.page().items.front().text == "gnueabihf",
                   "gnue predicts gnueabihf at rank 0");
        engController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 3. gnuea -> top candidate is gnueabihf
        for (char c : std::string_view("gnuea")) {
            engController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!engController.page().items.empty() &&
                   engController.page().items.front().text == "gnueabihf",
                   "gnuea predicts gnueabihf at rank 0");
        engController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 4. Escape cancels without learning:
        for (char c : std::string_view("notlearned")) {
            engController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        engController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});
        engProvider.flush();

        for (char c : std::string_view("notle")) {
            engController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(indexOf(engController.page(), "notlearned") == engController.page().items.size(),
                   "cancelled term is not learned");
        engController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        std::filesystem::remove(engLearningPath, ec);
        std::filesystem::remove(engLearningPath.string() + "-wal", ec);
        std::filesystem::remove(engLearningPath.string() + "-shm", ec);
    }

    // 显式取首/末字及标点回退集成测试
    {
        std::error_code ec;
        const auto extractLearningPath =
            std::filesystem::temp_directory_path() / "modernime-engine-extract.sqlite3";
        std::filesystem::remove(extractLearningPath, ec);
        std::filesystem::remove(extractLearningPath.string() + "-wal", ec);
        std::filesystem::remove(extractLearningPath.string() + "-shm", ec);

        modernime::pinyin::PinyinDataPaths extractPaths;
        extractPaths.learningStore = extractLearningPath.string();
        modernime::pinyin::PinyinCandidateProvider extractProvider(extractPaths);
        RecordingHost extractHost;
        modernime::fcitx5::ModernIMEController extractController(
            extractHost, &extractProvider);

        // 1. 输入 jianpan -> 首选 键盘
        for (char c : std::string_view("jianpan")) {
            extractController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!extractController.page().items.empty() &&
                   extractController.page().items.front().text == "键盘",
                   "jianpan produces 键盘 as top candidate");

        // 2. 按 [ (CommitFirstCharacter) 取首字
        assertTrue(extractController.handle(
                       {modernime::fcitx5::KeyKind::CommitFirstCharacter, '[', 0}),
                   "[ key extracts first character");
        assertTrue(extractHost.commits.back() == "键", "committed first character 键");
        assertTrue(extractController.page().preedit.empty(), "preedit cleared");
        extractProvider.flush();

        // 3. 验证温和提升：单次取字即被标记为 Learned 并获得学习加分
        for (char c : std::string_view("jian")) {
            extractController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto singleExtractIdx = indexOf(extractController.page(), "键");
        assertTrue(singleExtractIdx < extractController.page().items.size(),
                   "found 键 in candidates after single extraction");
        assertTrue(extractController.page().items[singleExtractIdx].source ==
                       modernime::core::CandidateSource::Learned,
                   "键 is classified as Learned after single extraction");
        extractController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 多次明确取字（累计 5 次）：逐步晋升至首位
        for (int rep = 0; rep < 4; ++rep) {
            for (char c : std::string_view("jianpan")) {
                extractController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
            }
            extractController.handle(
                {modernime::fcitx5::KeyKind::CommitFirstCharacter, '[', 0});
        }
        extractProvider.flush();

        for (char c : std::string_view("jian")) {
            extractController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!extractController.page().items.empty() &&
                   extractController.page().items.front().text == "键",
                   "键 is promoted to top candidate after repeated extractions");
        extractController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 4. 输入 jianpan，按 ] (CommitLastCharacter) 取末字
        for (int rep = 0; rep < 5; ++rep) {
            for (char c : std::string_view("jianpan")) {
                extractController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
            }
            assertTrue(extractController.handle(
                           {modernime::fcitx5::KeyKind::CommitLastCharacter, ']', 0}),
                       "] key extracts last character");
            assertTrue(extractHost.commits.back() == "盘", "committed last character 盘");
            assertTrue(extractController.page().preedit.empty(), "preedit cleared");
        }
        extractProvider.flush();

        // 5. 验证学习：输入 pan，多次取字后 盘 晋升至首位
        for (char c : std::string_view("pan")) {
            extractController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!extractController.page().items.empty() &&
                   extractController.page().items.front().text == "盘",
                   "盘 is promoted to top candidate for pan after extraction learning");
        extractController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 6. 空闲状态下按 [ 和 ]：回退至中文标点 【 和 】
        assertTrue(extractController.handle(
                       {modernime::fcitx5::KeyKind::CommitFirstCharacter, '[', 0}),
                   "[ commits Chinese punctuation when idle");
        assertTrue(extractHost.commits.back() == "【", "committed Chinese punctuation 【");

        assertTrue(extractController.handle(
                       {modernime::fcitx5::KeyKind::CommitLastCharacter, ']', 0}),
                   "] commits Chinese punctuation when idle");
        assertTrue(extractHost.commits.back() == "】", "committed Chinese punctuation 】");

        std::filesystem::remove(extractLearningPath, ec);
        std::filesystem::remove(extractLearningPath.string() + "-wal", ec);
        std::filesystem::remove(extractLearningPath.string() + "-shm", ec);
    }

    // -------------------------------------------------------------------------
    // Task 9 (P2): 经真实周边文本确认的后续删除反馈（键盘删除盘后学键）
    // -------------------------------------------------------------------------
    {
        std::error_code ec;
        const auto delLearningPath =
            std::filesystem::temp_directory_path() / "modernime-engine-del-feedback.sqlite3";
        std::filesystem::remove(delLearningPath, ec);
        std::filesystem::remove(delLearningPath.string() + "-wal", ec);
        std::filesystem::remove(delLearningPath.string() + "-shm", ec);

        modernime::pinyin::PinyinDataPaths delPaths;
        delPaths.learningStore = delLearningPath.string();
        modernime::pinyin::PinyinCandidateProvider delProvider(delPaths);
        RecordingHost delHost;
        modernime::fcitx5::ModernIMEController delController(
            delHost, &delProvider);

        // 1. 设置周边上下文 "使用"，输入 jianpan，选择候选 "键盘"
        delController.setContext("使用", "");
        for (char c : std::string_view("jianpan")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto jianpanIdx = indexOf(delController.page(), "键盘");
        assertTrue(jianpanIdx < delController.page().items.size(),
                   "found 键盘 in candidates");
        assertTrue(delController.select(jianpanIdx), "selected 键盘");
        assertTrue(delHost.commits.back() == "键盘", "committed 键盘");
        assertTrue(delController.page().preedit.empty(), "composition empty after commit");

        // 2. 空闲状态按 Backspace：控制器放行给宿主应用
        assertTrue(!delController.handle(
                       {modernime::fcitx5::KeyKind::Backspace, 0, 0}),
                   "Backspace when idle passes through to client application");

        // 3. 宿主应用删除末字 "盘"，周边文本从 "使用键盘" 更新为 "使用键"
        delController.setContext("使用键", "");
        delProvider.flush();

        // 4. 验证单次删除反馈学习：输入 jian，"键" 已被标记为 Learned
        for (char c : std::string_view("jian")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto learnedJianIdx = indexOf(delController.page(), "键");
        assertTrue(learnedJianIdx < delController.page().items.size(),
                   "found 键 in candidates after deletion feedback");
        assertTrue(delController.page().items[learnedJianIdx].source ==
                       modernime::core::CandidateSource::Learned,
                   "键 is Learned after deletion feedback");
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 5. 多次重复组词删除反馈（累计 5 次）：逐步将 "键" 晋升至首位
        for (int rep = 0; rep < 4; ++rep) {
            delController.setContext("使用", "");
            for (char c : std::string_view("jianpan")) {
                delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
            }
            const auto idx = indexOf(delController.page(), "键盘");
            delController.select(idx);
            delController.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0});
            delController.setContext("使用键", "");
        }
        delProvider.flush();

        delController.setContext("使用", "");
        for (char c : std::string_view("jian")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!delController.page().items.empty() &&
                   delController.page().items.front().text == "键",
                   "键 is promoted to top candidate after repeated deletion feedback");
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 6. 反例验证 1：删除整词（"使用键盘" -> "使用"）不误学字
        delController.setContext("使用", "");
        for (char c : std::string_view("ceshi")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        delController.select(0); // 提交 "测试"
        delController.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0});
        // 模拟全选删除整词：前后文本恢复为 "使用"
        delController.setContext("使用", "");
        delProvider.flush();
        // 验证没有误学单字 "测"
        for (char c : std::string_view("ce")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto ceIdx = indexOf(delController.page(), "测");
        if (ceIdx < delController.page().items.size()) {
            assertTrue(delController.page().items[ceIdx].source !=
                           modernime::core::CandidateSource::Learned,
                       "ce does not falsely learn 测 when entire word was deleted");
        }
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 7. 反例验证 2：光标跳转 / 编辑别处文字不误学字
        delController.setContext("前文", "");
        for (char c : std::string_view("ceshi")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        delController.select(0); // 提交 "测试"
        // 光标跳转至完全不同的位置
        delController.setContext("完全不同位置", "");
        delProvider.flush();
        for (char c : std::string_view("ce")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto ceIdx2 = indexOf(delController.page(), "测");
        if (ceIdx2 < delController.page().items.size()) {
            assertTrue(delController.page().items[ceIdx2].source !=
                           modernime::core::CandidateSource::Learned,
                       "ce does not falsely learn 测 on cursor jump");
        }
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 8. 反例验证 3：重置 / 取消组合清除最近提交记录
        delController.setContext("文档", "");
        for (char c : std::string_view("ceshi")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        delController.select(0);
        delController.reset();
        delController.setContext("文档测", ""); // 即使传入有效前缀，由于 reset 也不会触发
        delProvider.flush();
        for (char c : std::string_view("ce")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto ceIdx3 = indexOf(delController.page(), "测");
        if (ceIdx3 < delController.page().items.size()) {
            assertTrue(delController.page().items[ceIdx3].source !=
                           modernime::core::CandidateSource::Learned,
                       "ce does not learn 测 after explicit reset");
        }
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        std::filesystem::remove(delLearningPath, ec);
        std::filesystem::remove(delLearningPath.string() + "-wal", ec);
        std::filesystem::remove(delLearningPath.string() + "-shm", ec);
    }

    // -------------------------------------------------------------------------
    // Task 10: 成语/词语输入后删词留字（后缀与内部单字提取）即时提权至首位
    // -------------------------------------------------------------------------
    {
        std::error_code ec;
        const auto delLearningPath =
            std::filesystem::temp_directory_path() / "modernime-engine-del-feedback-idiom.sqlite3";
        std::filesystem::remove(delLearningPath, ec);
        std::filesystem::remove(delLearningPath.string() + "-wal", ec);
        std::filesystem::remove(delLearningPath.string() + "-shm", ec);

        modernime::pinyin::PinyinDataPaths delPaths;
        delPaths.learningStore = delLearningPath.string();
        modernime::pinyin::PinyinCandidateProvider delProvider(delPaths);
        RecordingHost delHost;
        modernime::fcitx5::ModernIMEController delController(
            delHost, &delProvider);

        // 1. 后缀留字测试：输入 jieshao，选择候选 "介绍"
        delController.setContext("我来", "");
        for (char c : std::string_view("jieshao")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto jieshaoIdx = indexOf(delController.page(), "介绍");
        assertTrue(jieshaoIdx < delController.page().items.size(), "found 介绍 in candidates");
        assertTrue(delController.select(jieshaoIdx), "selected 介绍");
        assertTrue(delHost.commits.back() == "介绍", "committed 介绍");

        // 删去前缀 "介"，留下目标字 "绍"，周边文本更新为 "我来绍"
        delController.setContext("我来绍", "");
        delProvider.flush();

        // 验证即时提权：输入 shao，目标字 "绍" 即跃升为第 1 候选！
        delController.setContext("我来", "");
        for (char c : std::string_view("shao")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!delController.page().items.empty(), "has candidates for shao");
        assertTrue(delController.page().items.front().text == "绍",
                   "绍 is immediately promoted to top candidate after suffix extraction");
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        // 2. 4 字成语多步退格与内部单字提取测试：输入 hualongdianjing，选择候选 "画龙点睛"
        delController.setContext("文章", "");
        for (char c : std::string_view("hualongdianjing")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        const auto hldjIdx = indexOf(delController.page(), "画龙点睛");
        assertTrue(hldjIdx < delController.page().items.size(), "found 画龙点睛 in candidates");
        assertTrue(delController.select(hldjIdx), "selected 画龙点睛");

        // 模拟用户连续退格删去 "睛"、"点"，保留 "画龙"
        delController.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0});
        delController.setContext("文章画龙点", "");
        delController.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0});
        delController.setContext("文章画龙", "");

        // 模拟用户删除 "画"，精确定位留下目标字 "龙"
        delController.setContext("文章龙", "");
        delProvider.flush();

        // 验证即时提权：输入 long，目标字 "龙" 稳居第 1 候选！
        delController.setContext("文章", "");
        for (char c : std::string_view("long")) {
            delController.handle({modernime::fcitx5::KeyKind::Character, c, 0});
        }
        assertTrue(!delController.page().items.empty(), "has candidates for long");
        assertTrue(delController.page().items.front().text == "龙",
                   "龙 is immediately promoted to top candidate after idiom internal character extraction");
        delController.handle({modernime::fcitx5::KeyKind::Escape, 0, 0});

        std::filesystem::remove(delLearningPath, ec);
        std::filesystem::remove(delLearningPath.string() + "-wal", ec);
        std::filesystem::remove(delLearningPath.string() + "-shm", ec);
    }

    return EXIT_SUCCESS;
}
