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

struct FreshRepublishProvider final : modernime::core::CandidateProvider {
    enum class Scenario { PartialSelection, Removal };

    explicit FreshRepublishProvider(Scenario selectedScenario)
        : scenario(selectedScenario) {}

    bool append(std::string_view input) override {
        current.rawInput.append(input);
        current.preedit = current.rawInput;
        current.pageBoundaries.clear();
        if (scenario == Scenario::PartialSelection) {
            current.items = {{"部分", current.rawInput, 0,
                              modernime::core::CandidateSource::Engine, 1}};
        } else {
            populate("移除后", 12);
        }
        return true;
    }

    bool eraseLast() override { return false; }

    bool select(std::size_t index) override {
        if (index >= current.items.size()) {
            return false;
        }
        if (scenario == Scenario::PartialSelection && !partialSelected) {
            partialSelected = true;
            current.rawInput = "i";
            current.preedit = "i";
            populate("剩余", 11);
        }
        return true;
    }

    bool remove(std::size_t index) override {
        if (scenario != Scenario::Removal || index >= current.items.size()) {
            return false;
        }
        current.items.erase(current.items.begin() +
                            static_cast<std::ptrdiff_t>(index));
        current.pageBoundaries.clear();
        return true;
    }

    void reset() override { current.clear(); }

    const modernime::core::CandidatePage &page() const override {
        return current;
    }

    void populate(std::string_view prefix, std::size_t count) {
        current.items.clear();
        current.pageBoundaries.clear();
        for (std::size_t index = 0; index < count; ++index) {
            current.items.push_back({std::string(prefix) +
                                         std::to_string(index + 1),
                                     current.rawInput, index});
        }
    }

    Scenario scenario;
    modernime::core::CandidatePage current;
    bool partialSelected = false;
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

    assertTrue(!controller.handle({modernime::fcitx5::KeyKind::Escape, 0, 0}),
               "escape passes through when nothing is being composed");

