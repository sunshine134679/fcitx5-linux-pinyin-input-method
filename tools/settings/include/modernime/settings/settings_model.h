#pragma once

#include "modernime/core/settings.h"

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::settings {

class SettingsWindowModel final {
public:
    using Revision = std::uint64_t;

    explicit SettingsWindowModel(std::filesystem::path path);

    const core::ModernIMESettings &settings() const { return edited_; }
    const core::ModernIMESettings &savedSettings() const { return loaded_; }
    bool dirty() const;
    core::SettingsValidationResult validation() const {
        return core::validateSettings(edited_);
    }
    const std::vector<std::string> &loadDiagnostics() const {
        return loadDiagnostics_;
    }

    void setSettings(core::ModernIMESettings settings);
    void setCandidateOptions(bool numberSelection, bool arrowNavigation,
                             bool pageNavigation);
    void setClipboardOptions(bool enabled, std::string trigger);
    void editDefaults();
    bool save(std::string *error = nullptr);
    void resetEdits();
    bool reloadRequired() const { return reloadedRevision_ < savedRevision_; }
    Revision savedRevision() const { return savedRevision_; }
    void markReloaded(Revision revision) {
        if (revision == savedRevision_) {
            reloadedRevision_ = revision;
        }
    }
    std::string_view validationError() const { return lastError_; }

private:
    std::filesystem::path path_;
    core::ModernIMESettings loaded_;
    core::ModernIMESettings edited_;
    std::vector<std::string> loadDiagnostics_;
    std::string lastError_;
    bool defaultsEdited_ = false;
    Revision savedRevision_ = 0;
    Revision reloadedRevision_ = 0;
};

} // namespace modernime::settings
