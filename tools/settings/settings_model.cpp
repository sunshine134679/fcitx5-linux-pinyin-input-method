#include "modernime/settings/settings_model.h"

#include <utility>

namespace modernime::settings {

SettingsWindowModel::SettingsWindowModel(std::filesystem::path path)
    : path_(std::move(path)),
      loaded_(core::SettingsStore::load(path_).settings),
      edited_(loaded_) {}

void SettingsWindowModel::setSettings(core::ModernIMESettings settings) {
    edited_ = std::move(settings);
    lastError_.clear();
}

void SettingsWindowModel::setCandidateOptions(bool numberSelection,
                                              bool arrowNavigation,
                                              bool pageNavigation) {
    auto settings = edited_;
    settings.numberSelection = numberSelection;
    settings.arrowNavigation = arrowNavigation;
    settings.pageNavigation = pageNavigation;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setClipboardOptions(bool enabled, std::string trigger) {
    auto settings = edited_;
    settings.clipboardEnabled = enabled;
    settings.clipboardTrigger = std::move(trigger);
    setSettings(std::move(settings));
}

bool SettingsWindowModel::save(std::string *error) {
    std::string saveError;
    if (!core::SettingsStore::save(path_, edited_, &saveError)) {
        lastError_ = saveError;
        if (error != nullptr) {
            *error = saveError;
        }
        return false;
    }
    loaded_ = edited_;
    lastError_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool SettingsWindowModel::resetDefaults(std::string *error) {
    std::string saveError;
    if (!core::SettingsStore::reset(path_, &saveError)) {
        lastError_ = saveError;
        if (error != nullptr) {
            *error = saveError;
        }
        return false;
    }
    loaded_ = core::defaultSettings();
    edited_ = loaded_;
    lastError_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void SettingsWindowModel::resetEdits() {
    edited_ = loaded_;
    lastError_.clear();
}

} // namespace modernime::settings
