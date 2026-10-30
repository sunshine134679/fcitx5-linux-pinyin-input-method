#include "modernime/settings/settings_model.h"

#include <utility>

namespace modernime::settings {

SettingsWindowModel::SettingsWindowModel(std::filesystem::path path)
    : path_(std::move(path)) {
    const auto loaded = core::SettingsStore::load(path_);
    loaded_ = loaded.settings;
    edited_ = loaded_;
    loadDiagnostics_ = loaded.diagnostics;
    loadedHash_ = loaded.contentHash;
    savedHash_ = loaded.contentHash;
    rawLines_ = loaded.rawLines;
}

bool SettingsWindowModel::dirty() const {
    return defaultsEdited_ || edited_ != loaded_;
}

bool SettingsWindowModel::hasExternalModification() const {
    const bool fileExists = std::filesystem::exists(path_);
    if (savedHash_.empty()) {
        return fileExists;
    }
    if (!fileExists) {
        return true;
    }
    return core::computeFileContentHash(path_) != savedHash_;
}

bool SettingsWindowModel::reloadFromDisk(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto loaded = core::SettingsStore::load(path_);
    loaded_ = loaded.settings;
    edited_ = loaded_;
    loadDiagnostics_ = loaded.diagnostics;
    loadedHash_ = loaded.contentHash;
    savedHash_ = loaded.contentHash;
    rawLines_ = loaded.rawLines;
    defaultsEdited_ = false;
    lastError_.clear();
    return true;
}

void SettingsWindowModel::setSettings(core::ModernIMESettings settings) {
    edited_ = std::move(settings);
    lastError_.clear();
}

void SettingsWindowModel::setCandidateAppearance(int candidatePageSize,
                                                 int candidateFontSize) {
    auto settings = edited_;
    settings.candidatePageSize = candidatePageSize;
    settings.candidateFontSize = candidateFontSize;
    setSettings(std::move(settings));
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

void SettingsWindowModel::setCandidateSettings(int candidatePageSize,
                                              int candidateFontSize,
                                              bool numberSelection,
                                              bool arrowNavigation,
                                              bool pageNavigation,
                                              bool englishDefinitionEnabled) {
    auto settings = edited_;
    settings.candidatePageSize = candidatePageSize;
    settings.candidateFontSize = candidateFontSize;
    settings.numberSelection = numberSelection;
    settings.arrowNavigation = arrowNavigation;
    settings.pageNavigation = pageNavigation;
    settings.englishDefinitionEnabled = englishDefinitionEnabled;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setCharacterExtractionEnabled(bool enabled) {
    auto settings = edited_;
    settings.characterExtractionEnabled = enabled;
    setSettings(std::move(settings));
}

void SettingsWindowModel::setClipboardOptions(bool enabled, std::string trigger) {
    auto settings = edited_;
    settings.clipboardEnabled = enabled;
    settings.clipboardTrigger = std::move(trigger);
    setSettings(std::move(settings));
}

void SettingsWindowModel::editDefaults() {
    setSettings(core::defaultSettings());
    defaultsEdited_ = true;
}

bool SettingsWindowModel::save(std::string *error) {
    const bool changed = edited_ != loaded_;
    auto saveResult = core::SettingsStore::saveWithResult(
        path_, edited_, savedHash_, &rawLines_);
    lastSaveResult_ = saveResult;
    if (saveResult.status != core::SettingsSaveStatus::Saved) {
        lastError_ = saveResult.error;
        if (error != nullptr) {
            *error = saveResult.error;
        }
        return false;
    }
    loaded_ = std::move(saveResult.settings);
    defaultsEdited_ = false;
    savedHash_ = std::move(saveResult.contentHash);
    loadedHash_ = savedHash_;
    rawLines_ = std::move(saveResult.rawLines);
    if (changed) {
        ++savedRevision_;
    }
    lastError_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void SettingsWindowModel::resetEdits() {
    edited_ = loaded_;
    defaultsEdited_ = false;
    lastError_.clear();
}

} // namespace modernime::settings
