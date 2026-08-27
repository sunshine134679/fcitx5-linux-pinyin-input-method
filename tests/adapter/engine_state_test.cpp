#include "modernime/fcitx5/engine.h"
#include "modernime/core/candidate_provider.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "engine state test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct RecordingHost final : modernime::fcitx5::EngineHost {
    std::vector<modernime::core::CandidatePage> pages;
    std::vector<std::string> commits;

    void publishPage(const modernime::core::CandidatePage &page) override {
        pages.push_back(page);
    }

    void commit(std::string_view text) override {
        commits.emplace_back(text);
    }
};

struct FakeProvider final : modernime::core::CandidateProvider {
    modernime::core::CandidatePage current;
    std::string contextBefore;
    std::string contextAfter;
    int appendCount = 0;
    int eraseCount = 0;
    int resetCount = 0;
    int selectCount = 0;
    int removeCount = 0;

    bool append(std::string_view input) override {
        ++appendCount;
        current.preedit.append(input);
        current.items.clear();
        if (current.preedit == "nihao") {
            current.items.push_back({"你好", "ni'hao", 0});
        }
        return true;
    }

    bool eraseLast() override {
        ++eraseCount;
        if (current.preedit.empty()) {
            return false;
        }
        current.preedit.pop_back();
        current.items.clear();
        return true;
    }

    bool select(std::size_t index) override {
        ++selectCount;
        return index < current.items.size();
    }

    bool remove(std::size_t index) override {
        ++removeCount;
        return index < current.items.size();
    }

    void setContext(std::string_view before, std::string_view after) override {
        contextBefore = before;
        contextAfter = after;
    }

    void reset() override {
        ++resetCount;
        current.clear();
    }

    const modernime::core::CandidatePage &page() const override {
        return current;
    }
};

struct ManyCandidateProvider final : modernime::core::CandidateProvider {
    modernime::core::CandidatePage current;

    bool append(std::string_view input) override {
        current.preedit.append(input);
        current.items.clear();
        if (!current.preedit.empty()) {
            for (std::size_t index = 0; index < 20; ++index) {
                current.items.push_back({"候选" + std::to_string(index + 1),
                                         current.preedit, index});
            }
        }
        return true;
    }

    bool eraseLast() override {
        if (current.preedit.empty()) {
            return false;
        }
        current.preedit.pop_back();
        current.items.clear();
        return true;
    }

    bool select(std::size_t index) override {
        return index < current.items.size();
    }

    void reset() override { current.clear(); }

    const modernime::core::CandidatePage &page() const override {
        return current;
    }
};

struct EmptyCandidateProvider final : modernime::core::CandidateProvider {
    modernime::core::CandidatePage current;

    bool append(std::string_view input) override {
        current.preedit.append(input);
        current.items.clear();
        return true;
    }

    bool eraseLast() override {
        if (current.preedit.empty()) {
            return false;
        }
        current.preedit.pop_back();
        return true;
    }

    bool select(std::size_t) override { return false; }

    void reset() override { current.clear(); }

    const modernime::core::CandidatePage &page() const override {
        return current;
    }
};

void type(modernime::fcitx5::ModernIMEController &controller,
          std::string_view text) {
    for (const char character : text) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "character event is handled");
    }
}

} // namespace

