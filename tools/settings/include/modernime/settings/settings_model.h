#pragma once

#include "modernime/core/settings.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace modernime::settings {

class SettingsWindowModel final {
public:
    explicit SettingsWindowModel(std::filesystem::path path);

    const core::ModernIMESettings &settings() const { return edited_; }
    bool dirty() const { return edited_ != loaded_; }

    void setSettings(core::ModernIMESettings settings);
    void setCandidateOptions(bool numberSelection, bool arrowNavigation,
                             bool pageNavigation);
    bool save(std::string *error = nullptr);
    bool resetDefaults(std::string *error = nullptr);
    void resetEdits();
    std::string_view validationError() const { return lastError_; }

private:
    std::filesystem::path path_;
    core::ModernIMESettings loaded_;
    core::ModernIMESettings edited_;
    std::string lastError_;
};

} // namespace modernime::settings
