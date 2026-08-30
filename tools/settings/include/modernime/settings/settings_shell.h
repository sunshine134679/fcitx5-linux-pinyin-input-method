#pragma once

#include "modernime/core/settings.h"
#include "modernime/settings/page_registry.h"

#include <memory>
#include <string_view>

struct _GtkApplication;
struct _GtkWidget;

namespace modernime::settings {

using GtkApplication = ::_GtkApplication;
using GtkWidget = ::_GtkWidget;

class SettingsShell final {
public:
    SettingsShell(GtkApplication *application, core::SettingsPaths paths);
    ~SettingsShell();

    SettingsShell(const SettingsShell &) = delete;
    SettingsShell &operator=(const SettingsShell &) = delete;

    GtkWidget *window() const;
    void present();
    void show(SettingsPageId page, std::string_view target = {});
    void presentError(std::string_view message);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
