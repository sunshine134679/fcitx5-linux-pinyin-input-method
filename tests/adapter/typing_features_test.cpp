#include "modernime/fcitx5/fcitx_engine.h"
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>
#include <fcitx-utils/utf8.h>
#include <getopt.h>
#include <cstdlib>
#include <iostream>

using namespace modernime;

namespace {
void check(bool condition, const char *message) {
    if (!condition) { std::cerr << "typing features: " << message << '\n'; std::exit(1); }
}
std::size_t byteAt(const std::string &text, std::size_t chars) {
    std::size_t i = 0;
    while (i < text.size() && chars--) {
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) ++i;
    }
    return i;
}
#ifdef MODERNIME_TEST_CURSOR_API
using DocumentContext = fcitx::InputContextV2;
#else
using DocumentContext = fcitx::InputContext;
#endif
class Document final : public DocumentContext {
public:
    Document(fcitx::InputContextManager &manager, const std::string &program = "typing-test")
        : DocumentContext(manager, program) {
        created();
        setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
#ifdef MODERNIME_TEST_CURSOR_API
        setCapabilityFlags(capabilityFlags() | fcitx::CapabilityFlag::CommitStringWithCursor);
#endif
        focusIn();
        sync();
    }
    ~Document() override { destroy(); }
    const char *frontend() const override { return "typing-test"; }
    std::string text;
    std::size_t cursor = 0;
    void sync() { surroundingText().setText(text, static_cast<unsigned>(cursor), static_cast<unsigned>(cursor)); }
protected:
    void commitStringImpl(const std::string &s) override {
        text.insert(byteAt(text, cursor), s); cursor += fcitx::utf8::length(s); sync();
    }
#ifdef MODERNIME_TEST_CURSOR_API
    void commitStringWithCursorImpl(const std::string &s, std::size_t pos) override {
        text.insert(byteAt(text, cursor), s); cursor += pos; sync();
    }
#endif
    void deleteSurroundingTextImpl(int offset, unsigned size) override {
        auto start = static_cast<std::size_t>(static_cast<int>(cursor) + offset);
        text.erase(byteAt(text, start), byteAt(text, start + size) - byteAt(text, start));
        cursor = start; sync();
    }
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &event) override {
        if (!event.isRelease() && event.key().sym() == FcitxKey_Left && cursor) --cursor;
        sync();
    }
    void updatePreeditImpl() override {}
};

void testPairs(fcitx::Instance &instance) {
    Document doc(instance.inputContextManager());
    fcitx5::FcitxEngineHost host(doc);
    fcitx5::ModernIMEController controller(host);
    host.setController(controller);
    auto punctuation = [&](char c) {
        controller.setContext(doc.text.substr(0, byteAt(doc.text, doc.cursor)),
                              doc.text.substr(byteAt(doc.text, doc.cursor)));
        return controller.handle({fcitx5::KeyKind::Punctuation, c});
    };
    check(punctuation('('), "opening parenthesis handled");
    check(doc.text == "（）" && doc.cursor == 1, "pair inserted with cursor inside");
    check(punctuation('"'), "nested quotes handled");
    check(doc.text == "（“”）" && doc.cursor == 2, "nested pair");
    controller.handle({fcitx5::KeyKind::Backspace});
    check(doc.text == "（）" && doc.cursor == 1, "backspace removes empty quote pair");
    check(punctuation(')'), "closing owned parenthesis handled");
    check(doc.text == "（）" && doc.cursor == 2, "closing key skips generated closer");
    controller.reset();
    doc.text.clear(); doc.cursor = 0; doc.sync();
    check(punctuation('\''), "single quote handled outside pinyin");
    check(doc.text == "‘’" && doc.cursor == 1, "single quotes pair");
    controller.reset(); doc.text.clear(); doc.cursor = 0; doc.sync();
    doc.setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
    check(punctuation('(') && doc.text == "（）" && doc.cursor == 1, "legacy client uses ordered cursor key fallback");
    doc.commitString("abc");
    check(punctuation(')') && doc.text == "（abc）" && doc.cursor == 5, "closer skips after ASCII content inside pair");
    controller.reset(); doc.text.clear(); doc.cursor = 0; doc.sync();
    check(punctuation('('), "new pair inserted");
    controller.reset();
    check(!controller.handle({fcitx5::KeyKind::Backspace}) && doc.text == "（）", "focus reset drops ownership of previous pair");
    doc.text = "选中"; doc.cursor = 0; doc.sync(); doc.surroundingText().setCursor(0, 2);
    check(!host.insertPair("（", "）") && doc.text == "选中", "selected text refuses automatic pair editing");
    doc.text.clear(); doc.cursor = 0; doc.sync();
    controller.setSensitive(true);
    check(punctuation('(') && doc.text == "（", "sensitive input retains ordinary punctuation without pair");
}

