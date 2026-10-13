#include "modernime/settings/settings_model.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "settings window model test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testDirectory() {
    const auto path = std::filesystem::temp_directory_path() /
                      "modernime-settings-window-test";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    assertTrue(!error, "test directory is created");
    return path;
}

} // namespace

int main() {
    const auto directory = testDirectory();
    const auto path = directory / "settings.conf";
    modernime::settings::SettingsWindowModel model(path);
    assertTrue(!model.dirty(), "fresh model is clean");
    assertTrue(model.savedSettings() == model.settings(),
               "fresh model exposes its saved settings");

    model.editDefaults();
    assertTrue(model.dirty(), "editing defaults marks the model dirty");
    assertTrue(!std::filesystem::exists(path),
               "editing defaults does not create a settings file");
    std::string error;
    assertTrue(model.save(&error), "edited defaults save: " + error);
    assertTrue(!model.reloadRequired(),
               "saving equal defaults does not require an engine reload");
    model.markReloaded(model.savedRevision());
    assertTrue(!model.reloadRequired(),
               "marking the engine reloaded clears reload state");

    auto settings = model.settings();
    settings.inputEnabled = false;
    settings.defaultMode = modernime::core::InputMode::English;
    model.setSettings(settings);
    assertTrue(model.dirty(), "edited settings are dirty");
    assertTrue(model.save(&error), "edited settings save: " + error);
    assertTrue(!model.dirty(), "saved model is clean");
    assertTrue(model.reloadRequired(),
               "saving changed settings requires an engine reload");
    const auto firstSavedRevision = model.savedRevision();
    auto settingsDuringReload = model.settings();
    settingsDuringReload.contextLearningEnabled = false;
    model.setSettings(settingsDuringReload);
    assertTrue(model.save(&error), "settings save during reload: " + error);
    const auto secondSavedRevision = model.savedRevision();
    assertTrue(secondSavedRevision > firstSavedRevision,
               "changed saves advance the saved configuration revision");
    model.markReloaded(firstSavedRevision);
    assertTrue(model.reloadRequired(),
               "an older reload cannot acknowledge a newer saved revision");
    model.markReloaded(secondSavedRevision);
    assertTrue(!model.reloadRequired(),
               "the current saved revision can be acknowledged as reloaded");

    modernime::settings::SettingsWindowModel reloaded(path);
    assertTrue(!reloaded.settings().inputEnabled &&
                   reloaded.settings().defaultMode ==
                       modernime::core::InputMode::English &&
                   !reloaded.settings().contextLearningEnabled,
               "saved settings reload into a new model");

    auto editedAgain = reloaded.settings();
    editedAgain.learningEnabled = false;
    reloaded.setSettings(editedAgain);
    assertTrue(reloaded.dirty(), "second edit is dirty");
    reloaded.resetEdits();
    assertTrue(reloaded.settings().learningEnabled,
               "reset edits restores last saved settings");
    assertTrue(!reloaded.dirty(), "reset edits clears dirty state");

    reloaded.setCandidateOptions(false, false, false);
    assertTrue(!reloaded.settings().numberSelection &&
                   !reloaded.settings().arrowNavigation &&
                   !reloaded.settings().pageNavigation,
               "candidate options can be edited");
    assertTrue(reloaded.save(&error), "candidate options save: " + error);
    modernime::settings::SettingsWindowModel candidateReloaded(path);
    assertTrue(!candidateReloaded.settings().numberSelection &&
                   !candidateReloaded.settings().arrowNavigation &&
                   !candidateReloaded.settings().pageNavigation,
               "candidate options persist");

    candidateReloaded.setClipboardOptions(false, "B+7");
    assertTrue(!candidateReloaded.settings().clipboardEnabled &&
                   candidateReloaded.settings().clipboardTrigger == "B+7",
               "clipboard options can be edited");
    assertTrue(candidateReloaded.save(&error),
               "clipboard options save: " + error);
    modernime::settings::SettingsWindowModel clipboardReloaded(path);
    assertTrue(!clipboardReloaded.settings().clipboardEnabled &&
                   clipboardReloaded.settings().clipboardTrigger == "B+7",
               "clipboard options persist");

    auto invalidSettings = clipboardReloaded.settings();
    invalidSettings.toggleKey = "Ctrl+A";
    clipboardReloaded.setSettings(invalidSettings);
    assertTrue(!clipboardReloaded.validation().valid &&
                   !clipboardReloaded.validation().issues.empty(),
               "model exposes validation errors for current edits");

    const auto blocked = directory / "blocked";
    std::filesystem::create_directory(blocked);
    modernime::settings::SettingsWindowModel failed(blocked);
    auto failedSettings = failed.settings();
    failedSettings.learningEnabled = false;
    const auto failedSavedSettings = failed.savedSettings();
    failed.setSettings(failedSettings);
    error.clear();
    assertTrue(!failed.save(&error), "failed save is reported");
    assertTrue(failed.dirty() && !error.empty(),
               "failed save keeps edits and exposes an error");
    assertTrue(failed.settings() == failedSettings &&
                   failed.savedSettings() == failedSavedSettings,
               "failed save preserves both the draft and saved snapshot");
    assertTrue(!failed.reloadRequired(),
               "failed save does not require an engine reload");

    reloaded.editDefaults();
    assertTrue(reloaded.settings() == modernime::core::defaultSettings() &&
                   reloaded.dirty(),
               "editing defaults replaces edits without saving");

    const auto diagnosticPath = directory / "diagnostic-settings.conf";
    {
        std::ofstream output(diagnosticPath);
        output << "input.toggle_key=Ctrl+A\n";
    }
    modernime::settings::SettingsWindowModel diagnosticModel(diagnosticPath);
    assertTrue(diagnosticModel.settings().toggleKey == "Ctrl+Shift+Space",
               "invalid loaded key falls back to the default");
    assertTrue(!diagnosticModel.loadDiagnostics().empty(),
               "loaded settings diagnostics are exposed to the client");

    // Test external modification and conflict handling in SettingsWindowModel
    const auto extPath = directory / "external-test-settings.conf";
    modernime::settings::SettingsWindowModel extModel(extPath);
    assertTrue(!extModel.hasExternalModification(), "fresh model has no external modification");
    std::string extErr;
    assertTrue(extModel.save(&extErr), "initial save succeeds: " + extErr);
    assertTrue(!extModel.hasExternalModification(), "saved model has no external modification");

    // Modify file externally behind model's back
    {
        std::ofstream extOut(extPath, std::ios::app);
        extOut << "# externally added line\n";
    }
    assertTrue(extModel.hasExternalModification(), "external modification is detected");

    // Attempting to save should fail due to external modification conflict
    auto modifiedDraft = extModel.settings();
    modifiedDraft.candidatePageSize = 7;
    extModel.setSettings(modifiedDraft);
    std::string conflictErr;
    assertTrue(!extModel.save(&conflictErr), "saving with external modification fails");
    assertTrue(!conflictErr.empty(), "conflict error is reported");

    // Reload from disk clears conflict
    assertTrue(extModel.reloadFromDisk(), "reloadFromDisk succeeds");
    assertTrue(!extModel.hasExternalModification(), "after reloadFromDisk, conflict is cleared");

    return EXIT_SUCCESS;
}
