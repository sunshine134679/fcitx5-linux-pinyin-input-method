#include "modernime/fcitx5/fcitx_engine.h"
#include "modernime/core/learning_store.h"

#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx-utils/keysymgen.h>
#include <filesystem>
#include <getopt.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "fcitx5 input panel test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool clientPreeditCleared(fcitx::InputContext &context) {
    const auto &preedit = context.inputPanel().clientPreedit();
    // Fcitx 5.0.14 preserves the empty segment in Text(""); newer versions
    // normalize it away. Verify client-visible text and cursor, not segments.
    return preedit.toString().empty() && preedit.cursor() == 0;
}

class TestInputContext final : public fcitx::InputContext {
public:
    explicit TestInputContext(fcitx::InputContextManager &manager)
        : InputContext(manager, "modernime-test") {
        created();
    }

    ~TestInputContext() override { destroy(); }

    const char *frontend() const override { return "modernime-test"; }

    int preeditUpdates = 0;
    std::vector<std::string> commits;

protected:
    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
    }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override { ++preeditUpdates; }
};

class TenCandidateProvider final : public modernime::core::CandidateProvider {
public:
    bool append(std::string_view input) override {
        current_.preedit.append(input);
        current_.rawInput = current_.preedit;
        current_.items.clear();
        for (std::size_t index = 0; index < candidateCount_; ++index) {
            current_.items.push_back({"候选" + std::to_string(index + 1),
                                      current_.preedit, index});
        }
        return true;
    }

    bool eraseLast() override { return false; }
    bool select(std::size_t index) override {
        return index < current_.items.size();
    }
    void reset() override { current_.clear(); }
    const modernime::core::CandidatePage &page() const override {
        return current_;
    }
    void setCandidateCount(std::size_t count, std::size_t cursor) {
        candidateCount_ = count;
        current_.cursor = cursor;
    }

private:
    modernime::core::CandidatePage current_;
    std::size_t candidateCount_ = 10;
};

void testDisabledActivationPreservesChineseMode() {
    char path[] = "/tmp/modernime-activation-XXXXXX";
    const auto *root = mkdtemp(path);
    assertTrue(root != nullptr, "activation configuration directory created");
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("XDG_DATA_HOME", root, 1);
    auto settings = modernime::core::defaultSettings();
    settings.inputEnabled = false;
    const auto settingsPath = modernime::core::SettingsPaths::fromEnvironment(
        root, root, "").settingsFile;
    assertTrue(modernime::core::SettingsStore::save(settingsPath, settings),
               "engine starts globally disabled in Chinese default mode");
    char program[] = "modernime-test";
    char disable[] = "--disable=all";
    char *argv[] = {program, disable, nullptr};
    {
        // getopt state is process-global, including across Fcitx instances.
        optind = 0;
        fcitx::Instance instance(2, argv);
        instance.initialize();
        modernime::fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
        TestInputContext context(instance.inputContextManager());
        context.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
        const auto entries = engine.listInputMethods();
        fcitx::InputContextEvent activation(&context,
                                            fcitx::EventType::InputContextFocusIn);
        engine.activate(entries.front(), activation);
        auto *state = dynamic_cast<modernime::fcitx5::FcitxInputContextState *>(
            context.property("modernime-fcitx5-state"));
        assertTrue(state != nullptr && !state->controller().active(),
                   "real activation respects the global input disable");
        settings.inputEnabled = true;
        state->applySettings(settings, 0);
        assertTrue(state->controller().active(),
                   "hot reenabling restores Chinese mode after a disabled activation");
        fcitx::KeyEvent event(&context, fcitx::Key(FcitxKey_n));
        engine.keyEvent(entries.front(), event);
        assertTrue(event.accepted() &&
                       context.inputPanel().clientPreedit().toString() == "n",
                   "the activated context accepts pinyin after hot reenabling");
    }
    std::filesystem::remove_all(root);
}