void testContextCacheHostEdits(fcitx::Instance &instance) {
    Document doc(instance.inputContextManager());
    fcitx5::FcitxEngineHost host(doc);
    fcitx5::ModernIMEController controller(host);
    fcitx5::SurroundingContextCache cache;
    host.setController(controller, &cache);
    cache.observe(doc.surroundingText());
    host.commit("你好");
    auto window = cache.current(doc.surroundingText());
    check(window.first == "你好" && window.second.empty(),
          "host commits invalidate the previous context projection");
    cache.observe(doc.surroundingText());
    check(host.insertPair("（", "）"), "context fixture inserts a real pair");
    window = cache.current(doc.surroundingText());
    check(window.first == "你好（" && window.second == "）",
          "pair insertion invalidates text and cursor projection");
    cache.observe(doc.surroundingText());
    check(host.replaceSurrounding("（", "）", ""), "context fixture removes its own pair");
    window = cache.current(doc.surroundingText());
    check(window.first == "你好" && window.second.empty(),
          "surrounding replacement invalidates the previous context projection");
}

void testSmartPunctuation(fcitx::Instance &instance) {
    Document doc(instance.inputContextManager());
    fcitx5::FcitxEngineHost host(doc);
    fcitx5::ModernIMEController controller(host);
    host.setController(controller);
    for (const auto &token : {"snake_", "https://", "file.name/", "name@"}) {
        doc.text = token; doc.cursor = fcitx::utf8::length(doc.text); doc.sync();
        controller.setContext(doc.text, "");
        check(!controller.handle({fcitx5::KeyKind::Punctuation, '_'}),
              "identifier punctuation retains ASCII underscore");
    }
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.setContext("", "");
    check(controller.handle({fcitx5::KeyKind::Punctuation, '_'}), "standalone underscore handled");
    check(doc.text == "——", "standalone underscore remains Chinese dash");
    fcitx5::ControllerOptions options;
    options.punctuationOverrides = {{'_', "_"}};
    controller.setOptions(options);
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.setContext("", "");
    check(controller.handle({fcitx5::KeyKind::Punctuation, '_'}) && doc.text == "_", "custom mapping overrides standalone punctuation");
    options.punctuationEnabled = false; controller.setOptions(options);
    check(!controller.handle({fcitx5::KeyKind::Punctuation, '_'}), "punctuation master switch disables override");
}

void testApplicationModes(fcitx::Instance &instance) {
    auto settings = core::defaultSettings();
    settings.appInputModes["code"] = core::InputMode::English;
    settings.rememberModePerApp = true;
    settings.learningEnabled = false;
    const auto path = core::SettingsPaths::fromEnvironment(std::getenv("XDG_CONFIG_HOME"),
        std::getenv("XDG_DATA_HOME"), "").settingsFile;
    check(core::SettingsStore::save(path, settings), "save app rules");
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    const auto &entry = entries.front();
    Document code(instance.inputContextManager(), "code");
    fcitx::InputContextEvent in(&code, fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, in);
    check(!engine.state(&code)->controller().active(), "per-app English default");
    fcitx::KeyEvent toggle(&code, fcitx::Key(FcitxKey_space,
        fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift})));
    engine.keyEvent(entry, toggle);
    check(toggle.accepted() && engine.state(&code)->controller().active(), "manual app mode switch");
    fcitx::InputContextEvent out(&code, fcitx::EventType::InputContextFocusOut);
    engine.deactivate(entry, out);
    engine.activate(entry, in);
    check(engine.state(&code)->controller().active(), "app remembers manual mode across focus");
    Document other(instance.inputContextManager(), "chat");
    fcitx::InputContextEvent otherIn(&other, fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, otherIn);
    check(engine.state(&other)->controller().active(), "other app uses global Chinese default");
}

