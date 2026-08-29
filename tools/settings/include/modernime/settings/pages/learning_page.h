#pragma once

#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_model.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class LearningPage final {
public:
    static constexpr auto pageId = SettingsPageId::Learning;

    LearningPage(SettingsWindowModel &settings,
                 std::filesystem::path learningPath,
                 std::function<void()> settingsChanged,
                 std::function<void(std::string)> notify);
    ~LearningPage();

    LearningPage(const LearningPage &) = delete;
    LearningPage &operator=(const LearningPage &) = delete;

    GtkWidget *widget() const;
    void refresh(bool notify);
    void refreshSettings();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
