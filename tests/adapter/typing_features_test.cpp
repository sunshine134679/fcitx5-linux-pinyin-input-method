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
    auto send = [&](fcitx::Key key) {
        fcitx::KeyEvent event(&doc, key); engine.keyEvent(entry, event); return event.accepted();
    };
    for (const char c : std::string("nihao")) check(send(fcitx::Key(std::string(1, c))), "pinyin accepted");
    check(send(fcitx::Key(FcitxKey_space)) && doc.text == "你好", "candidate commits to document");
    const auto undo = fcitx::Key(FcitxKey_BackSpace,
        fcitx::KeyStates({fcitx::KeyState::Ctrl, fcitx::KeyState::Shift}));
    check(send(undo), "undo shortcut handled");
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
        testSmartPunctuation(instance);
        testApplicationModes(instance);
#ifdef MODERNIME_HAS_LIBIME_PINYIN
        testUndo(instance);
#endif
        testTemporaryEnglish(instance);
        testComponentLookup(instance);
    }
    std::filesystem::remove_all(root);
}
