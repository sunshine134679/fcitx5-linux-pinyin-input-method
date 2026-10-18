#pragma once

#include "modernime/core/settings.h"
#include "modernime/settings/page_registry.h"

#include <memory>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class SettingsShell;

class SettingsWindow final {
public:
    SettingsWindow(void *application, core::SettingsPaths paths);
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow &) = delete;
    SettingsWindow &operator=(const SettingsWindow &) = delete;

    void present();
    GtkWidget *window() const;
    void show(SettingsPageId page, std::string_view target = {});
    void showOverviewPage();
    void showBasicPage();
    void showCandidatePage();
    void showClipboardPage();
    void showLearningPage();
    void showDictionaryPage();
    void showStatusPage();
    void showUpdatesPage();
    void presentError(std::string_view message);

private:
    std::unique_ptr<SettingsShell> shell_;
};

} // namespace modernime::settings