void testUndo(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    const auto &entry = entries.front();
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, in);
    auto send = [&](fcitx::Key key, bool release = false) {
        fcitx::KeyEvent event(&doc, key, release); engine.keyEvent(entry, event); return event.accepted();
    };
    for (const char c : std::string("nihao")) check(send(fcitx::Key(std::string(1, c))), "pinyin accepted");
    check(send(fcitx::Key(FcitxKey_space)) && doc.text == "你好", "candidate commits to document");
    const auto undo = fcitx::Key(FcitxKey_BackSpace,
        fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift}));
    send(fcitx::Key(FcitxKey_Control_L));
    send(fcitx::Key(FcitxKey_Shift_L, fcitx::KeyState::Ctrl));
    check(send(undo), "physical Ctrl then Shift preserves the undo shortcut");
    send(fcitx::Key(FcitxKey_Shift_L, fcitx::KeyState::Ctrl), true);
    send(fcitx::Key(FcitxKey_Control_L), true);
    check(doc.text.empty() && engine.state(&doc)->controller().page().rawInput == "nihao",
          "undo restores original keystrokes and candidates");
    check(!send(undo), "repeated undo cannot delete again");
    check(!send(fcitx::Key(FcitxKey_BackSpace, fcitx::KeyState::Ctrl)), "Ctrl+BackSpace remains application word deletion");
    send(fcitx::Key(FcitxKey_Escape));
    for (const char c : std::string("nihaoma")) send(fcitx::Key(std::string(1, c)));
    auto &controller = engine.state(&doc)->controller();
    bool selected = false;
    for (std::size_t i = 0; i < controller.page().items.size(); ++i) {
        if (controller.page().items[i].text == "你好") { selected = controller.select(i); break; }
    }
    check(selected && doc.text == "你好" && controller.page().rawInput == "ma", "partial selection keeps suffix");
    check(send(undo) && doc.text.empty() && controller.page().rawInput == "nihaoma",
          "partial undo restores consumed prefix plus original suffix");
    send(fcitx::Key(FcitxKey_space));
    doc.cursor = 0; doc.sync();
    check(!send(undo) && !doc.text.empty(), "moved cursor cannot undo another position");
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.reset();
    for (const char c : std::string("nihao")) send(fcitx::Key(std::string(1, c)));
    send(fcitx::Key(FcitxKey_space));
    doc.commitString("!");
    check(!send(undo) && doc.text == "你好!", "external text insertion cancels undo");
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.reset();
    for (const char c : std::string("nihao")) send(fcitx::Key(std::string(1, c)));
    send(fcitx::Key(FcitxKey_space));
    doc.surroundingText().setCursor(2, 0);
    check(!send(undo) && doc.text == "你好", "selection cancels undo without deleting text");
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.reset();
    send(fcitx::Key(FcitxKey_parenleft));
    for (const char c : std::string("nihao")) send(fcitx::Key(std::string(1, c)));
    send(fcitx::Key(FcitxKey_parenright));
    check(doc.text == "（你好）" && doc.cursor == 4, "closing pair commits pending Chinese without duplicating closer");
}

void testTemporaryEnglish(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entries.front(), in);
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key); engine.keyEvent(entries.front(), event); return event.accepted();
    };
    const auto trigger = fcitx::Key(FcitxKey_E,
        fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift}));
    check(send(trigger), "temporary English shortcut starts an idle fragment");
    for (const char c : std::string("User_42@example.com"))
        check(send(fcitx::Key(std::string(1,c))), "temporary English retains literal ASCII");
    check(send(fcitx::Key(FcitxKey_Return)) && doc.text == "User_42@example.com", "literal fragment commits");
    check(send(fcitx::Key(FcitxKey_n)) && engine.state(&doc)->controller().page().rawInput == "n", "Chinese resumes after temporary English");
    check(!send(trigger) && engine.state(&doc)->controller().page().rawInput == "n", "existing Chinese composition is preserved");
    send(fcitx::Key(FcitxKey_Escape));
    send(trigger); send(fcitx::Key(FcitxKey_a)); send(fcitx::Key(FcitxKey_Escape));
    check(doc.text == "User_42@example.com", "escape discards temporary fragment");
    send(trigger); send(fcitx::Key(FcitxKey_x)); send(fcitx::Key(FcitxKey_space));
    check(doc.text == "User_42@example.comx ", "space commits with literal separator");
    send(trigger); send(fcitx::Key(FcitxKey_v)); send(fcitx::Key(FcitxKey_2));
    send(fcitx::Key(FcitxKey_Return));
    check(doc.text.ends_with("v2"), "temporary English bypasses clipboard trigger");
    send(trigger); send(fcitx::Key(FcitxKey_a)); send(fcitx::Key(FcitxKey_b));
    send(fcitx::Key(FcitxKey_Left)); send(fcitx::Key(FcitxKey_X)); send(fcitx::Key(FcitxKey_Delete));
    send(fcitx::Key(FcitxKey_Return));
    check(doc.text.ends_with("aX"), "temporary fragment supports cursor insertion and forward deletion");
}

