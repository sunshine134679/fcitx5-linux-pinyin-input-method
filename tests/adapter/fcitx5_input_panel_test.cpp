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
    host.publishPage(page);
    assertTrue(inputContext.inputPanel().clientPreedit().empty(),
               "client preedit is cleared after reset");
    assertTrue(inputContext.preeditUpdates == 2,
               "clearing preedit sends an update");
    return EXIT_SUCCESS;
}
