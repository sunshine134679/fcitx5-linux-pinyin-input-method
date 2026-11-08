#include "modernime/fcitx5/engine.h"
#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx-utils/keysymgen.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "clipboard trigger test failed: " << message << '\n';
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

    bool append(std::string_view) override { return true; }
    bool eraseLast() override { return true; }
    bool replaceInput(std::string_view) override { return true; }
    bool select(std::size_t) override {
        current.clear();
        return true;
    }
    bool remove(std::size_t) override { return true; }
    void reset() override { current.clear(); }
    const modernime::core::CandidatePage &page() const override { return current; }
};

void testControllerCommandSelectionOpensClipboard() {
    RecordingHost host;
    FakeProvider provider;
    provider.current.items = {
        {"v", "v", 0, modernime::core::CandidateSource::Raw, 1},
        {"剪贴板 (V+2)", "v", 1, modernime::core::CandidateSource::Command, 1}
    };
    provider.current.rawInput = "v";
    provider.current.preedit = "v";

    modernime::fcitx5::ControllerOptions options;
    options.clipboardEnabled = true;
    options.clipboardTrigger = "V+2";
    modernime::fcitx5::ModernIMEController controller(host, &provider, options);
    controller.setClipboardEntries({"entry1", "entry2"});
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Character, 'v', 0}), "type v");

    // 选中命令项打开剪贴板，不提交文本
    assertTrue(controller.select(1), "select command candidate succeeds");
    assertTrue(controller.clipboardMode(), "controller enters clipboard mode");
    assertTrue(host.commits.empty(), "command selection does not commit text");
    assertTrue(controller.page().items.size() == 2, "clipboard candidates populated");
    assertTrue(controller.page().items[0].text == "entry1", "first clipboard candidate");

    // 在剪贴板中选词提交
    assertTrue(controller.select(0), "select first clipboard entry");
    assertTrue(host.commits.size() == 1 && host.commits.back() == "entry1",
               "clipboard entry committed");
    assertTrue(!controller.clipboardMode(), "clipboard mode exited");
}

void testEmptyClipboardStateAndEscape() {
    RecordingHost host;
    FakeProvider provider;
    provider.current.items = {
        {"v", "v", 0, modernime::core::CandidateSource::Raw, 1},
        {"剪贴板 (V+2)", "v", 1, modernime::core::CandidateSource::Command, 1}
    };
    provider.current.rawInput = "v";
    provider.current.preedit = "v";

    modernime::fcitx5::ControllerOptions options;
    options.clipboardEnabled = true;
    options.clipboardTrigger = "V+2";
    modernime::fcitx5::ModernIMEController controller(host, &provider, options);
    controller.setClipboardEntries({}); // empty clipboard
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Character, 'v', 0}), "type v");

    assertTrue(controller.select(1), "open empty clipboard");
    assertTrue(controller.clipboardMode(), "clipboard mode active");
    assertTrue(controller.page().items.size() == 1, "shows single placeholder");
    assertTrue(controller.page().items[0].text == "(剪贴板为空)", "placeholder text");
    assertTrue(controller.page().items[0].source == modernime::core::CandidateSource::Command,
               "placeholder has Command source");

    // Escape 退出
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Escape, 0, 0}),
               "Escape consumed");
    assertTrue(!controller.clipboardMode(), "clipboard mode closed on escape");
    assertTrue(host.commits.empty(), "nothing committed on escape");

    // 再次打开并选中空占位项，不应提交文本
    provider.current.items = {
        {"v", "v", 0, modernime::core::CandidateSource::Raw, 1},
        {"剪贴板 (V+2)", "v", 1, modernime::core::CandidateSource::Command, 1}
    };
    provider.current.rawInput = "v";
    provider.current.preedit = "v";
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Character, 'v', 0}), "type v again");
    assertTrue(controller.select(1), "open empty clipboard again");
    assertTrue(controller.select(0), "select empty placeholder");
    assertTrue(!controller.clipboardMode(), "clipboard closed");
    assertTrue(host.commits.empty(), "placeholder not committed");
}

bool fires(std::string_view preedit, const fcitx::Key &key,
           std::string_view trigger) {
    return modernime::fcitx5::clipboardTriggerFire(preedit, key, trigger);
}