void testTemporaryEnglishApplicationShortcuts(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    for (const auto symbol : {FcitxKey_c, FcitxKey_Delete}) {
        Document doc(instance.inputContextManager());
        fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
        engine.activate(entries.front(), in);
        auto send = [&](fcitx::Key key) {
            fcitx::KeyEvent event(&doc, key);
            engine.keyEvent(entries.front(), event);
            return event.accepted();
        };
        check(send(fcitx::Key(FcitxKey_E,
                  fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift}))),
              "temporary English opens for application shortcut");
        for (const char c : std::string("abc"))
            check(send(fcitx::Key(std::string(1, c))), "shortcut fragment accepted");
        auto &controller = engine.state(&doc)->controller();
        check(!send(fcitx::Key(FcitxKey_Control_L)) && controller.temporaryEnglishMode() && doc.text.empty(),
              "physical modifier preserves the pending fragment");
        check(!send(fcitx::Key(symbol, fcitx::KeyState::Ctrl)), "application shortcut passes through");
        check(doc.text == "abc" && !controller.temporaryEnglishMode(),
              "both unmapped Ctrl+C and mapped Ctrl+Delete commit before passing through");
        fcitx::InputContextEvent out(&doc, fcitx::EventType::InputContextFocusOut);
        engine.deactivate(entries.front(), out);
        check(doc.text == "abc", "application shortcut fragment survives later focus out");
    }
}

void testAuxiliaryArrowNavigation(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    struct Query {
        const char *input;
        const char *first;
        const char *second;
    };
    for (const auto &query : {Query{"u+yiyi", "丂", "丈"}, Query{"v;jiantou", "→", "←"}}) {
        Document doc(instance.inputContextManager());
        fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
        engine.activate(entries.front(), in);
        auto send = [&](fcitx::Key key) {
            fcitx::KeyEvent event(&doc, key);
            engine.keyEvent(entries.front(), event);
            return event.accepted();
        };
        auto typeQuery = [&] {
            for (const char c : std::string(query.input))
                check(send(fcitx::Key(std::string(1, c))), "auxiliary navigation query accepted");
        };
        typeQuery();
        auto &controller = engine.state(&doc)->controller();
        check(controller.page().items.size() > 1 && controller.page().items[0].text == query.first &&
                  controller.page().items[1].text == query.second,
              "auxiliary arrow fixture has two distinct candidates");
        check(send(fcitx::Key(FcitxKey_Down)) && controller.page().cursor == 1,
              "Down selects the next auxiliary candidate");
        check(send(fcitx::Key(FcitxKey_Up)) && controller.page().cursor == 0,
              "Up selects the previous auxiliary candidate");
        check(send(fcitx::Key(FcitxKey_Return)) && doc.text == query.first,
              "Up-selected auxiliary candidate commits");
        typeQuery();
        check(send(fcitx::Key(FcitxKey_Down)) && send(fcitx::Key(FcitxKey_space)) &&
                  doc.text == std::string(query.first) + query.second,
              "Down-selected auxiliary candidate commits");
    }
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entries.front(), in);
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key);
        engine.keyEvent(entries.front(), event);
        return event.accepted();
    };
    for (const char c : std::string("ni")) send(fcitx::Key(std::string(1, c)));
    auto &controller = engine.state(&doc)->controller();
    check(controller.page().pageBoundaries.size() > 1, "pinyin arrow fixture spans multiple pages");
    const auto nextPage = controller.page().pageBoundaries[1].begin;
    check(send(fcitx::Key(FcitxKey_Down)) && controller.page().cursor == nextPage,
          "ordinary pinyin Down still moves a whole page");
    check(send(fcitx::Key(FcitxKey_Up)) && controller.page().cursor == 0,
          "ordinary pinyin Up still returns to the previous page");
