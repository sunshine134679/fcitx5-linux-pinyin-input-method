#include "modernime/ui/ui_addon.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/core/settings.h"

#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <gtk/gtk.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void assertTrue(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "UI addon test failed: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

class TestInputContext final : public fcitx::InputContext {
public:
    explicit TestInputContext(fcitx::InputContextManager &manager)
        : InputContext(manager, "modernime-ui-test") {
        created();
    }
    ~TestInputContext() override { destroy(); }
    const char *frontend() const override { return "modernime-ui-test"; }
    void updatePreeditImpl() override {}
    void commitStringImpl(const std::string &) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void deleteSurroundingTextImpl(int, unsigned int) override {}
};

class TestCandidateWord final : public fcitx::CandidateWord {
public:
    TestCandidateWord(std::string text, int index, int *selectedStore)
        : text_(std::move(text)), index_(index), selectedStore_(selectedStore) {
        setText(fcitx::Text(text_));
    }
    void select(fcitx::InputContext *) const override {
        if (selectedStore_ != nullptr) {
            *selectedStore_ = index_;
        }
    }
    const std::string &candidateText() const { return text_; }
private:
    std::string text_;
    int index_;
    int *selectedStore_;
};

} // namespace

int main(int argc, char *argv[]) {
    // Initialize GTK in headless/test mode
    gtk_init_check(&argc, &argv);

    char templatePath[] = "/tmp/modernime-ui-addon-test-XXXXXX";
    char *tempDir = mkdtemp(templatePath);
    assertTrue(tempDir != nullptr, "Failed to create temporary directory");
    const std::filesystem::path testDir = tempDir;

    setenv("XDG_CONFIG_HOME", testDir.c_str(), 1);
    setenv("XDG_DATA_HOME", testDir.c_str(), 1);

    const auto configDir = testDir / "modernime";
    std::filesystem::create_directories(configDir);
    const auto settingsFile = configDir / "settings.conf";

    // 1. Initial settings with candidate.font_size = 14
    modernime::core::ModernIMESettings initialSettings = modernime::core::defaultSettings();
    initialSettings.candidateFontSize = 14;
    initialSettings.candidatePageSize = 5;
    assertTrue(modernime::core::SettingsStore::save(settingsFile, initialSettings),
               "Failed to write initial settings");

    char programName[] = "modernime_ui_addon_tests";
    char disableArg[] = "--disable=all";
    char *fcitxArgv[] = {programName, disableArg, nullptr};
    optind = 0;
    fcitx::Instance instance(2, fcitxArgv);
    instance.initialize();

    TestInputContext context(instance.inputContextManager());
    int selectedIndex = -1;

    auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
    candidateList->append(std::make_unique<TestCandidateWord>("你好", 0, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("拟好", 1, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("泥蒿", 2, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("拟稿", 3, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("呢好", 4, &selectedIndex));
    context.inputPanel().setCandidateList(std::move(candidateList));

    // Construct production UI addon
    modernime::ui::ModernIMEUserInterface uiAddon(&instance);

    // Initial update to measure and create layout
    uiAddon.update(fcitx::UserInterfaceComponent::InputPanel, &context);

    // 2. Perform click on first candidate area
    // In horizontal layout at font size 14, (x=30, y=14) hits candidate 0
    selectedIndex = -1;
    bool handled = uiAddon.handleButtonPress(30.0, 14.0);
    assertTrue(handled, "handleButtonPress should return true");
    assertTrue(selectedIndex == 0, "Candidate 0 should be selected with initial layout");

    // 3. Update configuration on disk: change font size to 28
    // This alters font metrics and requires layout invalidation
    modernime::core::ModernIMESettings newSettings = initialSettings;
    newSettings.candidateFontSize = 28;
    // Sleep to exceed the 1500ms throttling window in refreshSettingsIfNeeded
    g_usleep(1600000);
    assertTrue(modernime::core::SettingsStore::save(settingsFile, newSettings),
               "Failed to update settings file on disk");

    // 4. Test layout invalidation:
    // Before calling update(), call handleButtonPress().
    // handleButtonPress() refreshes settings, detects stamp change, and updates metrics.
    // The previous layout has old layoutGeneration, so hitTestCandidate() MUST reject it!
    selectedIndex = -1;
    uiAddon.handleButtonPress(30.0, 14.0);
    assertTrue(selectedIndex == -1,
               "Click with stale layout generation must be rejected when metrics changed");

    // 5. Now update UI: this remeasures layout with new metrics (font size 28)
    uiAddon.update(fcitx::UserInterfaceComponent::InputPanel, &context);

    // 6. Click on candidate with new layout:
    // With font size 28, candidate 0 and candidate 1 are wider.
    // Click candidate 0
    selectedIndex = -1;
    uiAddon.handleButtonPress(30.0, 20.0);
    assertTrue(selectedIndex == 0,
               "Candidate 0 should be selected in updated layout");

    // Verify candidate semantics and text
    auto activeList = context.inputPanel().candidateList();
    assertTrue(activeList != nullptr && activeList->size() == 5,
               "Candidate list preserved after layout refresh");
    assertTrue(activeList->candidate(selectedIndex).text().toString() == "你好",
               "Selected candidate text semantics preserved");

    // Cleanup
    std::filesystem::remove_all(testDir);

    std::cout << "[PASS] modernime_ui_addon_tests: config refresh, layout invalidation, and candidate selection verified\n";
    return 0;
}
