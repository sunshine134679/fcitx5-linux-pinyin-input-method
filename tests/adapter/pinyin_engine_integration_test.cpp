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
    return EXIT_SUCCESS;
}