#endif
}

void testPairSurvivesClientReset(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    const auto &entry = entries.front();
    Document doc(instance.inputContextManager());
    // GTK's legacy cursor fallback sends Reset after processing forwarded Left.
    doc.setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, in);
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key); engine.keyEvent(entry, event);
    };
    send(fcitx::Key(FcitxKey_parenleft));
    check(doc.text == "（）" && doc.cursor == 1, "legacy pair positions caret inside");
    fcitx::InputContextEvent reset(&doc, fcitx::EventType::InputContextReset);
    engine.reset(entry, reset);
    for (const char c : std::string("nihao")) send(fcitx::Key(std::string(1, c)));
    send(fcitx::Key(FcitxKey_space));
    check(doc.text == "（你好）" && doc.cursor == 3, "candidate commits inside the owned pair");
    send(fcitx::Key(FcitxKey_parenright));
    check(doc.text == "（你好）" && doc.cursor == 4, "client preedit reset preserves generated closer ownership");
    doc.text.clear(); doc.cursor = 0; doc.sync();
    send(fcitx::Key(FcitxKey_parenleft));
    fcitx::InputContextEvent out(&doc, fcitx::EventType::InputContextFocusOut);
    engine.reset(entry, out);
    send(fcitx::Key(FcitxKey_parenright));
    check(doc.text == "（））", "focus reset still drops generated pair ownership");
}
void testUnderscoreKeepsRawEnglish(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entries.front(), in);
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key); engine.keyEvent(entries.front(), event); return event.accepted();
    };
    auto type = [&](std::string_view s) { for (char c : s) send(fcitx::Key(std::string(1,c))); };
    type("inter");
    auto &controller = engine.state(&doc)->controller();
    const auto completion = std::find_if(controller.page().items.begin(), controller.page().items.end(),
        [](const auto &item) { return item.text == "interface"; });
    check(completion != controller.page().items.end(), "English completion is recalled");
    controller.moveCursor(std::distance(controller.page().items.begin(), completion));
    check(controller.page().items[controller.page().cursor].text == "interface", "expanded English completion is selected before underscore");
    check(send(fcitx::Key(FcitxKey_underscore)) && doc.text == "inter_" && controller.page().rawInput.empty(),
          "underscore commits typed English prefix rather than completion");
    type("name"); send(fcitx::Key(FcitxKey_underscore));
    check(doc.text == "inter_name_", "identifier continuation keeps raw spelling");
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.reset();
#ifdef MODERNIME_HAS_LIBIME_PINYIN
    type("foo");
    const auto chinese = std::find_if(controller.page().items.begin(), controller.page().items.end(),
        [](const auto &item) { return item.text == "佛"; });
    check(chinese != controller.page().items.end(), "foo recalls its competing Chinese candidate");
    controller.moveCursor(static_cast<int>(std::distance(controller.page().items.begin(), chinese)) -
                          static_cast<int>(controller.page().cursor));
    send(fcitx::Key(FcitxKey_underscore, fcitx::KeyState::Shift));
    type("bar"); send(fcitx::Key(FcitxKey_Return));
    check(doc.text == "foo_bar", "underscore keeps raw identifier even with a Chinese candidate selected");
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.reset();
#endif
    type("nihao"); send(fcitx::Key(FcitxKey_underscore));
    check(doc.text == "nihao_", "explicit identifier separator preserves spelling independent of pinyin validity");
    doc.text.clear(); doc.cursor = 0; doc.sync(); controller.reset();
    type("nihao"); send(fcitx::Key(FcitxKey_space)); send(fcitx::Key(FcitxKey_underscore));
    check(doc.text == "你好——", "underscore after confirmed Chinese still produces a dash");
}