void testLiveKeyEvents() {
    char path[] = "/tmp/modernime-key-events-XXXXXX";
    const auto *root = mkdtemp(path);
    assertTrue(root != nullptr, "isolated configuration directory created");
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("XDG_DATA_HOME", root, 1);
    auto configured = modernime::core::defaultSettings();
    configured.numberSelection = false;
    const auto settingsPath = modernime::core::SettingsPaths::fromEnvironment(
        root, root, "").settingsFile;
    assertTrue(modernime::core::SettingsStore::save(settingsPath, configured),
               "number selection is disabled in isolated engine settings");
    char program[] = "modernime-test";
    char disable[] = "--disable=all";
    char *argv[] = {program, disable, nullptr};
    {
        // getopt state is process-global, including across Fcitx instances.
        optind = 0;
        fcitx::Instance instance(2, argv);
        instance.initialize();
        modernime::fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
        TestInputContext context(instance.inputContextManager());
        context.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
        context.focusIn();
        const auto entries = engine.listInputMethods();
        const auto &entry = entries.front();
        for (const auto zero : {FcitxKey_0, FcitxKey_KP_0}) {
            for (const auto letter : {FcitxKey_n, FcitxKey_i}) {
                fcitx::KeyEvent event(&context, fcitx::Key(letter));
                engine.keyEvent(entry, event);
                assertTrue(event.accepted(), "pinyin keyEvent accepted");
            }
            fcitx::KeyEvent event(&context, fcitx::Key(zero));
            engine.keyEvent(entry, event);
            assertTrue(!event.accepted(), "zero passes through to the client");
            assertTrue(!context.commits.empty() && context.commits.back() == "ni",
                       "raw composition is committed before zero passes through");
            assertTrue(clientPreeditCleared(context),
                       "zero clears pending composition before client insertion");
            // Emulate the application's insertion after the engine returns.
            context.commits.emplace_back("0");
        }
        assertTrue(context.commits == std::vector<std::string>{"ni", "0", "ni", "0"},
                   "main keyboard and keypad preserve ni then zero ordering");
        for (const auto letter : {FcitxKey_n, FcitxKey_i}) {
            fcitx::KeyEvent event(&context, fcitx::Key(letter));
            engine.keyEvent(entry, event);
        }
        fcitx::KeyEvent literal(&context, fcitx::Key(FcitxKey_KP_3));
        engine.keyEvent(entry, literal);
        assertTrue(!literal.accepted() && context.commits.back() == "ni" &&
                       clientPreeditCleared(context),
                   "disabled number selection commits pinyin before a literal digit");
        for (const auto symbol : {FcitxKey_v, FcitxKey_1, FcitxKey_KP_3,
                                  FcitxKey_period, FcitxKey_0, FcitxKey_KP_5}) {
            fcitx::KeyEvent event(&context, fcitx::Key(symbol));
            engine.keyEvent(entry, event);
            assertTrue(event.accepted(),
                       "numeric composition accepts real keys with number selection off");
        }
        assertTrue(context.inputPanel().clientPreedit().toString() == "v13.05",
                   "number selection off preserves complete v decimal composition");
        fcitx::KeyEvent cancel(&context, fcitx::Key(FcitxKey_Escape));
        engine.keyEvent(entry, cancel);
        for (const auto symbol : {FcitxKey_v, FcitxKey_2}) {
            fcitx::KeyEvent event(&context, fcitx::Key(symbol));
            engine.keyEvent(entry, event);
            assertTrue(event.accepted(),
                       "clipboard trigger still works with number selection off");
        }
        assertTrue(context.inputPanel().clientPreedit().toString() != "v2",
                   "clipboard trigger takes priority over numeric composition");
        fcitx::KeyEvent closeClipboard(&context, fcitx::Key(FcitxKey_Escape));
        engine.keyEvent(entry, closeClipboard);
        auto *state = dynamic_cast<modernime::fcitx5::FcitxInputContextState *>(
            context.property("modernime-fcitx5-state"));
        assertTrue(state != nullptr, "real engine owns the live context state");
        fcitx::KeyEvent letter(&context, fcitx::Key(FcitxKey_n));
        engine.keyEvent(entry, letter);
        auto settings = modernime::core::defaultSettings();
        settings.inputEnabled = false;
        state->applySettings(settings, 0);
        assertTrue(!state->controller().active() &&
                       clientPreeditCleared(context),
                   "hot disabling input resets the existing composition immediately");
        fcitx::KeyEvent toggle(&context, fcitx::Key(FcitxKey_space,
                             fcitx::KeyStates(fcitx::KeyState::Ctrl)));
        engine.keyEvent(entry, toggle);
        fcitx::KeyEvent shiftPress(&context, fcitx::Key(FcitxKey_Shift_L));
        fcitx::KeyEvent shiftRelease(&context, fcitx::Key(FcitxKey_Shift_L), true);
        engine.keyEvent(entry, shiftPress);
        engine.keyEvent(entry, shiftRelease);
        assertTrue(!toggle.accepted() && !shiftRelease.accepted() &&
                       !state->controller().active(),
                   "explicit and Shift toggles cannot enable globally disabled input");
        settings.inputEnabled = true;
        state->applySettings(settings, 0);
        assertTrue(state->controller().active(),
                   "reenabling input restores the live context's Chinese mode");
        state->controller().setActive(false);
        settings.inputEnabled = false;
        state->applySettings(settings, 0);
        settings.inputEnabled = true;
        state->applySettings(settings, 0);
        assertTrue(!state->controller().active(),
                   "hot settings preserve the live context's temporary English mode");
        state->controller().setActive(true);
        for (const auto symbol : {FcitxKey_v, FcitxKey_1, FcitxKey_KP_3,
                                  FcitxKey_period, FcitxKey_0, FcitxKey_KP_5}) {
            fcitx::KeyEvent event(&context, fcitx::Key(symbol));
            engine.keyEvent(entry, event);
            assertTrue(event.accepted(),
                       "numeric composition accepts real keys with number selection on");
        }
        assertTrue(context.inputPanel().clientPreedit().toString() == "v13.05",
                   "number selection on preserves complete v decimal composition");
        fcitx::KeyEvent cancelNumber(&context, fcitx::Key(FcitxKey_Escape));
        engine.keyEvent(entry, cancelNumber);
        state->setClipboardEntries({"first", "second"});
        state->controller().handle({modernime::fcitx5::KeyKind::OpenClipboard, 0, 0});
        fcitx::KeyEvent selectSecond(&context, fcitx::Key(FcitxKey_2));
        engine.keyEvent(entry, selectSecond);
        assertTrue(selectSecond.accepted() && context.commits.back() == "second",
                   "enabled number selection selects the real candidate list");


    }
    std::filesystem::remove_all(root);
}

