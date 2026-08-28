#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "fcitx5 input panel test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
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

protected:
    void commitStringImpl(const std::string &) override {}
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override { ++preeditUpdates; }
};

} // namespace

int main() {
    fcitx::InputContextManager manager;
    TestInputContext inputContext(manager);
    inputContext.setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    inputContext.focusIn();
    modernime::fcitx5::FcitxEngineHost host(inputContext);

    modernime::core::CandidatePage page;
    page.preedit = "df";
    page.items.push_back({"地方", "df", 0});
    host.publishPage(page);

    assertTrue(inputContext.inputPanel().clientPreedit().toString() == "df",
               "preedit is published to the client input area");
    assertTrue(inputContext.inputPanel().clientPreedit().cursor() == 2,
               "client preedit cursor follows the end of the input");
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
    assertTrue(inputContext.inputPanel().clientPreedit().empty(),
               "client preedit is cleared after reset");
    assertTrue(inputContext.preeditUpdates == 3,
               "clearing preedit sends an update");

    modernime::fcitx5::ModernIMEController controller(host);
    host.setController(controller);
    controller.setClipboardEntries({"first clipboard", "second clipboard",
                                    "third clipboard", "fourth clipboard",
                                    "fifth clipboard", "sixth clipboard"});
    modernime::fcitx5::ClipboardTrigger featureTrigger;
    host.setBeforeCandidateSelection(
        [&featureTrigger] { featureTrigger.reset(); });
    featureTrigger.feed(
        {modernime::fcitx5::KeyKind::Character, 'v', 0}, true);
    assertTrue(featureTrigger.pending(),
               "feature trigger is pending before a candidate click");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::OpenFeatureMenu, 'V', '2'}),
               "feature menu is published before the clipboard list");
    const auto featureList = inputContext.inputPanel().candidateList();
    assertTrue(featureList != nullptr &&
                   featureList->layoutHint() ==
                       fcitx::CandidateLayoutHint::Horizontal &&
                   inputContext.inputPanel().preedit().toString() == "V" &&
                   featureList->size() == 1 &&
                   featureList->label(0).toString() == "2" &&
                   featureList->candidate(0).text().toString() == "剪切板",
               "feature menu uses the horizontal candidate panel");
    featureList->candidate(0).select(&inputContext);
    assertTrue(!featureTrigger.pending() && controller.clipboardMode(),
               "clicking a function candidate clears the pending trigger");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::OpenFeatureMenu, 'V', '2'}),
               "feature menu reopens after clicking its candidate");
    assertTrue(controller.handle(
                   {modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "feature menu digit opens the clipboard list");
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
    return EXIT_SUCCESS;
}