void testCalculator(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entries.front(), in);
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key); engine.keyEvent(entries.front(), event); return event.accepted();
    };
    auto type = [&](std::string_view s) { for (char c : s) check(send(fcitx::Key(std::string(1,c))), "calculator input accepted"); };
    auto &controller = engine.state(&doc)->controller();
    type("v");
    const auto menu = controller.page();
    check(menu.items.size() == 5 && menu.items[2].text == "离线计算器 (V=)" &&
          menu.items[3].action == core::SelectionAction::OpenSymbols &&
          menu.items[4].action == core::SelectionAction::OpenFavoriteSymbols, "v menu keeps calculator after clipboard and appends symbol entries");
    core::CandidateSelectionRequest request;
    request.sessionId = menu.sessionId; request.displayGeneration = menu.displayGeneration;
    request.globalIndex = 2; request.expected = core::candidateIdentity(menu.items[2], menu);
    auto endpoint = controller.endpoint().lock();
    check(endpoint && endpoint->select(request) && controller.auxiliaryInputMode() == fcitx5::AuxiliaryInputMode::Calculator && doc.text.empty(),
          "clicking calculator opens query mode without committing its label");
    check(!endpoint->select(request), "stale v menu click cannot select a new result");
    type("6*7"); send(fcitx::Key(FcitxKey_Return));
    check(doc.text == "42", "calculator opened from menu commits computed result");
    doc.text.clear(); doc.cursor = 0; doc.sync();
    type("v"); send(fcitx::Key(FcitxKey_Tab)); send(fcitx::Key(FcitxKey_Tab)); send(fcitx::Key(FcitxKey_space));
    check(controller.auxiliaryInputMode() == fcitx5::AuxiliaryInputMode::Calculator && doc.text.empty(),
          "keyboard navigation opens calculator without repurposing numeric macro keys");
    send(fcitx::Key(FcitxKey_Escape));
    type("v=(2+3)*4");
    check(controller.auxiliaryInputMode() == fcitx5::AuxiliaryInputMode::Calculator &&
          controller.page().rawInput == "(2+3)*4" && controller.page().items.front().text == "20", "v= preserves digits and operators and evaluates precedence");
    check(send(fcitx::Key(FcitxKey_Return)) && doc.text == "20" && !controller.auxiliaryInputActive(), "enter commits result and returns to Chinese");
    type("v=0.1+0.2=");
    check(doc.text == "200.3" && !controller.auxiliaryInputActive(), "trailing equals commits decimal result");
    type("v=1/0");
    check(send(fcitx::Key(FcitxKey_space)) && doc.text == "200.3" && controller.auxiliaryInputActive(), "division error cannot commit hint or expression");
    send(fcitx::Key(FcitxKey_BackSpace)); type("2");
    check(controller.page().items.front().text == "0.5" && controller.select(0) && doc.text == "200.30.5", "invalid expression can be edited and result clicked");
    type("v=42"); send(fcitx::Key(FcitxKey_Escape));
    check(doc.text == "200.30.5" && !controller.auxiliaryInputActive(), "escape cancels calculator");
    type("v123");
    check(!controller.auxiliaryInputActive() && controller.page().rawInput == "v123", "existing numeric macro stays available");
    controller.reset();
}

void testSymbolSearch(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entries.front(), in);
    auto send = [&](fcitx::Key stroke) { fcitx::KeyEvent event(&doc, stroke); engine.keyEvent(entries.front(), event); return event.accepted(); };
    auto type = [&](std::string_view s) { for (char c : s) check(send(fcitx::Key(std::string(1,c))), "symbol query accepted"); };
    type("v;jiantou");
    auto &controller = engine.state(&doc)->controller();
    check(controller.auxiliaryInputMode() == fcitx5::AuxiliaryInputMode::Symbols && !controller.page().items.empty(), "v; opens offline symbol query");
    check(controller.setSymbolFilter("arrows", false), "category filtering API");
    const auto expected = controller.page().items.front().text;
    const auto old = controller.page();
    core::CandidateSelectionRequest stale{old.sessionId, old.displayGeneration, 0, core::candidateIdentity(old.items[0], old)};
    send(fcitx::Key(FcitxKey_z));
    check(!controller.select(stale), "changed symbol query rejects old selection");
    send(fcitx::Key(FcitxKey_BackSpace));
    check(send(fcitx::Key(FcitxKey_1)) && doc.text == expected && !controller.auxiliaryInputActive(), "symbol number selection commits generated result");
    type("v");
    bool opened = false;
    for (std::size_t n = 0; n < controller.page().items.size(); ++n)
        if (controller.page().items[n].action == core::SelectionAction::OpenFavoriteSymbols) { opened = controller.select(n); break; }
    check(opened && controller.auxiliaryInputMode() == fcitx5::AuxiliaryInputMode::Symbols, "v menu opens favorite symbols");
    check(send(fcitx::Key(FcitxKey_Return)) && doc.text == expected && controller.auxiliaryInputActive(), "empty favorite hint cannot commit");
    send(fcitx::Key(FcitxKey_Escape));
    type("v;xyzxyz"); send(fcitx::Key(FcitxKey_space));
    check(doc.text == expected && controller.auxiliaryInputActive(), "no-match hint cannot commit");
    controller.reset();
}

