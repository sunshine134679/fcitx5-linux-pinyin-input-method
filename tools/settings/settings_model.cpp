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

void SettingsWindowModel::resetEdits() {
    edited_ = loaded_;
    lastError_.clear();
}

} // namespace modernime::settings