void publishTenCandidates(
    modernime::fcitx5::ModernIMEController &controller,
    modernime::fcitx5::FcitxEngineHost &host) {
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Character, 'n', 0}),
               "test composition publishes ten candidates");
    controller.setPageBoundaries({{0, 4}, {4, 7}, {7, 10}});
    host.publishPage(controller.page());
}

void publishFreshTenCandidates(
    modernime::fcitx5::ModernIMEController &controller) {
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Character, 'n', 0}),
               "test composition publishes a fresh ten-candidate page");
    assertTrue(!controller.page().pageBoundaries.empty(),
               "controller supplies conservative boundaries before UI "
               "measurement");
}

#ifdef MODERNIME_HAS_LIBIME_PINYIN
void testSensitiveContextStaysUnlearnedAfterSettingsReload() {
    char path[] = "/tmp/modernime-sensitive-reload-XXXXXX";
    const auto *root = mkdtemp(path);
    assertTrue(root != nullptr, "isolated sensitive-context data directory created");
    const auto storePath = std::filesystem::path(root) / "learning.sqlite3";
    {
        auto settings = modernime::core::defaultSettings();
        settings.learningEnabled = true;
        settings.contextLearningEnabled = true;
        modernime::pinyin::PinyinDataPaths paths;
        paths.userDictionary = (std::filesystem::path(root) / "user.tsv").string();
        paths.learningStore = storePath.string();
        modernime::fcitx5::FcitxEngineResources resources;
        resources.pinyin = modernime::pinyin::PinyinCandidateProvider::createSharedResources(
            paths, {});
        fcitx::InputContextManager manager;
        TestInputContext context(manager);
        modernime::fcitx5::FcitxInputContextState state(context, settings, resources, 0);
        auto &controller = state.controller();
        const auto commitChinese = [&] {
            for (const auto character : std::string_view("nihao")) {
                assertTrue(controller.handle(
                               {modernime::fcitx5::KeyKind::Character, character, 0}),
                           "sensitive context can compose Chinese");
            }
            std::size_t index = 0;
            while (index < controller.page().items.size() &&
                   controller.page().items[index].text != "你好") {
                ++index;
            }
            assertTrue(controller.select(index), "Chinese candidate is committed");
            assertTrue(context.commits.back() == "你好", "Chinese text reaches client");
            assertTrue(modernime::pinyin::PinyinCandidateProvider::reloadLearningStore(
                           resources.pinyin), "pending learning is flushed before inspecting store");
        };
        controller.setSensitive(true);
        state.applySettings(settings, 1);
        // The next key event repeats setSensitive(true); it must not undo the
        // gate or require a transition to make the reloaded settings safe.
        controller.setSensitive(true);
        commitChinese();
        modernime::core::LearningStore store(storePath);
        assertTrue(store.openReadOnly(), "isolated learning store opens");
        assertTrue(store.snapshot()->entries().empty(),
                   "settings reload cannot enable learning in an already-sensitive context");
        controller.setSensitive(false);
        commitChinese();
        assertTrue(store.snapshot()->hasPositiveFrequency("你好", "nihao"),
                   "leaving sensitive context restores configured learning");
    }
    std::filesystem::remove_all(root);
}
#endif

} // namespace