void testComponentLookup(fcitx::Instance &instance) {
    fcitx5::ModernIMEInputMethod engine(&instance.addonManager());
    const auto entries = engine.listInputMethods();
    const auto &entry = entries.front();
    Document doc(instance.inputContextManager());
    fcitx::InputContextEvent in(&doc, fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, in);
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key); engine.keyEvent(entry, event); return event.accepted();
    };
    auto type = [&](std::string_view s) { for (char c : s) check(send(fcitx::Key(std::string(1,c))), "component input accepted"); };
    type("u+mumu");
    auto &controller = engine.state(&doc)->controller();
    check(controller.auxiliaryInputMode() == fcitx5::AuxiliaryInputMode::ComponentLookup, "u+ explicitly opens lookup");
    check(controller.page().items.front().text == "林", "component pinyin recalls character");
    const auto oldPage = controller.page();
    core::CandidateSelectionRequest stale{oldPage.sessionId, oldPage.displayGeneration, 0,
        core::candidateIdentity(oldPage.items.front(), oldPage)};
    send(fcitx::Key(FcitxKey_m));
    check(!controller.select(stale) && doc.text.empty(), "edited query rejects stale selection");
    send(fcitx::Key(FcitxKey_BackSpace));
    check(send(fcitx::Key(FcitxKey_1)) && doc.text == "林" && !controller.auxiliaryInputActive(), "digit commits lookup without passing through provider");
    type("u+xyz");
    check(send(fcitx::Key(FcitxKey_Return)) && doc.text == "林" && controller.auxiliaryInputActive(), "missing query does not commit hint or raw input");
    send(fcitx::Key(FcitxKey_Escape));
    type("u+mumumu");
    check(controller.select(0) && doc.text == "林森", "candidate click shares validated selection path");
    type("u+riyue");
    controller.setSensitive(true);
    check(!controller.auxiliaryInputActive() && doc.text == "林森", "sensitive transition cancels lookup");
    controller.setSensitive(false);
    type("ni");
    check(!controller.handle({fcitx5::KeyKind::OpenComponentLookup}) && controller.page().rawInput == "ni", "lookup preserves unrelated Chinese composition");
    controller.reset();
}

}

int main() {
    char root[] = "/tmp/modernime-typing-XXXXXX";
    check(mkdtemp(root), "isolated environment");
    setenv("XDG_CONFIG_HOME", root, 1); setenv("XDG_DATA_HOME", root, 1);
    char name[] = "typing-test"; char disabled[] = "--disable=all";
    char *args[] = {name, disabled, nullptr};
    optind = 0;
    {
        fcitx::Instance instance(2, args);
        instance.addonManager().registerDefaultLoader(nullptr);
        instance.initialize();
        testPairs(instance);
        testContextCacheHostEdits(instance);
        testSmartPunctuation(instance);
        testApplicationModes(instance);
#ifdef MODERNIME_HAS_LIBIME_PINYIN
        testUndo(instance);
#endif
        testTemporaryEnglish(instance);
        testTemporaryEnglishApplicationShortcuts(instance);
        testAuxiliaryArrowNavigation(instance);
        testComponentLookup(instance);
        testUnderscoreKeepsRawEnglish(instance);
        testPairSurvivesClientReset(instance);
        testCalculator(instance);
        testSymbolSearch(instance);
    }
    std::filesystem::remove_all(root);
}
