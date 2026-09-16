#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <cstdlib>
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
    for (const char* word : {"fact", "good", "apple", "test"}) {
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

    return EXIT_SUCCESS;
}