    type(controller, "hail");
    assertTrue(controller.page().preedit == "hail", "preedit is published");
    assertTrue(controller.page().items.size() == 1,
               "fallback candidate exists");
    assertTrue(controller.page().items.front().text == "hail",
               "fallback candidate echoes the raw input");

    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0}),
               "backspace is handled");
    assertTrue(controller.page().preedit == "hai", "backspace removes one byte");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Escape, 0, 0}),
               "escape is handled");
    assertTrue(controller.page().preedit.empty(), "escape clears preedit");

    RecordingHost editingHost;
    modernime::fcitx5::ModernIMEController editingController(editingHost);
    type(editingController, "nihao");
    assertTrue(editingController.page().preeditCursor == 5,
               "new input leaves the composition cursor at the end");
    assertTrue(editingController.handle(
                   {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}) &&
                   editingController.handle(
                       {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}),
               "left moves inside the active composition");
    assertTrue(editingController.page().preeditCursor == 3,
               "composition cursor moves to the requested insertion point");
    assertTrue(editingController.handle(
                   {modernime::fcitx5::KeyKind::Character, 'n', 0}) &&
                   editingController.page().preedit == "nihnao" &&
                   editingController.page().preeditCursor == 4,
               "typing inserts at the composition cursor");
    assertTrue(editingController.handle(
                   {modernime::fcitx5::KeyKind::Backspace, 0, 0}) &&
                   editingController.page().preedit == "nihao" &&
                   editingController.page().preeditCursor == 3,
               "backspace removes the character before the cursor");
    assertTrue(editingController.handle(
                   {modernime::fcitx5::KeyKind::DeleteForward, 0, 0}) &&
                   editingController.page().preedit == "niho" &&
                   editingController.page().preeditCursor == 3,
               "delete removes the character after the cursor");
    assertTrue(editingController.handle(
                   {modernime::fcitx5::KeyKind::MoveCompositionRight, 0, 0}) &&
                   editingController.page().preeditCursor == 4,
               "right moves toward the end of the composition");
    RecordingHost deleteOnlyHost;
    modernime::fcitx5::ModernIMEController deleteOnlyController(deleteOnlyHost);
    type(deleteOnlyController, "n");
    assertTrue(deleteOnlyController.handle(
                   {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}) &&
                   deleteOnlyController.handle(
                       {modernime::fcitx5::KeyKind::DeleteForward, 0, 0}) &&
                   deleteOnlyController.page().preedit.empty(),
               "forward delete can remove the final remaining character");

    type(controller, "hail");
    assertTrue(controller.select(0), "candidate index selection is handled");
    assertTrue(host.commits.back() == "hail",
               "candidate index commits the fallback item");
    assertTrue(controller.page().preedit.empty(), "index selection clears page");

    type(controller, "hail");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '1'}),
               "digit selection is handled");
    assertTrue(host.commits.back() == "hail", "first candidate is committed");
    assertTrue(controller.page().preedit.empty(), "selection clears page");

    RecordingHost clipboardHost;
    modernime::fcitx5::ModernIMEController clipboardController(clipboardHost);
    clipboardController.setClipboardEntries({"second clipboard", "first clipboard",
                                             "third clipboard", "fourth clipboard",
                                             "fifth clipboard", "sixth clipboard"});
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode opens");
    assertTrue(clipboardController.clipboardMode() &&
                   clipboardController.page().preedit.empty() &&
                   clipboardController.page().items.size() == 6,
               "clipboard entries are published without a pinyin preedit");
    assertTrue(clipboardController.pageSize() == 5,
               "clipboard mode uses five rows per page");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::NextClipboardItem, 0, 0}),
               "down moves to the next clipboard item");
    assertTrue(clipboardController.page().cursor == 1 &&
                   clipboardHost.commits.empty(),
               "clipboard navigation only changes the highlight");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::PreviousClipboardItem, 0, 0}),
               "up moves to the previous clipboard item");
    assertTrue(clipboardController.page().cursor == 0,
               "up returns to the first clipboard item");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::Backspace, 0, 0}),
               "backspace exits clipboard mode");
    assertTrue(!clipboardController.clipboardMode() &&
                   clipboardController.page().items.empty() &&
                   clipboardHost.commits.empty(),
               "exiting clipboard mode does not commit a history entry");
    clipboardController.setClipboardEntries({"one"});
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode reopens for escape exit");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::Escape, 0, 0}),
               "escape exits clipboard mode");
    assertTrue(!clipboardController.clipboardMode() &&
                   clipboardController.page().items.empty() &&
                   clipboardHost.commits.empty(),
               "escape closes the clipboard panel without committing");
    clipboardController.setClipboardEntries(
        {"second clipboard", "first clipboard", "third clipboard",
         "fourth clipboard", "fifth clipboard", "sixth clipboard"});
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode reopens for enter selection");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::NextClipboardItem, 0, 0}),
               "down selects the second clipboard item");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::Enter, 0, 0}),
               "enter commits the highlighted clipboard item");
    assertTrue(clipboardHost.commits.back() == "first clipboard" &&
                   !clipboardController.clipboardMode(),
               "enter commits the highlighted clipboard history");
    clipboardController.setClipboardEntries(
        {"second clipboard", "first clipboard", "third clipboard",
         "fourth clipboard", "fifth clipboard", "sixth clipboard"});
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode reopens for digit selection");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "clipboard digit selection is handled");
    assertTrue(clipboardHost.commits.back() == "first clipboard" &&
                   !clipboardController.clipboardMode(),
               "selected clipboard text is committed and mode is cleared");

    clipboardController.setClipboardEntries({"one", "two", "three", "four",
                                             "five", "six"});
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode reopens for page navigation");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "clipboard page navigation is handled");
    assertTrue(clipboardController.currentPageIndex() == 1 &&
                   clipboardController.page().cursor == 5,
               "clipboard page navigation advances by five rows");
    assertTrue(clipboardController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '1'}),
               "clipboard selection works on the second page");
    assertTrue(clipboardHost.commits.back() == "six" &&
                   !clipboardController.clipboardMode(),
               "second clipboard page commits its first row");

    FakeProvider providerWithClipboard;
    RecordingHost providerClipboardHost;
    modernime::fcitx5::ModernIMEController providerClipboardController(
        providerClipboardHost, &providerWithClipboard);
    providerClipboardController.setClipboardEntries({"provider clipboard"});
    assertTrue(providerClipboardController.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode opens with a pinyin provider");
    assertTrue(providerClipboardController.handle(
                   {modernime::fcitx5::KeyKind::Enter, 0, 0}),
               "enter submits clipboard text with a pinyin provider");
    assertTrue(providerClipboardHost.commits.back() == "provider clipboard" &&
                   !providerClipboardController.clipboardMode(),
               "provider-backed clipboard submission commits the history item");

    type(controller, "hail");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space selects first candidate");
    assertTrue(host.commits.back() == "hail", "space commits first candidate");

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
    assertTrue(host.commits.back() == "，",
               "punctuation after a Chinese candidate is full-width");

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
    type(emptyController, "abc");
    assertTrue(!emptyController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "a literal digit passes through after raw fallback");
    assertTrue(emptyHost.commits.back() == "abc" &&
                   emptyController.page().preedit.empty(),
               "raw composition commits before a literal digit");

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
    const auto commitsBeforeUnavailableDigit = providerHost.commits.size();
    assertTrue(providerController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "an unavailable candidate digit is consumed during composition");
    assertTrue(providerController.page().preedit == "nihao" &&
                   providerHost.commits.size() == commitsBeforeUnavailableDigit,
               "an unavailable candidate digit neither leaks nor commits");
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
                   {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}) &&
                   removableController.handle(
                       {modernime::fcitx5::KeyKind::MoveCompositionLeft, 0, 0}),
               "composition cursor moves before candidate deletion");
    assertTrue(removableController.handle(
                   {modernime::fcitx5::KeyKind::DeleteCandidate, 0, 0}),
               "delete key is handled");
    assertTrue(removableProvider.removeCount == 1,
               "provider receives current candidate deletion");
    assertTrue(removableController.page().preeditCursor == 3,
               "candidate deletion preserves the composition insertion point");

    FreshRepublishProvider partialRepublishProvider(
        FreshRepublishProvider::Scenario::PartialSelection);
    RecordingHost partialRepublishHost;
    modernime::fcitx5::ModernIMEController partialRepublishController(
        partialRepublishHost, &partialRepublishProvider);
    type(partialRepublishController, "ni");
    assertTrue(partialRepublishController.select(0),
               "partial candidate selection republishes remaining candidates");
    assertTrue(partialRepublishController.page().items.size() == 11 &&
                   partialRepublishController.page().pageBoundaries ==
                       std::vector<modernime::core::PageBoundary>{{0, 9},
                                                                  {9, 11}},
               "partial selection applies fallback to the fresh provider page");
    assertTrue(partialRepublishController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   partialRepublishController.handle(
                       {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "partial selection remainder reaches its eleventh candidate");
    assertTrue(partialRepublishHost.commits.back() == "剩余11",
               "partial remainder digit maps through the shared boundary");

    FreshRepublishProvider removalRepublishProvider(
        FreshRepublishProvider::Scenario::Removal);
    RecordingHost removalRepublishHost;
    modernime::fcitx5::ModernIMEController removalRepublishController(
        removalRepublishHost, &removalRepublishProvider);
    type(removalRepublishController, "n");
    assertTrue(removalRepublishController.handle(
                   {modernime::fcitx5::KeyKind::DeleteCandidate, 0, 0}),
               "candidate removal republishes remaining candidates");
    assertTrue(removalRepublishController.page().items.size() == 11 &&
                   removalRepublishController.page().pageBoundaries ==
                       std::vector<modernime::core::PageBoundary>{{0, 9},
                                                                  {9, 11}},
               "candidate removal applies fallback to the fresh provider page");
    assertTrue(removalRepublishController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   removalRepublishController.handle(
                       {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "removal remainder reaches its eleventh candidate");
    assertTrue(removalRepublishHost.commits.back() == "移除后12",
               "removal remainder digit maps through the shared boundary");

    ManyCandidateProvider manyProvider;
    RecordingHost manyHost;
    modernime::fcitx5::ModernIMEController manyController(manyHost,
                                                           &manyProvider);
    type(manyController, "n");
    manyController.setPageBoundaries(
        {{0, 4}, {4, 7}, {7, 10}, {10, 14}, {14, 17}, {17, 20}});
    assertTrue(manyController.page().items.size() == 20,
               "all candidates remain available to the controller");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::PreviousCandidate, 0, 0}),
               "candidate navigation is consumed at the first boundary");
    assertTrue(manyController.page().cursor == 0,
               "the first-boundary navigation keeps the current candidate");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}),
               "next candidate moves the cursor");
    assertTrue(manyController.page().cursor == 1,
               "next candidate selects the second item");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "next page moves by one page");
    assertTrue(manyController.page().cursor == 4,
               "next page selects the first item on the next page");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::PreviousCandidate, 0, 0}),
               "previous candidate moves back");
    assertTrue(manyController.page().cursor == 3,
               "previous candidate selects the prior item");
    assertTrue(manyController.handle(
                   {modernime::fcitx5::KeyKind::Enter, 0, 0}),
               "enter commits the raw pinyin input");
    assertTrue(manyHost.commits.back() == "n",
               "enter commits the typed pinyin instead of the candidate");

    ManyCandidateProvider verticalProvider;
    RecordingHost verticalHost;
    modernime::fcitx5::ModernIMEController verticalController(
        verticalHost, &verticalProvider);
    type(verticalController, "n");
    assertTrue(verticalController.handle(
                   {modernime::fcitx5::KeyKind::NextClipboardItem, 0, 0}),
               "down moves the highlighted candidate in normal mode");
    assertTrue(verticalController.page().cursor == 1,
               "down selects the next candidate while page-down still pages");

    ManyCandidateProvider pagedProvider;
    RecordingHost pagedHost;
    modernime::fcitx5::ModernIMEController pagedController(pagedHost,
                                                              &pagedProvider);
    type(pagedController, "n");
    pagedController.setPageBoundaries(
        {{0, 4}, {4, 7}, {7, 10}, {10, 14}, {14, 17}, {17, 20}});
    for (int index = 0; index < 3; ++index) {
        assertTrue(pagedController.handle(
                       {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}),
                   "cursor reaches the fourth candidate");
    }
    assertTrue(pagedController.currentPageIndex() == 0,
               "cursor is still on the first page");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "page down works from the first page");
    assertTrue(pagedController.currentPageIndex() == 1,
               "page down enters the second page");
    assertTrue(pagedController.page().cursor == 4,
               "page down starts at item five");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '1'}),
               "digit selects from the current page");
    assertTrue(pagedHost.commits.back() == "候选5",
               "page digit selects the first item on the current page");

    ManyCandidateProvider unifiedNavigationProvider;
    RecordingHost unifiedNavigationHost;
    modernime::fcitx5::ModernIMEController unifiedNavigationController(
        unifiedNavigationHost, &unifiedNavigationProvider);
    type(unifiedNavigationController, "n");
    unifiedNavigationController.setPageBoundaries(
        {{0, 4}, {4, 7}, {7, 10}, {10, 14}, {14, 17}, {17, 20}});
    assertTrue(unifiedNavigationController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   unifiedNavigationController.page().cursor == 4,
               "page down uses the next variable boundary");
    assertTrue(unifiedNavigationController.handle(
                   {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}) &&
                   unifiedNavigationController.page().cursor == 5,
               "tab advances the global cursor inside a variable page");
    assertTrue(unifiedNavigationController.handle(
                   {modernime::fcitx5::KeyKind::NextClipboardItem, 0, 0}) &&
                   unifiedNavigationController.page().cursor == 6,
               "down advances the same global cursor");
    assertTrue(unifiedNavigationController.handle(
                   {modernime::fcitx5::KeyKind::PreviousCandidate, 0, 0}) &&
                   unifiedNavigationController.handle(
                       {modernime::fcitx5::KeyKind::PreviousClipboardItem, 0,
                        0}) &&
                   unifiedNavigationController.page().cursor == 4,
               "shift-tab and up retreat the same global cursor");
    assertTrue(unifiedNavigationController.handle(
                   {modernime::fcitx5::KeyKind::PreviousPage, 0, 0}) &&
                   unifiedNavigationController.page().cursor == 0,
               "page up uses the previous variable boundary");
    assertTrue(unifiedNavigationController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   unifiedNavigationController.handle(
                       {modernime::fcitx5::KeyKind::NextCandidate, 0, 0}) &&
                   unifiedNavigationController.handle(
                       {modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space commits the globally highlighted candidate");
    assertTrue(unifiedNavigationHost.commits.back() == "候选6",
               "space maps the page-local highlight to global index five");

    RecordingHost punctuationHost;
    modernime::fcitx5::ModernIMEController punctuationController(
        punctuationHost);
    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, ',', 0}),
               "Chinese mode intercepts an empty-preedit punctuation key");
    assertTrue(punctuationHost.commits.back() == "，",
               "comma commits full-width in Chinese mode");
    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '@', 0}) == false,
               "characters without a mapping pass through unchanged");

    punctuationController.setContext("3", "");
    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '.', 0}) == false,
               "a period after ASCII digits stays half-width");
    punctuationController.setContext("你好", "");
    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '.', 0}),
               "a period after Chinese text is intercepted");
    assertTrue(punctuationHost.commits.back() == "。",
               "period commits full-width after Chinese text");

    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}),
               "an opening quote is intercepted");
    assertTrue(punctuationHost.commits.back() == "“",
               "the first double quote commits the left form");
    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}),
               "a closing quote is intercepted");
    assertTrue(punctuationHost.commits.back() == "”",
               "the second double quote commits the right form");
    punctuationController.reset();
    assertTrue(punctuationController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   punctuationHost.commits.back() == "“",
               "reset restores the quote pairing state");

    RecordingHost quoteCompositionHost;
    modernime::fcitx5::ModernIMEController quoteCompositionController(
        quoteCompositionHost);
    assertTrue(quoteCompositionController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   quoteCompositionHost.commits.back() == "“",
               "a quote pair opens before composition");
    type(quoteCompositionController, "hail");
    assertTrue(quoteCompositionController.handle(
                   {modernime::fcitx5::KeyKind::Space, 0, 0}) &&
                   quoteCompositionHost.commits.back() == "还",
               "candidate commit clears the completed composition");
    quoteCompositionController.setContext("“还", "");
    assertTrue(quoteCompositionController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   quoteCompositionHost.commits.back() == "”",
               "candidate commit preserves the pending closing quote");

    RecordingHost existingQuoteHost;
    modernime::fcitx5::ModernIMEController existingQuoteController(
        existingQuoteHost);
    existingQuoteController.setContext("已有“未闭合", "");
    assertTrue(existingQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   existingQuoteHost.commits.back() == "”",
               "activation inside an existing double quote closes it");
    existingQuoteController.setContext("已有‘未闭合", "");
    assertTrue(existingQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '\'', 0}) &&
                   existingQuoteHost.commits.back() == "’",
               "activation inside an existing single quote closes it");
    existingQuoteController.setContext("可见前文被截断", "仍在引号内”");
    assertTrue(existingQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   existingQuoteHost.commits.back() == "”",
               "a visible closing double quote locates the cursor inside it");
    existingQuoteController.setContext("可见前文被截断", "仍在引号内’");
    assertTrue(existingQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '\'', 0}) &&
                   existingQuoteHost.commits.back() == "’",
               "a visible closing single quote locates the cursor inside it");

    RecordingHost movedCursorQuoteHost;
    modernime::fcitx5::ModernIMEController movedCursorQuoteController(
        movedCursorQuoteHost);
    assertTrue(movedCursorQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   movedCursorQuoteHost.commits.back() == "“",
               "double quote opens before the cursor moves");
    assertTrue(movedCursorQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '\'', 0}) &&
                   movedCursorQuoteHost.commits.back() == "‘",
               "single quote opens before the cursor moves");
    movedCursorQuoteController.setContext("无引号位置", "");
    assertTrue(movedCursorQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   movedCursorQuoteHost.commits.back() == "“",
               "moving outside a double quote starts a new pair");
    assertTrue(movedCursorQuoteController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '\'', 0}) &&
                   movedCursorQuoteHost.commits.back() == "‘",
               "moving outside a single quote starts a new pair");

    RecordingHost unavailableContextHost;
    modernime::fcitx5::ModernIMEController unavailableContextController(
        unavailableContextHost);
    unavailableContextController.setContext("", "");
    assertTrue(unavailableContextController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   unavailableContextHost.commits.back() == "“",
               "quote opens when surrounding text is unavailable");
    unavailableContextController.setContext("", "");
    assertTrue(unavailableContextController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '"', 0}) &&
                   unavailableContextHost.commits.back() == "”",
               "empty surrounding text preserves the internal quote pair");

    RecordingHost mixedContextHost;
    modernime::fcitx5::ModernIMEController mixedContextController(
        mixedContextHost);
    mixedContextController.setContext("第3", "章");
    assertTrue(mixedContextController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, ',', 0}) &&
                   mixedContextHost.commits.back() == "，",
               "a trailing digit inside Chinese context keeps Chinese punctuation");
    mixedContextController.setContext("3", "");
    assertTrue(!mixedContextController.handle(
                    {modernime::fcitx5::KeyKind::Punctuation, ',', 0}),
               "a standalone ASCII number keeps half-width punctuation");

    modernime::fcitx5::ModernIMEController legacyController(punctuationHost);
    modernime::fcitx5::ControllerOptions legacyOptions;
    legacyOptions.punctuationEnabled = false;
    legacyController.setOptions(legacyOptions);
    assertTrue(legacyController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, ',', 0}) == false,
               "disabling punctuation conversion restores the legacy path");

    FakeProvider composingProvider;
    RecordingHost composingHost;
    modernime::fcitx5::ModernIMEController composingController(
        composingHost, &composingProvider);
    type(composingController, "nihao");
    assertTrue(!composingController.page().items.empty(),
               "composition has candidates before punctuation");
    assertTrue(composingController.handle(
                   {modernime::fcitx5::KeyKind::Punctuation, '!', 0}),
               "punctuation during composition is handled");
    assertTrue(composingHost.commits.size() >= 2 &&
                   composingHost.commits[composingHost.commits.size() - 2] ==
                       "你好" &&
                   composingHost.commits.back() == "！",
               "composition commits the candidate then the full-width mark");
    return EXIT_SUCCESS;
}