int main() {
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    testSensitiveContextStaysUnlearnedAfterSettingsReload();
#endif
    testDisabledActivationPreservesChineseMode();
    testLiveKeyEvents();
    fcitx::InputContextManager manager;
    TestInputContext inputContext(manager);
    inputContext.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    inputContext.focusIn();
    modernime::fcitx5::FcitxEngineHost host(inputContext);

    modernime::core::CandidatePage page;
    page.preedit = "df";
    page.preeditCursor = 1;
    page.items.push_back({"地方", "df", 0});
    host.publishPage(page);

    assertTrue(inputContext.inputPanel().clientPreedit().toString() == "df",
               "preedit is published to the client input area");
    assertTrue(inputContext.inputPanel().clientPreedit().cursor() == 1,
               "client preedit cursor follows the controller insertion point");
    assertTrue(inputContext.preeditUpdates == 1,
               "client preedit update is sent");
    inputContext.inputPanel().candidateList()->candidate(0).select(
        &inputContext);

    page.clear();
    page.preedit = "xyz";
    host.publishPage(page);
    assertTrue(inputContext.inputPanel().clientPreedit().toString() == "xyz",
               "raw preedit remains visible without candidates");
    assertTrue(inputContext.inputPanel().clientPreedit().cursor() == 3,
               "raw preedit cursor follows the end without candidates");

    page.clear();
    host.publishPage(page);
    assertTrue(clientPreeditCleared(inputContext),
               "client preedit is cleared after reset");
    assertTrue(inputContext.preeditUpdates == 3,
               "clearing preedit sends an update");

    modernime::fcitx5::ModernIMEController controller(host);
    host.setController(controller);
    controller.setClipboardEntries({"first clipboard", "second clipboard",
                                    "third clipboard", "fourth clipboard",
                                    "fifth clipboard", "sixth clipboard"});
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode publishes the clipboard list");
    const auto clipboardList = inputContext.inputPanel().candidateList();
    assertTrue(clipboardList != nullptr &&
                   clipboardList->layoutHint() ==
                       fcitx::CandidateLayoutHint::Vertical,
               "clipboard candidates use a vertical layout hint");
    assertTrue(clipboardList->size() == 5,
               "clipboard candidate pages expose five rows");
    assertTrue(clipboardList->toPageable() != nullptr &&
                   clipboardList->toPageable()->totalPages() == 2,
               "clipboard candidates retain paging information");
    clipboardList->candidate(1).select(&inputContext);
    assertTrue(inputContext.commits.back() == "second clipboard",
               "clicking candidate in clipboard list commits the clicked entry");
    assertTrue(!controller.clipboardMode(),
               "selecting clipboard item exits clipboard mode");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::OpenClipboard, 0, 0}),
               "clipboard mode reopens for subsequent tests");

    modernime::core::CandidatePage secondClipboardPage;
    secondClipboardPage.mode = modernime::core::CandidatePageMode::Clipboard;
    secondClipboardPage.items = {{"first", {}, 0},  {"second", {}, 1},
                                 {"third", {}, 2},  {"fourth", {}, 3},
                                 {"fifth", {}, 4},  {"sixth", {}, 5}};
    secondClipboardPage.cursor = 5;
    host.publishPage(secondClipboardPage);
    const auto secondClipboardList = inputContext.inputPanel().candidateList();
    assertTrue(secondClipboardList->toPageable() != nullptr &&
                   secondClipboardList->toPageable()->currentPage() == 1 &&
                   secondClipboardList->candidate(0).text().toString() ==
                       "sixth",
               "clipboard publication selects the requested page");

    TenCandidateProvider provider;
    modernime::fcitx5::ModernIMEController pagedController(host, &provider);
    host.setController(pagedController);

    publishFreshTenCandidates(pagedController);
    auto freshList = inputContext.inputPanel().candidateList();
    assertTrue(freshList != nullptr && freshList->size() <= 9 &&
                   freshList->toPageable() != nullptr &&
                   freshList->toPageable()->totalPages() == 2,
               "fresh controller pages conservatively expose at most nine rows");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::Digit, 0, '1'}),
               "page down and a local digit reach the tenth fresh candidate");
    assertTrue(inputContext.commits.back() == "候选10",
               "fresh fallback boundaries map the tenth candidate globally");

    publishFreshTenCandidates(pagedController);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::Space, 0, 0}),
               "page down and space reach the tenth fresh candidate");
    assertTrue(inputContext.commits.back() == "候选10",
               "space follows the shared fresh fallback boundary");

    provider.setCandidateCount(10, 0);
    publishTenCandidates(pagedController, host);
    auto pagedList = inputContext.inputPanel().candidateList();
    assertTrue(pagedList != nullptr && pagedList->size() == 4 &&
                   pagedList->toPageable() != nullptr &&
                   pagedList->toPageable()->totalPages() == 3,
               "Fcitx exposes the first variable-width candidate page");
    for (int index = 0; index < pagedList->size(); ++index) {
        assertTrue(!pagedList->candidate(index).isPlaceHolder(),
                   "published candidates never use placeholder hiding");
    }
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::Digit, 0, '1'}),
               "page down and a local digit select the fifth candidate");
    assertTrue(inputContext.commits.back() == "候选5",
               "the fifth candidate maps to global index four");

    publishTenCandidates(pagedController, host);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "two page downs and a local digit select the ninth candidate");
    assertTrue(inputContext.commits.back() == "候选9",
               "the ninth candidate maps to global index eight");

    publishTenCandidates(pagedController, host);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::Digit, 0, '3'}),
               "two page downs and a local digit select the tenth candidate");
    assertTrue(inputContext.commits.back() == "候选10",
               "the tenth candidate maps to global index nine");

    publishTenCandidates(pagedController, host);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "mouse selection can reach the final candidate page");
    pagedList = inputContext.inputPanel().candidateList();
    assertTrue(pagedList != nullptr && pagedList->size() == 3,
               "the final variable page exposes candidates eight through ten");
    pagedList->candidate(2).select(&inputContext);
    assertTrue(inputContext.commits.back() == "候选10",
               "mouse selection maps the local row to global index nine");

    publishTenCandidates(pagedController, host);
    pagedList = inputContext.inputPanel().candidateList();
    auto *pageable = pagedList->toPageable();
    assertTrue(pageable != nullptr, "variable list remains pageable");
    const auto *firstPageList = pagedList.get();
    pageable->next();
    auto republishedList = inputContext.inputPanel().candidateList();
    assertTrue(republishedList.get() != firstPageList &&
                   pagedController.page().cursor == 4 &&
                   republishedList->toPageable()->currentPage() == 1,
               "standard pageable next republishes the shared controller page");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "digit selection follows standard pageable navigation");
    assertTrue(inputContext.commits.back() == "候选6",
               "pageable next and local digit share global index five");

    publishTenCandidates(pagedController, host);
    pagedList = inputContext.inputPanel().candidateList();
    pageable = pagedList->toPageable();
    pageable->setPage(2);
    republishedList = inputContext.inputPanel().candidateList();
    assertTrue(pagedController.page().cursor == 7 &&
                   republishedList->toPageable()->currentPage() == 2,
               "standard setPage moves the shared global cursor");
    republishedList->toPageable()->prev();
    assertTrue(pagedController.page().cursor == 4,
               "standard pageable prev uses the preceding variable boundary");
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space follows standard setPage and prev navigation");
    assertTrue(inputContext.commits.back() == "候选5",
               "pageable prev and space share global index four");

    publishTenCandidates(pagedController, host);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "cursor-movable test enters the second page");
    pagedList = inputContext.inputPanel().candidateList();
    auto *movable = pagedList->toCursorMovable();
    assertTrue(movable != nullptr, "variable list remains cursor movable");
    const auto *beforeCursorMove = pagedList.get();
    movable->nextCandidate();
    republishedList = inputContext.inputPanel().candidateList();
    assertTrue(republishedList.get() != beforeCursorMove &&
                   pagedController.page().cursor == 5,
               "standard nextCandidate republishes the shared global cursor");
    republishedList->toCursorMovable()->prevCandidate();
    assertTrue(pagedController.page().cursor == 4,
               "standard prevCandidate retreats the shared global cursor");
    inputContext.inputPanel()
        .candidateList()
        ->toCursorMovable()
        ->nextCandidate();
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space follows standard cursor-movable navigation");
    assertTrue(inputContext.commits.back() == "候选6",
               "cursor-movable navigation and space share global index five");

    publishTenCandidates(pagedController, host);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::NextPage, 0, 0}) &&
                   pagedController.handle(
                       {modernime::fcitx5::KeyKind::NextPage, 0, 0}),
               "shortening test starts with a cursor on the final page");
    provider.setCandidateCount(3, 9);
    assertTrue(pagedController.handle(
                   {modernime::fcitx5::KeyKind::Character, 'i', 0}),
               "changed content is republished before automatic pagination");
    pagedList = inputContext.inputPanel().candidateList();
    auto *variableList =
        dynamic_cast<modernime::fcitx5::FcitxCandidateList *>(pagedList.get());
    assertTrue(variableList != nullptr,
               "production list accepts recalculated UI boundaries");
    variableList->setPageBoundaries({{0, 2}, {2, 3}});
    assertTrue(pagedController.page().items.size() == 3 &&
                   pagedController.page().cursor == 2 &&
                   variableList->currentPage() == 1,
               "automatic repagination clamps a stale global cursor");

    provider.setCandidateCount(10, 0);
    publishTenCandidates(pagedController, host);
    pagedList = inputContext.inputPanel().candidateList();
    variableList =
        dynamic_cast<modernime::fcitx5::FcitxCandidateList *>(pagedList.get());
    assertTrue(variableList != nullptr,
               "production list is valid for label contract verification");
    for (int index = 0; index < variableList->size(); ++index) {
        assertTrue(!variableList->label(index).toString().empty(),
                   "page-local candidates provide non-empty labels");
    }
    assertTrue(variableList->label(0).toString() == "1. ",
               "first candidate label is 1.");
    assertTrue(variableList->label(2).toString() == "3. ",
               "third candidate label is 3.");
    assertTrue(variableList->label(-1).toString().empty(),
               "negative candidate index returns empty label");
    assertTrue(variableList->label(variableList->size()).toString().empty(),
               "out-of-bounds candidate index returns empty label");

    // 验证多于 5 个候选词（如 9 个候选词的大页面）时，索引 >= 5 正常返回对应标号，绝不抛出异常
    variableList->setPageBoundaries({{0, 9}, {9, 10}});
    assertTrue(variableList->size() == 9, "page size is expanded to nine");
    assertTrue(variableList->label(0).toString() == "1. ", "label 0 is 1.");
    assertTrue(variableList->label(4).toString() == "5. ", "label 4 is 5.");
    assertTrue(variableList->label(5).toString() == "6. ", "label 5 is 6.");
    assertTrue(variableList->label(8).toString() == "9. ", "label 8 is 9.");
    assertTrue(variableList->label(9).toString().empty(), "label 9 is out of bounds");

    return EXIT_SUCCESS;
}
