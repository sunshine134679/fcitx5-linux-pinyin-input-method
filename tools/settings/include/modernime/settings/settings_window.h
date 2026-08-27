#pragma once

#include "modernime/settings/settings_model.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace modernime::settings {

class SettingsWindow final {
public:
    SettingsWindow(void *application, std::filesystem::path settingsPath);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    void present();
    void showBasicPage();
    void showCandidatePage();
    void showLearningPage();
    void showDictionaryPage();
    void showStatusPage();
    void presentError(std::string_view message);

    class Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
