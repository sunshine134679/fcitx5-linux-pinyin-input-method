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

    return EXIT_SUCCESS;
}