void testAsyncClipboardPageLifecycle() {
    using modernime::core::ClipboardAvailability;
    using modernime::fcitx5::KeyKind;
    RecordingHost host;
    modernime::fcitx5::ModernIMEController controller(host);
    const auto empty = std::make_shared<const std::vector<std::string>>();
    const auto ready = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"loaded text"});
    controller.setClipboardSnapshot(empty, ClipboardAvailability::Loading);
    assertTrue(controller.handle({KeyKind::OpenClipboard, 0, 0}), "open loading history");
    assertTrue(controller.page().items.size() == 1 && controller.page().items[0].text == "(正在读取剪贴板…)",
               "initial history has explicit Loading placeholder");
    const auto oldPage = controller.page();
    modernime::core::CandidateSelectionRequest oldRequest;
    oldRequest.sessionId = oldPage.sessionId;
    oldRequest.displayGeneration = oldPage.displayGeneration;
    oldRequest.globalIndex = 0;
    oldRequest.expected = modernime::core::candidateIdentity(oldPage.items[0], oldPage);
    controller.setClipboardSnapshot(ready, ClipboardAvailability::Ready);
    assertTrue(controller.page().items[0].text == "loaded text" && controller.page().displayGeneration > oldPage.displayGeneration,
               "completion refreshes still-open placeholder with new identity generation");
    assertTrue(!controller.select(oldRequest) && host.commits.empty() && controller.clipboardMode(),
               "old Loading selection cannot commit or close newly loaded page");
    const auto newer = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"newer text"});
    controller.setClipboardSnapshot(newer, ClipboardAvailability::Ready);
    assertTrue(controller.page().items[0].text == "loaded text", "normal clipboard page remains frozen");
    assertTrue(controller.select(0) && host.commits == std::vector<std::string>{"loaded text"}, "frozen page commits the displayed entry");

    controller.setClipboardSnapshot(empty, ClipboardAvailability::Unavailable);
    controller.handle({KeyKind::OpenClipboard, 0, 0});
    assertTrue(controller.page().items[0].text == "(剪贴板暂不可用)", "read failure is visible");
    assertTrue(controller.select(0) && host.commits.size() == 1, "error placeholder never commits");
    controller.setClipboardSnapshot(empty, ClipboardAvailability::Loading);
    controller.handle({KeyKind::OpenClipboard, 0, 0});
    assertTrue(controller.select(0) && host.commits.size() == 1, "loading placeholder never commits");

    controller.setClipboardSnapshot(empty, ClipboardAvailability::Loading);
    controller.handle({KeyKind::OpenClipboard, 0, 0});
    controller.handle({KeyKind::Escape, 0, 0});
    controller.setClipboardSnapshot(ready, ClipboardAvailability::Ready);
    assertTrue(!controller.clipboardMode() && controller.page().items.empty(), "completion after Escape never reopens history");
    controller.handle({KeyKind::Character, 'n', 0});
    controller.setClipboardSnapshot(newer, ClipboardAvailability::Ready);
    assertTrue(!controller.clipboardMode() && controller.page().preedit == "n", "completion preserves new composition");
    controller.handle({KeyKind::Escape, 0, 0});
    controller.setClipboardSnapshot(ready, ClipboardAvailability::Ready);
    controller.handle({KeyKind::OpenClipboard, 0, 0});
    assertTrue(controller.page().items[0].text == "loaded text", "same snapshot can be attached after composition cleared");

    controller.handle({KeyKind::Escape, 0, 0});
    controller.setClipboardSnapshot(empty, ClipboardAvailability::Loading);
    controller.handle({KeyKind::OpenClipboard, 0, 0});
    controller.setSensitive(true);
    controller.setClipboardSnapshot(ready, ClipboardAvailability::Ready);
    assertTrue(controller.page().items[0].text == "(正在读取剪贴板…)", "sensitive context does not refresh Loading page");
    controller.setSensitive(false);
    controller.setClipboardSnapshot(ready, ClipboardAvailability::Ready);
    assertTrue(controller.page().items[0].text == "loaded text", "same completion snapshot can refresh after sensitive interval");
}

} // namespace

int main() {
    testAsyncClipboardPageLifecycle();
    testControllerCommandSelectionOpensClipboard();
    testEmptyClipboardStateAndEscape();

    const fcitx::Key digitTwo(FcitxKey_2);
    const fcitx::Key digitOne(FcitxKey_1);
    const fcitx::Key digitSeven(FcitxKey_7);
    const fcitx::Key ctrlDigitTwo(
        FcitxKey_2, fcitx::KeyStates(fcitx::KeyState::Ctrl));
    const auto shift = fcitx::KeyStates(fcitx::KeyState::Shift);
    const fcitx::Key shiftDigitTwo(FcitxKey_2, shift);

    // 组合恰好是单个触发字母时，触发数字打开剪贴板。
    assertTrue(fires("v", digitTwo, "V+2"),
               "single-letter preedit fires with the trigger digit");
    assertTrue(fires("V", digitTwo, "V+2"),
               "preedit case is normalized before matching");
    assertTrue(fires("b", digitSeven, "B+7"), "custom trigger letter fires");
    assertTrue(fires("B", digitSeven, "B+7"), "custom uppercase letter fires");

    // 组合不是单个触发字母或按键不匹配时，不触发任何东西。
    assertTrue(!fires("vn", digitTwo, "V+2"),
               "a longer preedit never fires the trigger");
    assertTrue(!fires("", digitTwo, "V+2"), "an empty preedit never fires");
    assertTrue(!fires("v", digitOne, "V+2"),
               "a different digit never fires the trigger");
    assertTrue(!fires("v", digitTwo, "B+7"),
               "a matching key with a different trigger does not fire");
    assertTrue(!fires("v", shiftDigitTwo, "V+2"),
               "shift-modified digits never fire");
    assertTrue(!fires("v", ctrlDigitTwo, "V+2"),
               "ctrl-modified digits never fire");
    assertTrue(!fires("v", digitTwo, "V+0"),
               "an invalid trigger never fires");
    return EXIT_SUCCESS;
}
