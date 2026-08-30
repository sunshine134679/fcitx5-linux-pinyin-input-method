#pragma once

#include "modernime/core/settings.h"

#include <memory>
#include <string_view>

namespace modernime::settings {

class SettingsShell;

class SettingsWindow final {
public:
    SettingsWindow(void *application, core::SettingsPaths paths);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    void present();
    void showBasicPage();
    void showCandidatePage();
    void showClipboardPage();
    void showLearningPage();
    void showDictionaryPage();
    void showStatusPage();
    void presentError(std::string_view message);

private:
    std::unique_ptr<SettingsShell> shell_;
};

} // namespace modernime::settings