int main() {
    RecordingHost host;
    modernime::fcitx5::ModernIMEController controller(host);

    type(controller, "hail");
    assertTrue(controller.page().preedit == "hail", "preedit is published");
    assertTrue(controller.page().items.size() == 9, "nine sample candidates exist");
    assertTrue(controller.page().items.front().text == "还",
               "first sample candidate is visible");

    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0}),
               "backspace is handled");
    assertTrue(controller.page().preedit == "hai", "backspace removes one byte");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Escape, 0, 0}),
               "escape is handled");
    assertTrue(controller.page().preedit.empty(), "escape clears preedit");

    type(controller, "hail");
    assertTrue(controller.select(1), "candidate index selection is handled");
    assertTrue(host.commits.back() == "海", "candidate index commits second item");
    assertTrue(controller.page().preedit.empty(), "index selection clears page");

    type(controller, "hail");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "digit selection is handled");
    assertTrue(host.commits.back() == "海", "second candidate is committed");
    assertTrue(controller.page().preedit.empty(), "selection clears page");

    RecordingHost clipboardHost;
    modernime::fcitx5::ModernIMEController clipboardController(clipboardHost);
    clipboardController.setClipboardEntries({"second clipboard", "first clipboard"});
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode opens");
    assertTrue(clipboardController.clipboardMode() &&
                   clipboardController.page().preedit.empty() &&
                   clipboardController.page().items.size() == 2,
               "clipboard entries are published without a pinyin preedit");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "clipboard digit selection is handled");
    assertTrue(clipboardHost.commits.back() == "first clipboard" &&
                   !clipboardController.clipboardMode(),
               "selected clipboard text is committed and mode is cleared");

    type(controller, "hail");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space selects first candidate");
    assertTrue(host.commits.back() == "还", "space commits first candidate");

    type(controller, "x");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Enter, 0, 0}),
               "enter commits raw fallback");
    assertTrue(host.commits.back() == "x", "raw fallback is committed");

    type(controller, "x");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '.', 0}),
               "punctuation commits raw fallback");
    assertTrue(host.commits.size() >= 2 &&
                   host.commits[host.commits.size() - 2] == "x",
               "punctuation preserves raw preedit");
    assertTrue(host.commits.back() == ".",
               "punctuation is committed after fallback");

    type(controller, "hail");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, ',', 0}),
               "punctuation commits the selected candidate");
    assertTrue(host.commits.size() >= 2 &&
                   host.commits[host.commits.size() - 2] == "还",
               "punctuation keeps the selected candidate");
    assertTrue(host.commits.back() == ",",
               "punctuation follows the selected candidate");

    EmptyCandidateProvider emptyProvider;
    RecordingHost emptyHost;
    modernime::fcitx5::ModernIMEController emptyController(emptyHost,
                                                            &emptyProvider);
    type(emptyController, "x");
    assertTrue(emptyController.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space commits a raw preedit without candidates");
    assertTrue(emptyHost.commits.size() == 2 && emptyHost.commits[0] == "x" &&
                   emptyHost.commits[1] == " ",
               "space is preserved after raw fallback");
    type(emptyController, "y");
    assertTrue(emptyController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, ';', 0}),
               "punctuation commits a raw preedit without candidates");
    assertTrue(emptyHost.commits.size() == 4 && emptyHost.commits[2] == "y" &&
                   emptyHost.commits[3] == ";",
               "punctuation is preserved after raw fallback");

    controller.handle({modernime::fcitx5::KeyKind::Toggle, 0, 0});
    assertTrue(!controller.active(), "toggle disables input");
    assertTrue(!controller.handle({modernime::fcitx5::KeyKind::Character, 'a', 0}),
               "disabled input ignores characters");
    controller.handle({modernime::fcitx5::KeyKind::Toggle, 0, 0});
    assertTrue(controller.active(), "toggle re-enables input");

    modernime::fcitx5::ControllerOptions disabledOptions;
    disabledOptions.inputEnabled = false;
    RecordingHost disabledHost;
    modernime::fcitx5::ModernIMEController disabledController(
        disabledHost, nullptr, disabledOptions);
    assertTrue(!disabledController.handle(
                    {modernime::fcitx5::KeyKind::Character, 'a', 0}),
                "disabled settings ignore characters before activation");

    ManyCandidateProvider navigationProvider;
    RecordingHost navigationHost;
    modernime::fcitx5::ControllerOptions navigationOptions;
    navigationOptions.arrowNavigation = false;
    navigationOptions.pageNavigation = false;
    modernime::fcitx5::ModernIMEController navigationController(
        navigationHost, &navigationProvider, navigationOptions);
    type(navigationController, "n");
    assertTrue(!navigationController.handle(
                    {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}),
                "disabled arrow navigation is not consumed");
    assertTrue(!navigationController.handle(
                    {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
                "disabled page navigation is not consumed");

    modernime::fcitx5::ControllerOptions numberOptions;
    numberOptions.numberSelection = false;
    RecordingHost numberHost;
    modernime::fcitx5::ModernIMEController numberController(
        numberHost, nullptr, numberOptions);
    type(numberController, "hail");
    assertTrue(!numberController.handle(
                    {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
                "disabled number selection is not consumed");

    modernime::fcitx5::ModernIMEController separatorController(host);
    assertTrue(!separatorController.handle(
                    {modernime::fcitx5::KeyKind::Character, '\'', 0}),
               "a pinyin separator cannot start a composition");
    assertTrue(separatorController.page().preedit.empty(),
               "invalid separator does not alter the preedit");
    assertTrue(separatorController.handle(
                   {modernime::fcitx5::KeyKind::Character, 'n', 0}),
               "a pinyin letter starts a composition");
    assertTrue(separatorController.handle(
                   {modernime::fcitx5::KeyKind::Character, '\'', 0}),
               "a separator is accepted after a syllable");
    assertTrue(!separatorController.handle(
                    {modernime::fcitx5::KeyKind::Character, '\'', 0}),
               "consecutive separators are rejected");

    FakeProvider provider;
    RecordingHost providerHost;
    modernime::fcitx5::ModernIMEController providerController(providerHost,
                                                               &provider);
    type(providerController, "nihao");
    assertTrue(provider.appendCount == 5,
               "controller delegates character input to provider");
    assertTrue(providerController.page().items.front().text == "你好",
               "controller publishes provider candidates");
    assertTrue(providerController.select(0), "provider selection is handled");
    assertTrue(provider.selectCount == 1,
               "controller delegates candidate selection to provider");
    assertTrue(providerHost.commits.back() == "你好",
               "provider candidate is committed");
    assertTrue(provider.resetCount == 1,
               "committing a provider candidate resets the provider");
    assertTrue(!providerController.handle(
                    {modernime::fcitx5::KeyKind::Character, '\'', 0}),
               "a separator cannot start a provider composition");
    assertTrue(provider.current.preedit.empty(),
               "invalid separator does not reach the provider");

    FakeProvider contextualProvider;
    RecordingHost contextualHost;
    modernime::fcitx5::ModernIMEController contextualController(
        contextualHost, &contextualProvider);
    contextualController.setContext("前文", "后文");
    type(contextualController, "nihao");
    assertTrue(contextualProvider.contextBefore == "前文" &&
                   contextualProvider.contextAfter == "后文",
               "controller forwards surrounding context to provider");

    FakeProvider removableProvider;
    RecordingHost removableHost;
    modernime::fcitx5::ModernIMEController removableController(
        removableHost, &removableProvider);
    type(removableController, "nihao");
    assertTrue(removableController.handle(
                   {modernime::fcitx5::KeyKind::DeleteCandidate, 0, 0}),
               "delete key is handled");
    assertTrue(removableProvider.removeCount == 1,
               "provider receives current candidate deletion");

    ManyCandidateProvider manyProvider;
    RecordingHost manyHost;
    modernime::fcitx5::ModernIMEController manyController(manyHost,
                                                           &manyProvider);
    type(manyController, "n");
    assertTrue(manyController.page().items.size() == 20,
               "all candidates remain available to the controller");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}),
               "next candidate moves the cursor");
    assertTrue(manyController.page().cursor == 1,
               "next candidate selects the second item");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "next page moves by one page");
    assertTrue(manyController.page().cursor == 9,
               "next page selects the first item on the next page");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::PreviousCandidate, 0, 0}),
               "previous candidate moves back");
    assertTrue(manyController.page().cursor == 8,
               "previous candidate selects the prior item");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::Enter, 0, 0}),
               "enter commits the highlighted candidate");
    assertTrue(manyHost.commits.back() == "候选9",
               "the highlighted paged candidate is committed");

    ManyCandidateProvider pagedProvider;
    RecordingHost pagedHost;
    modernime::fcitx5::ModernIMEController pagedController(pagedHost,
                                                              &pagedProvider);
    type(pagedController, "n");
    for (int index = 0; index < 8; ++index) {
        assertTrue(pagedController.handle(
                       {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}),
                   "cursor reaches the ninth candidate");
    }
    assertTrue(pagedController.currentPageIndex() == 0,
               "cursor is still on the first page");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "page down works from the first page");
    assertTrue(pagedController.currentPageIndex() == 1,
               "page down enters the second page");
    assertTrue(pagedController.page().cursor == 9,
               "page down starts at item ten");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '1'}),
               "digit selects from the current page");
    assertTrue(pagedHost.commits.back() == "候选10",
               "page digit selects the first item on the current page");
    return EXIT_SUCCESS;
}
